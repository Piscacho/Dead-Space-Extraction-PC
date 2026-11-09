// D3D12 backend for GX.
//
//   - The EFB is a 640x528 RGBA8 render target with a 24-bit depth buffer. Every EFB ->
//     XFB copy shows the copied rectangle in the window (scaled) and presents; the XFB in
//     guest memory is not written.
//   - Vertices arrive already transformed (gx_vertex.cpp) and are expanded to triangle,
//     line or point lists here.
//   - One pixel shader runs the TEV for every draw (an "ubershader"): the stage setup
//     (BP 0x28-0x2F orders, 0xC0-0xDF combiners, 0xF6-0xFD konst selection) and the
//     registers go in a constant buffer, so no shader is generated per combination.
//   - Blend, depth and topology select a pipeline state (cached).
//   - Textures are decoded on the CPU (gx_texture.cpp) and cached by address, size,
//     format and palette, with a content hash checked once per frame.
//
// Not handled yet: indirect texturing (unused by this game), logic ops and destination
// alpha. Fog (only the depth-of-field mask uses it) is optional: profundidad_campo in dse.ini.
#include "runtime.h"
#include "config.h"
extern uint64_t g_gxDrawSource;  // gx.cpp: where the current draw comes from
extern uint64_t g_profDecode;  // gx.cpp
#include "gx.h"
#include "gx_state.h"
#include "gx_texture.h"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>
#include <set>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kEfbWidth = 640, kEfbHeight = 528;   // the GX framebuffer, in GX pixels
// Internal resolution: the EFB is drawn at kEfbWidth x kEfbHeight times g_scale
// (DSE_SCALE=1..6, default 3). Everything the game sees stays in GX pixels.
UINT g_scale = 3, g_efbW = kEfbWidth * 3, g_efbH = kEfbHeight * 3;
// Horizontal pixels per GX pixel: g_scale, times aspect / (4:3) when the image is wider (the
// 640 GX pixels are shown that much wider, so the EFB gets that many more real pixels).
float g_scaleX = 3.0f;
float g_wideK = 1.0f;  // VideoWideFactor()
constexpr UINT kBackBuffers = 2;
constexpr UINT64 kUploadSize = 96ull << 20;   // per frame: vertices, constants, texture data
constexpr UINT kHeapSize = 262144;           // shader-visible SRV descriptors per frame
constexpr UINT kTexSrvCapacity = 16384;      // CPU-side SRVs for cached textures

const char* kShaders = R"(
struct VIn { float4 pos : POSITION; float4 nrm : NORMAL; float4 col : COLOR0; float2 tc[8] : TEXCOORD0; uint4 idx : BLENDINDICES; };
// tc: s, t and q; the pixel shader divides by q (perspective-correct projected textures, as on GX).
struct V { float4 pos : SV_Position; float4 col : COLOR0; float3 tc[8] : TEXCOORD0; };

cbuffer Draw : register(b0)
{
    uint4 tevColor[4];   // BP 0xC0 + 2s
    uint4 tevAlpha[4];   // BP 0xC1 + 2s
    uint4 tevOrder[2];   // BP 0x28-0x2F
    uint4 ksel[2];       // BP 0xF6-0xFD
    float4 regs[4];      // PREV, REG0, REG1, REG2
    float4 konst[4];
    uint4 misc;          // x stages, y alpha compare (BP 0xF3)
    uint4 samplers[2];   // per texture map: bits 0-2 index in sTex, bit 3 mirror S, bit 4 mirror T, bit 5 mipmaps
    // Vertex stage (XF registers).
    float4 proj0;        // XF 0x1020-0x1023
    float4 proj1;        // XF 0x1024-0x1025, ortho, clip x factor (wide screens)
    float4 vp0;          // viewport: width, height, z, x (- 342)
    float4 vp1;          // viewport: y (- 342), far
    uint4 chan;          // color 0 control, alpha 0 control, texgen count, dual texture
    float4 ambColor;     // XF 0x100A
    float4 matColor;     // XF 0x100C
    uint4 texgen[2];     // XF 0x1040-0x1047
    uint4 postInfo[2];   // XF 0x1050-0x1057
    float4 fogParams;    // A, C, B magnitude, B shift (BP 0xEE-0xF1)
    float4 zTex;         // z texture: op (0 off, 1 add, 2 replace), format (0 U8, 1 U16, 2 U24), bias
};
Texture2D tex[8] : register(t0);

// XF memory (0x000-0x67F): position/texture matrices, normal matrices, post matrices, lights.
cbuffer XfMem : register(b1) { uint4 xfm[0x1A0]; };
float XF(uint a) { return asfloat(xfm[a >> 2][a & 3]); }
float4 XfColor(uint a)
{
    uint c = xfm[a >> 2][a & 3];
    return float4((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) / 255.0;
}
float3 XfVec(uint a) { return float3(XF(a), XF(a + 1), XF(a + 2)); }
// Row-major matrix row at XF row `row` applied to (v, 1).
float MulRow(uint row, float3 v) { uint m = row * 4; return XF(m) * v.x + XF(m + 1) * v.y + XF(m + 2) * v.z + XF(m + 3); }
uint GetU2(uint4 a[2], uint i) { return a[i >> 2][i & 3]; }

// Lighting (XF color channels): control word bits 0 material from vertex, 1 lighting on,
// 2-5 and 11-14 light mask, 6 ambient from vertex, 7-8 diffuse function, 9-10
// attenuation. Lights at XF 0x600 + 16 * i. Same math as Dolphin.
float LightAmount(uint control, uint l, float3 pos, float3 n)
{
    uint attnFunc = (control >> 9) & 3, diffuse = (control >> 7) & 3;
    float3 lpos = XfVec(l + 10), ldirv = XfVec(l + 13), cosAtt = XfVec(l + 4), distAtt = XfVec(l + 7);
    float3 ldir;
    float attn = 1.0;
    if (attnFunc == 1)  // specular
    {
        float len = length(lpos);
        ldir = len > 0 ? lpos / len : float3(0, 0, 0);
        float a = dot(n, ldir) >= 0 ? max(0.0, dot(n, ldirv)) : 0.0;
        float c = cosAtt.x + cosAtt.y * a + cosAtt.z * a * a;
        float d = distAtt.x + distAtt.y * a + distAtt.z * a * a;
        attn = d != 0 ? max(0.0, c / d) : 0.0;
    }
    else
    {
        ldir = lpos - pos;
        float dist2 = dot(ldir, ldir), dist = sqrt(dist2);
        ldir = dist > 0 ? ldir / dist : n;
        if (attnFunc == 3)  // spot
        {
            float a = max(0.0, dot(ldir, ldirv));
            float c = max(0.0, cosAtt.x + cosAtt.y * a + cosAtt.z * a * a);
            float d = distAtt.x + distAtt.y * dist + distAtt.z * dist2;
            attn = d != 0 ? c / d : 0.0;
        }
    }
    float dd = dot(ldir, n);
    if (diffuse == 1) return attn * dd;
    if (diffuse == 2) return attn * max(0.0, dd);
    return attn;
}

float4 LightChannel(uint control, float4 vertexColor, float3 pos, float3 n)
{
    float4 mat = (control & 1) ? vertexColor : matColor;
    if (!((control >> 1) & 1))
        return mat;
    float4 sum = ((control >> 6) & 1) ? vertexColor : ambColor;
    uint mask = ((control >> 2) & 15) | ((control >> 7) & 0xF0);
    for (uint i = 0; i < 8; i++)
        if ((mask >> i) & 1)
            sum += LightAmount(control, 0x600 + 16 * i, pos, n) * XfColor(0x600 + 16 * i + 3);
    return mat * saturate(sum);
}

V vs_main(VIn i)
{
    V o;
    uint posRow = i.idx.x;
    float3 p = i.pos.xyz;
    float3 view = float3(MulRow(posRow, p), MulRow(posRow + 1, p), MulRow(posRow + 2, p));

    // Projection, then the GX viewport folded into D3D clip space for a 640x528 EFB.
    float cx, cy, cz, cw;
    if (proj1.z != 0)
    {
        cx = proj0.x * view.x + proj0.y;
        cy = proj0.z * view.y + proj0.w;
        cz = proj1.x * view.z + proj1.y;
        cw = 1.0;
    }
    else
    {
        cx = proj0.x * view.x + proj0.y * view.z;
        cy = proj0.z * view.y + proj0.w * view.z;
        cz = proj1.x * view.z + proj1.y;
        cw = -view.z;
    }
    // 21:9 / 32:9: proj1.w > 0 scales x (wider field of view for the world, 1 for full-screen
    // passes). proj1.w < 0 is the interface: what is inside the 16:9 screen keeps its 16:9
    // size in the middle, vertices beyond its edges stay out there (tints and overlays that
    // reach past the screen keep covering the wider one).
    if (proj1.w >= 0)
        cx *= proj1.w;
    else if (abs(cx) <= abs(cw))
        cx *= -proj1.w;
    float vpW = vp0.x, vpH = vp0.y, vpZ = vp0.z, vpX = vp0.w, vpY = vp1.x, vpFar = vp1.y;
    o.pos.x = (2.0 * vpX / 640.0 - 1.0) * cw + (2.0 * vpW / 640.0) * cx;
    o.pos.y = (1.0 - 2.0 * vpY / 528.0) * cw - (2.0 * vpH / 528.0) * cy;
    o.pos.z = (vpFar / 16777215.0) * cw + (vpZ / 16777215.0) * cz;
    o.pos.w = cw;

    // Color 0 / alpha 0.
    uint colorCtrl = chan.x, alphaCtrl = chan.y;
    float3 vn = float3(0, 0, 1);
    if (((colorCtrl | alphaCtrl) >> 1) & 1)
    {
        uint nm = 0x400 + (posRow & 31) * 3;
        float3 n = i.nrm.xyz;
        vn = float3(dot(XfVec(nm), n), dot(XfVec(nm + 3), n), dot(XfVec(nm + 6), n));
        float len = length(vn);
        vn = len > 0 ? vn / len : vn;
    }
    float4 c = LightChannel(colorCtrl, i.col, view, vn);
    float4 a = LightChannel(alphaCtrl, i.col, view, vn);
    o.col = float4(c.rgb, a.a);

    // Texture coordinates: texgen t takes its source through its matrix (2x4 ST or 3x4
    // STQ); with dual texture it can be normalized and goes through a post matrix.
    // (A local array: outputs cannot be indexed by a loop variable.)
    uint numTexGens = chan.z;
    float3 tcs[8];
    float2 itc[8] = i.tc;
    for (uint t = 0; t < 8; t++)
    {
        if (t >= numTexGens)
        {
            tcs[t] = float3(itc[t], 1);
            continue;
        }
        uint info = GetU2(texgen, t);
        bool stq = (info >> 1) & 1;
        uint type = (info >> 4) & 7, source = (info >> 7) & 31;
        if (type == 2 || type == 3)  // color texgen
        {
            tcs[t] = float3(o.col.rg, 1);
            continue;
        }
        if (type == 1)  // emboss: the source texgen's coordinates
        {
            uint from = (info >> 12) & 7;
            tcs[t] = from < t ? tcs[from] : float3(0, 0, 1);
            continue;
        }
        float3 src = float3(0, 0, 1);
        if (source == 0) src = i.pos.xyz;
        else if (source == 1 || source == 3 || source == 4) src = i.nrm.xyz;
        else if (source == 2) src = i.col.rgb;
        else if (source >= 5 && source <= 12) src = float3(itc[source - 5], 1);
        uint row = ((t < 4 ? i.idx.y : i.idx.z) >> ((t & 3) * 8)) & 63;
        float3 r = float3(MulRow(row, src), MulRow(row + 1, src), stq ? MulRow(row + 2, src) : 1.0);
        if (chan.w & 1)
        {
            uint post = GetU2(postInfo, t);
            if ((post >> 8) & 1)
            {
                float len = length(r);
                r = len > 0 ? r / len : r;
            }
            uint pm = 0x500 + (post & 63) * 4;
            r = float3(dot(XfVec(pm), r) + XF(pm + 3), dot(XfVec(pm + 4), r) + XF(pm + 7), dot(XfVec(pm + 8), r) + XF(pm + 11));
            stq = true;
        }
        float q = stq && r.z != 0 ? r.z : 1.0;
        tcs[t] = float3(r.xy, q);
        if (info >> 31)  // widened camera projection (see FillConstants): u/q around 0.5
            tcs[t].x = 0.5 * q + (tcs[t].x - 0.5 * q) * vp1.z;
    }
    o.tc = tcs;
    return o;
}

SamplerState sWrapLinear : register(s0);
SamplerState sClampLinear : register(s1);
SamplerState sWrapPoint : register(s2);
SamplerState sClampPoint : register(s3);
// Texture samplers, index bits (S repeat, T repeat, point); mirror is done on the coordinate.
SamplerState sT0 : register(s4);
SamplerState sT1 : register(s5);
SamplerState sT2 : register(s6);
SamplerState sT3 : register(s7);
SamplerState sT4 : register(s8);
SamplerState sT5 : register(s9);
SamplerState sT6 : register(s10);
SamplerState sT7 : register(s11);

uint Get4(uint4 a[4], uint i) { return a[i >> 2][i & 3]; }
uint Get2(uint4 a[2], uint i) { return a[i >> 2][i & 3]; }

float4 SampleMap(uint m, float2 uv)
{
    uint mode = Get2(samplers, m);
    // Mirror: fold the coordinate into one period of 2, then sample with repeat.
    if (mode & 8) uv.x = 1 - abs(frac(uv.x * 0.5) * 2 - 1);
    if (mode & 16) uv.y = 1 - abs(frac(uv.y * 0.5) * 2 - 1);
    // Bit 5: the game asked for mipmaps; without it the hardware always reads level 0.
#define SM(S) ((mode & 32) ? tex[m].Sample(S, uv) : tex[m].SampleLevel(S, uv, 0))
    switch (mode & 7)
    {
    case 0: return SM(sT0);
    case 1: return SM(sT1);
    case 2: return SM(sT2);
    case 3: return SM(sT3);
    case 4: return SM(sT4);
    case 5: return SM(sT5);
    case 6: return SM(sT6);
    default: return SM(sT7);
    }
#undef SM
}

float Frac8(uint v) { return v == 0 ? 1.0 : (8 - v) / 8.0; }

float3 KonstColor(uint sel)
{
    if (sel < 8) return Frac8(sel).xxx;
    if (sel >= 12 && sel < 16) return konst[sel - 12].rgb;
    if (sel >= 16 && sel < 20) return konst[sel - 16].rrr;
    if (sel >= 20 && sel < 24) return konst[sel - 20].ggg;
    if (sel >= 24 && sel < 28) return konst[sel - 24].bbb;
    if (sel >= 28) return konst[sel - 28].aaa;
    return 0;
}

float KonstAlpha(uint sel)
{
    if (sel < 8) return Frac8(sel);
    if (sel >= 16 && sel < 20) return konst[sel - 16].r;
    if (sel >= 20 && sel < 24) return konst[sel - 20].g;
    if (sel >= 24 && sel < 28) return konst[sel - 24].b;
    if (sel >= 28) return konst[sel - 28].a;
    return 0;
}

static float4 r[4];  // PREV, REG0, REG1, REG2 while the TEV runs

float3 ColorIn(uint s, float4 t, float4 ras, float3 k)
{
    switch (s)
    {
    case 0: return r[0].rgb; case 1: return r[0].aaa;
    case 2: return r[1].rgb; case 3: return r[1].aaa;
    case 4: return r[2].rgb; case 5: return r[2].aaa;
    case 6: return r[3].rgb; case 7: return r[3].aaa;
    case 8: return t.rgb; case 9: return t.aaa;
    case 10: return ras.rgb; case 11: return ras.aaa;
    case 12: return 1.0; case 13: return 0.5;
    case 14: return k;
    default: return 0.0;
    }
}

float AlphaIn(uint s, float4 t, float4 ras, float k)
{
    switch (s)
    {
    case 0: return r[0].a; case 1: return r[1].a; case 2: return r[2].a; case 3: return r[3].a;
    case 4: return t.a; case 5: return ras.a; case 6: return k;
    default: return 0.0;
    }
}

// Swap table n: KSEL 2n bits 0-1/2-3 pick the sources of red/green, KSEL 2n+1 those of blue/alpha.
float4 Swap(float4 c, uint table)
{
    uint lo = Get2(ksel, table * 2), hi = Get2(ksel, table * 2 + 1);
    return float4(c[lo & 3], c[(lo >> 2) & 3], c[hi & 3], c[(hi >> 2) & 3]);
}

// Compare mode (bias 3): op = scale * 2 + sub; d + (a OP b ? c : 0).
uint U8(float v) { return uint(round(saturate(v) * 255.0)); }
float3 CompareColor(uint op, float3 a, float3 b, float3 c, float3 d)
{
    bool gt = (op & 1) == 0;
    uint va, vb;
    if (op < 2) { va = U8(a.r); vb = U8(b.r); }
    else if (op < 4) { va = U8(a.g) << 8 | U8(a.r); vb = U8(b.g) << 8 | U8(b.r); }
    else if (op < 6) { va = U8(a.b) << 16 | U8(a.g) << 8 | U8(a.r); vb = U8(b.b) << 16 | U8(b.g) << 8 | U8(b.r); }
    else
    {
        float3 res = d;
        for (uint k = 0; k < 3; k++)
            if (gt ? U8(a[k]) > U8(b[k]) : U8(a[k]) == U8(b[k]))
                res[k] += c[k];
        return res;
    }
    return d + ((gt ? va > vb : va == vb) ? c : 0);
}
float CompareAlpha(uint op, float4 a, float4 b, float c, float d)
{
    bool gt = (op & 1) == 0;
    uint va, vb;
    if (op < 2) { va = U8(a.r); vb = U8(b.r); }
    else if (op < 4) { va = U8(a.g) << 8 | U8(a.r); vb = U8(b.g) << 8 | U8(b.r); }
    else if (op < 6) { va = U8(a.b) << 16 | U8(a.g) << 8 | U8(a.r); vb = U8(b.b) << 16 | U8(b.g) << 8 | U8(b.r); }
    else { va = U8(a.a); vb = U8(b.a); }
    return d + ((gt ? va > vb : va == vb) ? c : 0);
}

static const float kScale[4] = { 1.0, 2.0, 4.0, 0.5 };
static const float kBias[4] = { 0.0, 0.5, -0.5, 0.0 };

bool AlphaTest(uint func, float a, uint ref)
{
    float rv = ref / 255.0;
    switch (func)
    {
    case 0: return false; case 1: return a < rv; case 2: return a == rv; case 3: return a <= rv;
    case 4: return a > rv; case 5: return a != rv; case 6: return a >= rv;
    default: return true;
    }
}

float4 PsBody(V i, inout float zc)
{
    r[0] = regs[0]; r[1] = regs[1]; r[2] = regs[2]; r[3] = regs[3];
    uint stages = misc.x;
    float4 lastTex = float4(1, 1, 1, 1);  // the last stage's texel (z textures use it)
    for (uint s = 0; s < stages; s++)
    {
        uint order = Get2(tevOrder, s >> 1) >> ((s & 1) * 12);
        uint map = order & 7, coord = (order >> 3) & 7, chan = (order >> 7) & 7;
        uint aw0 = Get4(tevAlpha, s);
        float3 tq = i.tc[coord];
        float2 uv = tq.xy / (abs(tq.z) > 1e-7 ? tq.z : 1e-7);
        float4 t = ((order >> 6) & 1) ? Swap(SampleMap(map, uv), (aw0 >> 2) & 3) : float4(0, 0, 0, 0);
        lastTex = ((order >> 6) & 1) ? t : float4(1, 1, 1, 1);
        float4 ras = chan == 7 ? float4(0, 0, 0, 0) : Swap(i.col, aw0 & 3);
        uint ks = Get2(ksel, s >> 1) >> ((s & 1) * 10);
        float3 kc = KonstColor((ks >> 4) & 31);
        float ka = KonstAlpha((ks >> 9) & 31);

        uint cw = Get4(tevColor, s), aw = Get4(tevAlpha, s);
        float3 ca = saturate(ColorIn((cw >> 12) & 15, t, ras, kc));
        float3 cb = saturate(ColorIn((cw >> 8) & 15, t, ras, kc));
        float3 cc = saturate(ColorIn((cw >> 4) & 15, t, ras, kc));
        float3 cd = ColorIn(cw & 15, t, ras, kc);
        float3 cres;
        if (((cw >> 16) & 3) == 3)
            cres = CompareColor(((cw >> 20) & 3) * 2 + ((cw >> 18) & 1), ca, cb, cc, cd);
        else
        {
            float3 cl = ca * (1.0 - cc) + cb * cc;
            cres = (((cw >> 18) & 1) ? cd - cl : cd + cl) + kBias[(cw >> 16) & 3];
            cres *= kScale[(cw >> 20) & 3];
        }
        cres = ((cw >> 19) & 1) ? saturate(cres) : clamp(cres, -4.0, 4.0);

        float aa = saturate(AlphaIn((aw >> 13) & 7, t, ras, ka));
        float ab = saturate(AlphaIn((aw >> 10) & 7, t, ras, ka));
        float ac = saturate(AlphaIn((aw >> 7) & 7, t, ras, ka));
        float ad = AlphaIn((aw >> 4) & 7, t, ras, ka);
        float ares;
        if (((aw >> 16) & 3) == 3)
        {
            // Alpha compares use the color inputs' channels too (a and b as RGBA).
            float4 a4 = float4(ca, aa), b4 = float4(cb, ab);
            ares = CompareAlpha(((aw >> 20) & 3) * 2 + ((aw >> 18) & 1), a4, b4, ac, ad);
        }
        else
        {
            float al = aa * (1.0 - ac) + ab * ac;
            ares = (((aw >> 18) & 1) ? ad - al : ad + al) + kBias[(aw >> 16) & 3];
            ares *= kScale[(aw >> 20) & 3];
        }
        ares = ((aw >> 19) & 1) ? saturate(ares) : clamp(ares, -4.0, 4.0);

        uint cdst = (cw >> 22) & 3, adst = (aw >> 22) & 3;
        r[cdst].rgb = cres;
        r[adst].a = ares;
    }
    float4 o = saturate(r[0]);

    uint ac = misc.y;
    bool p0 = AlphaTest((ac >> 16) & 7, o.a, ac & 0xFF);
    bool p1 = AlphaTest((ac >> 19) & 7, o.a, (ac >> 8) & 0xFF);
    uint logic = (ac >> 22) & 3;
    bool passed = logic == 0 ? (p0 && p1) : logic == 1 ? (p0 || p1) : logic == 2 ? (p0 != p1) : (p0 == p1);
    if (!passed)
        discard;

    // Z texture (BP 0xF4 bias, 0xF5 format/op): the depth comes from the last stage's texel.
    if (zTex.x != 0)
    {
        float4 tb = round(saturate(lastTex) * 255.0);
        float tz = zTex.y == 0 ? tb.a : zTex.y == 1 ? tb.r * 256.0 + tb.a : tb.r * 65536.0 + tb.g * 256.0 + tb.b;
        tz += zTex.z;
        zc = zTex.x == 1 ? zc + tz : tz;
        zc = fmod(zc, 16777216.0);
    }

    uint fogType = misc.z & 7;
    if (fogType != 0)
    {
        // Same math as Dolphin: the 24-bit EFB depth turned into eye distance, minus C.
        float zCoord = zc;
        float ze;
        if ((misc.z >> 3) & 1)  // orthographic
            ze = fogParams.x * zCoord / 16777216.0;
        else
            ze = (fogParams.x * 16777216.0) / (fogParams.z - floor(zCoord / exp2(fogParams.w)));
        float f = saturate(ze - fogParams.y);
        if (fogType == 4) f = 1.0 - exp2(-8.0 * f);
        else if (fogType == 5) f = 1.0 - exp2(-8.0 * f * f);
        else if (fogType == 6) f = exp2(-8.0 * (1.0 - f));
        else if (fogType == 7) { float b = 1.0 - f; f = exp2(-8.0 * b * b); }
        float ifog = round(f * 256.0);
        uint fc = misc.w;
        float3 fogColor = float3((fc >> 16) & 0xFF, (fc >> 8) & 0xFF, fc & 0xFF);
        float3 prev = round(o.rgb * 255.0);
        o.rgb = floor((prev * (256.0 - ifog) + fogColor * ifog) / 256.0) / 255.0;
    }
    return o;
}

float4 ps_main(V i) : SV_Target
{
    float zc = round(i.pos.z * 16777215.0);
    return PsBody(i, zc);
}

// Draws with a z texture write their own depth (a separate pipeline, so the others keep early z).
struct PsOutZ { float4 c : SV_Target; float d : SV_Depth; };
PsOutZ ps_main_z(V i)
{
    PsOutZ o;
    float zc = round(i.pos.z * 16777215.0);
    o.c = PsBody(i, zc);
    o.d = zc / 16777215.0;
    return o;
}

// Present: the copied EFB rectangle, scaled to the window.
// EFB -> texture copies use the same vertex shader; wI/wA turn the EFB color into the
// texel's intensity and alpha (cparams.x = 1), or the color passes through (cparams.x = 0).
cbuffer Blit : register(b0) { float4 rect; float4 wI; float4 wA; float4 cparams; };   // rect: u0, v0, u1, v1
Texture2D efb : register(t0);
struct B { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
B vs_blit(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);
    B o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = lerp(rect.xy, rect.zw, p);
    return o;
}
float4 ps_blit(B i) : SV_Target { return float4(efb.Sample(sClampLinear, i.uv).rgb, 1); }
float4 ps_copy(B i) : SV_Target
{
    float4 c;
    if (cparams.w >= 2)
    {
        uint w, h;
        efb.GetDimensions(w, h);
        uint z = uint(round(efb.Load(int3(min(uint2(i.uv * float2(w, h)), uint2(w - 1, h - 1)), 0)).r * 16777215.0));
        c = float4((z >> 16) & 0xFF, (z >> 8) & 0xFF, z & 0xFF, 255) / 255.0;
    }
    else
        c = efb.Sample(sClampLinear, i.uv);
    if (cparams.x == 0)
        return float4(c.rgb, cparams.w != 0 ? 1 : c.a);
    float v = saturate(dot(c, wI) + cparams.y);
    return float4(v, v, v, saturate(dot(c, wA) + cparams.z));
}
)";

