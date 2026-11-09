// GX command processor: receives the command stream the game writes to the write-gather
// pipe (0xCC008000), walks it command by command, keeps the register state (gx_state.h)
// and hands draws and EFB copies to the D3D12 backend.
//
//   - Exact command sizes, including draws (vertex size from the current VCD/VAT), and
//     display lists called from the stream.
//   - XF loads, direct (0x10) and indexed from memory arrays (0x20-0x38).
//   - PE tokens and "draw done" (BP 0x47, 0x48, 0x45): token register + PE interrupts.
//   - EFB -> XFB copies (BP 0x52): the backend shows the EFB. Without the backend, the
//     XFB in memory is filled with the clear color (GDI window fallback).
#include "runtime.h"
#include <windows.h>
uint64_t g_profDecode = 0;
uint32_t g_xfVersion = 0;
// Where the current draw comes from, to recognize the same draw in the next frame (frame
// interpolation): its address when it is in a display list, else a hash of its vertex
// format, arrays and first vertex bytes.
uint64_t g_gxDrawSource = 0;
static int g_dlDepth = 0;  // DSE_PROF: time decoding vertices
#include <cstdlib>
#include <unordered_map>
#include "gx.h"
#include "gx_state.h"
#include "gx_texture.h"
#include <cstring>
#include <vector>

