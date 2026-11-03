// Software rasterizer: EDRAM, draws and resolves. See xenos_raster.h.
//
// EDRAM: 2048 tiles of 80x16 32-bit samples (10 MiB). A render target is
// addressed by its base tile, the surface pitch (pixels) and the MSAA mode:
// 2x stacks two samples vertically per pixel, 4x uses 2x2. Each pixel is
// shaded once and written to all of its samples. Rules follow the hardware
// as documented by Xenia (BSD-3-Clause).
#include "../dev_env.h"
#include "xenos_raster.h"
#include "xenos_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace xenos {
namespace {

constexpr uint32_t kTiles = 2048, kTileW = 80, kTileH = 16;
std::vector<uint32_t> g_edram(size_t(kTiles) * kTileW * kTileH, 0);
RasterStats g_stats;
// HC09_PICK=x,y: note every draw that writes this screen pixel (window offset
// removed, so tiled passes report frame coordinates), with the colour written.
int32_t g_pickX = -1, g_pickY = -1;
uint32_t g_drawNumber = 0, g_pickVertexDraw = ~0u, g_pickVertexIndex = ~0u;

inline float AsF(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }
inline float Clamp01(float f) { return f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f); }

struct Surface
{
    uint32_t base, pitch, msaa;   // msaa: 0 1x, 1 2x, 2 4x
    bool wide;                    // 64 bpp: two dwords per sample, so 40 samples per tile row
    uint32_t sw() const { return msaa == 2 ? 2 : 1; }
    uint32_t sh() const { return msaa >= 1 ? 2 : 1; }
    // EDRAM dword index of sample (s) of pixel (x, y); a 64-bpp sample also uses the next dword.
    // A 64-bpp surface takes twice the tiles per row of the same pitch at 32 bpp.
    uint32_t Index(uint32_t x, uint32_t y, uint32_t s) const
    {
        uint32_t sx = x * sw() + (msaa == 2 ? (s & 1) : 0);
        uint32_t sy = y * sh() + (msaa == 2 ? (s >> 1) : (msaa == 1 ? (s & 1) : 0));
        uint32_t pitchTiles = (pitch * sw() + kTileW - 1) / kTileW * (wide ? 2 : 1);
        uint32_t tw = wide ? kTileW / 2 : kTileW;
        uint32_t tile = (base + (sy / kTileH) * pitchTiles + sx / tw) % kTiles;
        return tile * kTileW * kTileH + (sy % kTileH) * kTileW + (sx % tw) * (wide ? 2 : 1);
    }
    uint32_t Samples() const { return msaa == 2 ? 4 : msaa == 1 ? 2 : 1; }
};

// ---- render target formats --------------------------------------------------------
// RB_COLOR_INFO.color_format: 0 8888, 1 8888_GAMMA, 2 2_10_10_10, 3 2_10_10_10_FLOAT (7e3),
// 4 16_16 / 5 16_16_16_16 (fixed point -32..32), 6 16_16_FLOAT, 7 16_16_16_16_FLOAT,
// 10 2_10_10_10_AS_10_10_10_10, 12 2_10_10_10_FLOAT_AS_16_16_16_16, 14 32_FLOAT, 15 32_32_FLOAT.
// Layouts in EDRAM put R in the low bits (64 bpp: R,G in the first dword).
const uint32_t kColorInfoRegs[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
bool Is64bpp(uint32_t fmt) { return fmt == 5 || fmt == 7 || fmt == 15; }

Surface ColorSurface(uint32_t rt)
{
    uint32_t si = Reg(reg::RB_SURFACE_INFO), ci = Reg(kColorInfoRegs[rt & 3]);
    return Surface{ ci & 0xFFF, si & 0x3FFF, (si >> 16) & 3, Is64bpp((ci >> 16) & 0xF) };
}
Surface DepthSurface()
{
    uint32_t si = Reg(reg::RB_SURFACE_INFO), di = Reg(reg::RB_DEPTH_INFO);
    return Surface{ di & 0xFFF, si & 0x3FFF, (si >> 16) & 3, false };
}

// 7e3: 3-bit exponent (bias 3), 7-bit mantissa, no sign; [0, 31.875].
float Float7e3(uint32_t v)
{
    uint32_t m = v & 0x7F, e = (v >> 7) & 7;
    return e ? std::ldexp(float(m | 0x80), int(e) - 10) : std::ldexp(float(m), -9);
}
uint32_t To7e3(float f)
{
    if (!(f > 0.0f)) return 0;
    if (f >= 31.875f) return 0x3FF;
    if (f < 0.25f) return uint32_t(std::lround(f * 512.0f));   // denormal; 128 rounds into exponent 1
    int e;
    float m = std::frexp(f, &e);                                // f = m * 2^e, m in [0.5, 1)
    uint32_t mant = uint32_t(std::lround((m * 2.0f - 1.0f) * 128.0f));
    uint32_t exp = uint32_t(e - 1 + 3);
    if (mant == 128) { mant = 0; ++exp; }
    return exp > 7 ? 0x3FF : (exp << 7) | mant;
}
// Piecewise-linear gamma of 8888_GAMMA (4 segments over 10-bit linear).
float PwlToLinear(float g)
{
    float v = g * 255.0f;
    float l = v < 64.0f ? v : v < 96.0f ? v * 2.0f - 64.0f : v < 192.0f ? v * 4.0f - 256.0f : v * 8.0f - 1024.0f;
    return std::min(1.0f, l / 1023.0f);
}
float LinearToPwl(float l)
{
    float v = Clamp01(l) * 1023.0f;
    float g = v < 64.0f ? std::trunc(v) : v < 128.0f ? std::trunc(v / 2.0f) + 32.0f
            : v < 512.0f ? std::trunc(v / 4.0f) + 64.0f : std::trunc(v / 8.0f) + 128.0f;
    return g / 255.0f;
}
float Fixed16(uint32_t v) { return std::max(-1.0f, float(int16_t(v & 0xFFFF)) / 32767.0f) * 32.0f; }
uint32_t ToFixed16(float f) { return uint32_t(int32_t(std::lround(std::min(32.0f, std::max(-32.0f, f)) / 32.0f * 32767.0f))) & 0xFFFF; }
uint32_t Unorm(float f, int bits) { return uint32_t(std::lround(Clamp01(f) * float((1u << bits) - 1))); }

// Sample -> RGBA. `linear`: 8888_GAMMA converted to linear (for blending); raw otherwise (resolves).
void DecodeRt(uint32_t fmt, const uint32_t* p, float c[4], bool linear)
{
    uint32_t v = p[0];
    float f;
    switch (fmt)
    {
    case 0: case 1:
        for (int i = 0; i < 4; ++i) c[i] = float((v >> (8 * i)) & 0xFF) / 255.0f;
        if (fmt == 1 && linear) for (int i = 0; i < 3; ++i) c[i] = PwlToLinear(c[i]);
        break;
    case 2: case 10:
        for (int i = 0; i < 3; ++i) c[i] = float((v >> (10 * i)) & 0x3FF) / 1023.0f;
        c[3] = float(v >> 30) / 3.0f;
        break;
    case 3: case 12:
        for (int i = 0; i < 3; ++i) c[i] = Float7e3(v >> (10 * i));
        c[3] = float(v >> 30) / 3.0f;
        break;
    case 4: c[0] = Fixed16(v); c[1] = Fixed16(v >> 16); c[2] = 0; c[3] = 1; break;
    case 5: c[0] = Fixed16(v); c[1] = Fixed16(v >> 16); c[2] = Fixed16(p[1]); c[3] = Fixed16(p[1] >> 16); break;
    case 6: c[0] = HalfToFloat(v & 0xFFFF); c[1] = HalfToFloat(v >> 16); c[2] = 0; c[3] = 1; break;
    case 7: c[0] = HalfToFloat(v & 0xFFFF); c[1] = HalfToFloat(v >> 16); c[2] = HalfToFloat(p[1] & 0xFFFF); c[3] = HalfToFloat(p[1] >> 16); break;
    case 14: memcpy(&f, &v, 4); c[0] = f; c[1] = 0; c[2] = 0; c[3] = 1; break;
    case 15: memcpy(&c[0], &p[0], 4); memcpy(&c[1], &p[1], 4); c[2] = 0; c[3] = 1; break;
    default: c[0] = c[1] = c[2] = 0; c[3] = 1; break;
    }
}
void EncodeRt(uint32_t fmt, const float c[4], uint32_t* p, bool linear)
{
    switch (fmt)
    {
    case 0: case 1:
    {
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i)
        {
            float v = (fmt == 1 && linear && i < 3) ? LinearToPwl(c[i]) : c[i];
            r |= Unorm(v, 8) << (8 * i);
        }
        p[0] = r;
        break;
    }
    case 2: case 10: p[0] = Unorm(c[0], 10) | (Unorm(c[1], 10) << 10) | (Unorm(c[2], 10) << 20) | (Unorm(c[3], 2) << 30); break;
    case 3: case 12: p[0] = To7e3(c[0]) | (To7e3(c[1]) << 10) | (To7e3(c[2]) << 20) | (Unorm(c[3], 2) << 30); break;
    case 4: p[0] = ToFixed16(c[0]) | (ToFixed16(c[1]) << 16); break;
    case 5: p[0] = ToFixed16(c[0]) | (ToFixed16(c[1]) << 16); p[1] = ToFixed16(c[2]) | (ToFixed16(c[3]) << 16); break;
    case 6: p[0] = FloatToHalf(c[0]) | (FloatToHalf(c[1]) << 16); break;
    case 7: p[0] = FloatToHalf(c[0]) | (FloatToHalf(c[1]) << 16); p[1] = FloatToHalf(c[2]) | (FloatToHalf(c[3]) << 16); break;
    case 14: memcpy(&p[0], &c[0], 4); break;
    case 15: memcpy(&p[0], &c[0], 4); memcpy(&p[1], &c[1], 4); break;
    default: p[0] = 0; break;
    }
}
// Range of a format's components: shader output and blend results are clamped to it.
void ClampRt(uint32_t fmt, float c[4])
{
    switch (fmt)
    {
    case 0: case 1: case 2: case 10: for (int i = 0; i < 4; ++i) c[i] = Clamp01(c[i]); break;
    case 3: case 12: for (int i = 0; i < 3; ++i) c[i] = std::min(31.875f, std::max(0.0f, c[i])); c[3] = Clamp01(c[3]); break;
    case 4: case 5: for (int i = 0; i < 4; ++i) c[i] = std::min(32.0f, std::max(-32.0f, c[i])); break;
    default: break;   // floating point
    }
}