struct DrawConstants
{
    uint32_t tevColor[16];
    uint32_t tevAlpha[16];
    uint32_t tevOrder[8];
    uint32_t ksel[8];
    float regs[4][4];
    float konst[4][4];
    uint32_t misc[4];
    uint32_t samplers[8];
    float proj0[4], proj1[4], vp0[4], vp1[4];
    uint32_t chan[4];
    float ambColor[4], matColor[4];
    uint32_t texgen[8], postInfo[8];
    float fogParams[4];
    float zTex[4];
};

struct CachedTexture
{
    ComPtr<ID3D12Resource> res;
    UINT srv = 0;          // index in g_texSrvHeap
    uint64_t hash = 0;
    uint32_t checkedFrame = 0;
};

bool g_active = false;
HWND g_hwnd;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGISwapChain3> g_swap;
ComPtr<ID3D12Resource> g_backBuffers[kBackBuffers];
ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap, g_srvHeap, g_texSrvHeap;
UINT g_rtvSize = 0, g_srvSize = 0;
ComPtr<ID3D12Resource> g_efb, g_depth, g_upload, g_white, g_readback;
uint8_t* g_uploadPtr = nullptr;
UINT64 g_uploadOffset = 0;
uint32_t g_uploadEpoch = 0;  // bumped whenever the upload buffer starts over
UINT g_srvOffset = 0;
ComPtr<ID3D12CommandAllocator> g_alloc;
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue = 0;
HANDLE g_fenceEvent;
ComPtr<ID3D12RootSignature> g_rootSig;
ComPtr<ID3DBlob> g_vs, g_ps, g_psZ, g_vsBlit, g_psBlit, g_psCopy;
ComPtr<ID3D12PipelineState> g_blitPso, g_copyPso;
std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_psos;
std::unordered_map<uint64_t, CachedTexture> g_textures;

// EFB -> texture copies stay on the GPU. The destination in guest memory gets a unique
// pattern; while it is still there, a texture at that address is the GPU copy.
struct EfbCopy
{
    ComPtr<ID3D12Resource> res;
    UINT srv = UINT_MAX;
    uint32_t width = 0, height = 0, bytes = 0;
    uint64_t hash = 0;
    uint32_t checkedFrame = 0;
    uint32_t serial = 0;   // g_copySerial of the last copy here
    bool valid = false;
    // Targets of every size copied to this address (games reuse one address for a chain of
    // sizes, such as blur passes): kept instead of recreated on each copy.
    struct Target { uint32_t width, height; ComPtr<ID3D12Resource> res; UINT srv; };
    std::vector<Target> pool;
};
std::unordered_map<uint32_t, EfbCopy> g_efbCopies;
uint32_t g_copySerial = 0;
// Textures the game reads across several EFB copies placed one after another in memory
// (same width, the second right after the first's bytes): stacked into one texture.
struct StackedCopy
{
    ComPtr<ID3D12Resource> res;
    UINT srv = UINT_MAX;
    std::vector<uint32_t> serials;  // of the pieces when it was last assembled
};
std::unordered_map<uint64_t, StackedCopy> g_stacked;
std::vector<ComPtr<ID3D12Resource>> g_frameGarbage;  // released after the frame's fence

// Frames are recorded into slots: each has its own command lists (the frame's work, and
// the texture uploads that must run only once), allocators, upload buffer and
// shader-visible heap (g_list, g_ulist, g_alloc, g_upload, g_uploadPtr and g_srvHeap point
// at the current slot's). A recorded frame goes to the presenter thread, which can run its
// lists several times with interpolated matrices (see "Frame interpolation" below) and
// gives the slot back when it is done.
constexpr int kSlots = 4;
struct FrameSlot
{
    ComPtr<ID3D12CommandAllocator> alloc, ualloc;
    ComPtr<ID3D12GraphicsCommandList> list, ulist;
    ComPtr<ID3D12Resource> upload;
    uint8_t* uploadPtr = nullptr;
    // GPU-memory copy of the upload buffer, made once per frame: draws read vertices and
    // constants from here, so re-running a frame does not read system memory again.
    ComPtr<ID3D12Resource> vram;
    bool vramReadable = false;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT64 fence = 0;
    bool inUse = false;
    std::vector<ComPtr<ID3D12Resource>> garbage;
};
FrameSlot g_slots[kSlots];
int g_slot = 0;
ComPtr<ID3D12GraphicsCommandList> g_ulist;   // the current slot's texture uploads
std::mutex g_slotLock, g_queueLock;          // slot ownership; queue submissions and g_fenceValue
std::condition_variable g_slotCv;
HANDLE g_guestEvent = nullptr;               // fence waits on the emulation thread
bool g_direct = true;                        // run lists at once (initialization)

// One recorded game frame (from one XFB copy to the next).
struct XfSnapshot
{
    uint8_t* dst;                 // its copy in the slot's upload buffer (what the GPU reads)
    uint64_t key;                 // the draw that first used it: where it comes from...
    uint32_t occurrence;          // ...and how many times that source was drawn before it
    std::vector<uint32_t> words;  // its values (XF memory 0x000-0x67F)
};
struct FrameRecord
{
    std::vector<int> slots;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> ulists, lists;
    std::vector<XfSnapshot> snaps;
    LARGE_INTEGER time{};         // when the game finished it
    bool shot = false;
};
FrameRecord g_record;
// Draw state already set in the current list (reset when a list starts or after a copy or
// blit sets its own state).
struct GxDrawState
{
    bool valid = false;
    ID3D12PipelineState* pso = nullptr;
    UINT64 table = 0;
    D3D12_GPU_VIRTUAL_ADDRESS xf = 0;
    D3D12_RECT scissor{};
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_GPU_VIRTUAL_ADDRESS cb = 0;
    // Draws waiting to go out as one: same state, vertices one after another.
    bool pending = false;
    D3D12_GPU_VIRTUAL_ADDRESS vbStart = 0;
    UINT vbCount = 0;
};
GxDrawState g_drawState;
uint64_t g_drawCalls = 0;  // D3D12 draws after merging (DSE_PROF)

void FlushDraws()
{
    GxDrawState& st = g_drawState;
    if (!st.pending)
        return;
    D3D12_VERTEX_BUFFER_VIEW vbv{ st.vbStart, UINT(st.vbCount * sizeof(GxVertex)), sizeof(GxVertex) };
    g_list->IASetVertexBuffers(0, 1, &vbv);
    g_list->DrawInstanced(st.vbCount, 1, 0, 0);
    st.pending = false;
    g_drawCalls++;
}
std::mutex g_recLock;
std::condition_variable g_recCv;
std::deque<FrameRecord> g_recQueue;
std::atomic<bool> g_presenterBusy{ false };
int g_targetFps = 0;                          // DSE_FPS: 0 = no interpolation
ComPtr<ID3D12Resource> g_output;              // the finished frame
void WaitPresenterIdle();
std::unordered_map<uint64_t, uint32_t> g_keyCounts;  // draw key -> times seen this frame
uint64_t g_drawKey = 0;                              // the current draw's source
uint32_t g_drawOccurrence = 0;                       // and its occurrence this frame
UINT g_texSrvCount = 0;
uint32_t g_frame = 0;
uint32_t g_frameDrawCount = 0;
uint32_t g_depthCopyFrame = 0;
std::vector<std::string> g_tailDraws;  // DSE_COPYPROBE: draws since the last texture copy  // last frame with an EFB depth copy (debugging)
bool g_inFrame = false;
bool g_efbIsTarget = true;   // EFB state: render target (true) or shader resource
bool g_pendingClear = true;
float g_clearColor[4] = { 0, 0, 0, 1 };
float g_clearDepth = 1.0f;
uint64_t g_draws = 0;

