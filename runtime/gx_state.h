// GX register state shared by the command processor (gx.cpp), the vertex decoder
// (gx_vertex.cpp) and the D3D12 backend (gx_d3d12.cpp).
#pragma once
#include <cstdint>

struct GxState
{
    uint32_t cp[256];       // CP: vertex descriptors (0x50/0x60), formats (0x70-0x9F), arrays (0xA0-0xBF)
    uint32_t bp[256];       // BP: rasterizer, TEV, textures, pixel engine
    uint32_t xfMem[0x800];  // XF memory: matrices 0x000-0x0FF (position/texture), 0x400 normal, 0x500 post
    uint32_t xfReg[0x100];  // XF registers 0x1000-0x10FF (index = address - 0x1000)
    // BP 0xE0-0xE7 write either a TEV color register or a konst color (bit 23), as
    // (red, alpha) and (blue, green) pairs for PREV, REG0, REG1, REG2 / KONST0-3.
    uint32_t tevReg[8];
    uint32_t tevKonst[8];
};
extern GxState g_gx;

inline float GxXfFloat(uint32_t addr)
{
    uint32_t bits = addr >= 0x1000 ? g_gx.xfReg[addr - 0x1000] : g_gx.xfMem[addr & 0x7FF];
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

// One decoded vertex with its raw attributes; the vertex shader transforms it.
struct GxVertex
{
    float pos[4];      // object space (w = 1)
    float nrm[4];      // object space
    float color[4];    // vertex color 0 (1 when absent)
    float tex[8][2];   // texture coordinate inputs
    uint32_t idx[4];   // position matrix row; texture matrix rows 0-3 and 4-7, one byte each
};

// Bumped on every XF memory write (matrices, lights): the backend re-uploads it then.
extern uint32_t g_xfVersion;

// Primitive types (bits 3-6 of the draw command).
enum GxPrimitive : uint32_t
{
    kGxQuads = 0x80, kGxQuads2 = 0x88, kGxTriangles = 0x90, kGxTriangleStrip = 0x98,
    kGxTriangleFan = 0xA0, kGxLines = 0xA8, kGxLineStrip = 0xB0, kGxPoints = 0xB8,
};

// Size in bytes of one vertex for vertex format `vat` (from the current VCD/VAT).
uint32_t GxVertexSize(uint32_t vat);
// Decodes `count` vertices of format `vat` from the command stream.
void GxDecodeVertices(uint32_t vat, const uint8_t* data, uint32_t count, GxVertex* out);

// Backend (gx_d3d12.cpp).
bool GxBackendInit(void* hwnd);
bool GxBackendActive();
void GxBackendDraw(uint32_t primitive, const GxVertex* vertices, uint32_t count);
// EFB -> XFB copy: show the EFB and optionally clear it.
void GxBackendCopyToXfb(bool clear);
// EFB clear without a copy to the screen (EFB -> texture copies with the clear bit).
void GxBackendClear();
// EFB -> texture copy (BP 0x52 word); the result is used for textures at that address.
void GxBackendCopyToTexture(uint32_t value);
