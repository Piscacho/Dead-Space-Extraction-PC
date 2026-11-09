// GX texture decoding to RGBA8 (CPU).
//
// Wii textures are stored in tiles: 8x8 (I4, C4, CMPR), 8x4 (I8, IA4, C8) or 4x4 pixels
// (IA8, RGB565, RGB5A3, RGBA8, C14X2), row-major tiles of row-major texels. RGBA8 tiles
// are 64 bytes: 16 AR pairs, then 16 GB pairs. CMPR is DXT1 in 8x8 tiles of four 4x4
// sub-blocks, big-endian. Palette formats read their TLUT from TMEM, which the game
// fills with BP 0x64/0x65 (load TLUT).
#include "runtime.h"
#include "gx_state.h"
#include "gx_texture.h"
#include <cstring>

namespace
{
uint8_t g_tmem[0x100000];  // texture memory; TLUTs live in its upper half

uint32_t Be16(const uint8_t* p) { return uint32_t(p[0] << 8 | p[1]); }

uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
uint32_t Expand5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t Expand6(uint32_t v) { return (v << 2) | (v >> 4); }
uint32_t Expand4(uint32_t v) { return (v << 4) | v; }
uint32_t Expand3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

uint32_t Rgb565(uint32_t v) { return Rgba(Expand5(v >> 11), Expand6((v >> 5) & 63), Expand5(v & 31), 255); }
uint32_t Rgb5a3(uint32_t v)
{
    if (v & 0x8000)
        return Rgba(Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), 255);
    return Rgba(Expand4((v >> 8) & 15), Expand4((v >> 4) & 15), Expand4(v & 15), Expand3((v >> 12) & 7));
}
uint32_t Ia8(uint32_t v) { uint32_t i = v & 0xFF; return Rgba(i, i, i, v >> 8); }

uint32_t Palette(uint32_t tlutAddr, uint32_t tlutFmt, uint32_t index)
{
    uint32_t v = Be16(&g_tmem[(tlutAddr + index * 2) & (sizeof(g_tmem) - 2)]);
    switch (tlutFmt)
    {
    case 0: return Ia8(v);
    case 1: return Rgb565(v);
    default: return Rgb5a3(v);
    }
}

void BlockDims(uint32_t fmt, uint32_t& bw, uint32_t& bh)
{
    switch (fmt)
    {
    case kTexI4: case kTexC4: case kTexCMPR: bw = 8; bh = 8; break;
    case kTexI8: case kTexIA4: case kTexC8: bw = 8; bh = 4; break;
    default: bw = 4; bh = 4; break;
    }
}

uint32_t BitsPerTexel(uint32_t fmt)
{
    switch (fmt)
    {
    case kTexI4: case kTexC4: case kTexCMPR: return 4;
    case kTexI8: case kTexIA4: case kTexC8: return 8;
    case kTexRGBA8: return 32;
    default: return 16;
    }
}
} // namespace

uint32_t GxTextureSize(uint32_t width, uint32_t height, uint32_t fmt)
{
    uint32_t bw, bh;
    BlockDims(fmt, bw, bh);
    uint32_t w = (width + bw - 1) / bw * bw, h = (height + bh - 1) / bh * bh;
    return w * h * BitsPerTexel(fmt) / 8;
}

void GxLoadTlut(uint32_t source, uint32_t tmemAddr, uint32_t bytes)
{
    const uint8_t* src = g_base + (source | 0x80000000);
    for (uint32_t i = 0; i < bytes; i++)
        g_tmem[(tmemAddr + i) & (sizeof(g_tmem) - 1)] = src[i];
}