void PrintDebugMessages();

// DSE_PROF=1: time spent in the backend, reported every 600 frames.
bool ProfOn()
{
    static const bool on = getenv("DSE_PROF") != nullptr;
    return on;
}
struct ProfTimer
{
    uint64_t& acc;
    LARGE_INTEGER t0{};
    explicit ProfTimer(uint64_t& a) : acc(a)
    {
        if (ProfOn())
            QueryPerformanceCounter(&t0);
    }
    ~ProfTimer()
    {
        if (!ProfOn())
            return;
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        acc += uint64_t(t1.QuadPart - t0.QuadPart);
    }
};
uint64_t g_profDraw = 0, g_profTex = 0, g_profWait = 0, g_profFill = 0, g_profCopy = 0;

bool Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LOG("[d3d12] %s failed (0x%08lX)", what, (unsigned long)hr);
        PrintDebugMessages();
    }
    return SUCCEEDED(hr);
}

// With DSE_D3DDEBUG=1: the debug layer's messages go to the log.
void PrintDebugMessages()
{
    ComPtr<ID3D12InfoQueue> q;
    if (!g_device || FAILED(g_device.As(&q)))
        return;
    for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; i++)
    {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<uint8_t> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(q->GetMessage(i, m, &len)))
            LOG("[d3d12 debug] %s", m->pDescription);
    }
    q->ClearStoredMessages();
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * g_rtvSize;
    return h;
}

void BarrierOn(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    BarrierOn(g_list.Get(), res, from, to);
}

ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{ heap };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Check(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}

ComPtr<ID3D12Resource> CreateTexture(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, UINT16 mips = 1)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = mips;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    Check(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&r)), "texture");
    return r;
}

// Bump allocation in this frame's upload buffer.
void* Upload(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpu, UINT64* offset = nullptr, bool live = false);

void CreateSrv(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE where)
{
    UINT mips = res->GetDesc().MipLevels;
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = mips;
    g_device->CreateShaderResourceView(res, &d, where);
}

D3D12_CPU_DESCRIPTOR_HANDLE TexSrv(UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_texSrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * g_srvSize;
    return h;
}

void WaitFence(UINT64 value, HANDLE event)
{
    if (g_fence->GetCompletedValue() < value)
    {
        g_fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, INFINITE);
    }
}

void WaitGpu()
{
    UINT64 value;
    {
        std::lock_guard lock(g_queueLock);
        g_queue->Signal(g_fence.Get(), ++g_fenceValue);
        value = g_fenceValue;
    }
    WaitFence(value, g_guestEvent);
}

void BeginFrame()
{
    if (g_inFrame)
        return;
    // Next slot, once the presenter gave it back and the GPU is done with it.
    g_slot = (g_slot + 1) % kSlots;
    FrameSlot& slot = g_slots[g_slot];
    {
        ProfTimer pt(g_profWait);
        std::unique_lock lock(g_slotLock);
        g_slotCv.wait(lock, [&] { return !slot.inUse; });
        slot.inUse = true;
    }
    {
        ProfTimer pt(g_profWait);
        WaitFence(slot.fence, g_guestEvent);
    }
    slot.garbage.clear();
    g_alloc = slot.alloc;
    g_upload = slot.upload;
    g_uploadPtr = slot.uploadPtr;
    g_srvHeap = slot.srvHeap;
    g_alloc->Reset();
    slot.ualloc->Reset();
    g_list = slot.list;
    g_ulist = slot.ulist;
    g_list->Reset(g_alloc.Get(), nullptr);
    g_ulist->Reset(slot.ualloc.Get(), nullptr);
    g_drawState.valid = false;
    if (!g_direct)
        g_record.slots.push_back(g_slot);
    g_uploadOffset = 0;
    g_uploadEpoch++;
    g_srvOffset = 0;
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    if (!g_efbIsTarget)
    {
        Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_efbIsTarget = true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (g_pendingClear)
    {
        g_list->ClearRenderTargetView(Rtv(kBackBuffers), g_clearColor, 0, nullptr);
        g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, g_clearDepth, 0, 0, nullptr);
        g_pendingClear = false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kBackBuffers);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    g_inFrame = true;
}

// Closes the slot's lists. They join the frame record (run by the presenter), or during
// initialization run at once.
void EndFrame()
{
    if (!g_inFrame)
        return;
    FlushDraws();
    FrameSlot& slot = g_slots[g_slot];
    // The frame's uploads to GPU memory, before its lists run (in the once-only list).
    if (g_uploadOffset)
    {
        if (slot.vramReadable)
            BarrierOn(g_ulist.Get(), slot.vram.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_COPY_DEST);
        g_ulist->CopyBufferRegion(slot.vram.Get(), 0, slot.upload.Get(), 0, g_uploadOffset);
        BarrierOn(g_ulist.Get(), slot.vram.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        slot.vramReadable = true;
    }
    g_list->Close();
    g_ulist->Close();
    for (auto& r : g_frameGarbage)
        slot.garbage.push_back(std::move(r));
    g_frameGarbage.clear();
    if (g_direct)
    {
        ID3D12CommandList* lists[] = { g_ulist.Get(), g_list.Get() };
        {
            std::lock_guard lock(g_queueLock);
            g_queue->ExecuteCommandLists(2, lists);
            g_queue->Signal(g_fence.Get(), ++g_fenceValue);
            slot.fence = g_fenceValue;
        }
        WaitFence(slot.fence, g_guestEvent);
        std::lock_guard lock(g_slotLock);
        slot.inUse = false;
    }
    else
    {
        g_record.ulists.push_back(g_ulist);
        g_record.lists.push_back(g_list);
    }
    g_inFrame = false;
}

// Room for `bytes` more in the upload buffer (else the frame moves on to a new slot).
void EnsureUpload(UINT64 bytes)
{
    if (g_uploadOffset + bytes + 1024 > kUploadSize)
    {
        EndFrame();
        BeginFrame();
    }
}

// `gpu` is where the GPU reads the data: the slot's GPU-memory copy (made when the frame
// ends), or with `live` the upload buffer itself (XF snapshots, rewritten between runs).
void* Upload(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpu, UINT64* offset, bool live)
{
    UINT64 at = (g_uploadOffset + align - 1) & ~(align - 1);
    if (at + size > kUploadSize)
    {
        // Out of space mid-frame: submit what there is and start over.
        EndFrame();
        BeginFrame();
        at = 0;
    }
    g_uploadOffset = at + size;
    gpu = (live ? g_upload : g_slots[g_slot].vram)->GetGPUVirtualAddress() + at;
    if (offset)
        *offset = at;
    return g_uploadPtr + at;
}

// --- Pipeline states -----------------------------------------------------------------

D3D12_BLEND SrcFactor(uint32_t f)
{
    static const D3D12_BLEND t[8] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA };
    return t[f & 7];
}
D3D12_BLEND DstFactor(uint32_t f)
{
    static const D3D12_BLEND t[8] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA };
    return t[f & 7];
}
D3D12_COMPARISON_FUNC DepthFunc(uint32_t f)
{
    static const D3D12_COMPARISON_FUNC t[8] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS };
    return t[f & 7];
}

