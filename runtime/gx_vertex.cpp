// GX vertex decoding and transform, on the CPU.
//
// Vertex layout (in this order, each attribute direct, 8-bit or 16-bit indexed, or absent
// as the VCD says): PNMTXIDX, TEX0-7MTXIDX, position, normal, color 0, color 1, tex 0-7.
// Indexed attributes come from arrays in memory (CP 0xA0+n base, 0xB0+n stride; arrays
// 0 position, 1 normal, 2-3 colors, 4-11 texture coordinates).
//
// Only the raw attributes are decoded here (position, normal, color 0, texture
// coordinates and matrix rows). Transform, projection, viewport, lighting and texture
// coordinate generation run in the vertex shader (gx_d3d12.cpp) with the XF memory.
#include "runtime.h"
#include "gx_state.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

GxState g_gx;

namespace
{
constexpr float kEfbWidth = 640.0f, kEfbHeight = 528.0f;

uint32_t ComponentSize(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
uint32_t ColorSize(uint32_t fmt)
{
    static const uint32_t sizes[8] = { 2, 3, 4, 2, 3, 4, 4, 4 };
    return sizes[fmt & 7];
}
uint32_t IndexSize(uint32_t type) { return type == 2 ? 1 : 2; }

struct Format
{
    uint32_t pos, nrm, col[2], tex[8];       // VCD type per attribute
    uint32_t posFmt, posCnt, posShift;
    uint32_t nrmFmt;
    bool nbt, nrmIndex3;
    uint32_t colFmt[2];
    uint32_t texFmt[8], texCnt[8], texShift[8];
    uint32_t mtxIdx;                          // bit 0 PNMTXIDX, bits 1-8 TEXnMTXIDX
};

Format GetFormat(uint32_t vat)
{
    uint32_t lo = g_gx.cp[0x50], hi = g_gx.cp[0x60];
    uint32_t a = g_gx.cp[0x70 + vat], b = g_gx.cp[0x80 + vat], c = g_gx.cp[0x90 + vat];
    Format f{};
    f.mtxIdx = lo & 0x1FF;
    f.pos = (lo >> 9) & 3;
    f.nrm = (lo >> 11) & 3;
    f.col[0] = (lo >> 13) & 3;
    f.col[1] = (lo >> 15) & 3;
    for (int i = 0; i < 8; i++)
        f.tex[i] = (hi >> (2 * i)) & 3;
    f.posCnt = (a & 1) ? 3 : 2;
    f.posFmt = (a >> 1) & 7;
    f.posShift = (a >> 4) & 31;
    f.nbt = (a >> 9) & 1;
    f.nrmFmt = (a >> 10) & 7;
    f.colFmt[0] = (a >> 14) & 7;
    f.colFmt[1] = (a >> 18) & 7;
    f.nrmIndex3 = (a >> 31) & 1;
    const uint32_t cnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
    const uint32_t fmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
    const uint32_t sh[8] = { (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31 };
    for (int i = 0; i < 8; i++)
    {
        f.texCnt[i] = cnt[i] ? 2 : 1;
        f.texFmt[i] = fmt[i];
        f.texShift[i] = sh[i];
    }
    return f;
}

uint32_t Be16(const uint8_t* p) { return uint32_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// One component of a position/normal/texcoord in format fmt, scaled by 2^-shift.
float ReadComponent(const uint8_t*& p, uint32_t fmt, uint32_t shift)
{
    float scale = 1.0f / float(1u << shift);
    switch (fmt)
    {
    case 0: return float(*p++) * scale;
    case 1: return float(int8_t(*p++)) * scale;
    case 2: { float v = float(Be16(p)) * scale; p += 2; return v; }
    case 3: { float v = float(int16_t(Be16(p))) * scale; p += 2; return v; }
    default:
    {
        uint32_t bits = Be32(p);
        p += 4;
        float v;
        memcpy(&v, &bits, 4);
        return v;
    }
    }
}

void ReadColor(const uint8_t*& p, uint32_t fmt, float out[4])
{
    auto n8 = [](uint32_t v) { return float(v) / 255.0f; };
    switch (fmt)
    {
    case 0:  // RGB565
    {
        uint32_t v = Be16(p);
        p += 2;
        out[0] = float((v >> 11) & 31) / 31.0f;
        out[1] = float((v >> 5) & 63) / 63.0f;
        out[2] = float(v & 31) / 31.0f;
        out[3] = 1.0f;
        break;
    }
    case 1:  // RGB888
        out[0] = n8(p[0]); out[1] = n8(p[1]); out[2] = n8(p[2]); out[3] = 1.0f;
        p += 3;
        break;
    case 2:  // RGB888x
        out[0] = n8(p[0]); out[1] = n8(p[1]); out[2] = n8(p[2]); out[3] = 1.0f;
        p += 4;
        break;
    case 3:  // RGBA4444
    {
        uint32_t v = Be16(p);
        p += 2;
        out[0] = float((v >> 12) & 15) / 15.0f;
        out[1] = float((v >> 8) & 15) / 15.0f;
        out[2] = float((v >> 4) & 15) / 15.0f;
        out[3] = float(v & 15) / 15.0f;
        break;
    }
    case 4:  // RGBA6666
    {
        uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
        p += 3;
        out[0] = float((v >> 18) & 63) / 63.0f;
        out[1] = float((v >> 12) & 63) / 63.0f;
        out[2] = float((v >> 6) & 63) / 63.0f;
        out[3] = float(v & 63) / 63.0f;
        break;
    }
    default:  // RGBA8888
        out[0] = n8(p[0]); out[1] = n8(p[1]); out[2] = n8(p[2]); out[3] = n8(p[3]);
        p += 4;
        break;
    }
}

// Pointer to an indexed attribute: array `array`, element `index`.
const uint8_t* ArrayElement(uint32_t array, uint32_t index)
{
    uint32_t base = g_gx.cp[0xA0 + array] & 0x3FFFFFFF;
    uint32_t stride = g_gx.cp[0xB0 + array] & 0xFF;
    return g_base + ((base + index * stride) | 0x80000000);
}

uint32_t ReadIndex(const uint8_t*& p, uint32_t type)
{
    if (type == 2)
        return *p++;
    uint32_t v = Be16(p);
    p += 2;
    return v;
}

void Unpack(uint32_t color, float out[4])
{
    constexpr float k = 1.0f / 255.0f;
    out[0] = float((color >> 24) & 0xFF) * k;
    out[1] = float((color >> 16) & 0xFF) * k;
    out[2] = float((color >> 8) & 0xFF) * k;
    out[3] = float(color & 0xFF) * k;
}

} // namespace

uint32_t GxVertexSize(uint32_t vat)
{
    Format f = GetFormat(vat);
    uint32_t size = 0;
    for (int i = 0; i < 9; i++)
        size += (f.mtxIdx >> i) & 1;
    size += f.pos == 1 ? ComponentSize(f.posFmt) * f.posCnt : f.pos ? IndexSize(f.pos) : 0;
    if (f.nrm == 1)
        size += ComponentSize(f.nrmFmt) * (f.nbt ? 9 : 3);
    else if (f.nrm)
        size += IndexSize(f.nrm) * (f.nbt && f.nrmIndex3 ? 3 : 1);
    for (int i = 0; i < 2; i++)
        size += f.col[i] == 1 ? ColorSize(f.colFmt[i]) : f.col[i] ? IndexSize(f.col[i]) : 0;
    for (int i = 0; i < 8; i++)
        size += f.tex[i] == 1 ? ComponentSize(f.texFmt[i]) * f.texCnt[i] : f.tex[i] ? IndexSize(f.tex[i]) : 0;
    return size;
}

void GxDecodeVertices(uint32_t vat, const uint8_t* data, uint32_t count, GxVertex* out)
{
    Format f = GetFormat(vat);
    const uint8_t* p = data;

    // Matrix rows: CP 0x30/0x40 give the defaults, the vertex can carry its own.
    uint32_t matA = g_gx.cp[0x30], matB = g_gx.cp[0x40];
    uint32_t defaultTexRow[8] = { (matA >> 6) & 63, (matA >> 12) & 63, (matA >> 18) & 63, (matA >> 24) & 63,
        matB & 63, (matB >> 6) & 63, (matB >> 12) & 63, (matB >> 18) & 63 };

    for (uint32_t v = 0; v < count; v++)
    {
        GxVertex& o = out[v];
        uint32_t posRow = matA & 63;
        uint32_t texRow[8];
        memcpy(texRow, defaultTexRow, sizeof(texRow));
        if (f.mtxIdx & 1)
            posRow = *p++;
        for (int i = 0; i < 8; i++)
            if ((f.mtxIdx >> (i + 1)) & 1)
                texRow[i] = *p++;
        o.idx[0] = posRow;
        o.idx[1] = texRow[0] | texRow[1] << 8 | texRow[2] << 16 | texRow[3] << 24;
        o.idx[2] = texRow[4] | texRow[5] << 8 | texRow[6] << 16 | texRow[7] << 24;
        o.idx[3] = 0;

        o.pos[0] = o.pos[1] = o.pos[2] = 0.0f;
        o.pos[3] = 1.0f;
        if (f.pos)
        {
            const uint8_t* q = f.pos == 1 ? p : ArrayElement(0, ReadIndex(p, f.pos));
            for (uint32_t c = 0; c < f.posCnt; c++)
                o.pos[c] = ReadComponent(q, f.posFmt, f.posShift);
            if (f.pos == 1)
                p = q;
        }

        // Normal (only the first one of NBT): s8 has 6 fraction bits, s16 has 14.
        o.nrm[0] = o.nrm[1] = 0.0f;
        o.nrm[2] = 1.0f;
        o.nrm[3] = 0.0f;
        if (f.nrm)
        {
            uint32_t shift = f.nrmFmt == 1 ? 6 : f.nrmFmt == 3 ? 14 : 0;
            const uint8_t* q = f.nrm == 1 ? p : ArrayElement(1, ReadIndex(p, f.nrm));
            for (int c = 0; c < 3; c++)
                o.nrm[c] = ReadComponent(q, f.nrmFmt, shift);
            if (f.nrm == 1)
                p += ComponentSize(f.nrmFmt) * (f.nbt ? 9 : 3);
            else if (f.nbt && f.nrmIndex3)
                p += IndexSize(f.nrm) * 2;
        }

        // Colors: color 0 goes to the GPU, color 1 is read past (not used yet).
        float color[2][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
        for (int i = 0; i < 2; i++)
        {
            if (!f.col[i])
                continue;
            const uint8_t* q = f.col[i] == 1 ? p : ArrayElement(2 + i, ReadIndex(p, f.col[i]));
            ReadColor(q, f.colFmt[i], color[i]);
            if (f.col[i] == 1)
                p = q;
        }
        memcpy(o.color, color[0], sizeof(o.color));

        for (int i = 0; i < 8; i++)
        {
            o.tex[i][0] = o.tex[i][1] = 0.0f;
            if (!f.tex[i])
                continue;
            const uint8_t* q = f.tex[i] == 1 ? p : ArrayElement(4 + i, ReadIndex(p, f.tex[i]));
            for (uint32_t c = 0; c < f.texCnt[i]; c++)
                o.tex[i][c] = ReadComponent(q, f.texFmt[i], f.texShift[i]);
            if (f.tex[i] == 1)
                p = q;
        }
    }
}