void GxDecodeTexture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, uint32_t tlutAddr,
    uint32_t tlutFmt, uint32_t* out)
{
    uint32_t bw, bh;
    BlockDims(fmt, bw, bh);
    uint32_t blocksX = (width + bw - 1) / bw, blocksY = (height + bh - 1) / bh;
    auto put = [&](uint32_t x, uint32_t y, uint32_t c) {
        if (x < width && y < height)
            out[y * width + x] = c;
    };

    for (uint32_t by = 0; by < blocksY; by++)
    {
        for (uint32_t bx = 0; bx < blocksX; bx++)
        {
            uint32_t x0 = bx * bw, y0 = by * bh;
            switch (fmt)
            {
            case kTexI4:
                for (uint32_t i = 0; i < 64; i++)
                {
                    uint32_t v = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                    uint32_t c = Expand4(v);
                    put(x0 + i % 8, y0 + i / 8, Rgba(c, c, c, c));
                }
                src += 32;
                break;
            case kTexI8:
                for (uint32_t i = 0; i < 32; i++)
                    put(x0 + i % 8, y0 + i / 8, Rgba(src[i], src[i], src[i], src[i]));
                src += 32;
                break;
            case kTexIA4:
                for (uint32_t i = 0; i < 32; i++)
                {
                    uint32_t c = Expand4(src[i] & 15), a = Expand4(src[i] >> 4);
                    put(x0 + i % 8, y0 + i / 8, Rgba(c, c, c, a));
                }
                src += 32;
                break;
            case kTexIA8:
                for (uint32_t i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, Ia8(Be16(src + i * 2)));
                src += 32;
                break;
            case kTexRGB565:
                for (uint32_t i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, Rgb565(Be16(src + i * 2)));
                src += 32;
                break;
            case kTexRGB5A3:
                for (uint32_t i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, Rgb5a3(Be16(src + i * 2)));
                src += 32;
                break;
            case kTexRGBA8:
                for (uint32_t i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, Rgba(src[i * 2 + 1], src[32 + i * 2], src[32 + i * 2 + 1], src[i * 2]));
                src += 64;
                break;
            case kTexC4:
                for (uint32_t i = 0; i < 64; i++)
                    put(x0 + i % 8, y0 + i / 8, Palette(tlutAddr, tlutFmt, (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15));
                src += 32;
                break;
            case kTexC8:
                for (uint32_t i = 0; i < 32; i++)
                    put(x0 + i % 8, y0 + i / 8, Palette(tlutAddr, tlutFmt, src[i]));
                src += 32;
                break;
            case kTexC14X2:
                for (uint32_t i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, Palette(tlutAddr, tlutFmt, Be16(src + i * 2) & 0x3FFF));
                src += 32;
                break;
            case kTexCMPR:
                // Four DXT1 sub-blocks: top-left, top-right, bottom-left, bottom-right.
                for (uint32_t sb = 0; sb < 4; sb++)
                {
                    uint32_t c0 = Be16(src), c1 = Be16(src + 2);
                    uint32_t pal[4] = { Rgb565(c0), Rgb565(c1), 0, 0 };
                    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
                    if (c0 > c1)
                    {
                        for (int k = 0; k < 2; k++)
                        {
                            uint32_t w0 = k ? 1 : 2, w1 = k ? 2 : 1;
                            pal[2 + k] = Rgba((ch(pal[0], 0) * w0 + ch(pal[1], 0) * w1) / 3,
                                (ch(pal[0], 8) * w0 + ch(pal[1], 8) * w1) / 3,
                                (ch(pal[0], 16) * w0 + ch(pal[1], 16) * w1) / 3, 255);
                        }
                    }
                    else
                    {
                        pal[2] = Rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                            (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
                        pal[3] = 0;  // transparent black
                    }
                    uint32_t sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    for (uint32_t row = 0; row < 4; row++)
                    {
                        uint8_t bits = src[4 + row];
                        for (uint32_t col = 0; col < 4; col++)
                            put(sx + col, sy + row, pal[(bits >> (6 - 2 * col)) & 3]);
                    }
                    src += 8;
                }
                break;
            default:
                for (uint32_t i = 0; i < bw * bh; i++)
                    put(x0 + i % bw, y0 + i / bw, Rgba(255, 0, 255, 255));  // unknown format: magenta
                src += 32;
                break;
            }
        }
    }
}