ID3D12PipelineState* GetPso(uint32_t cmode0, uint32_t zmode, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, uint32_t cull,
    bool zTexture)
{
    uint64_t key = uint64_t(cmode0 & 0xFFFF) | uint64_t(zmode & 0x1F) << 16 | uint64_t(topo) << 24 |
        uint64_t(cull & 3) << 28 | uint64_t(zTexture) << 30;
    auto it = g_psos.find(key);
    if (it != g_psos.end())
        return it->second.Get();

    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, 80, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT, 0, 88, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32G32_FLOAT, 0, 96, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32_FLOAT, 0, 104, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 112, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g_rootSig.Get();
    d.VS = { g_vs->GetBufferPointer(), g_vs->GetBufferSize() };
    ID3DBlob* ps = zTexture ? g_psZ.Get() : g_ps.Get();
    d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    d.InputLayout = { layout, UINT(std::size(layout)) };
    d.PrimitiveTopologyType = topo;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = 1;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // DSE_CULL: 0 off, 1 GX front faces are clockwise on screen (default), 2 counter-clockwise.
    static const int cullWinding = [] { const char* e = getenv("DSE_CULL"); return e ? atoi(e) : 1; }();
    d.RasterizerState.CullMode = cullWinding == 0 || topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ? D3D12_CULL_MODE_NONE
        : (cull & 3) == 1 ? D3D12_CULL_MODE_BACK : (cull & 3) == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = cullWinding == 2;
    d.RasterizerState.DepthClipEnable = TRUE;

    // PE_CMODE0: bit 0 blend, bit 3 color update, bit 4 alpha update, bits 5-7 dst
    // factor, bits 8-10 src factor, bit 11 subtract.
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
    bool subtract = (cmode0 >> 11) & 1;
    b.BlendEnable = (cmode0 & 1) || subtract;
    b.SrcBlend = subtract ? D3D12_BLEND_ONE : SrcFactor((cmode0 >> 8) & 7);
    b.DestBlend = subtract ? D3D12_BLEND_ONE : DstFactor((cmode0 >> 5) & 7);
    b.BlendOp = subtract ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_ZERO;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = UINT8(((cmode0 >> 3) & 1 ? 7 : 0) | ((cmode0 >> 4) & 1 ? 8 : 0));

    // ZMODE: bit 0 test, bits 1-3 function, bit 4 update.
    d.DepthStencilState.DepthEnable = (zmode & 1) != 0;
    d.DepthStencilState.DepthFunc = DepthFunc((zmode >> 1) & 7);
    d.DepthStencilState.DepthWriteMask = ((zmode >> 4) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;

    ComPtr<ID3D12PipelineState> pso;
    if (!Check(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "pipeline state"))
        return nullptr;
    return (g_psos[key] = pso).Get();
}

// --- Textures --------------------------------------------------------------------------

// Texture-change hash: 8 bytes per step (byte-at-a-time FNV cost milliseconds per frame).
uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// EFB copy pattern check: a few words spread over the destination (hashing megabytes
// per copy cost several milliseconds per frame).
uint64_t SampleHash(const uint8_t* p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t step = n > 64 * 64 ? n / 64 & ~7u : 8;
    for (uint32_t i = 0; i + 8 <= n; i += step)
    {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    }
    return h;
}

void FlushDraws();
// A width x height texture at `addr` made of EFB copies that follow one another in memory
// (the game's blur builds a 160x112 image from a 160x80 copy and a 160x32 copy right after
// it). UINT_MAX if the copies don't cover it.
UINT StackedCopyFor(uint32_t addr, uint32_t width, uint32_t height)
{
    std::vector<EfbCopy*> pieces;
    uint32_t a = addr, h = 0;
    while (h < height && pieces.size() < 8)
    {
        auto it = g_efbCopies.find(a);
        if (it == g_efbCopies.end())
            return UINT_MAX;
        EfbCopy& e = it->second;
        if (e.checkedFrame != g_frame)
        {
            e.checkedFrame = g_frame;
            e.valid = SampleHash(g_base + a, e.bytes) == e.hash;
        }
        if (!e.valid || e.width != width || !e.res)
            return UINT_MAX;
        pieces.push_back(&e);
        h += e.height;
        a += e.bytes;
    }
    if (h < height)
        return UINT_MAX;
    static const UINT cs = getenv("DSE_COPYSCALE") ? g_scale : 1;
    StackedCopy& st = g_stacked[uint64_t(addr) | uint64_t(width) << 32 | uint64_t(height) << 44];
    if (!st.res)
    {
        if (g_texSrvCount >= kTexSrvCapacity)
            return UINT_MAX;
        st.res = CreateTexture(width * cs, height * cs, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
        if (!st.res)
            return UINT_MAX;
        st.srv = g_texSrvCount++;
        CreateSrv(st.res.Get(), TexSrv(st.srv));
    }
    std::vector<uint32_t> serials;
    for (EfbCopy* e : pieces)
        serials.push_back(e->serial);
    if (serials != st.serials)
    {
        st.serials = serials;
        FlushDraws();
        Barrier(st.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        uint32_t y = 0;
        for (EfbCopy* e : pieces)
        {
            uint32_t rows = std::min(e->height, height - y);
            Barrier(e->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = e->res.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = st.res.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{ 0, 0, 0, width * cs, rows * cs, 1 };
            g_list->CopyTextureRegion(&to, 0, y * cs, 0, &from, &box);
            Barrier(e->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            y += rows;
        }
        Barrier(st.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        static int logged = 0;
        if (logged++ < 4)
            LOG("[d3d12] stacked %zu EFB copies into a %ux%u texture at 0x%08X", pieces.size(), width, height, addr);
    }
    return st.srv;
}

// SRV (CPU heap index) for texture map `map` as currently set up, or UINT_MAX.
UINT TextureForMap(uint32_t map)
{
    uint32_t image0 = g_gx.bp[(map < 4 ? 0x88 : 0xA8) + (map & 3)];
    uint32_t image3 = g_gx.bp[(map < 4 ? 0x94 : 0xB4) + (map & 3)];
    uint32_t tlut = g_gx.bp[(map < 4 ? 0x98 : 0xB8) + (map & 3)];
    uint32_t width = (image0 & 0x3FF) + 1, height = ((image0 >> 10) & 0x3FF) + 1, fmt = (image0 >> 20) & 15;
    uint32_t addr = ((image3 & 0xFFFFFF) << 5) | 0x80000000;
    uint32_t tlutAddr = (tlut & 0x3FF) << 9, tlutFmt = (tlut >> 10) & 3;
    bool palette = fmt == kTexC4 || fmt == kTexC8 || fmt == kTexC14X2;

    auto copy = g_efbCopies.find(addr);
    if (copy != g_efbCopies.end())
    {
        EfbCopy& e = copy->second;
        if (e.checkedFrame != g_frame)
        {
            e.checkedFrame = g_frame;
            e.valid = SampleHash(g_base + addr, e.bytes) == e.hash;
        }
        if (e.valid && (e.width != width || e.height != height))
        {
            static std::set<uint64_t> seenMis;
            uint64_t k = uint64_t(addr) ^ uint64_t(width) << 32 ^ uint64_t(height) << 44 ^ uint64_t(e.height) << 54;
            if (seenMis.insert(k).second && seenMis.size() < 40)
                LOG("[d3d12] texture 0x%08X read as %ux%u fmt %u, EFB copy there is %ux%u (%u bytes)", addr, width, height, fmt,
                    e.width, e.height, e.bytes);
        }
        if (e.valid && e.width == width && e.height == height)
            return e.srv;
        if (e.valid)
        {
            UINT srv = e.width == width && height > e.height ? StackedCopyFor(addr, width, height) : UINT_MAX;
            if (srv != UINT_MAX)
                return srv;
            // Read at another size than the last copy here: an earlier copy of that size (the
            // game reads it after a smaller copy reused the start of its memory).
            static const UINT cs = getenv("DSE_COPYSCALE") ? g_scale : 1;
            for (const EfbCopy::Target& t : e.pool)
                if (t.width == width * cs && t.height == height * cs)
                    return t.srv;
            return e.srv;
        }
    }

    uint64_t key = uint64_t(addr) | uint64_t(fmt) << 32 | uint64_t(width) << 36 | uint64_t(height) << 46 |
        (palette ? uint64_t(tlutFmt) << 56 | uint64_t(tlutAddr >> 9) << 58 : 0);
    CachedTexture& t = g_textures[key];
    if (t.res && t.checkedFrame == g_frame)
        return t.srv;

    uint32_t bytes = GxTextureSize(width, height, fmt);
    uint64_t hash = Fnv(g_base + addr, bytes);
    t.checkedFrame = g_frame;
    if (t.res && t.hash == hash)
        return t.srv;
    t.hash = hash;

    std::vector<uint32_t> rgba(size_t(width) * height);
    GxDecodeTexture(g_base + addr, width, height, fmt, tlutAddr, tlutFmt, rgba.data());
    static bool dump = getenv("DSE_TEXDUMP") != nullptr;
    if (dump)
    {
        char path[256];
        snprintf(path, sizeof(path), "G:/Dead Space Extraction PC/Recomp/logs/tex/%08X_f%u_%ux%u.bmp", addr, fmt, width, height);
        if (FILE* f = fopen(path, "wb"))
        {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(width);
            ih.biHeight = -LONG(height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + DWORD(rgba.size() * 4);
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            for (uint32_t px : rgba)
            {
                uint32_t bgra = (px & 0xFF00FF00) | ((px & 0xFF) << 16) | ((px >> 16) & 0xFF);
                fwrite(&bgra, 4, 1, f);
            }
            fclose(f);
        }
    }

    if (!t.res)
    {
        if (g_texSrvCount >= kTexSrvCapacity)
            return UINT_MAX;
        t.srv = g_texSrvCount++;
    }
    else
        g_frameGarbage.push_back(t.res);
    // Mip chain (2x2 box filter down to 1x1): far and slanted surfaces stay clean with the
    // anisotropic samplers instead of shimmering.
    std::vector<std::vector<uint32_t>> levels;
    levels.push_back(std::move(rgba));
    uint32_t lw = width, lh = height;
    while (lw > 1 || lh > 1)
    {
        uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        const std::vector<uint32_t>& src = levels.back();
        std::vector<uint32_t> dst(size_t(nw) * nh);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++)
            {
                uint32_t x0 = x * 2, y0 = y * 2;
                uint32_t x1 = std::min(x0 + 1, lw - 1), y1 = std::min(y0 + 1, lh - 1);
                uint32_t p[4] = { src[size_t(y0) * lw + x0], src[size_t(y0) * lw + x1], src[size_t(y1) * lw + x0],
                    src[size_t(y1) * lw + x1] };
                uint32_t out = 0;
                for (int c = 0; c < 32; c += 8)
                {
                    uint32_t sum = 0;
                    for (uint32_t q : p)
                        sum += (q >> c) & 0xFF;
                    out |= ((sum + 2) / 4) << c;
                }
                dst[size_t(y) * nw + x] = out;
            }
        levels.push_back(std::move(dst));
        lw = nw;
        lh = nh;
    }

    t.res = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, UINT16(levels.size()));
    if (!t.res)
        return UINT_MAX;

    lw = width;
    lh = height;
    for (UINT level = 0; level < levels.size(); level++)
    {
        UINT pitch = (lw * 4 + 255) & ~255u;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        UINT64 offset;
        uint8_t* dst = static_cast<uint8_t*>(Upload(UINT64(pitch) * lh, 512, gpu, &offset));
        for (uint32_t y = 0; y < lh; y++)
            memcpy(dst + size_t(y) * pitch, &levels[level][size_t(y) * lw], lw * 4);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = g_upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = offset;
        from.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, lw, lh, 1, pitch };
        to.pResource = t.res.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = level;
        g_ulist->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    BarrierOn(g_ulist.Get(), t.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateSrv(t.res.Get(), TexSrv(t.srv));
    return t.srv;
}

float S11(uint32_t v) { return float(int32_t(v << 21) >> 21) / 255.0f; }

void FillConstants(DrawConstants& c, uint32_t& textureMask)
{
    uint32_t genMode = g_gx.bp[0x00];
    uint32_t stages = ((genMode >> 10) & 15) + 1;
    for (int s = 0; s < 16; s++)
    {
        c.tevColor[s] = g_gx.bp[0xC0 + 2 * s];
        c.tevAlpha[s] = g_gx.bp[0xC1 + 2 * s];
    }
    for (int i = 0; i < 8; i++)
    {
        c.tevOrder[i] = g_gx.bp[0x28 + i];
        c.ksel[i] = g_gx.bp[0xF6 + i];
    }
    // Registers: (red, alpha) at 2i, (blue, green) at 2i + 1. Konst colors are 8-bit.
    for (int i = 0; i < 4; i++)
    {
        uint32_t ra = g_gx.tevReg[2 * i], bg = g_gx.tevReg[2 * i + 1];
        c.regs[i][0] = S11(ra);
        c.regs[i][3] = S11(ra >> 12);
        c.regs[i][2] = S11(bg);
        c.regs[i][1] = S11(bg >> 12);
        uint32_t kra = g_gx.tevKonst[2 * i], kbg = g_gx.tevKonst[2 * i + 1];
        c.konst[i][0] = float(kra & 0xFF) / 255.0f;
        c.konst[i][3] = float((kra >> 12) & 0xFF) / 255.0f;
        c.konst[i][2] = float(kbg & 0xFF) / 255.0f;
        c.konst[i][1] = float((kbg >> 12) & 0xFF) / 255.0f;
    }
    c.misc[0] = stages;
    c.misc[1] = g_gx.bp[0xF3];
    {
        static const bool noZTex = getenv("DSE_NOZTEX") != nullptr;
        uint32_t op = (g_gx.bp[0xF5] >> 2) & 3;
        if (op != 0 && op != 3 && !noZTex)
        {
            c.zTex[0] = float(op);
            c.zTex[1] = float(g_gx.bp[0xF5] & 3);
            c.zTex[2] = float(g_gx.bp[0xF4] & 0xFFFFFF);
        }
    }
    // Fog (BP 0xEE A, 0xEF B magnitude, 0xF0 B shift, 0xF1 C + projection + type, 0xF2 color).
    {
        uint32_t f3 = g_gx.bp[0xF1];
        uint32_t type = (f3 >> 21) & 7;
        // The game only uses fog for its depth-of-field mask. Off unless profundidad_campo = 1 in
        // dse.ini (launcher: "Desenfoque de profundidad"), or DSE_FOG=1.
        static const bool noFog = getenv("DSE_FOG") == nullptr && !ConfigBool("profundidad_campo", false);
        if (type != 0 && !noFog)
        {
            auto fogFloat = [](uint32_t v) {
                uint32_t bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12;
                float f;
                memcpy(&f, &bits, 4);
                return f;
            };
            c.misc[2] = type | ((f3 >> 20) & 1) << 3;
            c.misc[3] = g_gx.bp[0xF2] & 0xFFFFFF;
            c.fogParams[0] = fogFloat(g_gx.bp[0xEE]);
            c.fogParams[1] = fogFloat(f3);
            c.fogParams[2] = float(g_gx.bp[0xEF] & 0xFFFFFF);
            c.fogParams[3] = float(g_gx.bp[0xF0] & 0x1F);
        }
    }
    // Indirect texturing is not done yet: count draws that use it (DSE_INDLOG=1).
    static bool indLog = getenv("DSE_INDLOG") != nullptr;
    if (indLog)
    {
        static uint32_t indDraws = 0;
        uint32_t numInd = (genMode >> 16) & 7;
        bool any = false;
        for (uint32_t s = 0; s < stages; s++)
            any |= (g_gx.bp[0x10 + s] & 0x1FFFFF) != 0;
        if (numInd || any)
            if ((indDraws++ % 2000) == 0)
                LOG("[d3d12] indirect texturing in use: %u draws (%u indirect stages, IND_CMD0 0x%X)", indDraws, numInd,
                    g_gx.bp[0x10]);
    }
    textureMask = 0;
    for (uint32_t s = 0; s < stages; s++)
    {
        uint32_t order = g_gx.bp[0x28 + s / 2] >> ((s & 1) * 12);
        if ((order >> 6) & 1)
            textureMask |= 1u << (order & 7);
    }
    for (uint32_t m = 0; m < 8; m++)
    {
        uint32_t mode0 = g_gx.bp[(m < 4 ? 0x80 : 0xA0) + (m & 3)];
        // TEXMODE0: bits 0-1 wrap S, 2-3 wrap T (0 clamp, 1 repeat, 2 mirror), bit 4 linear magnification.
        uint32_t wrapS = mode0 & 3, wrapT = (mode0 >> 2) & 3;
        bool point = ((mode0 >> 4) & 1) == 0;
        // Mipmaps only when the game uses them: a mip min filter (TEXMODE0 bits 5-7: 1, 2, 5,
        // 6) and a max LOD above 0 (TEXMODE1 bits 8-15).
        uint32_t mode1 = g_gx.bp[(m < 4 ? 0x84 : 0xA4) + (m & 3)];
        uint32_t minFilter = (mode0 >> 5) & 7;
        bool mips = (minFilter & 3) != 0 && ((mode1 >> 8) & 0xFF) != 0;
        c.samplers[m] = (wrapS != 0 ? 4 : 0) | (wrapT != 0 ? 2 : 0) | (point ? 1 : 0) |
            (wrapS == 2 ? 8 : 0) | (wrapT == 2 ? 16 : 0) | (mips ? 32 : 0);
    }

    // Vertex stage: projection, viewport, color channels and texgens (the shader reads the
    // matrices and lights from the XF memory buffer).
    for (int i = 0; i < 4; i++)
        c.proj0[i] = GxXfFloat(0x1020 + i);
    c.proj1[0] = GxXfFloat(0x1024);
    c.proj1[1] = GxXfFloat(0x1025);
    c.proj1[2] = g_gx.xfReg[0x26] != 0 ? 1.0f : 0.0f;
    c.proj1[3] = 1.0f;  // GxBackendDraw sets the wide-screen factor
    c.vp0[0] = GxXfFloat(0x101A);
    c.vp0[1] = GxXfFloat(0x101B);
    c.vp0[2] = GxXfFloat(0x101C);
    c.vp0[3] = GxXfFloat(0x101D) - 342.0f;
    c.vp1[0] = GxXfFloat(0x101E) - 342.0f;
    c.vp1[1] = GxXfFloat(0x101F);
    c.chan[0] = g_gx.xfReg[0x0E];
    c.chan[1] = g_gx.xfReg[0x10];
    c.chan[2] = g_gx.xfReg[0x3F] & 15;
    c.chan[3] = g_gx.xfReg[0x12] & 1;
    auto unpack = [](uint32_t v, float* o) {
        for (int k = 0; k < 4; k++)
            o[k] = float((v >> (24 - 8 * k)) & 0xFF) / 255.0f;
    };
    unpack(g_gx.xfReg[0x0A], c.ambColor);
    unpack(g_gx.xfReg[0x0C], c.matColor);
    for (int i = 0; i < 8; i++)
    {
        c.texgen[i] = g_gx.xfReg[0x40 + i];
        c.postInfo[i] = g_gx.xfReg[0x50 + i];
    }
    // 21:9 / 32:9: coordinates projected from the camera (STQ from the position) that read an
    // EFB copy of a widened view look up that copy as widened too (bit 31, factor in vp1.z).
    c.vp1[2] = g_wideK;
    if (g_wideK < 0.999f && g_gx.xfReg[0x26] == 0)
    {
        uint32_t stages = ((g_gx.bp[0] >> 10) & 15) + 1;
        for (uint32_t st = 0; st < stages; st++)
        {
            uint32_t order = g_gx.bp[0x28 + st / 2] >> ((st & 1) * 12);
            if (!((order >> 6) & 1))
                continue;
            uint32_t map = order & 7, coord = (order >> 3) & 7;
            uint32_t tg = g_gx.xfReg[0x40 + coord];
            bool projected = ((tg >> 1) & 1) && ((tg >> 4) & 7) == 0 && ((tg >> 7) & 31) == 0;
            uint32_t a3 = g_gx.bp[(map < 4 ? 0x94 : 0xB4) + (map & 3)];
            uint32_t addr = ((a3 & 0xFFFFFF) << 5) | 0x80000000;
            if (projected && g_efbCopies.count(addr))
                c.texgen[coord] |= 0x80000000u;
        }
    }
}

// XF memory 0x000-0x67F for the vertex shader: uploaded again only after it changed
// (g_xfVersion) or when a new upload buffer started (g_uploadEpoch).
constexpr UINT kXfWords = 0x680;
uint32_t g_xfEpoch = UINT32_MAX, g_xfUploaded = 0;
uint64_t g_profSnapshots = 0;  // DSE_PROF: XF memory uploads
D3D12_GPU_VIRTUAL_ADDRESS g_xfGpu = 0;

D3D12_GPU_VIRTUAL_ADDRESS XfMemory()
{
    if (g_xfEpoch != g_uploadEpoch || g_xfUploaded != g_xfVersion)
    {
        g_profSnapshots++;
        auto* dst = static_cast<uint8_t*>(Upload(kXfWords * 4, 256, g_xfGpu, nullptr, true));
        memcpy(dst, g_gx.xfMem, kXfWords * 4);
        if (!g_direct)
            g_record.snaps.push_back({ dst, g_drawKey, g_drawOccurrence,
                std::vector<uint32_t>(g_gx.xfMem, g_gx.xfMem + kXfWords) });
        g_xfEpoch = g_uploadEpoch;
        g_xfUploaded = g_xfVersion;
    }
    return g_xfGpu;
}

// --- Frame interpolation -----------------------------------------------------------------
// The game draws 30 frames a second. Each recorded frame N is run here (on its own
// thread) k times, k = DSE_FPS / game frame rate: run i uses XF matrices interpolated
// between frame N-1 and N at t = i / k, and is presented at its share of the frame time.
// Every object, the camera (it is in each modelview matrix) and skinned characters move
// smoothly; what the CPU animates by rewriting vertices (particles) stays at 30. The last
// run (t = 1) is exactly frame N, so the game's state on the GPU (EFB, copies) ends right.
// The cost is one frame of delay. Matching: an XF snapshot in frame N goes with the
// snapshot of frame N-1 first used by the same draw (g_drawKey: source and occurrence).

struct LerpWord
{
    uint32_t index;
    float from, to;
};

// The float words of XF memory that move: position/texture matrices, normal matrices,
// post matrices and light position/direction/attenuation (not the light colors).
// Which matrix (or light) a word belongs to, and whether it is a translation (or light
// position) rather than a rotation/scale term.
uint32_t MatrixGroup(uint32_t i)
{
    if (i < 0x100) return i / 12;                       // 3x4 position/texture matrices
    if (i >= 0x400 && i < 0x460) return 0x100 + (i - 0x400) / 9;   // 3x3 normal matrices
    if (i >= 0x500 && i < 0x600) return 0x200 + (i - 0x500) / 12;  // post matrices
    return 0x300 + (i - 0x600) / 16;                    // lights
}
bool IsTranslation(uint32_t i)
{
    if (i < 0x100 || (i >= 0x500 && i < 0x600)) return (i & 3) == 3;
    if (i >= 0x600) return (i & 15) >= 10 && (i & 15) < 13;  // light position
    return false;
}

// Lights are not interpolated: the game gives the 8 light slots to different lights from
// one frame to the next, and mixing two lights made the lighting flicker.
bool Interpolated(uint32_t i)
{
    return i < 0x100 || (i >= 0x400 && i < 0x460) || (i >= 0x500 && i < 0x600);
}

// How different two snapshots' position matrices are (to pick the right previous one).
float MatrixDistance(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    float sum = 0;
    for (uint32_t w = 0; w < 0x100; w++)
    {
        if (a[w] == b[w])
            continue;
        float fa, fb;
        memcpy(&fa, &a[w], 4);
        memcpy(&fb, &b[w], 4);
        float d = std::fabs(fa - fb);
        sum += std::isfinite(d) ? d : 1e6f;
    }
    return sum;
}

void WaitPresenterIdle()
{
    for (;;)
    {
        {
            std::lock_guard lock(g_recLock);
            if (g_recQueue.empty() && !g_presenterBusy)
                return;
        }
        Sleep(1);
    }
}

void SleepUntil(LONGLONG target)
{
    for (;;)
    {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        LONGLONG left = target - now.QuadPart;
        if (left <= 0)
            return;
        if (left * 1000 / f.QuadPart >= 2)
            Sleep(1);
        else
            SwitchToThread();
    }
}

DWORD WINAPI Presenter(LPVOID)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<ID3D12CommandAllocator> palloc;
    ComPtr<ID3D12GraphicsCommandList> plist;
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&palloc));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, palloc.Get(), nullptr, IID_PPV_ARGS(&plist));
    plist->Close();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::vector<std::vector<uint32_t>> prevWords;       // frame N-1's snapshots
    std::unordered_map<uint64_t, std::vector<uint32_t>> prevBySource;  // source -> indices in prevWords
    std::vector<uint32_t> prevOccurrence;
    LONGLONG lastTime = 0;
    double frameTime = 1.0 / 30.0;                      // seconds, smoothed
    UINT64 lastFence = 0;
    // Report every 5 s: presents and game frames per second, cuts, matched snapshots.
    LARGE_INTEGER reportStart;
    QueryPerformanceCounter(&reportStart);
    uint32_t presents = 0, frames = 0, cuts = 0, timedRuns = 0, jumpsDropped = 0;
    double gpuMsTotal = 0;
    double gpuRunMs = 0;  // GPU time of one run, smoothed
    uint64_t snapsTotal = 0, snapsMatched = 0;

    auto submit = [&](ID3D12CommandList* const* lists, UINT n) {
        std::lock_guard lock(g_queueLock);
        g_queue->ExecuteCommandLists(n, lists);
        g_queue->Signal(g_fence.Get(), ++g_fenceValue);
        lastFence = g_fenceValue;
    };
    // Presenting: g_output drawn into the back buffer at the image rectangle (blit pipeline),
    // with its own descriptors and constants.
    ComPtr<ID3D12DescriptorHeap> pheap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 8;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&pheap));
        for (UINT m = 0; m < 8; m++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dst = pheap->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += SIZE_T(m) * g_srvSize;
            if (m == 0)
                CreateSrv(g_output.Get(), dst);
            else
                g_device->CopyDescriptorsSimple(1, dst, TexSrv(kTexSrvCapacity), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    ComPtr<ID3D12Resource> pconst = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        float* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        pconst->Map(0, &none, reinterpret_cast<void**>(&p));
        float rect[4] = { 0, 0, 1, 1 };
        memcpy(p, rect, sizeof(rect));
    }
    auto resizeIfNeeded = [&]() {
        if (!VideoTakeResize())
            return;
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0)
            return;  // minimized
        WaitFence(lastFence, event);
        for (UINT i = 0; i < kBackBuffers; i++)
            g_backBuffers[i].Reset();
        if (FAILED(g_swap->ResizeBuffers(0, UINT(rc.right), UINT(rc.bottom), DXGI_FORMAT_UNKNOWN, 0)))
            LOG("[d3d12] swap chain resize to %ldx%ld failed", rc.right, rc.bottom);
        for (UINT i = 0; i < kBackBuffers; i++)
        {
            g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
            g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, Rtv(i));
        }
    };

    // DSE_RUNSHOTS=n: every n game frames, every run's image goes to logs/runs/ (to check the
    // interpolated frames one after another).
    static const int runShots = getenv("DSE_RUNSHOTS") ? std::max(1, atoi(getenv("DSE_RUNSHOTS"))) : 0;
    D3D12_RESOURCE_DESC od = g_output->GetDesc();
    UINT shotPitch = (UINT(od.Width) * 4 + 255) & ~255u;
    std::vector<ComPtr<ID3D12Resource>> shotBuffers;
    if (runShots)
        for (int i = 0; i < 16; i++)
            shotBuffers.push_back(CreateBuffer(UINT64(shotPitch) * od.Height, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
    uint32_t gameFrame = 0;
    auto present = [&](int shotIndex) {
        resizeIfNeeded();
        WaitFence(lastFence, event);
        palloc->Reset();
        plist->Reset(palloc.Get(), nullptr);
        UINT bb = g_swap->GetCurrentBackBufferIndex();
        BarrierOn(plist.Get(), g_output.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (shotIndex >= 0 && shotIndex < int(shotBuffers.size()))
        {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = g_output.Get();
            to.pResource = shotBuffers[shotIndex].Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, UINT(od.Width), od.Height, 1, shotPitch };
            plist->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        BarrierOn(plist.Get(), g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        BarrierOn(plist.Get(), g_backBuffers[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_RESOURCE_DESC bd = g_backBuffers[bb]->GetDesc();
        int x, y, w, h;
        VideoImageRect(int(bd.Width), int(bd.Height), x, y, w, h);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(bb);
        const float black[4] = { 0, 0, 0, 1 };
        plist->ClearRenderTargetView(rtv, black, 0, nullptr);
        plist->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ float(x), float(y), float(w), float(h), 0, 1 };
        D3D12_RECT sc{ x, y, x + w, y + h };
        ID3D12DescriptorHeap* heaps[] = { pheap.Get() };
        plist->SetDescriptorHeaps(1, heaps);
        plist->SetGraphicsRootSignature(g_rootSig.Get());
        plist->SetPipelineState(g_blitPso.Get());
        plist->SetGraphicsRootConstantBufferView(0, pconst->GetGPUVirtualAddress());
        plist->SetGraphicsRootDescriptorTable(1, pheap->GetGPUDescriptorHandleForHeapStart());
        plist->RSSetViewports(1, &vp);
        plist->RSSetScissorRects(1, &sc);
        plist->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        plist->DrawInstanced(3, 1, 0, 0);
        BarrierOn(plist.Get(), g_backBuffers[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        plist->Close();
        ID3D12CommandList* lists[] = { plist.Get() };
        submit(lists, 1);
        g_swap->Present(0, 0);
        presents++;
    };

    for (;;)
    {
        FrameRecord rec;
        {
            std::unique_lock lock(g_recLock);
            g_recCv.wait(lock, [] { return !g_recQueue.empty(); });
            rec = std::move(g_recQueue.front());
            g_recQueue.pop_front();
            g_presenterBusy = true;
        }

        // Game frame rate (smoothed), and how many runs this frame gets.
        if (lastTime)
        {
            double dt = double(rec.time.QuadPart - lastTime) / double(freq.QuadPart);
            if (dt > 0.008 && dt < 0.1)
                frameTime = frameTime * 0.9 + dt * 0.1;
        }
        lastTime = rec.time.QuadPart;

        // What moves: for each snapshot with a match in the previous frame, the words that
        // changed. Few matches means a cut (new scene, camera jump): no interpolation.
        std::vector<std::vector<LerpWord>> moves(rec.snaps.size());
        size_t matched = 0;
        for (size_t i = 0; i < rec.snaps.size(); i++)
        {
            // Same source; when it was drawn several times (props sharing a display list,
            // which the game may sort differently each frame), the one whose matrices are
            // closest, among those drawn around the same position in the order.
            auto it = prevBySource.find(rec.snaps[i].key);
            if (it == prevBySource.end())
                continue;
            uint32_t best = UINT32_MAX;
            float bestDistance = 0;
            for (uint32_t candidate : it->second)
            {
                if (it->second.size() > 64 &&
                    std::abs(int(prevOccurrence[candidate]) - int(rec.snaps[i].occurrence)) > 32)
                    continue;
                float d = MatrixDistance(prevWords[candidate], rec.snaps[i].words);
                if (best == UINT32_MAX || d < bestDistance)
                {
                    best = candidate;
                    bestDistance = d;
                }
            }
            if (best == UINT32_MAX)
                continue;
            matched++;
            const std::vector<uint32_t>& a = prevWords[best];
            const std::vector<uint32_t>& b = rec.snaps[i].words;
            for (uint32_t w = 0; w < b.size(); w++)
            {
                if (a[w] == b[w] || !Interpolated(w))
                    continue;
                float fa, fb;
                memcpy(&fa, &a[w], 4);
                memcpy(&fb, &b[w], 4);
                if (!std::isfinite(fa) || !std::isfinite(fb))
                    continue;
                moves[i].push_back({ w, fa, fb });
            }
            // Matrix by matrix: a big jump means the slot held another object in the previous
            // frame (matrix slots are reused). Those are not interpolated (they would flicker).
            std::vector<LerpWord>& mv = moves[i];
            size_t keep = 0;
            for (size_t j = 0; j < mv.size();)
            {
                uint32_t group = MatrixGroup(mv[j].index);
                size_t k = j;
                bool jump = false;
                for (; k < mv.size() && MatrixGroup(mv[k].index) == group; k++)
                {
                    float d = std::fabs(mv[k].to - mv[k].from);
                    bool translation = IsTranslation(mv[k].index);
                    if (translation ? d > 3.0f + 0.25f * std::fabs(mv[k].to) : d > 0.35f)
                        jump = true;
                }
                if (!jump)
                    for (size_t q = j; q < k; q++)
                        mv[keep++] = mv[q];
                else
                    jumpsDropped++;
                j = k;
            }
            mv.resize(keep);
        }
        bool cut = rec.snaps.empty() || matched * 10 < rec.snaps.size() * 6;
        frames++;
        cuts += cut;
        snapsTotal += rec.snaps.size();
        snapsMatched += matched;
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double secs = double(now.QuadPart - reportStart.QuadPart) / double(freq.QuadPart);
            if (secs >= 5.0)
            {
                LOG("[present] %.0f fps shown, %.1f game fps, %u cuts, %.0f%% matrices matched, GPU %.1f ms per run, "
                    "%u jumps not interpolated", presents / secs, frames / secs, cuts,
                    snapsTotal ? 100.0 * snapsMatched / snapsTotal : 0.0, timedRuns ? gpuMsTotal / timedRuns : 0.0,
                    jumpsDropped);
                jumpsDropped = 0;
                gpuMsTotal = 0;
                timedRuns = 0;
                reportStart = now;
                presents = frames = cuts = 0;
                snapsTotal = snapsMatched = 0;
            }
        }
        // Runs per game frame: the target rate over the game's own 30, as many as the GPU
        // can fit in a game frame (never more, or the game itself would slow down).
        int runs = 1;
        if (g_targetFps > 0 && !cut)
        {
            int wanted = std::max(1, int(std::lround(g_targetFps / 30.0)));
            int fit = gpuRunMs > 0 ? int((1000.0 / 30.0) * 0.85 / gpuRunMs) : wanted;
            runs = std::clamp(std::min(wanted, fit), 1, 16);
        }

        bool capture = runShots && (++gameFrame % runShots) == 0;
        std::vector<ID3D12CommandList*> ulists, lists;
        for (auto& l : rec.ulists)
            ulists.push_back(l.Get());
        for (auto& l : rec.lists)
            lists.push_back(l.Get());
        if (!ulists.empty())
            submit(ulists.data(), UINT(ulists.size()));

        for (int run = 1; run <= runs; run++)
        {
            // The next game frame is ready: go straight to the exact one.
            if (run < runs)
            {
                std::lock_guard lock(g_recLock);
                if (!g_recQueue.empty())
                    run = runs;
            }
            float t = float(run) / float(runs);
            WaitFence(lastFence, event);  // the previous run has read the matrices
            for (size_t i = 0; i < rec.snaps.size(); i++)
                for (const LerpWord& m : moves[i])
                {
                    float v = run == runs ? m.to : m.from + (m.to - m.from) * t;
                    memcpy(rec.snaps[i].dst + m.index * 4, &v, 4);
                }
            LARGE_INTEGER g0, g1;
            QueryPerformanceCounter(&g0);
            submit(lists.data(), UINT(lists.size()));
            WaitFence(lastFence, event);
            QueryPerformanceCounter(&g1);
            double runMs = double(g1.QuadPart - g0.QuadPart) * 1000.0 / double(freq.QuadPart);
            gpuMsTotal += runMs;
            gpuRunMs = gpuRunMs > 0 ? gpuRunMs * 0.9 + runMs * 0.1 : runMs;
            timedRuns++;
            present(capture ? run - 1 : -1);
            if (run < runs)
                SleepUntil(rec.time.QuadPart + LONGLONG(double(freq.QuadPart) * std::min(frameTime, 1.0 / 30.0) * run / runs));
        }

        if (capture)
        {
            WaitFence(lastFence, event);
            for (int r = 0; r < runs && r < int(shotBuffers.size()); r++)
            {
                void* data = nullptr;
                D3D12_RANGE all{ 0, SIZE_T(shotPitch) * od.Height };
                if (FAILED(shotBuffers[r]->Map(0, &all, &data)))
                    continue;
                char path[256];
                snprintf(path, sizeof(path), "G:/Dead Space Extraction PC/Recomp/logs/runs/f%05u_r%02d.bmp", gameFrame, r + 1);
                if (FILE* f = fopen(path, "wb"))
                {
                    BITMAPFILEHEADER fh{};
                    BITMAPINFOHEADER ih{};
                    ih.biSize = sizeof(ih);
                    ih.biWidth = LONG(od.Width);
                    ih.biHeight = -LONG(od.Height);
                    ih.biPlanes = 1;
                    ih.biBitCount = 32;
                    fh.bfType = 0x4D42;
                    fh.bfOffBits = sizeof(fh) + sizeof(ih);
                    fh.bfSize = fh.bfOffBits + DWORD(od.Width * 4 * od.Height);
                    fwrite(&fh, sizeof(fh), 1, f);
                    fwrite(&ih, sizeof(ih), 1, f);
                    std::vector<uint8_t> row(size_t(od.Width) * 4);
                    for (UINT y = 0; y < od.Height; y++)
                    {
                        const uint8_t* src = static_cast<const uint8_t*>(data) + size_t(y) * shotPitch;
                        for (size_t x = 0; x < row.size(); x += 4)
                        {
                            row[x] = src[x + 2];
                            row[x + 1] = src[x + 1];
                            row[x + 2] = src[x];
                            row[x + 3] = 255;
                        }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                }
                D3D12_RANGE none{ 0, 0 };
                shotBuffers[r]->Unmap(0, &none);
            }
        }

        // Keep this frame's matrices for the next one, and give its slots back.
        prevWords.clear();
        prevBySource.clear();
        prevOccurrence.clear();
        for (XfSnapshot& snap : rec.snaps)
        {
            prevBySource[snap.key].push_back(uint32_t(prevWords.size()));
            prevOccurrence.push_back(snap.occurrence);
            prevWords.push_back(std::move(snap.words));
        }
        {
            std::lock_guard lock(g_slotLock);
            for (int slot : rec.slots)
            {
                g_slots[slot].fence = lastFence;
                g_slots[slot].inUse = false;
            }
        }
        g_slotCv.notify_all();
        rec = FrameRecord{};
        g_presenterBusy = false;
    }
}
} // namespace

bool GxBackendActive() { return g_active; }

bool GxBackendInit(void* hwnd)
{
    if (getenv("DSE_NOD3D12"))
        return false;
    {
        int v = ConfigInt("escala", 3);  // dse.ini (DSE_ESCALA), or DSE_SCALE for older scripts
        if (const char* sc = getenv("DSE_SCALE"))
            v = atoi(sc);
        g_scale = UINT(v < 1 ? 1 : v > 6 ? 6 : v);
    }
    g_scaleX = float(g_scale) * std::max(1.0f, VideoAspect() / (4.0f / 3.0f));
    g_wideK = VideoWideFactor();
    g_efbW = (UINT(kEfbWidth * g_scaleX) + 3) & ~3u;
    g_efbH = kEfbHeight * g_scale;
    g_hwnd = (HWND)hwnd;
    if (getenv("DSE_D3DDEBUG"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
        return false;
    // The fastest GPU (with an integrated one too, the default adapter can be the slow one).
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice"))
        return false;
    {
        LUID luid = g_device->GetAdapterLuid();
        ComPtr<IDXGIAdapter1> used;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&used))) && SUCCEEDED(used->GetDesc1(&desc)))
        {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            LOG("[d3d12] GPU: %s", name);
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "command queue"))
        return false;

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(rc.right - rc.left);
    sd.Height = UINT(rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBuffers;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap1;
    if (!Check(factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &sd, nullptr, nullptr, &swap1), "swap chain"))
        return false;
    swap1.As(&g_swap);
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBackBuffers + 3;  // back buffers + EFB + EFB copy target + output
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsvHeap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for (FrameSlot& slot : g_slots)
        g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&slot.srvHeap));
    g_srvHeap = g_slots[0].srvHeap;
    hd.NumDescriptors = kTexSrvCapacity + 3;  // + white texture + EFB + EFB depth
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_texSrvHeap));
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < kBackBuffers; i++)
    {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
        g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, Rtv(i));
    }

    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 1 } };
    g_efb = CreateTexture(g_efbW, g_efbH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    D3D12_CLEAR_VALUE dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.DepthStencil.Depth = 1.0f;
    // Typeless so that EFB depth copies can read it (R24_UNORM_X8).
    g_depth = CreateTexture(g_efbW, g_efbH, DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv);
    if (!g_efb || !g_depth)
        return false;
    g_device->CreateRenderTargetView(g_efb.Get(), nullptr, Rtv(kBackBuffers));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_device->CreateDepthStencilView(g_depth.Get(), &dsvDesc, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    CreateSrv(g_efb.Get(), TexSrv(kTexSrvCapacity + 1));
    D3D12_SHADER_RESOURCE_VIEW_DESC zs{};
    zs.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    zs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    zs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    zs.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_depth.Get(), &zs, TexSrv(kTexSrvCapacity + 2));

    D3D12_RANGE none{ 0, 0 };
    for (FrameSlot& slot : g_slots)
    {
        slot.upload = CreateBuffer(kUploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.vram = CreateBuffer(kUploadSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!slot.upload || !slot.vram)
            return false;
        slot.upload->Map(0, &none, reinterpret_cast<void**>(&slot.uploadPtr));
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.alloc));
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.ualloc));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.alloc.Get(), nullptr, IID_PPV_ARGS(&slot.list));
        slot.list->Close();
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.ualloc.Get(), nullptr, IID_PPV_ARGS(&slot.ulist));
        slot.ulist->Close();
    }
    // The finished frame (what the XFB copy shows), copied to the back buffer to present.
    // At the internal resolution and the game's aspect ratio; the presenter scales it to the
    // window with black bars.
    UINT outH = 480 * g_scale, outW = UINT(outH * VideoAspect() + 0.5f) & ~1u;
    g_output = CreateTexture(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
    if (!g_output)
        return false;
    g_device->CreateRenderTargetView(g_output.Get(), nullptr, Rtv(kBackBuffers + 2));
    g_upload = g_slots[0].upload;
    g_uploadPtr = g_slots[0].uploadPtr;
    g_alloc = g_slots[0].alloc;
    g_readback = CreateBuffer(UINT64(g_efbW) * 4 * g_efbH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_readback)
        return false;

    g_list = g_slots[0].list;
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_guestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Root signature: b0 constants, t0-t7 textures, s0-s3 static samplers.
    D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3]{};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[4 + 8]{};
    for (UINT i = 0; i < 4; i++)
    {
        bool clamp = i & 1, point = i & 2;
        D3D12_TEXTURE_ADDRESS_MODE am = clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].Filter = point ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = am;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    const D3D12_TEXTURE_ADDRESS_MODE kWrap[2] = { D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP };
    // Hardware with resource binding tier 1 allows 16 samplers per shader: 4 + 8.
    for (UINT i = 0; i < 8; i++)
    {
        D3D12_STATIC_SAMPLER_DESC& d = samplers[4 + i];
        d.Filter = (i & 1) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = 16;
        d.AddressU = kWrap[(i >> 2) & 1];
        d.AddressV = kWrap[(i >> 1) & 1];
        d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        d.MaxLOD = D3D12_FLOAT32_MAX;
        d.ShaderRegister = 4 + i;
        d.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{ 3, params, 4 + 8, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ComPtr<ID3DBlob> rsBlob, err;
    if (!Check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err), "root signature"))
        return false;
    g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(kShaders, strlen(kShaders), "gx", nullptr, nullptr, entry, target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(hr))
            LOG("[d3d12] shader %s: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        return SUCCEEDED(hr);
    };
    if (!compile("vs_main", "vs_5_1", g_vs) || !compile("ps_main", "ps_5_1", g_ps) ||
        !compile("ps_main_z", "ps_5_1", g_psZ) ||
        !compile("vs_blit", "vs_5_1", g_vsBlit) || !compile("ps_blit", "ps_5_1", g_psBlit) ||
        !compile("ps_copy", "ps_5_1", g_psCopy))
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC bd{};
    bd.pRootSignature = g_rootSig.Get();
    bd.VS = { g_vsBlit->GetBufferPointer(), g_vsBlit->GetBufferSize() };
    bd.PS = { g_psBlit->GetBufferPointer(), g_psBlit->GetBufferSize() };
    bd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bd.NumRenderTargets = 1;
    bd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    bd.SampleDesc.Count = 1;
    bd.SampleMask = UINT_MAX;
    bd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    bd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    bd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (!Check(g_device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&g_blitPso)), "blit pipeline"))
        return false;
    bd.PS = { g_psCopy->GetBufferPointer(), g_psCopy->GetBufferSize() };
    if (!Check(g_device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&g_copyPso)), "copy pipeline"))
        return false;

    // White 1x1 texture for unused texture slots.
    g_white = CreateTexture(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    BeginFrame();
    {
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        UINT64 offset;
        auto* px = static_cast<uint32_t*>(Upload(256, 512, gpu, &offset));
        *px = 0xFFFFFFFF;
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = g_upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = offset;
        from.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256 };
        to.pResource = g_white.Get();
        g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(g_white.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CreateSrv(g_white.Get(), TexSrv(kTexSrvCapacity));
    }
    EndFrame();

    g_direct = false;
    g_targetFps = std::max(0, ConfigInt("fps", 0));
    CreateThread(nullptr, 0, Presenter, nullptr, 0, nullptr);
    if (g_targetFps > 0)
        LOG("[d3d12] frame interpolation: up to %d fps", g_targetFps);

    g_active = true;
    LOG("[d3d12] backend ready (%ux%u window, EFB %ux%u at %ux = %ux%u)", sd.Width, sd.Height, kEfbWidth, kEfbHeight,
        g_scale, g_efbW, g_efbH);
    return true;
}