// Depth: D24S8 (unorm) or D24FS8 (20e4 float: 4-bit exponent bias 15, 20-bit
// mantissa, [0, 2)), in the high 24 bits of the sample.
float DecodeDepth(uint32_t d24, bool isFloat)
{
    if (!isFloat) return float(d24) / 16777215.0f;
    uint32_t m = d24 & 0xFFFFF, e = d24 >> 20;
    return e ? std::ldexp(float(m | 0x100000), int(e) - 35) : std::ldexp(float(m), -34);
}
uint32_t EncodeDepth(float z, bool isFloat)
{
    z = Clamp01(z);
    if (!isFloat) return uint32_t(z * 16777215.0f);
    if (z < std::ldexp(1.0f, -14)) return uint32_t(std::lround(std::ldexp(z, 34)));
    int e;
    float m = std::frexp(z, &e);
    uint32_t mant = uint32_t(std::lround((m * 2.0f - 1.0f) * 1048576.0f));
    uint32_t exp = uint32_t(e - 1 + 15);
    if (mant == 1048576) { mant = 0; ++exp; }
    return exp > 15 ? 0xFFFFFF : (exp << 20) | mant;
}

// RGBA8 texel (R in the low byte) -> floats.
void UnpackColor(uint32_t v, float c[4]) { for (int i = 0; i < 4; ++i) c[i] = float((v >> (8 * i)) & 0xFF) / 255.0f; }

bool Compare(uint32_t func, float a, float b)
{
    switch (func)
    {
    case 0: return false; case 1: return a < b; case 2: return a == b; case 3: return a <= b;
    case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return true;
    }
}

uint32_t StencilOp(uint32_t op, uint32_t s, uint32_t ref)
{
    switch (op)
    {
    case 0: return s;                          // keep
    case 1: return 0;                          // zero
    case 2: return ref;                        // replace
    case 3: return s < 255 ? s + 1 : 255;      // increment, clamp
    case 4: return s > 0 ? s - 1 : 0;          // decrement, clamp
    case 5: return ~s & 0xFF;                  // invert
    case 6: return (s + 1) & 0xFF;             // increment, wrap
    default: return (s - 1) & 0xFF;            // decrement, wrap
    }
}

float BlendFactor(uint32_t f, const float s[4], const float d[4], const float k[4], int i)
{
    switch (f)
    {
    case 0: return 0.0f; case 1: return 1.0f;
    case 4: return s[i]; case 5: return 1.0f - s[i];
    case 6: return s[3]; case 7: return 1.0f - s[3];
    case 8: return d[i]; case 9: return 1.0f - d[i];
    case 10: return d[3]; case 11: return 1.0f - d[3];
    case 12: return k[i]; case 13: return 1.0f - k[i];
    case 14: return k[3]; case 15: return 1.0f - k[3];
    case 16: return i == 3 ? 1.0f : std::min(s[3], 1.0f - d[3]);
    default: return 0.0f;
    }
}
float BlendOp(uint32_t op, float a, float b)
{
    switch (op) { case 1: return a - b; case 2: return std::min(a, b); case 3: return std::max(a, b); case 4: return b - a; default: return a + b; }
}

// ---- textures for shaders ----------------------------------------------------------
// Decoded textures (per cube face) with a box-filtered mip chain, cached for
// the captured frame and dropped on every resolve, which may overwrite one.
// Mips are generated from level 0 rather than decoded from the mip address:
// close to well-made mips, and no packed-mip layout needed yet.
struct TexLevels { std::vector<Image> levels; };
std::map<std::array<uint32_t, 7>, TexLevels> g_textures;

const TexLevels* GetTexture(const TextureFetch& tf, uint32_t layer)
{
    // Sampler state (clamp, filters, LOD bias) is not part of the key.
    std::array<uint32_t, 7> key = { tf.dword[0] & ~0x0007FC00u, tf.dword[1], tf.dword[2], tf.dword[3] & 0x7FFFFu,
                                    tf.dword[4] & 0x3FCu, tf.dword[5] & ~0x1FFu, layer };
    auto it = g_textures.find(key);
    if (it != g_textures.end()) return it->second.levels.empty() ? nullptr : &it->second;
    TexLevels& t = g_textures[key];
    Image base;
    if (!DecodeTextureLayer(tf, layer, base)) return nullptr;
    uint32_t maxLevel = tf.is3D() ? 0 : Bits(tf.dword[4], 6, 4);   // 3D: slices only, no mips yet
    t.levels.push_back(std::move(base));
    while (t.levels.size() <= maxLevel)
    {
        const Image& src = t.levels.back();
        if (src.width == 1 && src.height == 1) break;
        Image d;
        d.width = std::max(1u, src.width / 2); d.height = std::max(1u, src.height / 2);
        d.rgba.resize(size_t(d.width) * d.height);
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x)
            {
                uint32_t sx0 = std::min(2 * x, src.width - 1), sx1 = std::min(2 * x + 1, src.width - 1);
                uint32_t sy0 = std::min(2 * y, src.height - 1), sy1 = std::min(2 * y + 1, src.height - 1);
                uint32_t p[4] = { src.rgba[size_t(sy0) * src.width + sx0], src.rgba[size_t(sy0) * src.width + sx1],
                                  src.rgba[size_t(sy1) * src.width + sx0], src.rgba[size_t(sy1) * src.width + sx1] };
                uint32_t r = 0;
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t sum = 0;
                    for (uint32_t v : p) sum += (v >> (8 * c)) & 0xFF;
                    r |= ((sum + 2) / 4) << (8 * c);
                }
                d.rgba[size_t(y) * d.width + x] = r;
            }
        t.levels.push_back(std::move(d));
    }
    return &t;
}

// Pixel shaders run per 2x2 quad. For texturing shaders the quad first runs
// in "record" mode (coordinates of every fetch remembered per lane, level 0
// sampled); then each lane runs again with the LOD from the quad's coarse
// derivatives (lane 1 - lane 0, lane 2 - lane 0), like the hardware's.
struct QuadSampler : TextureSampler
{
    int lane = 0;
    bool record = false;
    bool needLod = false;   // a recorded fetch could pick another level/filter with the real LOD
    std::vector<std::array<float, 2>> coords[4], grads[4];
    void BeginQuad() { for (auto& c : coords) c.clear(); for (auto& g : grads) g.clear(); needLod = false; }

