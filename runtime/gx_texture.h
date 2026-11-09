// GX texture decoding (gx_texture.cpp).
#pragma once
#include <cstdint>

enum GxTextureFormat : uint32_t
{
    kTexI4 = 0, kTexI8 = 1, kTexIA4 = 2, kTexIA8 = 3, kTexRGB565 = 4, kTexRGB5A3 = 5, kTexRGBA8 = 6,
    kTexC4 = 8, kTexC8 = 9, kTexC14X2 = 10, kTexCMPR = 14,
};

// Bytes a width x height texture of this format takes in memory (padded to whole tiles).
uint32_t GxTextureSize(uint32_t width, uint32_t height, uint32_t fmt);
// Decodes to RGBA8 (R in the low byte), width * height texels.
void GxDecodeTexture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, uint32_t tlutAddr,
    uint32_t tlutFmt, uint32_t* out);
// BP 0x65 "load TLUT": copies `bytes` from guest memory into TMEM.
void GxLoadTlut(uint32_t source, uint32_t tmemAddr, uint32_t bytes);