// 21:9 / 32:9: the factor clip x is multiplied by for this draw (1 = untouched).
// - Full-width 3D views: widened (more field of view at the sides).
// - Full-width 2D (orthographic) draws: kept at their 16:9 size in the middle (the HUD and
//   menus), unless they span the whole width (fades, full-screen effects), which keep
//   covering it.
// - Smaller viewports (rendering into a texture): untouched.
float WideFactor(const GxVertex* v, uint32_t count)
{
    if (g_wideK >= 0.999f)
        return 1.0f;
    if (std::fabs(GxXfFloat(0x101A)) < 300.0f)
    {
        // Small views are rendered for 2D panels (the video call's 512x512 head), except the
        // 480x480 3D pre-pass whose copy the scene projects back from the camera: that one
        // gets the wider view too (the lookups are widened in the vertex shader).
        bool prePass = g_gx.xfReg[0x26] == 0 && std::fabs(GxXfFloat(0x101A) * 2 - 480.0f) < 1.0f &&
            std::fabs(GxXfFloat(0x101B) * 2 + 480.0f) < 1.0f;
        return prePass ? g_wideK : 1.0f;
    }
    if (g_gx.xfReg[0x26] == 0)
    {
        // Perspective: the world gets the wider view, except flat quads facing the camera that
        // already cover the whole 16:9 screen (tints, visor overlays placed just in front of
        // the camera): those must keep covering the whole screen.
        // The game's interface (menus, HUD, crosshair, screen tints) is made of planes fixed in
        // front of the camera (no rotation in the position matrix, ~750 units away, or centered).
        {
            uint32_t m = (v[0].idx[0] & 63) * 4;
            bool noRot = std::fabs(GxXfFloat(m + 1)) < 0.02f && std::fabs(GxXfFloat(m + 2)) < 0.02f &&
                std::fabs(GxXfFloat(m + 4)) < 0.02f && std::fabs(GxXfFloat(m + 6)) < 0.02f &&
                std::fabs(GxXfFloat(m + 8)) < 0.02f && std::fabs(GxXfFloat(m + 9)) < 0.02f;
            // Also centered planes closer in (the green screen tint is a center piece and two side
            // pieces at -85 that reach past the 16:9 edges).
            float tz = GxXfFloat(m + 11);
            bool centered = std::fabs(GxXfFloat(m + 3)) < 10.0f && std::fabs(GxXfFloat(m + 7)) < 10.0f;  // it sways a little
            if (noRot && (tz < -500.0f || (tz < -20.0f && centered)))
                return -g_wideK;  // the game's interface layer: see the vertex shader
        }
        if (count > 6)
            return g_wideK;
        float p0 = GxXfFloat(0x1020), p1 = GxXfFloat(0x1021);
        float lo = 1e9f, hi = -1e9f, zlo = 1e9f, zhi = -1e9f;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t m = (v[i].idx[0] & 63) * 4;
            float xv = GxXfFloat(m) * v[i].pos[0] + GxXfFloat(m + 1) * v[i].pos[1] + GxXfFloat(m + 2) * v[i].pos[2] +
                GxXfFloat(m + 3);
            float zv = GxXfFloat(m + 8) * v[i].pos[0] + GxXfFloat(m + 9) * v[i].pos[1] + GxXfFloat(m + 10) * v[i].pos[2] +
                GxXfFloat(m + 11);
            if (zv >= -1e-6f)
                return g_wideK;
            float cx = (p0 * xv + p1 * zv) / -zv;
            lo = std::min(lo, cx);
            hi = std::max(hi, cx);
            zlo = std::min(zlo, zv);
            zhi = std::max(zhi, zv);
        }
        bool flat = (zhi - zlo) <= 0.001f * std::fabs(zlo);
        return (flat && lo <= -0.97f && hi >= 0.97f) ? 1.0f : g_wideK;
    }
    // 2D draws that read an EFB copy are post-effect steps (downsampling a copy into a corner
    // of the EFB to copy it again): they work in EFB pixels, unlike the HUD.
    {
        uint32_t stages = ((g_gx.bp[0] >> 10) & 15) + 1;
        for (uint32_t st = 0; st < stages; st++)
        {
            uint32_t order = g_gx.bp[0x28 + st / 2] >> ((st & 1) * 12);
            if (!((order >> 6) & 1))
                continue;
            uint32_t map = order & 7;
            uint32_t a3 = g_gx.bp[(map < 4 ? 0x94 : 0xB4) + (map & 3)];
            if (g_efbCopies.count(((a3 & 0xFFFFFF) << 5) | 0x80000000))
                return 1.0f;
        }
    }
    // 2D passes with the viewport moved below the screen (center y not 240) are the game's
    // post effects (haze/bloom downsampled into a corner of the EFB, then copied back): they
    // work in EFB pixels and must not be squeezed like the HUD.
    if (std::fabs(GxXfFloat(0x101E) - 342.0f - 240.0f) > 1.0f)
        return 1.0f;
    float p0 = GxXfFloat(0x1020), p1 = GxXfFloat(0x1021);
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t m = (v[i].idx[0] & 63) * 4;
        float xv = GxXfFloat(m) * v[i].pos[0] + GxXfFloat(m + 1) * v[i].pos[1] + GxXfFloat(m + 2) * v[i].pos[2] +
            GxXfFloat(m + 3);
        float cx = p0 * xv + p1;
        lo = std::min(lo, cx);
        hi = std::max(hi, cx);
    }
    if (lo <= -0.97f && hi >= 0.97f)
    {
        // A whole-screen picture (videos, the safety screen, title cards): a big texture from
        // memory with color writes on. It keeps its 16:9 shape in the middle; fades, tints
        // (small textures) and depth-only passes keep covering the whole width.
        if ((g_gx.bp[0x41] >> 3) & 1)
        {
            uint32_t stages = ((g_gx.bp[0] >> 10) & 15) + 1;
            for (uint32_t st = 0; st < stages; st++)
            {
                uint32_t order = g_gx.bp[0x28 + st / 2] >> ((st & 1) * 12);
                if (!((order >> 6) & 1))
                    continue;
                uint32_t map = order & 7;
                uint32_t img = g_gx.bp[(map < 4 ? 0x88 : 0xA8) + (map & 3)];
                if ((img & 0x3FF) + 1 >= 256 || ((img >> 10) & 0x3FF) + 1 >= 256)
                    return g_wideK;
            }
        }
        return 1.0f;
    }
    return -g_wideK;  // 2D interface: see the vertex shader
}