    // getGradients: recorded per lane like the fetch coordinates; the real pass
    // returns lane 1 - lane 0 (x) and lane 2 - lane 0 (y).
    void Gradients(uint32_t seq, float x, float y, float out[4]) override
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        if (record)
        {
            auto& g = grads[lane];
            if (g.size() <= seq) g.resize(seq + 1, { NAN, NAN });
            g[seq] = { x, y };
            needLod = true;
            return;
        }
        if (grads[0].size() > seq && grads[1].size() > seq && grads[2].size() > seq)
        {
            const auto &g0 = grads[0][seq], &g1 = grads[1][seq], &g2 = grads[2][seq];
            out[0] = g1[0] - g0[0]; out[1] = g2[0] - g0[0]; out[2] = g1[1] - g0[1]; out[3] = g2[1] - g0[1];
        }
    }

    // Clamp modes (fetch dword 0, as Xenia's sampler): 0 repeat, 1 mirrored repeat, 2/4 clamp to
    // edge, 3/5/7 mirror once then clamp to edge, 6 clamp to border (-1: the border colour).
    static int Wrap(int v, int n, uint32_t mode)
    {
        switch (mode)
        {
        case 0: { int m = v % n; return m < 0 ? m + n : m; }                       // wrap
        case 1: { int p = 2 * n; int m = v % p; if (m < 0) m += p; return m < n ? m : p - 1 - m; }   // mirror
        case 3: case 5: case 7: { int m = v < 0 ? -v - 1 : v; return m >= n ? n - 1 : m; }   // mirror once
        case 6: return v < 0 || v >= n ? -1 : v;                                    // border
        default: return v < 0 ? 0 : (v >= n ? n - 1 : v);                          // clamp to edge
        }
    }
    // Border colour (fetch dword 5 bits 0-1): opaque white for 1, else transparent black.
    static void Border(uint32_t borderColor, float c[4]) { float b = borderColor == 1 ? 1.0f : 0.0f; c[0] = c[1] = c[2] = c[3] = b; }
    static void SampleLevel(const Image& img, float u, float v, bool linear, uint32_t cx, uint32_t cy, uint32_t borderColor, float out[4])
    {
        auto texel = [&](int x, int y, float c[4]) {
            int ix = Wrap(x, int(img.width), cx), iy = Wrap(y, int(img.height), cy);
            if (ix < 0 || iy < 0) { Border(borderColor, c); return; }
            UnpackColor(img.rgba[size_t(iy) * img.width + ix], c);
        };
        if (!linear) { texel(int(std::floor(u)), int(std::floor(v)), out); return; }
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = int(std::floor(fu)), y0 = int(std::floor(fv));
        float ax = fu - x0, ay = fv - y0;
        float c00[4], c10[4], c01[4], c11[4];
        texel(x0, y0, c00); texel(x0 + 1, y0, c10); texel(x0, y0 + 1, c01); texel(x0 + 1, y0 + 1, c11);
        for (int i = 0; i < 4; ++i)
            out[i] = (c00[i] * (1 - ax) + c10[i] * ax) * (1 - ay) + (c01[i] * (1 - ax) + c11[i] * ax) * ay;
    }
    // 3D textures (e.g. colour-grading LUTs): bilinear within the two nearest
    // slices and linear between them, or nearest; level 0 only.
    void Sample3D(const TextureFetch& tf, const TextureFetchRequest& r, float out[4])
    {
        const TexLevels* s0 = GetTexture(tf, 0);
        if (!s0) return;
        float W = float(s0->levels[0].width), H = float(s0->levels[0].height);
        int D = int(tf.depth());
        float u = r.unnormalized ? r.coord[0] : r.coord[0] * W, v = r.unnormalized ? r.coord[1] : r.coord[1] * H;
        float w = r.unnormalized ? r.coord[2] : r.coord[2] * float(D);
        uint32_t magF = r.magFilter == 3 ? Bits(tf.dword[3], 19, 2) : r.magFilter;
        bool linear = magF == 1;
        uint32_t cx = Bits(tf.dword[0], 10, 3), cy = Bits(tf.dword[0], 13, 3), cz = Bits(tf.dword[0], 16, 3);
        uint32_t border = Bits(tf.dword[5], 0, 2);
        auto slice = [&](int z, float o[4]) {
            int iz = Wrap(z, D, cz);
            if (iz < 0) { Border(border, o); return; }
            const TexLevels* t = GetTexture(tf, uint32_t(iz));
            if (!t) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
            SampleLevel(t->levels[0], u, v, linear, cx, cy, border, o);
        };
        if (!linear) { slice(int(std::floor(w)), out); return; }
        float fw = w - 0.5f;
        int z0 = int(std::floor(fw));
        float f = fw - float(z0), b[4];
        slice(z0, out);
        slice(z0 + 1, b);
        for (int i = 0; i < 4; ++i) out[i] += (b[i] - out[i]) * f;
    }
    void Sample(const TextureFetchRequest& r, float out[4]) override
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        TextureFetch tf = ReadTextureFetch(r.slot);
        if (!tf.valid()) return;
        if (tf.is3D()) { Sample3D(tf, r, out); return; }
        bool cube = tf.dimension() == 3;
        // Cube fetches carry face coordinates in [1, 2) and the face index.
        uint32_t face = cube ? uint32_t(std::min(5.0f, std::max(0.0f, std::floor(r.coord[2])))) : 0;
        if (tf.stacked() && r.dimension == 2)
        {
            // Stacked: z picks the layer (z * depth, floored as 3D slices are, clamped to the stack);
            // linear between layers with the volume filter, as Xenia.
            float D = float(tf.depth()), w = r.unnormalized ? r.coord[2] : r.coord[2] * D;
            if (!(w == w)) w = 0.0f;
            bool volLinear = (r.volMagFilter == 3 ? Bits(tf.dword[4], 0, 1) : r.volMagFilter) == 1;
            if (volLinear) w -= 0.5f;
            float fl = std::floor(w), f = volLinear ? w - fl : 0.0f;
            // The layer obeys the Z clamp mode, as Sample3D's slices: outside the stack with clamp to border it
            // is the border colour (the jersey numbers' missing second digit of a single digit).
            const uint32_t cz = Bits(tf.dword[0], 16, 3), border = Bits(tf.dword[5], 0, 2);
            auto layer = [&](float l, float o[4]) {
                int z = Wrap(int(std::min(std::max(l, -65536.0f), 65536.0f)), int(D), cz);
                if (z < 0) Border(border, o);
                else SampleLayer(tf, r, uint32_t(z), false, o);
            };
            layer(fl, out);
            if (f > 0.0f)
            {
                float b[4];
                layer(fl + 1.0f, b);
                for (int i = 0; i < 4; ++i) out[i] += (b[i] - out[i]) * f;
            }
            return;
        }
        SampleLayer(tf, r, face, cube, out);
    }
    // One 2D layer (a 2D texture, a cube face or a stacked layer), mipmapped.
    void SampleLayer(const TextureFetch& tf, const TextureFetchRequest& r, uint32_t face, bool cube, float out[4])
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        const TexLevels* t = GetTexture(tf, face);
        if (!t) return;
        float W = float(t->levels[0].width), H = float(t->levels[0].height);
        float cu = cube ? r.coord[0] - 1.0f : r.coord[0], cv = cube ? r.coord[1] - 1.0f : r.coord[1];
        float u = r.unnormalized ? cu : cu * W, v = r.unnormalized ? cv : cv * H;
        u += r.offsetX * 0.5f; v += r.offsetY * 0.5f;
        uint32_t magF = r.magFilter == 3 ? Bits(tf.dword[3], 19, 2) : r.magFilter;
        uint32_t minF = r.minFilter == 3 ? Bits(tf.dword[3], 21, 2) : r.minFilter;
        uint32_t mipF = r.mipFilter == 3 ? Bits(tf.dword[3], 23, 2) : r.mipFilter;
        uint32_t cx = Bits(tf.dword[0], 10, 3), cy = Bits(tf.dword[0], 13, 3);
        uint32_t last = uint32_t(t->levels.size()) - 1;
        if (record)
        {
            auto& c = coords[lane];
            if (c.size() <= r.sequence) c.resize(r.sequence + 1, { NAN, NAN });
            c[r.sequence] = { u, v };
            if (last > 0 || magF != minF) needLod = true;
        }
        float lod = 0.0f;
        bool haveLod = false;
        float fetchBias = float(SBits(tf.dword[4], 12, 10)) / 32.0f;
        if (r.useComputedLod && r.useRegisterGradients && !cube)
        {
            // setGradientsH/V (normalized, unless the coordinates are not): in level-0 texels.
            float sx = r.unnormalized ? 1.0f : W, sy = r.unnormalized ? 1.0f : H;
            float dux = r.gradH[0] * sx, dvx = r.gradH[1] * sy, duy = r.gradV[0] * sx, dvy = r.gradV[1] * sy;
            float rho2 = std::max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
            if (std::isfinite(rho2))
            {
                lod = rho2 > 0.0f ? 0.5f * Log2(rho2) : -16.0f;
                lod += fetchBias + r.lodBias;
                haveLod = true;
            }
        }
        else if (!record && r.useComputedLod && coords[0].size() > r.sequence && coords[1].size() > r.sequence &&
                 coords[2].size() > r.sequence)
        {
            const auto &c0 = coords[0][r.sequence], &c1 = coords[1][r.sequence], &c2 = coords[2][r.sequence];
            float dux = c1[0] - c0[0], dvx = c1[1] - c0[1], duy = c2[0] - c0[0], dvy = c2[1] - c0[1];
            float rho2 = std::max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
            if (std::isfinite(rho2))
            {
                lod = rho2 > 0.0f ? 0.5f * Log2(rho2) : -16.0f;
                lod += fetchBias + r.lodBias;
                haveLod = true;
            }
        }
        if (r.useRegisterLod)   // setTexLOD: added to the computed LOD, or the explicit LOD
        {
            if (haveLod) lod += r.registerLod;
            else if (!r.useComputedLod) { lod = r.registerLod + fetchBias + r.lodBias; haveLod = true; }
        }
        uint32_t minLevel = std::min(Bits(tf.dword[4], 2, 4), last), maxLevel = std::min(Bits(tf.dword[4], 6, 4), last);
        if (maxLevel < minLevel) maxLevel = minLevel;
        bool minify = haveLod && lod > 0.0f;
        bool linear = (minify ? minF : magF) == 1;
        auto at = [&](uint32_t level, float o[4]) {
            const Image& img = t->levels[level];
            SampleLevel(img, u * float(img.width) / W, v * float(img.height) / H, linear, cx, cy, Bits(tf.dword[5], 0, 2), o);
        };
        if (!minify || mipF == 2 || maxLevel == minLevel) { at(minLevel, out); return; }
        float l = std::min(std::max(lod, float(minLevel)), float(maxLevel));
        if (mipF == 0) { at(uint32_t(std::floor(l + 0.5f)), out); return; }
        uint32_t l0 = uint32_t(std::floor(l)), l1 = std::min(l0 + 1, maxLevel);
        float f = l - float(l0);
        at(l0, out);
        if (f > 0.0f && l1 != l0)
        {
            float b[4];
            at(l1, b);
            for (int i = 0; i < 4; ++i) out[i] += (b[i] - out[i]) * f;
        }
    }
};