namespace
{
std::vector<uint8_t> g_fifo;   // bytes received and not yet consumed
size_t g_fifoPos = 0;
uint32_t g_bpMask = 0xFFFFFF;  // BP 0xFE masks the next BP write
uint32_t g_pending = 0;        // PE interrupts to raise (bit n = interrupt n)
bool g_lost = false;
std::vector<GxVertex> g_vertices;

GxStats g_stats;

constexpr uint32_t kPeToken = 0xCC00100E;  // PE token register (16-bit)

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

// Without the backend: the XFB in memory gets the EFB clear color.
void FillXfbWithClearColor()
{
    uint32_t dim = g_gx.bp[0x4A];
    uint32_t width = (dim & 0x3FF) + 1, height = ((dim >> 10) & 0x3FF) + 1;
    uint32_t dest = ((g_gx.bp[0x4B] & 0xFFFFFF) << 5) | 0x80000000;
    uint32_t stride = (g_gx.bp[0x4D] & 0x3FF) << 5;
    uint32_t yscale = g_gx.bp[0x4E] & 0x1FF;
    uint32_t lines = yscale ? height * 256 / yscale : height;
    int r = g_gx.bp[0x4F] & 0xFF, g = (g_gx.bp[0x50] >> 8) & 0xFF, b = g_gx.bp[0x50] & 0xFF;
    auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    uint8_t y = clamp((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
    uint8_t u = clamp((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
    uint8_t v = clamp((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
    if (stride == 0 || width > 1024 || lines > 1024)
        return;
    for (uint32_t row = 0; row < lines; row++)
    {
        uint8_t* p = g_base + dest + row * stride;
        for (uint32_t x = 0; x + 1 < width && x * 2 + 3 < stride; x += 2)
        {
            p[x * 2 + 0] = y;
            p[x * 2 + 1] = u;
            p[x * 2 + 2] = y;
            p[x * 2 + 3] = v;
        }
    }
}

void WriteBp(uint32_t word)
{
    uint32_t reg = word >> 24, value = word & 0xFFFFFF;
    if (reg == 0xFE)
    {
        g_bpMask = value;
        return;
    }
    value = (g_gx.bp[reg] & ~g_bpMask) | (value & g_bpMask);
    g_bpMask = 0xFFFFFF;
    g_gx.bp[reg] = value;

    switch (reg)
    {
    case 0x45:  // PE_DONE: GXSetDrawDone -> "finish" interrupt
        g_pending |= 1u << kGxInterruptFinish;
        break;
    case 0x47:  // PE_TOKEN_INT: token register + token interrupt
        Store16(kPeToken, uint16_t(value));
        g_pending |= 1u << kGxInterruptToken;
        break;
    case 0x48:  // PE_TOKEN: token register only
        Store16(kPeToken, uint16_t(value));
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        // TEV color register or konst color (bit 23).
        ((value >> 23) & 1 ? g_gx.tevKonst : g_gx.tevReg)[reg - 0xE0] = value;
        break;
    case 0x65:  // LOADTLUT1: copy a palette from memory (address in BP 0x64) into TMEM
        GxLoadTlut((g_gx.bp[0x64] & 0xFFFFFF) << 5, (value & 0x3FF) << 9, (value & 0x1FFC00) >> 5);
        break;
    case 0x52:  // PE_COPY_EXECUTE: bit 14 = to the XFB (else to a texture), bit 11 = clear EFB
    {
        bool clear = (value >> 11) & 1;
        if ((value >> 14) & 1)
        {
            g_stats.xfbCopies++;
            if (GxBackendActive())
                GxBackendCopyToXfb(clear);
            else
                FillXfbWithClearColor();
        }
        else
        {
            g_stats.textureCopies++;
            if (GxBackendActive())
                GxBackendCopyToTexture(value);
            if (clear && GxBackendActive())
                GxBackendClear();
        }
        break;
    }
    default:
        break;
    }
}

// DSE_MTXLOG=1: logs each new matrix index value (CP 0x30/0x40, XF 0x1018/0x1019).
void LogMatrixIndex(const char* where, uint32_t value)
{
    static bool on = getenv("DSE_MTXLOG") != nullptr;
    static std::unordered_map<uint64_t, bool> seen;
    if (!on || seen.size() > 200)
        return;
    uint64_t key = uint64_t(value) << 8 | uint8_t(where[0]) << 1 | (where[7] == '0' || where[8] == '8');
    if (!seen[key])
    {
        seen[key] = true;
        LOG("[mtxidx] %s = 0x%08X", where, value);
    }
}

void LoadXf(uint32_t addr, const uint8_t* words, uint32_t count, bool bigEndian)
{
    for (uint32_t i = 0; i < count; i++, addr++)
    {
        uint32_t v = bigEndian ? Be32(words + 4 * i) : 0;
        if (addr < 0x800)
        {
            g_gx.xfMem[addr] = v;
            g_xfVersion++;
        }
        else if (addr >= 0x1000 && addr < 0x1100)
        {
            g_gx.xfReg[addr - 0x1000] = v;
            if (addr == 0x1018 || addr == 0x1019)
                LogMatrixIndex(addr == 0x1018 ? "XF 0x1018" : "XF 0x1019", v);
        }
    }
}

// Indexed XF load (0x20 position, 0x28 normal, 0x30 texture, 0x38 light matrices):
// index << 16 | (size - 1) << 12 | address; data from array 12 + n.
void LoadXfIndexed(uint32_t op, uint32_t word)
{
    uint32_t array = 12 + ((op - 0x20) >> 3);
    uint32_t index = word >> 16, count = ((word >> 12) & 15) + 1, addr = word & 0xFFF;
    uint32_t base = g_gx.cp[0xA0 + array] & 0x3FFFFFFF, stride = g_gx.cp[0xB0 + array] & 0xFF;
    LoadXf(addr, g_base + ((base + index * stride) | 0x80000000), count, true);
}

size_t RunCommand(const uint8_t* p, size_t avail);

void RunDisplayList(uint32_t addr, uint32_t size)
{
    g_stats.displayLists++;
    g_dlDepth++;
    const uint8_t* p = g_base + (addr | 0x80000000);
    size_t pos = 0;
    while (pos < size)
    {
        size_t n = RunCommand(p + pos, size - pos);
        if (n == 0 || n == SIZE_MAX)
            break;  // truncated list (padding at the end)
        pos += n;
    }
    g_dlDepth--;
}

size_t RunCommand(const uint8_t* p, size_t avail)
{
    uint8_t op = p[0];
    switch (op)
    {
    case 0x00:  // NOP
    case 0x48:  // invalidate vertex cache
        return 1;
    case 0x08:  // load CP register: reg, value
        if (avail < 6)
            return 0;
        g_gx.cp[p[1]] = Be32(p + 2);
        if (p[1] == 0x30 || p[1] == 0x40)
            LogMatrixIndex(p[1] == 0x30 ? "CP 0x30" : "CP 0x40", Be32(p + 2));
        return 6;
    case 0x10:  // load XF: (count - 1) << 16 | address, then count words
    {
        if (avail < 5)
            return 0;
        uint32_t header = Be32(p + 1);
        uint32_t count = ((header >> 16) & 15) + 1;
        size_t n = 5 + 4 * size_t(count);
        if (avail < n)
            return 0;
        LoadXf(header & 0xFFFF, p + 5, count, true);
        g_stats.xfLoads++;
        return n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:  // indexed XF loads
        if (avail < 5)
            return 0;
        LoadXfIndexed(op, Be32(p + 1));
        return 5;
    case 0x40:  // call display list: address, size
        if (avail < 9)
            return 0;
        RunDisplayList(Be32(p + 1), Be32(p + 5));
        return 9;
    case 0x61:  // load BP register
        if (avail < 5)
            return 0;
        WriteBp(Be32(p + 1));
        g_stats.bpWrites++;
        return 5;
    default:
        break;
    }
    if (op >= 0x80 && op < 0xC0)  // draw: primitive in bits 3-6, vertex format in bits 0-2
    {
        if (avail < 3)
            return 0;
        uint32_t count = Be16(p + 1);
        uint32_t vat = op & 7;
        size_t n = 3 + size_t(count) * GxVertexSize(vat);
        if (avail < n)
            return 0;
        g_stats.draws++;
        g_stats.vertices += count;
        if (count && GxBackendActive())
        {
            if (g_dlDepth > 0)
                g_gxDrawSource = (1ull << 40) | uint64_t(p - g_base);
            else
            {
                uint64_t h = 1469598103934665603ull ^ (uint64_t(count) << 8 | vat);
                h = (h ^ g_gx.cp[0xA0]) * 1099511628211ull;
                h = (h ^ g_gx.cp[0xA2]) * 1099511628211ull;
                for (size_t i = 3; i < n && i < 3 + 16; i++)
                    h = (h ^ p[i]) * 1099511628211ull;
                g_gxDrawSource = h;
            }
            g_vertices.resize(count);
            static const bool prof = getenv("DSE_PROF") != nullptr;
            LARGE_INTEGER t0{}, t1{};
            if (prof)
                QueryPerformanceCounter(&t0);
            GxDecodeVertices(vat, p + 3, count, g_vertices.data());
            if (prof)
            {
                QueryPerformanceCounter(&t1);
                g_profDecode += uint64_t(t1.QuadPart - t0.QuadPart);
            }
            GxBackendDraw(op & 0xF8, g_vertices.data(), count);
        }
        return n;
    }
    g_stats.unknown++;
    if (g_stats.unknown <= 8)
        LOG("[gx] unknown command 0x%02X: dropping the rest of the stream", op);
    return SIZE_MAX;
}

void Process()
{
    while (g_fifoPos < g_fifo.size())
    {
        size_t n = RunCommand(g_fifo.data() + g_fifoPos, g_fifo.size() - g_fifoPos);
        if (n == 0)
            break;
        if (n == SIZE_MAX)
        {
            g_fifoPos = g_fifo.size();
            g_lost = true;
            break;
        }
        g_fifoPos += n;
    }
    if (g_fifoPos == g_fifo.size())
    {
        g_fifo.clear();
        g_fifoPos = 0;
    }
    else if (g_fifoPos > 1 << 20)
    {
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + g_fifoPos);
        g_fifoPos = 0;
    }
}

void Append(const uint8_t* bytes, size_t n)
{
    g_stats.bytes += n;
    g_fifo.insert(g_fifo.end(), bytes, bytes + n);
    Process();
}
} // namespace

void GxPipeWrite8(uint8_t v) { Append(&v, 1); }
void GxPipeWrite16(uint16_t v) { uint8_t b[2] = { uint8_t(v >> 8), uint8_t(v) }; Append(b, 2); }
void GxPipeWrite32(uint32_t v)
{
    uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
    Append(b, 4);
}

uint32_t GxTakePendingInterrupts()
{
    uint32_t p = g_pending;
    g_pending = 0;
    return p;
}

const GxStats& GxGetStats() { return g_stats; }

bool GxStreamLost()
{
    bool lost = g_lost;
    g_lost = false;
    return lost;
}