void GxBackendDraw(uint32_t primitive, const GxVertex* v, uint32_t count)
{
    if (!g_active)
        return;
    ProfTimer pt(g_profDraw);
    BeginFrame();

    // Expand to a plain list.
    static std::vector<GxVertex> list;
    list.clear();
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    switch (primitive)
    {
    case kGxQuads:
    case kGxQuads2:
        for (uint32_t i = 0; i + 3 < count; i += 4)
            for (uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u })
                list.push_back(v[i + k]);
        break;
    case kGxTriangles:
        list.assign(v, v + count - count % 3);
        break;
    case kGxTriangleStrip:
        for (uint32_t i = 2; i < count; i++)
        {
            bool odd = i & 1;
            list.push_back(v[i - 2]);
            list.push_back(v[odd ? i : i - 1]);
            list.push_back(v[odd ? i - 1 : i]);
        }
        break;
    case kGxTriangleFan:
        for (uint32_t i = 2; i < count; i++)
        {
            list.push_back(v[0]);
            list.push_back(v[i - 1]);
            list.push_back(v[i]);
        }
        break;
    case kGxLines:
        list.assign(v, v + count - count % 2);
        topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case kGxLineStrip:
        for (uint32_t i = 1; i < count; i++)
        {
            list.push_back(v[i - 1]);
            list.push_back(v[i]);
        }
        topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    default:  // points
        list.assign(v, v + count);
        topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
    }
    if (list.empty())
        return;

    g_frameDrawCount++;
    {
        // DSE_QUADLOG=frame: every 2D draw of that frame (post effects): where, which texture.
        static const uint32_t quadFrom = getenv("DSE_QUADLOG") ? uint32_t(atoi(getenv("DSE_QUADLOG"))) : 0;
        // The first frame from quadFrom on in which the game copies depth (its post effects run).
        static uint32_t quadFrame = 0;
        if (quadFrom && !quadFrame && g_frame >= quadFrom && g_depthCopyFrame == g_frame)
        {
            quadFrame = g_frame + 1;
            LOG("[quad] logging frame %u", quadFrame);
        }
        if (quadFrame && g_frame == quadFrame && g_gx.xfReg[0x26] == 0 && count > 0 && count <= 8)
        {
            float f = WideFactor(v, count);
            LOG("[quad] persp #%u count %u factor %.2f cmode0 %04X zmode %02X tex0 0x%08X %ux%u stages %u", g_frameDrawCount, count, f,
                g_gx.bp[0x41] & 0xFFFF, g_gx.bp[0x40] & 0xFF, ((g_gx.bp[0x94] & 0xFFFFFF) << 5) | 0x80000000,
                (g_gx.bp[0x88] & 0x3FF) + 1, ((g_gx.bp[0x88] >> 10) & 0x3FF) + 1, ((g_gx.bp[0] >> 10) & 15) + 1);
        }
        if (quadFrame && g_frame == quadFrame && g_gx.xfReg[0x26] != 0 && count > 0)
        {
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t m = (v[i].idx[0] & 63) * 4;
                float xv = GxXfFloat(m) * v[i].pos[0] + GxXfFloat(m + 1) * v[i].pos[1] + GxXfFloat(m + 2) * v[i].pos[2] + GxXfFloat(m + 3);
                float yv = GxXfFloat(m + 4) * v[i].pos[0] + GxXfFloat(m + 5) * v[i].pos[1] + GxXfFloat(m + 6) * v[i].pos[2] + GxXfFloat(m + 7);
                float cx = GxXfFloat(0x1020) * xv + GxXfFloat(0x1021), cy = GxXfFloat(0x1022) * yv + GxXfFloat(0x1023);
                x0 = std::min(x0, cx); x1 = std::max(x1, cx); y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                u0 = std::min(u0, v[i].tex[0][0]); u1 = std::max(u1, v[i].tex[0][0]);
                t0 = std::min(t0, v[i].tex[0][1]); t1 = std::max(t1, v[i].tex[0][1]);
            }
            uint32_t img0 = g_gx.bp[0x88], img1 = g_gx.bp[0x89];
            LOG("[quad] factor %.2f", WideFactor(v, count));
            LOG("[quad] #%u clip x %.3f..%.3f y %.3f..%.3f vp %gx%g at %g,%g raw tc %.3f..%.3f %.3f..%.3f tex0 0x%08X %ux%u f%u "
                "tex1 0x%08X %ux%u texgen %08X mtx %08X cmode0 %04X stages %u order %06X",
                g_frameDrawCount, x0, x1, y0, y1, GxXfFloat(0x101A) * 2, -GxXfFloat(0x101B) * 2, GxXfFloat(0x101D) - 342,
                GxXfFloat(0x101E) - 342, u0, u1, t0, t1, ((g_gx.bp[0x94] & 0xFFFFFF) << 5) | 0x80000000, (img0 & 0x3FF) + 1,
                ((img0 >> 10) & 0x3FF) + 1, (img0 >> 20) & 15, ((g_gx.bp[0x95] & 0xFFFFFF) << 5) | 0x80000000, (img1 & 0x3FF) + 1,
                ((img1 >> 10) & 0x3FF) + 1, g_gx.xfReg[0x40], g_gx.cp[0x30], g_gx.bp[0x41] & 0xFFFF, ((g_gx.bp[0] >> 10) & 15) + 1,
                g_gx.bp[0x28] & 0xFFFFFF);
        }
    }
    uint32_t cull = (g_gx.bp[0x00] >> 14) & 3;
    if (cull == 3 && type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        return;  // cull all: only points and lines are drawn
    static const bool noZTex = getenv("DSE_NOZTEX") != nullptr;
    const bool zTexture = ((g_gx.bp[0xF5] >> 2) & 3) != 0 && !noZTex;
    ID3D12PipelineState* pso = GetPso(g_gx.bp[0x41], g_gx.bp[0x40], type, cull, zTexture);
    if (!pso)
        return;

    DrawConstants consts{};
    uint32_t textureMask = 0;
    {
        ProfTimer pf(g_profFill);
        FillConstants(consts, textureMask);
    }
    consts.proj1[3] = WideFactor(v, count);
    {
        static const uint32_t probeFrame = getenv("DSE_COPYPROBE") ? uint32_t(atoi(getenv("DSE_COPYPROBE"))) : 0;
        if (probeFrame && g_frame == probeFrame && g_tailDraws.size() < 400)
        {
            float x0 = 1e9f, x1 = -1e9f;
            bool ortho = g_gx.xfReg[0x26] != 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t m = (v[i].idx[0] & 63) * 4;
                float xv = GxXfFloat(m) * v[i].pos[0] + GxXfFloat(m + 1) * v[i].pos[1] + GxXfFloat(m + 2) * v[i].pos[2] + GxXfFloat(m + 3);
                float zv = GxXfFloat(m + 8) * v[i].pos[0] + GxXfFloat(m + 9) * v[i].pos[1] + GxXfFloat(m + 10) * v[i].pos[2] + GxXfFloat(m + 11);
                float cx = ortho ? GxXfFloat(0x1020) * xv + GxXfFloat(0x1021) : (zv < -1e-6f ? (GxXfFloat(0x1020) * xv + GxXfFloat(0x1021) * zv) / -zv : 0);
                x0 = std::min(x0, cx); x1 = std::max(x1, cx);
            }
            char line[300];
            snprintf(line, sizeof(line), "#%u %s count %u factor %.2f x %.2f..%.2f vp %gx%g@%g cmode0 %04X zmode %02X tex0 0x%08X %ux%u tex1 0x%08X stages %u texgen0 %08X scissor %06X %06X",
                g_frameDrawCount, ortho ? "2D" : "3D", count, consts.proj1[3], x0, x1, std::fabs(GxXfFloat(0x101A)) * 2,
                std::fabs(GxXfFloat(0x101B)) * 2, GxXfFloat(0x101D) - 342, g_gx.bp[0x41] & 0xFFFF, g_gx.bp[0x40] & 0xFF,
                ((g_gx.bp[0x94] & 0xFFFFFF) << 5) | 0x80000000, (g_gx.bp[0x88] & 0x3FF) + 1, ((g_gx.bp[0x88] >> 10) & 0x3FF) + 1,
                ((g_gx.bp[0x95] & 0xFFFFFF) << 5) | 0x80000000, ((g_gx.bp[0] >> 10) & 15) + 1, g_gx.xfReg[0x40],
                g_gx.bp[0x20] & 0xFFFFFF, g_gx.bp[0x21] & 0xFFFFFF);
            uint32_t mm = (v[0].idx[0] & 63) * 4;
            char mtx[200];
            snprintf(mtx, sizeof(mtx), " mtx %.2f %.2f %.2f %.1f | %.2f %.2f %.2f %.1f | %.2f %.2f %.2f %.1f", GxXfFloat(mm), GxXfFloat(mm + 1),
                GxXfFloat(mm + 2), GxXfFloat(mm + 3), GxXfFloat(mm + 4), GxXfFloat(mm + 5), GxXfFloat(mm + 6), GxXfFloat(mm + 7),
                GxXfFloat(mm + 8), GxXfFloat(mm + 9), GxXfFloat(mm + 10), GxXfFloat(mm + 11));
            g_tailDraws.push_back(std::string(line) + mtx);
        }
    }
    // DSE_GXSTATS=1: how many draws use GX features the backend does not do yet.
    static const bool gxStats = getenv("DSE_GXSTATS") != nullptr;
    if (gxStats)
    {
        static uint64_t n = 0, fog = 0, color1 = 0, bump = 0, ztex = 0, points = 0, lines = 0, logic = 0,
            dstAlpha = 0, cullFront = 0, cullBack = 0, cullAll = 0;
        n++;
        uint32_t genMode = g_gx.bp[0x00];
        uint32_t stages = ((genMode >> 10) & 15) + 1;
        bool c1 = false, b = false;
        for (uint32_t st = 0; st < stages; st++)
        {
            uint32_t order = g_gx.bp[0x28 + st / 2] >> ((st & 1) * 12);
            uint32_t chan = (order >> 7) & 7;
            c1 |= chan == 1;
            b |= chan == 5 || chan == 6;
        }
        color1 += c1;
        bump += b;
        fog += ((g_gx.bp[0xF1] >> 21) & 7) != 0;
        ztex += ((g_gx.bp[0xF5] >> 2) & 3) != 0;
        points += primitive == kGxPoints;
        lines += primitive == kGxLines || primitive == kGxLineStrip;
        logic += (g_gx.bp[0x41] >> 1) & 1;
        dstAlpha += (g_gx.bp[0x42] >> 8) & 1;
        uint32_t cull = (genMode >> 14) & 3;
        cullFront += cull == 1;
        cullBack += cull == 2;
        cullAll += cull == 3;
        if (n % 200000 == 0)
            LOG("[gxstats] %llu draws: fog %llu, color1 %llu, alpha bump %llu, z texture %llu, points %llu, lines %llu, "
                "logic op %llu, dst alpha %llu, cull front %llu back %llu all %llu", (unsigned long long)n,
                (unsigned long long)fog, (unsigned long long)color1, (unsigned long long)bump, (unsigned long long)ztex,
                (unsigned long long)points, (unsigned long long)lines, (unsigned long long)logic,
                (unsigned long long)dstAlpha, (unsigned long long)cullFront, (unsigned long long)cullBack,
                (unsigned long long)cullAll);
    }

    // The draw's identity, to match its matrices with the same draw in the previous frame:
    // where it comes from (gx.cpp) and how many times that source was drawn this frame.
    {
        uint32_t& n = g_keyCounts[g_gxDrawSource];
        g_drawKey = g_gxDrawSource;
        g_drawOccurrence = n++;
    }

    // Textures (may upload), then room for this draw's data, then 8 consecutive descriptors
    // in the shader-visible heap (reused while the textures stay the same).
    UINT srvs[8];
    for (uint32_t m = 0; m < 8; m++)
    {
        UINT srv = UINT_MAX;
        if ((textureMask >> m) & 1)
        {
            ProfTimer ptx(g_profTex);
            srv = TextureForMap(m);
        }
        // DSE_TEXWATCH=<hex address>: log draws that use that texture (first 4 vertices).
        static uint32_t watch = getenv("DSE_TEXWATCH") ? uint32_t(strtoul(getenv("DSE_TEXWATCH"), nullptr, 16)) : 0;
        if (watch && ((textureMask >> m) & 1))
        {
            uint32_t image3 = g_gx.bp[(m < 4 ? 0x94 : 0xB4) + (m & 3)];
            static int watchLogs = 0;
            if ((((image3 & 0xFFFFFF) << 5) | 0x80000000) == watch && watchLogs++ < 6)
            {
                LOG("[watch] map %u mode0 0x%X image0 0x%X texgen0 0x%X post0 0x%X dual %u count %u tevorder 0x%X",
                    m, g_gx.bp[(m < 4 ? 0x80 : 0xA0) + (m & 3)], g_gx.bp[(m < 4 ? 0x88 : 0xA8) + (m & 3)],
                    g_gx.xfReg[0x40], g_gx.xfReg[0x50], g_gx.xfReg[0x12], count, g_gx.bp[0x28]);
                uint32_t row = (g_gx.cp[0x30] >> 6) & 63;
                LOG("[watch]   matA 0x%X vcd 0x%X row %u: %g %g %g %g / %g %g %g %g", g_gx.cp[0x30], g_gx.cp[0x50], row,
                    GxXfFloat(row * 4), GxXfFloat(row * 4 + 1), GxXfFloat(row * 4 + 2), GxXfFloat(row * 4 + 3),
                    GxXfFloat(row * 4 + 4), GxXfFloat(row * 4 + 5), GxXfFloat(row * 4 + 6), GxXfFloat(row * 4 + 7));
                for (uint32_t k = 0; k < count && k < 4; k++)
                    LOG("[watch]   v%u pos %.3f %.3f %.3f row %u tc0 %.4f %.4f tc1 %.4f %.4f", k, v[k].pos[0], v[k].pos[1],
                        v[k].pos[2], v[k].idx[0], v[k].tex[0][0], v[k].tex[0][1], v[k].tex[1][0], v[k].tex[1][1]);
            }
        }
        srvs[m] = srv == UINT_MAX ? kTexSrvCapacity : srv;
    }
    UINT64 vbSize = UINT64(list.size()) * sizeof(GxVertex);
    EnsureUpload(vbSize + sizeof(consts) + kXfWords * 4 + 1024);
    if (g_srvOffset + 8 > kHeapSize)
    {
        EndFrame();
        BeginFrame();
    }
    static UINT lastSrvs[8];
    static UINT lastTableOffset = UINT_MAX;
    static uint32_t lastTableEpoch = UINT32_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    if (lastTableEpoch == g_uploadEpoch && memcmp(lastSrvs, srvs, sizeof(srvs)) == 0)
        tableGpu.ptr += UINT64(lastTableOffset) * g_srvSize;
    else
    {
        D3D12_CPU_DESCRIPTOR_HANDLE table = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        table.ptr += SIZE_T(g_srvOffset) * g_srvSize;
        tableGpu.ptr += UINT64(g_srvOffset) * g_srvSize;
        for (uint32_t m = 0; m < 8; m++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dst = table;
            dst.ptr += SIZE_T(m) * g_srvSize;
            g_device->CopyDescriptorsSimple(1, dst, TexSrv(srvs[m]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        memcpy(lastSrvs, srvs, sizeof(srvs));
        lastTableOffset = g_srvOffset;
        lastTableEpoch = g_uploadEpoch;
        g_srvOffset += 8;
    }

    // XF memory, then the constants (reused when they did not change), then the vertices.
    D3D12_GPU_VIRTUAL_ADDRESS vbGpu, cbGpu;
    D3D12_GPU_VIRTUAL_ADDRESS xfGpu = XfMemory();
    static DrawConstants lastConsts;
    static D3D12_GPU_VIRTUAL_ADDRESS lastCb = 0;
    static uint32_t lastCbEpoch = UINT32_MAX;
    if (lastCbEpoch == g_uploadEpoch && memcmp(&lastConsts, &consts, sizeof(consts)) == 0)
        cbGpu = lastCb;
    else
    {
        memcpy(Upload(sizeof(consts), 256, cbGpu), &consts, sizeof(consts));
        lastConsts = consts;
        lastCb = cbGpu;
        lastCbEpoch = g_uploadEpoch;
    }
    memcpy(Upload(vbSize, 16, vbGpu), list.data(), vbSize);

    // Scissor: BP 0x20 (top-left) and 0x21 (bottom-right), y in bits 0-10, x in 12-22,
    // both offset by 342.
    uint32_t tl = g_gx.bp[0x20], br = g_gx.bp[0x21];
    auto clampTo = [](int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; };
    D3D12_RECT sc{};
    sc.left = clampTo(int((tl >> 12) & 0x7FF) - 342, kEfbWidth);
    sc.top = clampTo(int(tl & 0x7FF) - 342, kEfbHeight);
    sc.right = clampTo(int((br >> 12) & 0x7FF) - 342 + 1, kEfbWidth);
    sc.bottom = clampTo(int(br & 0x7FF) - 342 + 1, kEfbHeight);
    if (sc.right <= sc.left || sc.bottom <= sc.top)
        sc = { 0, 0, LONG(kEfbWidth), LONG(kEfbHeight) };
    sc = { LONG(sc.left * g_scaleX), sc.top * LONG(g_scale), LONG(sc.right * g_scaleX), sc.bottom * LONG(g_scale) };
    D3D12_VIEWPORT vp{ 0, 0, float(g_efbW), float(g_efbH), 0, 1 };

    // Same state as the waiting draws and the vertices right after theirs: one draw.
    GxDrawState& st = g_drawState;
    if (st.valid && st.pending && st.pso == pso && st.table == tableGpu.ptr && st.xf == xfGpu && st.cb == cbGpu &&
        st.topo == topo && memcmp(&st.scissor, &sc, sizeof(sc)) == 0 &&
        vbGpu == st.vbStart + UINT64(st.vbCount) * sizeof(GxVertex))
    {
        st.vbCount += UINT(list.size());
        g_draws++;
        return;
    }
    FlushDraws();

    // Only what changed since the previous draw in this list (state changes cost GPU time,
    // and recorded frames may run several times).
    if (!st.valid)
    {
        g_list->SetGraphicsRootSignature(g_rootSig.Get());
        g_list->RSSetViewports(1, &vp);
        st = GxDrawState{};
        st.valid = true;
    }
    if (st.pso != pso)
        g_list->SetPipelineState(st.pso = pso);
    if (st.cb != cbGpu)
        g_list->SetGraphicsRootConstantBufferView(0, st.cb = cbGpu);
    if (st.table != tableGpu.ptr)
        g_list->SetGraphicsRootDescriptorTable(1, tableGpu), st.table = tableGpu.ptr;
    if (st.xf != xfGpu)
        g_list->SetGraphicsRootConstantBufferView(2, st.xf = xfGpu);
    if (memcmp(&st.scissor, &sc, sizeof(sc)) != 0)
        g_list->RSSetScissorRects(1, &sc), st.scissor = sc;
    if (st.topo != topo)
        g_list->IASetPrimitiveTopology(st.topo = topo);
    st.pending = true;
    st.vbStart = vbGpu;
    st.vbCount = UINT(list.size());
    g_draws++;
}

static void SaveShot()
{
    // EFB -> readback buffer -> logs/frame.bmp (640 x 528). The frame must have been run.
    WaitPresenterIdle();
    WaitGpu();
    void* data = nullptr;
    D3D12_RANGE all{ 0, SIZE_T(g_efbW) * 4 * g_efbH };
    if (FAILED(g_readback->Map(0, &all, &data)))
        return;
    std::vector<uint8_t> bgra(size_t(g_efbW) * 4 * g_efbH);
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bgra.size(); i += 4)
    {
        bgra[i] = src[i + 2];
        bgra[i + 1] = src[i + 1];
        bgra[i + 2] = src[i];
        bgra[i + 3] = 255;
    }
    D3D12_RANGE none{ 0, 0 };
    g_readback->Unmap(0, &none);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(g_efbW);
    ih.biHeight = -LONG(g_efbH);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(bgra.size());
    // DSE_SHOT=seq keeps every shot as logs/shots/frame_NNN.bmp (one per second).
    static bool seq = getenv("DSE_SHOT") && strcmp(getenv("DSE_SHOT"), "seq") == 0;
    static int shotIndex = 0;
    char path[256];
    if (seq)
        snprintf(path, sizeof(path), "G:/Dead Space Extraction PC/Recomp/logs/shots/frame_%03d.bmp", shotIndex++);
    else
        snprintf(path, sizeof(path), "G:/Dead Space Extraction PC/Recomp/logs/frame.bmp");
    if (FILE* f = fopen(path, "wb"))
    {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bgra.data(), 1, bgra.size(), f);
        fclose(f);
    }
}