// ---- vertex processing ----------------------------------------------------------------
struct Vertex { float pos[4] = { 0, 0, 0, 1 }; float v[16][4] = {}; bool hasPos = false; };

struct VsSink : ExportSink
{
    Vertex* out;
    void Export(uint32_t reg, const float v[4], uint32_t mask) override
    {
        float* dst = reg == 62 ? out->pos : (reg < 16 ? out->v[reg] : nullptr);
        if (!dst) return;
        if (reg == 62) out->hasPos = true;
        for (int i = 0; i < 4; ++i) if (mask & (1u << i)) dst[i] = v[i];
    }
};

struct PsSink : ExportSink
{
    float color[4][4]; uint32_t written = 0;
    void Export(uint32_t reg, const float v[4], uint32_t mask) override
    {
        if (reg >= 4) return;
        for (int i = 0; i < 4; ++i) if (mask & (1u << i)) color[reg][i] = v[i];
        written |= 1u << reg;
    }
};

struct ScreenVertex { float x, y, z, invW; const Vertex* src; };

// ---- resolves -------------------------------------------------------------------------
bool DecodeResolve(ResolveInfo& r, std::string* note)
{
    uint32_t cc = Reg(reg::RB_COPY_CONTROL);
    r.src = cc & 7;
    r.sampleSel = (cc >> 4) & 7;
    uint32_t cmd = (cc >> 20) & 3;
    r.clearColor = (cc >> 8) & 1;
    r.clearDepth = (cc >> 9) & 1;
    // Resolve rectangle: three float2 vertices in vertex fetch constant 0.
    uint32_t vf0 = Reg(reg::FETCH_CONSTANTS), vf1 = Reg(reg::FETCH_CONSTANTS + 1);
    if ((vf0 & 3) != 3) { if (note) *note = "resolve: no vertex fetch 0"; return false; }
    float half = (Reg(reg::PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
    float px[3], py[3];
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 2; ++k)
        {
            uint32_t raw;
            memcpy(&raw, Phys((vf0 & ~3u) + (i * 2 + k) * 4), 4);
            uint32_t e = vf1 & 3;
            uint32_t v = e == 1 ? (((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8)) : e == 2 ? Bswap32(raw)
                       : e == 3 ? (raw << 16) | (raw >> 16) : raw;
            (k ? py : px)[i] = AsF(v) + half;
        }
    }
    int32_t x0 = int32_t(std::floor(std::min({ px[0], px[1], px[2] }) + 0.5f)), x1 = int32_t(std::floor(std::max({ px[0], px[1], px[2] }) + 0.5f));
    int32_t y0 = int32_t(std::floor(std::min({ py[0], py[1], py[2] }) + 0.5f)), y1 = int32_t(std::floor(std::max({ py[0], py[1], py[2] }) + 0.5f));
    if (Reg(reg::PA_SU_SC_MODE_CNTL) & (1u << 16))
    {
        uint32_t wo = Reg(reg::PA_SC_WINDOW_OFFSET);
        x0 += SBits(wo, 0, 15); x1 += SBits(wo, 0, 15); y0 += SBits(wo, 16, 15); y1 += SBits(wo, 16, 15);
    }
    x0 = std::max(0, x0) & ~7; y0 = std::max(0, y0) & ~7;
    x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
    Surface surf = r.src >= 4 ? DepthSurface() : ColorSurface(r.src);
    r.srcFmt = r.src >= 4 ? 0 : Bits(Reg(kColorInfoRegs[r.src & 3]), 16, 4);
    x1 = std::min<int32_t>(x1, int32_t(surf.pitch));
    if (x0 >= x1 || y0 >= y1) { if (note) *note = "resolve: empty rect"; return false; }
    r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
    r.base = surf.base; r.pitch = surf.pitch; r.msaa = surf.msaa; r.wide = surf.wide;

    r.destBase = Reg(reg::RB_COPY_DEST_BASE) & 0x1FFFFFFF;
    uint32_t destPitchReg = Reg(reg::RB_COPY_DEST_PITCH), destInfo = Reg(reg::RB_COPY_DEST_INFO);
    r.destPitch = ((destPitchReg & 0x3FFF) + 31) & ~31u;
    r.destHeight = (destPitchReg >> 16) & 0x3FFF;
    r.endian = destInfo & 7;
    r.destFmt = (destInfo >> 7) & 0x3F;
    r.swapRB = (destInfo >> 24) & 1;
    r.destScale = std::ldexp(1.0f, SBits(destInfo, 16, 6));   // copy_dest_exp_bias
    // Destination texel size by format (0: not supported); depth copies the raw 24:8 sample.
    r.destBytes = 0;
    switch (r.destFmt)
    {
    case 2: r.destBytes = 1; break;                                   // 8
    case 4: case 10: case 24: r.destBytes = 2; break;                 // 5_6_5, 8_8, 16
    case 6: case 7: case 25: case 31: case 36: r.destBytes = 4; break;
    case 26: case 32: case 37: r.destBytes = 8; break;
    default: break;
    }
    if (r.src >= 4) r.destBytes = 4;
    r.copy = cmd != 3 && r.destBytes != 0;
    r.colorClear = Reg(reg::RB_COLOR_CLEAR);
    r.colorClearLo = Reg(reg::RB_COLOR_CLEAR_LO);
    r.depthClear = Reg(reg::RB_DEPTH_CLEAR);
    r.depthBase = DepthSurface().base;
    return true;
}

// Pack a resolved colour into the destination format (v[1]: second dword of 64-bit formats).
void PackResolve(const ResolveInfo& r, const float c[4], uint32_t v[2])
{
    auto snorm16 = [](float f) { return ToFixed16(f); };   // fixed-point RT data keeps its encoding
    v[0] = v[1] = 0;
    switch (r.destFmt)
    {
    case 2: v[0] = Unorm(c[0], 8); break;
    case 4: v[0] = Unorm(c[0], 5) | (Unorm(c[1], 6) << 5) | (Unorm(c[2], 5) << 11); break;
    case 10: v[0] = Unorm(c[0], 8) | (Unorm(c[1], 8) << 8); break;
    case 24: v[0] = r.srcFmt == 4 || r.srcFmt == 5 ? snorm16(c[0]) : Unorm(c[0], 16); break;
    case 6: for (int i = 0; i < 4; ++i) v[0] |= Unorm(c[i], 8) << (8 * i); break;
    case 7: v[0] = Unorm(c[0], 10) | (Unorm(c[1], 10) << 10) | (Unorm(c[2], 10) << 20) | (Unorm(c[3], 2) << 30); break;
    case 25: v[0] = snorm16(c[0]) | (snorm16(c[1]) << 16); break;
    case 26: v[0] = snorm16(c[0]) | (snorm16(c[1]) << 16); v[1] = snorm16(c[2]) | (snorm16(c[3]) << 16); break;
    case 31: v[0] = FloatToHalf(c[0]) | (FloatToHalf(c[1]) << 16); break;
    case 32: v[0] = FloatToHalf(c[0]) | (FloatToHalf(c[1]) << 16); v[1] = FloatToHalf(c[2]) | (FloatToHalf(c[3]) << 16); break;
    case 36: memcpy(&v[0], &c[0], 4); break;
    case 37: memcpy(&v[0], &c[0], 4); memcpy(&v[1], &c[1], 4); break;
    default: break;
    }
}

void ResolveDone(const ResolveInfo& r, const std::string& debugDir, std::string* note)
{
    ++g_stats.resolves;
    if (note)
    {
        char b[200];
        snprintf(b, sizeof(b), "resolve rect (%d,%d)-(%d,%d) from %s base %u fmt %u -> 0x%08X pitch %u fmt %u%s%s%s", r.x0, r.y0, r.x1,
                 r.y1, r.src >= 4 ? "depth" : "color", r.base, r.srcFmt, r.destBase, r.destPitch, r.destFmt, r.copy ? "" : " (no copy)",
                 r.clearColor ? " +clear color" : "", r.clearDepth ? " +clear depth" : "");
        *note = b;
    }
    if (r.copy) g_textures.clear();   // the destination may be a cached texture
    if (r.copy && !debugDir.empty() && r.destHeight && r.src < 4)
    {
        TextureFetch tf;
        tf.dword[0] = 2 | (1u << 31) | ((r.destPitch / 32) << 22);
        tf.dword[1] = (r.destFmt == 4 || r.destFmt == 10 || r.destFmt == 24 || r.destBytes >= 4 ? r.destFmt : 6) | (2u << 6) | r.destBase;
        tf.dword[2] = (std::min<uint32_t>(r.destPitch, 8192) - 1) | ((r.destHeight - 1) << 13);
        tf.dword[3] = (0x60Au << 1);
        tf.dword[5] = 1u << 9;
        if (r.endian != 2) tf.dword[1] = (tf.dword[1] & ~(3u << 6)) | ((r.endian & 3) << 6);
        Image img;
        if (DecodeTexture2D(tf, img))
        {
            char name[96];
            snprintf(name, sizeof(name), "/resolve_%02u_%08X.png", g_stats.resolves, r.destBase);
            WritePng(debugDir + name, img);
        }
    }
}

void Resolve(const std::string& debugDir, std::string* note)
{
    ResolveInfo r;
    if (!DecodeResolve(r, note)) return;
    Surface surf{ r.base, r.pitch, r.msaa, r.wide };
    int bppLog2 = r.destBytes == 1 ? 0 : r.destBytes == 2 ? 1 : r.destBytes == 4 ? 2 : 3;
    uint32_t samples = surf.Samples();
    auto writeDest = [&](uint32_t x, uint32_t y, const uint32_t v[2]) {
        uint8_t b[8];
        memcpy(b, v, 8);
        // Endian swap per GPU word: 8in16, 8in32, 16in32.
        for (uint32_t i = 0; i + 1 < r.destBytes; i += r.endian == 1 ? 2 : 4)
        {
            if (r.endian == 1) std::swap(b[i], b[i + 1]);
            else if (r.endian == 2 && i + 3 < r.destBytes) { std::swap(b[i], b[i + 3]); std::swap(b[i + 1], b[i + 2]); }
            else if (r.endian == 3 && i + 3 < r.destBytes) { std::swap(b[i], b[i + 2]); std::swap(b[i + 1], b[i + 3]); }
        }
        memcpy(const_cast<uint8_t*>(Phys(r.destBase + TiledOffset2D(x, y, r.destPitch, bppLog2))), b, r.destBytes);
    };
    if (r.copy)
    {
        for (int32_t y = r.y0; y < r.y1; ++y)
            for (int32_t x = r.x0; x < r.x1; ++x)
            {
                uint32_t v[2] = { 0, 0 };
                if (r.src >= 4) v[0] = g_edram[surf.Index(x, y, 0)];   // depth: the 24:8 sample as stored
                else
                {
                    // Sample select: 0-3 one sample, 4 = 0+1, 5 = 2+3, 6 = all.
                    uint32_t first = r.sampleSel <= 3 ? r.sampleSel : r.sampleSel == 5 ? 2 : 0;
                    uint32_t count = r.sampleSel <= 3 ? 1 : r.sampleSel == 6 ? 4 : 2;
                    if (first >= samples) first = 0;
                    if (first + count > samples) count = samples - first;
                    float acc[4] = { 0, 0, 0, 0 };
                    for (uint32_t s = first; s < first + count; ++s)
                    {
                        float c[4];
                        DecodeRt(r.srcFmt, &g_edram[surf.Index(x, y, s)], c, false);
                        for (int i = 0; i < 4; ++i) acc[i] += c[i] / float(count);
                    }
                    for (float& a : acc) a *= r.destScale;
                    if (r.swapRB) std::swap(acc[0], acc[2]);
                    PackResolve(r, acc, v);
                }
                writeDest(uint32_t(x), uint32_t(y), v);
            }
    }
    // Clears after the copy. The clear value is in the target's own encoding
    // (64 bpp: RB_COLOR_CLEAR in the first dword, RB_COLOR_CLEAR_LO in the second).
    if (r.clearColor && r.src < 4)
    {
        for (int32_t y = r.y0; y < r.y1; ++y)
            for (int32_t x = r.x0; x < r.x1; ++x)
                for (uint32_t s = 0; s < samples; ++s)
                {
                    uint32_t k = surf.Index(x, y, s);
                    g_edram[k] = r.colorClear;
                    if (surf.wide) g_edram[k + 1] = r.colorClearLo;
                }
    }
    if (r.clearDepth)
    {
        Surface ds{ r.depthBase, r.pitch, r.msaa, false };
        for (int32_t y = r.y0; y < r.y1; ++y)
            for (int32_t x = r.x0; x < r.x1; ++x)
                for (uint32_t s = 0; s < samples; ++s) g_edram[ds.Index(x, y, s)] = r.depthClear;
    }
    ResolveDone(r, debugDir, note);
}

}  // namespace