void CaptureEfb(const char* name);
void SaveCaptures();
void GxBackendCopyToXfb(bool clear)
{
    if (!g_active)
        return;
    {
        // DSE_MEMSEARCH=text (debugging): at frame DSE_MEMSEARCH_FRAME (default 6000), find the
        // text in guest RAM (ASCII and UTF-16 big endian) and log what is around each match.
        static const char* needle = getenv("DSE_MEMSEARCH");
        static const uint32_t at = getenv("DSE_MEMSEARCH_FRAME") ? uint32_t(atoi(getenv("DSE_MEMSEARCH_FRAME"))) : 6000;
        if (needle && g_frame == at)
        {
            std::string n = needle;
            auto lower = [](uint8_t c) { return uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c); };
            int found = 0;
            for (uint32_t base : { 0x80000000u, 0x90000000u })
            {
                uint32_t size = base == 0x80000000u ? 0x01800000u : 0x04000000u;
                const uint8_t* m = g_base + base;
                for (uint32_t i = 0; i + n.size() * 2 < size && found < 60; i++)
                {
                    bool ascii = true, wide = true;
                    for (size_t k = 0; k < n.size() && (ascii || wide); k++)
                    {
                        uint8_t c = lower(uint8_t(n[k]));
                        ascii = ascii && lower(m[i + k]) == c;
                        wide = wide && m[i + 2 * k] == 0 && lower(m[i + 2 * k + 1]) == c;
                    }
                    if (!ascii && !wide)
                        continue;
                    // Print the whole string around it.
                    std::string text;
                    if (ascii)
                    {
                        uint32_t s0 = i;
                        while (s0 > 0 && i - s0 < 200 && m[s0 - 1] >= 32 && m[s0 - 1] < 127) s0--;
                        for (uint32_t k = s0; k < size && k < s0 + 300 && m[k] >= 32 && m[k] < 127; k++) text += char(m[k]);
                    }
                    else
                    {
                        uint32_t s0 = i;
                        while (s0 >= 2 && i - s0 < 400 && m[s0 - 2] == 0 && m[s0 - 1] >= 32) s0 -= 2;
                        for (uint32_t k = s0; k + 1 < size && k < s0 + 600 && m[k] == 0 && m[k + 1] >= 32; k += 2) text += char(m[k + 1]);
                    }
                    LOG("[memsearch] 0x%08X %s: %s", base + i, ascii ? "ascii" : "utf16", text.c_str());
                    found++;
                    i += uint32_t(n.size());
                }
            }
            LOG("[memsearch] %d matches for \"%s\" at frame %u", found, needle, g_frame);
        }
    }
    BeginFrame();
    FlushDraws();
    {
        static const uint32_t probeFrame = getenv("DSE_COPYPROBE") ? uint32_t(atoi(getenv("DSE_COPYPROBE"))) : 0;
        if (probeFrame && g_frame == probeFrame)
        {
            LOG("[copyprobe] final image, draw %u; draws since the last texture copy:", g_frameDrawCount);
            for (const std::string& l : g_tailDraws)
                LOG("[tail] %s", l.c_str());
            CaptureEfb("copyprobe_final");
        }
        if (probeFrame)
            SaveCaptures();
    }

    // Source rectangle (BP 0x49: x, y; 0x4A: width - 1, height - 1).
    uint32_t src = g_gx.bp[0x49], dim = g_gx.bp[0x4A];
    float x0 = float(src & 0x3FF), y0 = float((src >> 10) & 0x3FF);
    float w = float((dim & 0x3FF) + 1), h = float(((dim >> 10) & 0x3FF) + 1);
    float rect[4] = { x0 / kEfbWidth, y0 / kEfbHeight, (x0 + w) / kEfbWidth, (y0 + h) / kEfbHeight };

    static bool shots = getenv("DSE_SHOT") != nullptr;
    static const uint32_t every = getenv("DSE_SHOT_EVERY") ? std::max(1, atoi(getenv("DSE_SHOT_EVERY"))) : 60;
    g_frameDrawCount = 0;
    static const uint32_t shotFrom = getenv("DSE_SHOT_FROM") ? uint32_t(atoi(getenv("DSE_SHOT_FROM"))) : 0;
    bool shot = shots && (++g_frame % every) == 0 && g_frame >= shotFrom;
    if (!shots)
        g_frame++;

    Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_efbIsTarget = false;
    if (shot)
    {
        Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = g_efb.Get();
        to.pResource = g_readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, g_efbW, g_efbH, 1, g_efbW * 4 };
        g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    Barrier(g_output.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kBackBuffers + 2);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_RESOURCE_DESC bd = g_output->GetDesc();
    D3D12_VIEWPORT vp{ 0, 0, float(bd.Width), float(bd.Height), 0, 1 };
    D3D12_RECT sc{ 0, 0, LONG(bd.Width), LONG(bd.Height) };
    D3D12_GPU_VIRTUAL_ADDRESS cb;
    memcpy(Upload(256, 256, cb), rect, sizeof(rect));
    D3D12_CPU_DESCRIPTOR_HANDLE table = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    table.ptr += SIZE_T(g_srvOffset) * g_srvSize;
    D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    tableGpu.ptr += UINT64(g_srvOffset) * g_srvSize;
    for (UINT m = 0; m < 8; m++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = table;
        dst.ptr += SIZE_T(m) * g_srvSize;
        g_device->CopyDescriptorsSimple(1, dst, TexSrv(m == 0 ? kTexSrvCapacity + 1 : kTexSrvCapacity),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    g_srvOffset += 8;
    g_list->SetGraphicsRootSignature(g_rootSig.Get());
    g_drawState.valid = false;
    g_list->SetPipelineState(g_blitPso.Get());
    g_list->SetGraphicsRootConstantBufferView(0, cb);
    g_list->SetGraphicsRootDescriptorTable(1, tableGpu);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    Barrier(g_output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    EndFrame();
    QueryPerformanceCounter(&g_record.time);
    g_record.shot = shot;
    {
        std::lock_guard lock(g_recLock);
        g_recQueue.push_back(std::move(g_record));
    }
    g_recCv.notify_one();
    g_record = FrameRecord{};
    g_keyCounts.clear();
    if (shot)
        SaveShot();

    if (clear)
    {
        // Clear color (BP 0x4F: alpha/red, 0x50: green/blue) and depth (BP 0x51, 24-bit).
        g_clearColor[0] = float(g_gx.bp[0x4F] & 0xFF) / 255.0f;
        g_clearColor[3] = float((g_gx.bp[0x4F] >> 8) & 0xFF) / 255.0f;
        g_clearColor[1] = float((g_gx.bp[0x50] >> 8) & 0xFF) / 255.0f;
        g_clearColor[2] = float(g_gx.bp[0x50] & 0xFF) / 255.0f;
        g_clearDepth = float(g_gx.bp[0x51] & 0xFFFFFF) / 16777215.0f;
        g_pendingClear = true;
    }
    static bool prof = getenv("DSE_PROF") != nullptr;
    if (prof && g_frame % 100 == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto ms = [&](uint64_t& v) { double r = double(v) * 1000.0 / double(f.QuadPart) / 100.0; v = 0; return r; };
        double decode = ms(g_profDecode);
        double draw = ms(g_profDraw), tex = ms(g_profTex), fill = ms(g_profFill), wait = ms(g_profWait), copy = ms(g_profCopy);
        static uint64_t lastDraws = 0, lastCalls = 0;
        LOG("[prof] XF snapshots per frame: %.0f, GX draws %.0f -> D3D12 draws %.0f", double(g_profSnapshots) / 100.0,
            double(g_draws - lastDraws) / 100.0, double(g_drawCalls - lastCalls) / 100.0);
        lastDraws = g_draws;
        lastCalls = g_drawCalls;
        g_profSnapshots = 0;
        LOG("[prof] per frame: vertex decode %.2f ms, draw %.2f ms (textures %.2f, constants %.2f), GPU wait %.2f ms, EFB copies %.2f ms",
            decode, draw, tex, fill, wait, copy);
    }
    if (g_frame == 1 || g_frame % 600 == 0)
        LOG("[d3d12] frame %u, %llu draws so far, %zu textures cached", g_frame, (unsigned long long)g_draws, g_textures.size());
}

void GxBackendClear()
{
    if (!g_active)
        return;
    g_clearColor[0] = float(g_gx.bp[0x4F] & 0xFF) / 255.0f;
    g_clearColor[3] = float((g_gx.bp[0x4F] >> 8) & 0xFF) / 255.0f;
    g_clearColor[1] = float((g_gx.bp[0x50] >> 8) & 0xFF) / 255.0f;
    g_clearColor[2] = float(g_gx.bp[0x50] & 0xFF) / 255.0f;
    g_clearDepth = float(g_gx.bp[0x51] & 0xFFFFFF) / 16777215.0f;
    // Only the copied rectangle is cleared (BP 0x49/0x4A), like the hardware.
    BeginFrame();
    FlushDraws();
    uint32_t src = g_gx.bp[0x49], dim = g_gx.bp[0x4A];
    D3D12_RECT r;
    r.left = LONG(src & 0x3FF);
    r.top = LONG((src >> 10) & 0x3FF);
    r.right = std::min<LONG>(r.left + LONG(dim & 0x3FF) + 1, kEfbWidth);
    r.bottom = std::min<LONG>(r.top + LONG((dim >> 10) & 0x3FF) + 1, kEfbHeight);
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    r = { LONG(r.left * g_scaleX), r.top * LONG(g_scale), LONG(r.right * g_scaleX), r.bottom * LONG(g_scale) };
    g_list->ClearRenderTargetView(Rtv(kBackBuffers), g_clearColor, 1, &r);
    g_list->ClearDepthStencilView(g_dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH,
        g_clearDepth, 0, 1, &r);
}

// DSE_COPYPROBE=frame: in that game frame, save the EFB (half size) before every texture copy
// as logs/copyprobe_NN.bmp, with the copy's details in the log (debugging the post effects).
struct EfbCapture { ComPtr<ID3D12Resource> buf; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t frame = 0; char name[64]; };
std::vector<EfbCapture> g_captures;
void CaptureEfb(const char* name)
{
    EfbCapture c;
    D3D12_RESOURCE_DESC cd = g_efb->GetDesc();
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&cd, 0, 1, 0, &c.fp, nullptr, nullptr, &total);
    c.buf = CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = g_efb.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = c.buf.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = c.fp;
    g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.frame = g_frame;
    snprintf(c.name, sizeof(c.name), "%s", name);
    g_captures.push_back(c);
}
void SaveCaptures()
{
    for (size_t i = 0; i < g_captures.size();)
    {
        EfbCapture& c = g_captures[i];
        if (g_frame < c.frame + 10) { i++; continue; }
        void* data = nullptr;
        if (SUCCEEDED(c.buf->Map(0, nullptr, &data)))
        {
            uint32_t w = c.fp.Footprint.Width / 2, h = c.fp.Footprint.Height / 2;
            std::vector<uint8_t> bgr(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    const uint8_t* px = (const uint8_t*)data + size_t(y * 2) * c.fp.Footprint.RowPitch + x * 8;
                    uint8_t* o = &bgr[(size_t(y) * w + x) * 4];
                    o[0] = px[2]; o[1] = px[1]; o[2] = px[0]; o[3] = 255;
                }
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih); ih.biWidth = LONG(w); ih.biHeight = -LONG(h); ih.biPlanes = 1; ih.biBitCount = 32;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + DWORD(bgr.size());
            char path[256];
            snprintf(path, sizeof(path), "G:/Dead Space Extraction PC/Recomp/logs/%s.bmp", c.name);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(bgr.data(), 1, bgr.size(), f);
                fclose(f);
            }
            D3D12_RANGE none{ 0, 0 };
            c.buf->Unmap(0, &none);
        }
        g_captures.erase(g_captures.begin() + i);
    }
}

// EFB -> texture copy (PE_COPY_EXECUTE without the XFB bit). `value` is the BP 0x52 word:
// bits 3-6 target format, bit 9 half scale (2x2 box filter), bit 15 intensity (YUV luma).
void GxBackendCopyToTexture(uint32_t value)
{
    if (!g_active)
        return;
    ProfTimer pt(g_profCopy);
    BeginFrame();
    FlushDraws();

    uint32_t src = g_gx.bp[0x49], dim = g_gx.bp[0x4A];
    uint32_t x0 = src & 0x3FF, y0 = (src >> 10) & 0x3FF;
    uint32_t w = (dim & 0x3FF) + 1, h = ((dim >> 10) & 0x3FF) + 1;
    bool half = (value >> 9) & 1;
    uint32_t tw = half ? (w + 1) / 2 : w, th = half ? (h + 1) / 2 : h;
    uint32_t fmt = ((value >> 4) & 7) | ((value >> 3) & 1) << 3;
    bool intensity = (value >> 15) & 1;
    uint32_t addr = ((g_gx.bp[0x4B] & 0xFFFFFF) << 5) | 0x80000000;
    {
        static const uint32_t probeFrame = getenv("DSE_COPYPROBE") ? uint32_t(atoi(getenv("DSE_COPYPROBE"))) : 0;
        static uint32_t n = 0;
        if (probeFrame)
        {
            SaveCaptures();
            if (g_frame == probeFrame && n < 40)
            {
                char name[64];
                snprintf(name, sizeof(name), "copyprobe_%02u", n);
                LOG("[copyprobe] %02u before copy %ux%u at (%u,%u) -> 0x%08X fmt %u%s pixfmt %u value %06X draw %u", n, w, h, x0, y0,
                    addr, fmt, half ? " half" : "", g_gx.bp[0x43] & 7, value & 0xFFFFFF, g_frameDrawCount);
                CaptureEfb(name);
                n++;
                g_tailDraws.clear();
            }
        }
    }
    if ((g_gx.bp[0x43] & 7) == 3)
    {
        static bool logged = false;
        if (!logged)
            LOG("[d3d12] EFB depth copy to 0x%08X", addr);
        logged = true;
    }

    static std::unordered_map<uint64_t, bool> seen;   // log each kind of copy once
    uint64_t kind = uint64_t(addr) | uint64_t(w) << 32 | uint64_t(h) << 42 | uint64_t(fmt) << 52 |
        uint64_t(half) << 56 | uint64_t(intensity) << 57;
    if (seen.size() < 64 && !seen[kind])
    {
        seen[kind] = true;
        LOG("[d3d12] EFB copy %ux%u at (%u,%u) -> 0x%08X fmt %u%s%s", w, h, x0, y0, addr, fmt,
            half ? " half" : "", intensity ? " intensity" : "");
    }
    // Size in memory: the texture format the copy format is read back as.
    static const uint32_t kSizeFormat[16] = { kTexI4, kTexI8, kTexI8, kTexIA8, kTexRGB565, kTexRGB5A3, kTexRGBA8,
        kTexI8, kTexI8, kTexI8, kTexI8, kTexIA8, kTexIA8, kTexI8, kTexI8, kTexI8 };
    uint32_t bytes = GxTextureSize(tw, th, kSizeFormat[fmt]);
    if (bytes == 0 || addr + bytes < addr)
        return;

    // Conversion constants: color pass-through, or intensity/alpha weights over (r, g, b, a).
    struct { float rect[4], wI[4], wA[4], params[4]; } k{};
    k.rect[0] = float(x0) / kEfbWidth;
    k.rect[1] = float(y0) / kEfbHeight;
    k.rect[2] = float(x0 + w) / kEfbWidth;
    k.rect[3] = float(y0 + h) / kEfbHeight;
    auto set = [](float* d, float r, float g, float b, float a) { d[0] = r; d[1] = g; d[2] = b; d[3] = a; };
    const float luma[3] = { 0.257f, 0.504f, 0.098f };
    k.params[0] = 1;
    if (intensity && fmt <= 3)
    {
        set(k.wI, luma[0], luma[1], luma[2], 0);
        k.params[1] = 16.0f / 255.0f;
        if (fmt <= 1)
        {
            set(k.wA, luma[0], luma[1], luma[2], 0);
            k.params[2] = 16.0f / 255.0f;
        }
        else
            set(k.wA, 0, 0, 0, 1);
    }
    else
    {
        switch (fmt)
        {
        case 0: case 1: case 8: set(k.wI, 1, 0, 0, 0); set(k.wA, 1, 0, 0, 0); break;   // R4, R8
        case 2: case 3: set(k.wI, 1, 0, 0, 0); set(k.wA, 0, 0, 0, 1); break;           // RA4, RA8
        case 7: set(k.wI, 0, 0, 0, 1); set(k.wA, 0, 0, 0, 1); break;                    // A8
        case 9: set(k.wI, 0, 1, 0, 0); set(k.wA, 0, 1, 0, 0); break;                    // G8
        case 10: set(k.wI, 0, 0, 1, 0); set(k.wA, 0, 0, 1, 0); break;                   // B8
        case 11: set(k.wI, 0, 1, 0, 0); set(k.wA, 1, 0, 0, 0); break;                   // RG8
        case 12: set(k.wI, 0, 0, 1, 0); set(k.wA, 0, 1, 0, 0); break;                   // GB8
        default: k.params[0] = 0; k.params[3] = fmt == 4 ? 1.0f : 0.0f; break;          // RGB565/5A3/RGBA8
        }
    }
    // Pixel format Z24: the copy reads the depth buffer (depth of field masks, etc.).
    static const bool noZCopy = getenv("DSE_NOZCOPY") != nullptr;
    const bool depthCopy = (g_gx.bp[0x43] & 7) == 3 && !noZCopy;
    if (depthCopy)
    {
        k.params[3] = 2.0f;
        g_depthCopyFrame = g_frame;
    }

    // Target texture (reused while the size stays the same).
    // Copy textures at the console's size by default: the game's effects (blur chains,
    // feedback) are tuned for it. DSE_COPYSCALE=1 makes them follow the internal resolution.
    static const UINT cs = getenv("DSE_COPYSCALE") ? g_scale : 1;
    EfbCopy& e = g_efbCopies[addr];
    if (!e.res || e.width != tw || e.height != th)
    {
        EfbCopy::Target* t = nullptr;
        for (EfbCopy::Target& c : e.pool)
            if (c.width == tw * cs && c.height == th * cs)
                t = &c;
        bool created = false;
        if (!t)
        {
            if (g_texSrvCount >= kTexSrvCapacity)
                return;
            ComPtr<ID3D12Resource> res = CreateTexture(tw * cs, th * cs, DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
            if (!res)
                return;
            UINT srv = g_texSrvCount++;
            CreateSrv(res.Get(), TexSrv(srv));
            e.pool.push_back({ tw * cs, th * cs, res, srv });
            t = &e.pool.back();
            created = true;
        }
        e.res = t->res;
        e.srv = t->srv;
        e.width = tw;
        e.height = th;
        (void)created;
    }
    // Always from shader resource: recorded lists may run more than once.
    Barrier(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (depthCopy)
        Barrier(g_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kBackBuffers + 1);
    g_device->CreateRenderTargetView(e.res.Get(), nullptr, rtv);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_GPU_VIRTUAL_ADDRESS cb;
    memcpy(Upload(256, 256, cb), &k, sizeof(k));
    D3D12_CPU_DESCRIPTOR_HANDLE table = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    table.ptr += SIZE_T(g_srvOffset) * g_srvSize;
    D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    tableGpu.ptr += UINT64(g_srvOffset) * g_srvSize;
    for (UINT m = 0; m < 8; m++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = table;
        dst.ptr += SIZE_T(m) * g_srvSize;
        g_device->CopyDescriptorsSimple(1, dst,
            TexSrv(m == 0 ? (depthCopy ? kTexSrvCapacity + 2 : kTexSrvCapacity + 1) : kTexSrvCapacity),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    g_srvOffset += 8;
    D3D12_VIEWPORT vp{ 0, 0, float(tw * cs), float(th * cs), 0, 1 };
    D3D12_RECT sc{ 0, 0, LONG(tw * cs), LONG(th * cs) };
    g_list->SetGraphicsRootSignature(g_rootSig.Get());
    g_drawState.valid = false;
    g_list->SetPipelineState(g_copyPso.Get());
    g_list->SetGraphicsRootConstantBufferView(0, cb);
    g_list->SetGraphicsRootDescriptorTable(1, tableGpu);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    Barrier(e.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);


    // Back to drawing into the EFB.
    Barrier(g_efb.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (depthCopy)
        Barrier(g_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE efbRtv = Rtv(kBackBuffers);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &efbRtv, FALSE, &dsv);

    // What the copy "wrote" to memory: a pattern no other data has.
    g_copySerial++;
    uint8_t* dst = g_base + addr;
    // Only the words SampleHash reads get the pattern.
    uint32_t step = bytes > 64 * 64 ? bytes / 64 & ~7u : 8;
    for (uint32_t i = 0; i + 8 <= bytes; i += step)
    {
        uint64_t v = (uint64_t(g_copySerial) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(i) * 0x85EBCA6Bull) ^ 0xEFBC0B1Eull;
        memcpy(dst + i, &v, 8);
    }
    e.bytes = bytes;
    e.serial = g_copySerial;
    e.hash = SampleHash(dst, bytes);
    e.checkedFrame = g_frame;
    e.valid = true;
}