RasterStats& RasterGetStats() { return g_stats; }

bool RasterDecodeResolve(ResolveInfo& r, std::string* note) { return DecodeResolve(r, note); }

void RasterDecodeRt(uint32_t fmt, const uint32_t* p, float c[4], bool linear) { DecodeRt(fmt, p, c, linear); }
void RasterEncodeRt(uint32_t fmt, const float c[4], uint32_t* out, bool linear) { EncodeRt(fmt, c, out, linear); }
void RasterClampRt(uint32_t fmt, float c[4]) { ClampRt(fmt, c); }
float RasterDecodeDepth(uint32_t d24, bool isFloat) { return DecodeDepth(d24, isFloat); }
uint32_t RasterEncodeDepth(float z, bool isFloat) { return EncodeDepth(z, isFloat); }

float RasterBlend(uint32_t blend, const float src[4], const float dst[4], const float k[4], int i)
{
    bool a = i == 3;
    uint32_t sf = Bits(blend, a ? 16 : 0, 5), op = Bits(blend, a ? 21 : 5, 3), df = Bits(blend, a ? 24 : 8, 5);
    return BlendOp(op, src[i] * BlendFactor(sf, src, dst, k, i), dst[i] * BlendFactor(df, src, dst, k, i));
}

void RasterPackResolve(uint32_t destFmt, uint32_t srcFmt, const float c[4], uint32_t out[2])
{
    ResolveInfo r;
    r.destFmt = destFmt;
    r.srcFmt = srcFmt;
    PackResolve(r, c, out);
}

void RasterResolveDone(const ResolveInfo& r, const std::string& debugDir, std::string* note) { ResolveDone(r, debugDir, note); }

uint32_t* RasterEdram() { return g_edram.data(); }

uint32_t RasterEdramIndex(uint32_t base, uint32_t pitch, uint32_t msaa, bool wide, uint32_t x, uint32_t y, uint32_t s)
{
    return Surface{ base, pitch, msaa, wide }.Index(x, y, s);
}

void RasterSetDrawNumber(uint32_t n) { g_drawNumber = n; }

void RasterNewFrame() { g_textures.clear(); }

void RasterBeginFrame()
{
    g_stats = RasterStats{};
    g_textures.clear();
    if (const char* p = Hc09Env("HC09_PICK_VERTEX"))
    {
        char* e;
        g_pickVertexDraw = uint32_t(strtoul(p, &e, 10));
        g_pickVertexIndex = *e ? uint32_t(strtoul(e + 1, nullptr, 10)) : 0;
    }
    if (const char* p = Hc09Env("HC09_PICK")) { char* e; g_pickX = int32_t(strtol(p, &e, 10)); g_pickY = *e ? int32_t(strtol(e + 1, nullptr, 10)) : -1; }
    std::fill(g_edram.begin(), g_edram.end(), 0u);
}

void RasterDraw(const DrawPacket& d, const ShaderCode* vs, const ShaderCode* ps, const std::string& debugDir,
                std::string* note)
{
    uint32_t mode = Reg(reg::RB_MODECONTROL) & 7;
    if (mode == 6) { Resolve(debugDir, note); return; }
    if (mode != 4 && mode != 5) { ++g_stats.skipped; return; }
    if (!vs || d.primType == 1 || d.sourceSelect == 1) { ++g_stats.skipped; return; }   // points, immediate indices
    ++g_stats.draws;

    // Indices.
    std::vector<uint32_t> idx(d.numIndices);
    uint32_t minI = Reg(reg::VGT_MIN_VTX_INDX), maxI = Reg(reg::VGT_MAX_VTX_INDX), off = Reg(reg::VGT_INDX_OFFSET);
    for (uint32_t i = 0; i < d.numIndices; ++i)
    {
        uint32_t v = i;
        if (d.sourceSelect == 0)
        {
            uint32_t sz = d.indexSize32 ? 4 : 2, addr = (d.dmaBase & ~(sz - 1)) + i * sz;
            if (i >= d.dmaWords) { v = 0; }
            else if (sz == 2)
            {
                uint16_t raw; memcpy(&raw, Phys(addr), 2);
                v = (d.dmaEndian == 1 || d.dmaEndian == 2) ? uint16_t((raw >> 8) | (raw << 8)) : raw;
            }
            else
            {
                uint32_t raw; memcpy(&raw, Phys(addr), 4);
                v = d.dmaEndian == 1 ? (((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8)) : d.dmaEndian == 2 ? Bswap32(raw)
                  : d.dmaEndian == 3 ? (raw << 16) | (raw >> 16) : raw;
            }
        }
        idx[i] = std::min(maxI, std::max(minI, (v + off) & 0xFFFFFF));
    }

    // Vertex shading (cached per index).
    ShaderConstants consts;
    consts.Snapshot();
    QuadSampler sampler;
    std::map<uint32_t, Vertex> verts;
    std::string vsTrace;
    for (uint32_t i : idx)
    {
        if (verts.count(i)) continue;
        Vertex& v = verts[i];
        VsSink sink; sink.out = &v;
        ShaderInterpreter in(consts, sink, &sampler);
        memset(in.r, 0, sizeof(in.r));
        in.r[0][0] = float(i);
        if (g_drawNumber == g_pickVertexDraw && i == g_pickVertexIndex) in.trace = &vsTrace;
        in.Run(*vs);
        if (in.trace)
        {
            char b[160];
            snprintf(b, sizeof(b), "  -> pos (%g %g %g %g) o0 (%g %g %g %g)\n", v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.v[0][0],
                     v.v[0][1], v.v[0][2], v.v[0][3]);
            vsTrace += b;
        }
    }

    // Viewport transform.
    uint32_t vte = Reg(reg::PA_CL_VTE_CNTL), su = Reg(reg::PA_SU_SC_MODE_CNTL);
    float sx = (vte & 1) ? AsF(Reg(reg::PA_CL_VPORT_XSCALE)) : 1.0f, ox = (vte & 2) ? AsF(Reg(reg::PA_CL_VPORT_XOFFSET)) : 0.0f;
    float sy = (vte & 4) ? AsF(Reg(reg::PA_CL_VPORT_YSCALE)) : 1.0f, oy = (vte & 8) ? AsF(Reg(reg::PA_CL_VPORT_YOFFSET)) : 0.0f;
    float sz = (vte & 16) ? AsF(Reg(reg::PA_CL_VPORT_ZSCALE)) : 1.0f, oz = (vte & 32) ? AsF(Reg(reg::PA_CL_VPORT_ZOFFSET)) : 0.0f;
    bool xyFmt = (vte >> 8) & 1, zFmt = (vte >> 9) & 1, w0Fmt = (vte >> 10) & 1;
    float half = (Reg(reg::PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
    int32_t wox = 0, woy = 0;
    if (su & (1u << 16)) { uint32_t wo = Reg(reg::PA_SC_WINDOW_OFFSET); wox = SBits(wo, 0, 15); woy = SBits(wo, 16, 15); }
    auto toScreen = [&](const Vertex& v) {
        float w = v.pos[3];
        // VTX_W0_FMT = 1: the incoming W is not 1/W, take the reciprocal; 0: it already is 1/W.
        float invW = w0Fmt ? (w != 0.0f ? 1.0f / w : 1.0f) : w;
        float x = xyFmt ? v.pos[0] : v.pos[0] * invW, y = xyFmt ? v.pos[1] : v.pos[1] * invW, z = zFmt ? v.pos[2] : v.pos[2] * invW;
        return ScreenVertex{ x * sx + ox + wox + half, y * sy + oy + woy + half, z * sz + oz, invW, &v };
    };
    std::map<uint32_t, ScreenVertex> scr;
    for (auto& [i, v] : verts) scr[i] = toScreen(v);

    std::string dbg, pick;
    if (!vsTrace.empty()) pick += " PICK-VERTEX{\n" + vsTrace + "}";
    {
        char b[200];
        int k = 0;
        for (auto& [i, s] : scr)
        {
            if (k++ >= 4) break;
            snprintf(b, sizeof(b), " v%u(%.1f,%.1f,%.2f w%.2f | %.3f %.3f %.3f %.3f)", i, s.x, s.y, s.z, s.invW,
                     s.src->v[0][0], s.src->v[0][1], s.src->v[0][2], s.src->v[0][3]);
            dbg += b;
        }
    }

    // Primitive assembly into triangles.
    std::vector<uint32_t> tris;
    uint32_t n = d.numIndices;
    switch (d.primType)
    {
    case 4: for (uint32_t i = 0; i + 2 < n; i += 3) tris.insert(tris.end(), { idx[i], idx[i + 1], idx[i + 2] }); break;
    case 5: for (uint32_t i = 1; i + 1 < n; ++i) tris.insert(tris.end(), { idx[0], idx[i], idx[i + 1] }); break;
    case 6: for (uint32_t i = 0; i + 2 < n; ++i) { if (i & 1) tris.insert(tris.end(), { idx[i + 1], idx[i], idx[i + 2] }); else tris.insert(tris.end(), { idx[i], idx[i + 1], idx[i + 2] }); } break;
    case 13: for (uint32_t i = 0; i + 3 < n; i += 4) tris.insert(tris.end(), { idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3] }); break;
    case 8: break;   // rectangles below
    default: ++g_stats.skipped; if (note) *note = "primitive type not rasterized"; return;
    }
    std::deque<Vertex> extra;   // clipper output and generated 4th vertices of rectangles (stable addresses)
    std::vector<std::array<ScreenVertex, 3>> prims;
    // Homogeneous clipping before the divide, against w > 0 and the near/far
    // planes (PA_CL_CLIP_CNTL.dx_clip_space_def: 0 <= z <= w, else -w <= z <= w).
    // X/Y are left to the guard band and the scissor. Without it, triangles
    // crossing the camera plane project to huge slivers across the screen.
    // Off when clip_disable is set or positions arrive already divided.
    uint32_t clipCntl = Reg(reg::PA_CL_CLIP_CNTL);
    bool clipOn = !((clipCntl >> 16) & 1) && !xyFmt && !zFmt && w0Fmt;
    bool dxClip = (clipCntl >> 19) & 1;
    auto planeDist = [&](const Vertex& v, int plane) {
        switch (plane)
        {
        case 0: return v.pos[3] - 1e-6f;                                 // w > 0
        case 1: return dxClip ? v.pos[2] : v.pos[2] + v.pos[3];          // near
        default: return v.pos[3] - v.pos[2];                             // far
        }
    };
    std::vector<Vertex> poly, next;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        const Vertex* tv[3] = { &verts[tris[t]], &verts[tris[t + 1]], &verts[tris[t + 2]] };
        bool inside = true;
        for (int p = 0; clipOn && inside && p < 3; ++p)
            for (int k = 0; k < 3; ++k) if (planeDist(*tv[k], p) < 0.0f) { inside = false; break; }
        if (inside) { prims.push_back({ scr[tris[t]], scr[tris[t + 1]], scr[tris[t + 2]] }); continue; }
        poly.assign({ *tv[0], *tv[1], *tv[2] });
        for (int p = 0; p < 3 && poly.size() >= 3; ++p)
        {
            next.clear();
            for (size_t k = 0; k < poly.size(); ++k)
            {
                const Vertex &a = poly[k], &b = poly[(k + 1) % poly.size()];
                float da = planeDist(a, p), db = planeDist(b, p);
                if (da >= 0.0f) next.push_back(a);
                if ((da >= 0.0f) != (db >= 0.0f))
                {
                    float f = da / (da - db);
                    Vertex m;
                    m.hasPos = true;
                    for (int j = 0; j < 4; ++j) m.pos[j] = a.pos[j] + f * (b.pos[j] - a.pos[j]);
                    for (int r = 0; r < 16; ++r) for (int j = 0; j < 4; ++j) m.v[r][j] = a.v[r][j] + f * (b.v[r][j] - a.v[r][j]);
                    next.push_back(m);
                }
            }
            poly.swap(next);
        }
        if (poly.size() < 3) { ++g_stats.clipped; continue; }
        size_t first = extra.size();
        for (auto& v : poly) extra.push_back(v);
        for (size_t k = 1; k + 1 < poly.size(); ++k)   // fan keeps the winding
            prims.push_back({ toScreen(extra[first]), toScreen(extra[first + k]), toScreen(extra[first + k + 1]) });
    }
    if (d.primType == 8)
    {
        for (uint32_t i = 0; i + 2 < n; i += 3)
        {
            ScreenVertex a = scr[idx[i]], b = scr[idx[i + 1]], c = scr[idx[i + 2]];
            // The vertex opposite the longest edge is mirrored across it.
            auto len = [](const ScreenVertex& p, const ScreenVertex& q) { return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y); };
            float lab = len(a, b), lbc = len(b, c), lca = len(c, a);
            const ScreenVertex *o = &a, *p = &b, *q = &c;
            if (lab >= lbc && lab >= lca) { o = &c; p = &a; q = &b; }
            else if (lca >= lbc) { o = &b; p = &c; q = &a; }
            Vertex m;
            for (int k = 0; k < 16; ++k) for (int j = 0; j < 4; ++j) m.v[k][j] = p->src->v[k][j] + q->src->v[k][j] - o->src->v[k][j];
            extra.push_back(m);
            ScreenVertex r{ p->x + q->x - o->x, p->y + q->y - o->y, p->z + q->z - o->z, p->invW, &extra.back() };
            prims.push_back({ a, b, c });
            prims.push_back({ *p, *q, r });
        }
    }

    // Render state.
    // Up to four colour targets (MRT), each with its format, write mask, blend
    // control and exp bias (PS output scaled by 2^bias before blending).
    Surface depth = DepthSurface();
    bool depthFloat = (Reg(reg::RB_DEPTH_INFO) >> 16) & 1;
    struct RtState { Surface s; uint32_t fmt, mask, blend; float scale; bool on; } rts[4];
    const uint32_t kBlendRegs[4] = { reg::RB_BLENDCONTROL0, reg::RB_BLENDCONTROL1, reg::RB_BLENDCONTROL2, reg::RB_BLENDCONTROL3 };
    uint32_t colorMask = Reg(reg::RB_COLOR_MASK);
    bool colorOn = false;
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t ci = Reg(kColorInfoRegs[i]);
        rts[i] = RtState{ ColorSurface(i), Bits(ci, 16, 4), (colorMask >> (4 * i)) & 0xF, Reg(kBlendRegs[i]),
                          std::ldexp(1.0f, SBits(ci, 20, 6)), false };
        rts[i].on = mode == 4 && rts[i].mask && ps;
        colorOn |= rts[i].on;
    }
    const Surface& color = rts[0].s;
    float blendConst[4];
    for (int i = 0; i < 4; ++i) blendConst[i] = AsF(Reg(reg::RB_BLEND_RED + i));
    uint32_t dc = Reg(reg::RB_DEPTHCONTROL);
    bool zEnable = (dc >> 1) & 1, zWrite = (dc >> 2) & 1;
    uint32_t zFunc = (dc >> 4) & 7;
    // Stencil (low byte of the D24S8 sample). Front: func/fail/zpass/zfail at
    // bits 8/11/14/17 with RB_STENCILREFMASK; back faces use bits 20/23/26/29
    // and RB_STENCILREFMASK_BF when backface_enable (bit 7) is set.
    bool stencilOn = dc & 1, stencilTwoSided = (dc >> 7) & 1;
    struct StencilState { uint32_t func, fail, zpass, zfail, ref, mask, writeMask; } stencilFace[2];
    for (int f = 0; f < 2; ++f)
    {
        bool back = f == 1 && stencilTwoSided;
        uint32_t rm = Reg(back ? reg::RB_STENCILREFMASK_BF : reg::RB_STENCILREFMASK);
        int lo = back ? 20 : 8;
        stencilFace[f] = { Bits(dc, lo, 3), Bits(dc, lo + 3, 3), Bits(dc, lo + 6, 3), Bits(dc, lo + 9, 3),
                           Bits(rm, 0, 8), Bits(rm, 8, 8), Bits(rm, 16, 8) };
    }
    bool faceCw = (su >> 2) & 1;   // 0: front faces are CCW on screen
    // Face culling (PA_SU_SC_MODE_CNTL cull_front/cull_back). Rectangle lists
    // are screen-space blits; their generated triangles carry no winding.
    bool cullFront = (su & 1) && d.primType != 8, cullBack = ((su >> 1) & 1) && d.primType != 8;
    uint32_t cc = Reg(reg::RB_COLORCONTROL);
    bool alphaTest = (cc >> 3) & 1;
    uint32_t alphaFunc = cc & 7;
    float alphaRef = AsF(Reg(reg::RB_ALPHA_REF));
    uint32_t pc = Reg(reg::SQ_PROGRAM_CNTL);
    uint32_t interpCount = std::min(16u, Bits(pc, 20, 4) + 1);
    uint32_t psTemps = std::max(interpCount, Bits(pc, 8, 6) + 1);
    {
        uint32_t pg = Bits(Reg(reg::SQ_CONTEXT_MISC), 8, 8);
        if ((pc >> 18) & 1) psTemps = std::max(psTemps, pg + 1);
        psTemps = std::min<uint32_t>(psTemps, ShaderInterpreter::kTemps);
    }
    bool paramGen = (pc >> 18) & 1;
    uint32_t paramGenReg = Bits(Reg(reg::SQ_CONTEXT_MISC), 8, 8);
    bool psFetches = ps && ShaderHasTextureFetch(*ps);
    uint32_t tl = Reg(reg::PA_SC_WINDOW_SCISSOR_TL), br = Reg(reg::PA_SC_WINDOW_SCISSOR_BR);
    int32_t scx0 = Bits(tl, 0, 14), scy0 = Bits(tl, 16, 14), scx1 = Bits(br, 0, 14), scy1 = Bits(br, 16, 14);
    if (!(tl >> 31)) { scx0 += wox; scx1 += wox; scy0 += woy; scy1 += woy; }
    scx0 = std::max(0, scx0); scy0 = std::max(0, scy0);
    scx1 = std::min<int32_t>(scx1, int32_t(color.pitch ? color.pitch : 8192)); scy1 = std::min(scy1, 8192);

    for (auto& tri : prims)
    {
        ++g_stats.triangles;
        const ScreenVertex &A = tri[0], &B = tri[1], &C = tri[2];
        float area = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (area == 0.0f || !std::isfinite(area)) continue;
        // Window coordinates are y-down, so a positive area is clockwise on screen.
        bool front = (area > 0.0f) == faceCw;
        if (front ? cullFront : cullBack) { ++g_stats.culled; continue; }
        const StencilState& st = stencilFace[front ? 0 : 1];
        // Top-left fill rule: a pixel centre exactly on an edge belongs to the
        // triangle only if that edge is a top or left edge. With the vertices in
        // clockwise (positive-area) order, edge P->Q is top if dy == 0 && dx > 0,
        // left if dy < 0. Edge i is the one opposite vertex i.
        const ScreenVertex* cw[3] = { &A, area > 0.0f ? &B : &C, area > 0.0f ? &C : &B };
        bool topLeft[3];
        for (int i = 0; i < 3; ++i)
        {
            const ScreenVertex &P = *cw[(i + 1) % 3], &Q = *cw[(i + 2) % 3];
            float dx = Q.x - P.x, dy = Q.y - P.y;
            topLeft[i] = (dy == 0.0f && dx > 0.0f) || dy < 0.0f;
        }
        // cw[] index of the edge opposite A, B, C.
        const int edgeOf[3] = { 0, area > 0.0f ? 1 : 2, area > 0.0f ? 2 : 1 };
        auto covered = [&](float e, int vertex) { return e > 0.0f || (e == 0.0f && topLeft[edgeOf[vertex]]); };
        int32_t minX = std::max(scx0, int32_t(std::floor(std::min({ A.x, B.x, C.x }))));
        int32_t maxX = std::min(scx1 - 1, int32_t(std::ceil(std::max({ A.x, B.x, C.x }))));
        int32_t minY = std::max(scy0, int32_t(std::floor(std::min({ A.y, B.y, C.y }))));
        int32_t maxY = std::min(scy1 - 1, int32_t(std::ceil(std::max({ A.y, B.y, C.y }))));
        bool persp = !(A.invW == B.invW && B.invW == C.invW);
        float sgn = area > 0.0f ? 1.0f : -1.0f;
        struct Lane { int32_t x, y; bool cov; float b[3], z; };
        auto setupLane = [&](Lane& L, int32_t x, int32_t y) {
            L.x = x; L.y = y;
            float fx = x + 0.5f, fy = y + 0.5f;
            // Edge functions, sign-normalized so inside is positive.
            float e0 = sgn * ((B.x - fx) * (C.y - fy) - (C.x - fx) * (B.y - fy));
            float e1 = sgn * ((C.x - fx) * (A.y - fy) - (A.x - fx) * (C.y - fy));
            float e2 = sgn * ((A.x - fx) * (B.y - fy) - (B.x - fx) * (A.y - fy));
            L.cov = x >= minX && x <= maxX && y >= minY && y <= maxY && covered(e0, 0) && covered(e1, 1) && covered(e2, 2);
            float w0 = e0 * sgn / area, w1 = e1 * sgn / area, w2 = 1.0f - w0 - w1;
            L.b[0] = w0; L.b[1] = w1; L.b[2] = w2;
            if (persp)
            {
                float q0 = w0 * A.invW, q1 = w1 * B.invW, q2 = w2 * C.invW, q = q0 + q1 + q2;
                if (q != 0.0f) { L.b[0] = q0 / q; L.b[1] = q1 / q; L.b[2] = q2 / q; }
            }
            L.z = w0 * A.z + w1 * B.z + w2 * C.z;
        };
        std::string psTrace;
        auto runPs = [&](const Lane& L, PsSink& sink) {
            ShaderInterpreter in(consts, sink, &sampler);
            bool tracePick = L.x - wox == g_pickX && L.y - woy == g_pickY && !sampler.record && psTrace.empty();
            if (tracePick) in.trace = &psTrace;
            memset(in.r, 0, sizeof(float) * 4 * psTemps);
            for (uint32_t k = 0; k < interpCount; ++k)
                for (int j = 0; j < 4; ++j)
                    in.r[k][j] = L.b[0] * A.src->v[k][j] + L.b[1] * B.src->v[k][j] + L.b[2] * C.src->v[k][j];
            if (paramGen && paramGenReg < ShaderInterpreter::kTemps)
            { in.r[paramGenReg][0] = L.x + 0.5f; in.r[paramGenReg][1] = L.y + 0.5f; in.r[paramGenReg][2] = 0; in.r[paramGenReg][3] = 0; }
            if (tracePick)
            {
                char b[96];
                for (uint32_t k = 0; k < interpCount; ++k)
                {
                    snprintf(b, sizeof(b), "    in r%u (%g %g %g %g)\n", k, in.r[k][0], in.r[k][1], in.r[k][2], in.r[k][3]);
                    psTrace += b;
                }
            }
            in.Run(*ps);
            return in.killed;
        };
        bool shade = colorOn || alphaTest;
        for (int32_t qy = minY & ~1; qy <= maxY; qy += 2)
            for (int32_t qx = minX & ~1; qx <= maxX; qx += 2)
            {
                Lane lanes[4];
                bool any = false;
                for (int l = 0; l < 4; ++l) { setupLane(lanes[l], qx + (l & 1), qy + (l >> 1)); any |= lanes[l].cov; }
                if (!any) continue;
                // Texturing shaders: record pass over the quad (helper lanes included) for derivatives.
                PsSink first;
                bool firstKilled = false, haveFirst = false;
                if (shade && psFetches)
                {
                    sampler.BeginQuad();
                    sampler.record = true;
                    sampler.lane = 0;
                    firstKilled = runPs(lanes[0], first);
                    haveFirst = true;
                    if (sampler.needLod)
                        for (int l = 1; l < 4; ++l) { sampler.lane = l; PsSink tmp; runPs(lanes[l], tmp); }
                    sampler.record = false;
                }
                for (int l = 0; l < 4; ++l)
                {
                    const Lane& L = lanes[l];
                    if (!L.cov) continue;
                    int32_t x = L.x, y = L.y;
                    float z = L.z;
                    uint32_t di = depth.Index(x, y, 0);
                    uint32_t stencil = g_edram[di] & 0xFF;
                    bool sPass = !stencilOn || Compare(st.func, float(st.ref & st.mask), float(stencil & st.mask));
                    bool zPass = !zEnable || !sPass || Compare(zFunc, z, DecodeDepth(g_edram[di] >> 8, depthFloat));
                    // A failing pixel only matters if its stencil op writes something.
                    uint32_t failOp = !sPass ? st.fail : st.zfail;
                    bool failWrites = stencilOn && failOp != 0 && st.writeMask;
                    bool picked = x - wox == g_pickX && y - woy == g_pickY;
                    if (picked)
                    {
                        char b[96];
                        snprintf(b, sizeof(b), " PICK-DS(stencil %02X depth %06X: s %d z %d)", stencil, g_edram[di] >> 8, int(sPass), int(zPass));
                        pick += b;
                    }
                    if (!(sPass && zPass) && !failWrites) continue;
                    float out[4] = { 0, 0, 0, 1 }, outs[4][4] = {};
                    uint32_t psWritten = 0;
                    if (shade)
                    {
                        PsSink sink;
                        bool killed;
                        if (haveFirst && l == 0 && !sampler.needLod) { sink = first; killed = firstKilled; }
                        else { sampler.lane = l; killed = runPs(L, sink); }
                        if (dbg.size() < 600)
                        {
                            char b[160];
                            snprintf(b, sizeof(b), " | px(%d,%d) out (%.3f %.3f %.3f %.3f)%s", x, y,
                                     sink.color[0][0], sink.color[0][1], sink.color[0][2], sink.color[0][3], killed ? " killed" : "");
                            dbg += b;
                        }
                        if (picked && (killed || (sink.written & 1)))
                        {
                            char b[96];
                            snprintf(b, sizeof(b), " PICK-PS(%s a %.3f)", killed ? "killed" : "ok", sink.color[0][3]);
                            pick += b;
                        }
                        if (killed) continue;
                        memcpy(outs, sink.color, sizeof(outs));
                        psWritten = sink.written;
                        if (!(sink.written & 1)) { if (!zWrite && !stencilOn) continue; }
                        else memcpy(out, sink.color[0], sizeof(out));
                        if (alphaTest && !Compare(alphaFunc, out[3], alphaRef)) continue;
                    }
                    // Depth/stencil update: the stencil op for the test outcome,
                    // depth only when both tests pass.
                    bool pass = sPass && zPass;
                    uint32_t dv = g_edram[di];
                    if (stencilOn)
                    {
                        uint32_t ns = StencilOp(pass ? st.zpass : failOp, stencil, st.ref);
                        dv = (dv & ~0xFFu) | ((stencil & ~st.writeMask) | (ns & st.writeMask));
                    }
                    if (pass && zEnable && zWrite) dv = (EncodeDepth(z, depthFloat) << 8) | (dv & 0xFF);
                    if (dv != g_edram[di])
                        for (uint32_t s = 0; s < depth.Samples(); ++s) g_edram[depth.Index(x, y, s)] = dv;
                    if (picked)
                    {
                        char b[64];
                        snprintf(b, sizeof(b), " PICK-DS-> %08X;", dv);
                        pick += b;
                    }
                    if (!pass) continue;
                    ++g_stats.pixels;
                    if (!colorOn) continue;
                    if (picked && !psTrace.empty())
                    {
                        // The pixel shader's constant window (non-zero entries), so a trace's cN operands can be read.
                        std::string cs = "    ps consts:";
                        for (uint32_t i = 0; i < consts.psSize && consts.psBase + i < 512; ++i)
                        {
                            const float* c = consts.alu[consts.psBase + i];
                            if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) continue;
                            char b[120];
                            snprintf(b, sizeof(b), " c%u(%g %g %g %g)", i, c[0], c[1], c[2], c[3]);
                            cs += b;
                        }
                        pick += " PICK-TRACE{\n" + cs + "\n" + psTrace + "}";
                        psTrace = " ";
                    }
                    for (uint32_t rt = 0; rt < 4; ++rt)
                    {
                        const RtState& T = rts[rt];
                        if (!T.on || !(psWritten & (1u << rt))) continue;
                        float src[4], dst[4], res[4];
                        for (int i = 0; i < 4; ++i) src[i] = outs[rt][i] * T.scale;
                        ClampRt(T.fmt, src);
                        DecodeRt(T.fmt, &g_edram[T.s.Index(x, y, 0)], dst, true);
                        for (int i = 0; i < 4; ++i)
                        {
                            bool a = i == 3;
                            uint32_t sf = Bits(T.blend, a ? 16 : 0, 5), op = Bits(T.blend, a ? 21 : 5, 3), df = Bits(T.blend, a ? 24 : 8, 5);
                            res[i] = BlendOp(op, src[i] * BlendFactor(sf, src, dst, blendConst, i), dst[i] * BlendFactor(df, src, dst, blendConst, i));
                            if (!(T.mask & (1u << i))) res[i] = dst[i];
                        }
                        ClampRt(T.fmt, res);
                        uint32_t pv[2] = { 0, 0 };
                        EncodeRt(T.fmt, res, pv, true);
                        for (uint32_t smp = 0; smp < T.s.Samples(); ++smp)
                        {
                            uint32_t k = T.s.Index(x, y, smp);
                            g_edram[k] = pv[0];
                            if (T.s.wide) g_edram[k + 1] = pv[1];
                        }
                        if (rt == 0 && picked)
                        {
                            char b[160];
                            snprintf(b, sizeof(b), " PICK(%d,%d) rt base %u: ps out (%.3f %.3f %.3f %.3f) -> %08X;", g_pickX, g_pickY,
                                     T.s.base, src[0], src[1], src[2], src[3], pv[0]);
                            pick += b;
                        }
                    }
                }
            }
    }
    if (note)
    {
        char b[160];
        snprintf(b, sizeof(b), "rt base %u pitch %u msaa %u fmt %u mask %X colorOn %d z %d/%d f%u interp %u;",
                 color.base, color.pitch, color.msaa, rts[0].fmt, colorMask, int(colorOn), int(zEnable), int(zWrite), zFunc, interpCount);
        *note = b + pick + dbg;
    }
}

}  // namespace xenos
