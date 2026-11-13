#include "xe_glsl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hc09vk {

namespace {

using xenos::ShaderCode;
using xenos::ShaderType;

inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }

struct Cf { uint32_t d0, d1; uint32_t op() const { return Bits(d1, 12, 4); } };
Cf GetCf(const std::vector<uint32_t>& dw, uint32_t index)
{
    size_t p = 3 * (index >> 1);
    if (p + 2 >= dw.size()) return { 0, 0 };
    if (index & 1) return { (dw[p + 1] >> 16) | (dw[p + 2] << 16), dw[p + 2] >> 16 };
    return { dw[p], dw[p + 1] & 0xFFFF };
}
bool CfIsExec(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
bool CfEnds(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }
uint32_t CfCount(const std::vector<uint32_t>& dw)
{
    uint32_t limit = 2 * uint32_t(dw.size() / 3);
    for (uint32_t cfi = 0; cfi < limit; ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        if (CfIsExec(cf.op()))
        {
            limit = std::min(limit, 2 * Bits(cf.d0, 0, 12));
            if (CfEnds(cf.op())) return std::min(limit, cfi + 1);
        }
    }
    return limit;
}

// Operand components used per vector opcode (src1, src2, src3) — as the interpreter.
const uint8_t kVecOperands[32][3] = {
    { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 },
    { 15, 0, 0 },  { 15, 0, 0 },  { 15, 0, 0 },  { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 },
    { 15, 15, 0 }, { 7, 7, 0 },   { 3, 3, 1 },   { 15, 15, 0 }, { 15, 0, 0 },  { 9, 9, 0 },  { 9, 9, 0 },  { 9, 9, 0 },
    { 9, 9, 0 },   { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 6, 10, 0 }, { 15, 15, 0 }, { 0, 0, 0 },
    { 0, 0, 0 } };
const uint8_t kScaOperands[64] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 2, 2, 2, 2, 2, 2, 1, 1, 0 };
const bool kScaTwoComponent[64] = { true, false, true, false, true, true, true, false, false, false, false, false, false,
                                    false, false, false, false, false, false, false, false, false, false, true, true, true };

inline uint32_t SwizzledComponent(uint32_t swz, uint32_t i) { return ((swz >> (2 * i)) + i) & 3; }
const char kXyzw[] = "xyzw";

// Correctly rounded float operations, as the software rasterizer's C++
// computes them. Vulkan's fp32 division/sqrt are only accurate to ~2.5 ulp
// (NVIDIA: 3/255 comes out one ulp high) and its exp2/log2/sin/cos far less,
// fma() may be unfused; xenos_shader.h evaluates exp2/log2/sin/cos in double
// and rounds once. So these are evaluated in double (series after range
// reduction for the transcendentals) and rounded once: exact except when the
// true value lies within ~1e-16 of a float rounding midpoint.
const char* kExactMath = R"GLSL(
// Double constants from their bit patterns (decimal double literals are not
// reliably parsed to the nearest double).
#define XD(lo, hi) packDouble2x32(uvec2(lo, hi))
#define XD_LN2       XD(0xFEFA39EFu, 0x3FE62E42u)
#define XD_INV_LN2   XD(0x652B82FEu, 0x3FF71547u)
#define XD_SQRT_HALF XD(0x667F3BCDu, 0x3FE6A09Eu)
#define XD_2_OVER_PI XD(0x6DC9C883u, 0x3FE45F30u)
#define XD_PIO2_1    XD(0x54000000u, 0x3FF921FBu)
#define XD_PIO2_2    XD(0x10000000u, 0x3E110B46u)
#define XD_PIO2_3    XD(0x30000000u, 0x3C5A6263u)
#define XD_PIO2_4    XD(0xE0000000u, 0x3A945C06u)
// a / b correctly rounded: the fp32 quotient (within 2.5 ulp), then the
// neighbour (+-2 ulp) whose exact residual |a - c * b| is smallest (products of
// two floats and their difference with a are exact in double; ties to even).
// Specials and tiny/huge quotients take a double division.
float xdiv(float a, float b)
{
    float q = a / b;
    float aq = abs(q);
    if (!(aq > 1e-30 && aq < 1e30) || isinf(a) || isinf(b)) return float(double(a) / double(b));
    precise double da = double(a), db = double(b);
    uint u = floatBitsToUint(q);
    float best = q;
    precise double rb = abs(da - double(q) * db);
    for (int k = -2; k <= 2; ++k)
    {
        if (k == 0) continue;
        float c = uintBitsToFloat(uint(int(u) + k));
        precise double r = abs(da - double(c) * db);
        if (r < rb || (r == rb && (floatBitsToUint(c) & 1u) == 0u)) { best = c; rb = r; }
    }
    return best;
}
float xsqrtf(float a) { return float(sqrt(double(a))); }
float xexp2(float x)
{
    if (isnan(x)) return x;
    if (x >= 128.0) return uintBitsToFloat(0x7F800000u);
    if (x <= -160.0) return 0.0;
    precise double n = floor(double(x) + 0.5LF);
    precise double t = (double(x) - n) * XD_LN2;   // |t| <= ln2 / 2
    precise double s = 1.0LF, term = 1.0LF;
    for (int k = 1; k <= 18; ++k) { term *= t * kInvInt[k]; s += term; }
    return float(ldexp(s, int(n)));
}
float xlog2(float x)
{
    if (isnan(x) || x < 0.0) return uintBitsToFloat(0x7FC00000u);
    if (x == 0.0) return uintBitsToFloat(0xFF800000u);
    if (isinf(x)) return x;
    int e;
    precise double m = frexp(double(x), e);   // [0.5, 1)
    if (m < XD_SQRT_HALF) { m *= 2.0LF; e -= 1; }
    precise double z = (m - 1.0LF) / (m + 1.0LF), z2 = z * z, s = 0.0LF, p = z;
    for (int k = 0; k < 16; ++k) { s += p * kInvInt[2 * k + 1]; p *= z2; }
    precise double l = double(e) + 2.0LF * s * XD_INV_LN2;
    return float(l);
}
// sin (cos = false) or cos of x: r = x - k * pi/2 with pi/2 in four 28-bit
// parts (each k * part exact for |x| < 2^24), Taylor on [-pi/4, pi/4].
float xsincos(float xf, bool cosine)
{
    if (isnan(xf) || isinf(xf)) return uintBitsToFloat(0x7FC00000u);
    double x = double(xf);
    precise double k = floor(x * XD_2_OVER_PI + 0.5LF);
    precise double r = (((x - k * XD_PIO2_1) - k * XD_PIO2_2) - k * XD_PIO2_3) - k * XD_PIO2_4;
    int q = int(k - 4.0LF * floor(k * 0.25LF)) + (cosine ? 1 : 0);
    precise double r2 = r * r, sn = r, cs = 1.0LF, ts = r, tc = 1.0LF;
    for (int i = 1; i <= 11; ++i)
    {
        ts *= -r2 * (kInvInt[2 * i] * kInvInt[2 * i + 1]); sn += ts;
        tc *= -r2 * (kInvInt[2 * i - 1] * kInvInt[2 * i]); cs += tc;
    }
    precise double v = (q & 1) != 0 ? cs : sn;
    return float((q & 2) != 0 ? -v : v);
}
float xsin(float x) { return xsincos(x, false); }
float xcos(float x) { return xsincos(x, true); }
)GLSL";

// Tables for kExactMath and the EDRAM library: 1/k in double (series
// coefficients, no divisions), and the unorm decodes v / 255, v / 1023, v / 3
// as float bits, computed here by the CPU's own expression.
std::string ExactTables()
{
    std::string t = "const double kInvInt[40] = double[40](0.0LF";
    char b[64];
    for (int k = 1; k < 40; ++k)
    {
        snprintf(b, sizeof(b), ", %.17eLF", 1.0 / double(k));
        t += b;
    }
    t += ");\n";
    auto table = [&](const char* name, int n, float d) {
        t += std::string("const uint ") + name + "[" + std::to_string(n) + "] = uint[" + std::to_string(n) + "](";
        for (int i = 0; i < n; ++i)
        {
            float f = float(i) / d;
            uint32_t u;
            memcpy(&u, &f, 4);
            snprintf(b, sizeof(b), "%s0x%08Xu", i ? ", " : "", u);
            t += b;
        }
        t += ");\n";
    };
    table("kU8Bits", 256, 255.0f);
    table("kU10Bits", 1024, 1023.0f);
    table("kU2Bits", 4, 3.0f);
    return t;
}

// kExactMath with its tables in front (the functions reference them).
// `fast`: the GPU's own fp32 operations instead (TranslateOptions::fastMath).
std::string ExactMath(bool fast = false)
{
    std::string m = kExactMath;
    size_t at = m.find("float xdiv(");
    if (fast)
        return m.substr(0, at) + ExactTables() + R"GLSL(
float xdiv(float a, float b) { return a / b; }
float xsqrtf(float a) { return sqrt(a); }
float xexp2(float x) { return exp2(x); }
float xlog2(float x) { return log2(x); }
float xsin(float x) { return sin(x); }
float xcos(float x) { return cos(x); }
)GLSL";
    return m.substr(0, at) + ExactTables() + m.substr(at);
}

// Shared by both stages.
const char* kPrelude = R"GLSL(#version 450
layout(std430, set = 0, binding = 0) readonly buffer GuestMemory { uint mem[]; };
layout(std430, set = 0, binding = 1) readonly buffer DrawConstsBlock {
    vec4 c[512];
    uint bools[8];
    uint loops[32];
    uint fetch[192];
    vec4 vpScale;
    vec4 vpOffset;
    vec4 rtSize;
    uint vte;
    uint interpCount;
    uint paramGen;
    uint alpha;
    uint vsBase;
    uint vsSize;
    uint psBase;
    uint psSize;
    float alphaRef;
    float fragScale;
    float wideClip;
    float pad2;
    vec4 rtScale;
    vec4 texSize[32];
    uvec4 rtBase;
    uvec4 rtFmt;
    uvec4 rtBlend;
    uvec4 rtMask;
    uvec4 surf;
    uvec4 ds;
    vec4 blendConst;
    uvec4 clip;
    vec4 winOfs;
} D;
layout(set = 0, binding = 2) uniform sampler2D tex2d[32];
layout(set = 0, binding = 4) uniform sampler3D tex3d[32];
layout(set = 0, binding = 5) uniform sampler2DArray texCube[32];
layout(set = 0, binding = 6) uniform usampler2D utex2d[32];          // the same textures as RGBA8_UINT
layout(set = 0, binding = 7) uniform usampler3D utex3d[32];
layout(set = 0, binding = 8) uniform usampler2DArray utexCube[32];
// Texture kind per fetch slot (2 bits: 0 2D, 1 3D, 2 cube, 3 stacked = 2D array) as specialization
// constants, so each pipeline keeps only the sampling code of the kinds bound.
layout(constant_id = 0) const uint kTexKindsLo = 0u;   // slots 0-15
layout(constant_id = 1) const uint kTexKindsHi = 0u;   // slots 16-31
uint xeKind(int slot) { return slot < 16 ? (kTexKindsLo >> (2 * slot)) & 3u : (kTexKindsHi >> (2 * (slot - 16))) & 3u; }

// Xenos multiply: 0 * anything (inf, NaN) = 0.
float xmul(float a, float b) { precise float m = a * b; return (a != 0.0 && b != 0.0) ? m : 0.0; }
vec4 xmul4(vec4 a, vec4 b) { return vec4(xmul(a.x, b.x), xmul(a.y, b.y), xmul(a.z, b.z), xmul(a.w, b.w)); }
float sat(float f) { return f > 1.0 ? 1.0 : (f >= 0.0 ? f : 0.0); }   // NaN -> 0
vec4 sat4(vec4 v) { return vec4(sat(v.x), sat(v.y), sat(v.z), sat(v.w)); }
// Interpreter: isgreaterequal(a, b) ? a : b and isless(a, b) ? a : b (NaN -> b);
// isnan keeps the driver from folding them into min/max instructions.
float xmax(float a, float b) { return (!isnan(a) && !isnan(b) && a >= b) ? a : b; }
float xmin(float a, float b) { return (!isnan(a) && !isnan(b) && a < b) ? a : b; }
vec4 xmax4(vec4 a, vec4 b) { return vec4(xmax(a.x, b.x), xmax(a.y, b.y), xmax(a.z, b.z), xmax(a.w, b.w)); }
vec4 xmin4(vec4 a, vec4 b) { return vec4(xmin(a.x, b.x), xmin(a.y, b.y), xmin(a.z, b.z), xmin(a.w, b.w)); }
vec4 sel4(bvec4 c, vec4 a, vec4 b) { return vec4(c.x ? a.x : b.x, c.y ? a.y : b.y, c.z ? a.z : b.z, c.w ? a.w : b.w); }
uint boolBit(uint a) { return (D.bools[(a >> 5) & 7u] >> (a & 31u)) & 1u; }

uint cBase, cSize;
uint cPrev = 0u;   // hc09: temporal upscaling (below)
vec4 cnst(int i) { return (i < 0 || uint(i) >= cSize || cBase + uint(i) >= 512u) ? vec4(0.0) : D.c[cBase + uint(i)]; }
// hc09: temporal upscaling (vk_fsr3.h): the draw's motion. The vertex shader runs a second time with the vertex
// constants of the frame before (cPrev: how many draws on they are; DrawIndexedConsts), and the pixel shader writes
// where each pixel was then (attachment 4, the motion target).
layout(constant_id = 2) const uint kMotion = 0u;

int loopDepth = 0;
uint loopIter[4];
uint loopConst[4];
int aL()
{
    if (loopDepth <= 0 || loopDepth > 4) return 0;
    uint lc = loopConst[loopDepth - 1];
    int v = int(loopIter[loopDepth - 1]) * bitfieldExtract(int(lc), 16, 8) + int((lc >> 8) & 0xFFu);
    return clamp(v, -256, 256);
}

bool p = false;
int a0 = 0;
float ps = 0.0;
bool killed = false;
float xeRegLod = 0.0;                         // setTexLOD
vec3 xeGradH = vec3(0.0), xeGradV = vec3(0.0);   // setGradientsH / setGradientsV
)GLSL";

std::string Prelude(bool fast) { return std::string(kPrelude) + ExactMath(fast); }

const char* kVertexFetch = R"GLSL(
uint swapE(uint v, uint e)
{
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
float xhalf(uint h)   // Xenos half: no inf/NaN, denormals flush to 0
{
    uint m = h & 0x3FFu, e = (h >> 10) & 0x1Fu, s = (h & 0x8000u) << 16;
    return e == 0u ? uintBitsToFloat(s) : uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13));
}
uint vfConst = 0u, vfStride = 0u, vfAddr = 0u;
float vfPacked(uint word, uint w, uint o, bool sgn, bool norm, bool noZero)
{
    if (sgn)
    {
        float v = float(bitfieldExtract(int(word), int(o), int(w)));
        if (norm) { precise float t = (v + 0.5) * 2.0; v = noZero ? xdiv(t, float((1u << w) - 1u)) : max(-1.0, xdiv(v, float((1u << (w - 1u)) - 1u))); }
        return v;
    }
    float v = float(bitfieldExtract(word, int(o), int(w)));
    return norm ? xdiv(v, float((1u << w) - 1u)) : v;
}
vec4 vfetch(uint a, uint fmt, bool sgn, bool norm, bool noZero, int expAdj)
{
    uint d0 = D.fetch[vfConst * 2u], d1 = D.fetch[vfConst * 2u + 1u];
    uint bufStart = d0 >> 2, bufEnd = bufStart + ((d1 >> 2) & 0xFFFFFFu), endian = d1 & 3u;
    uint data[4];
    for (uint k = 0u; k < 4u; ++k)
    {
        uint ad = a + k;
        data[k] = (ad >= bufStart && ad < bufEnd) ? swapE(mem[ad & 0x7FFFFFFu], endian) : 0u;
    }
    vec4 r = vec4(0.0);
    switch (fmt)
    {
    case 6u:  for (uint k = 0u; k < 4u; ++k) r[k] = vfPacked(data[0], 8u, 8u * k, sgn, norm, noZero); break;
    case 7u:  r.x = vfPacked(data[0], 10u, 0u, sgn, norm, noZero); r.y = vfPacked(data[0], 10u, 10u, sgn, norm, noZero);
              r.z = vfPacked(data[0], 10u, 20u, sgn, norm, noZero); r.w = vfPacked(data[0], 2u, 30u, sgn, norm, noZero); break;
    case 16u: r.x = vfPacked(data[0], 11u, 0u, sgn, norm, noZero); r.y = vfPacked(data[0], 11u, 11u, sgn, norm, noZero);
              r.z = vfPacked(data[0], 10u, 22u, sgn, norm, noZero); break;
    case 17u: r.x = vfPacked(data[0], 10u, 0u, sgn, norm, noZero); r.y = vfPacked(data[0], 11u, 10u, sgn, norm, noZero);
              r.z = vfPacked(data[0], 11u, 21u, sgn, norm, noZero); break;
    case 25u: r.x = vfPacked(data[0], 16u, 0u, sgn, norm, noZero); r.y = vfPacked(data[0], 16u, 16u, sgn, norm, noZero); break;
    case 26u: r.x = vfPacked(data[0], 16u, 0u, sgn, norm, noZero); r.y = vfPacked(data[0], 16u, 16u, sgn, norm, noZero);
              r.z = vfPacked(data[1], 16u, 0u, sgn, norm, noZero); r.w = vfPacked(data[1], 16u, 16u, sgn, norm, noZero); break;
    case 32u: r.w = xhalf(data[1] >> 16); r.z = xhalf(data[1] & 0xFFFFu);
              r.y = xhalf(data[0] >> 16); r.x = xhalf(data[0] & 0xFFFFu); break;
    case 31u: r.y = xhalf(data[0] >> 16); r.x = xhalf(data[0] & 0xFFFFu); break;
    case 33u: case 34u: case 35u:
        for (uint k = 0u; k < 4u; ++k)
        {
            if (sgn) { r[k] = float(int(data[k])); if (norm) r[k] = noZero ? xdiv(r[k] + 0.5, 2147483647.5) : xdiv(r[k], 2147483647.0); }
            else { r[k] = float(data[k]); if (norm) r[k] = xdiv(r[k], 4294967295.0); }
        }
        break;
    case 36u: case 37u: case 38u: case 57u: for (uint k = 0u; k < 4u; ++k) r[k] = uintBitsToFloat(data[k]); break;
    default: break;
    }
    if (expAdj != 0) r = ldexp(r, ivec4(expAdj));
    return r;
}
)GLSL";

// EDRAM model and render-target arithmetic of runtime/xenos/xenos_raster.cpp,
// operation for operation (Surface::Index, DecodeRt/EncodeRt/ClampRt,
// DecodeDepth/EncodeDepth, Compare, StencilOp, BlendFactor/BlendOp), so the
// GPU stores the bits the software stores. Divisions are corrected to the
// correctly rounded quotient (the hardware's is approximate); std::min/max
// semantics (NaN picks the first operand) are spelled out.
const char* kEdramLib = R"GLSL(
float xsat(float f) { return f > 1.0 ? 1.0 : (f >= 0.0 ? f : 0.0); }
// std::min/max: a NaN operand gives the first operand. Spelled out with
// isnan, or the driver folds the selects into min/max instructions, which
// return the other operand.
float stdmin(float a, float b) { return (isnan(a) || isnan(b) || !(b < a)) ? a : b; }
float stdmax(float a, float b) { return (isnan(a) || isnan(b) || !(a < b)) ? a : b; }
uint lroundU(float v) { float f = floor(v); return uint(f) + (v - f >= 0.5 ? 1u : 0u); }   // v >= 0
int lroundI(float v) { return v < 0.0 ? -int(lroundU(-v)) : int(lroundU(v)); }
uint unorm(float f, uint bits) { precise float v = xsat(f) * float((1u << bits) - 1u); return lroundU(v); }

uint edramIndex(uint base, uint pitch, uint msaa, bool wide, uint x, uint y, uint s)
{
    uint sw = msaa == 2u ? 2u : 1u, sh = msaa >= 1u ? 2u : 1u;
    uint sx = x * sw + (msaa == 2u ? (s & 1u) : 0u);
    uint sy = y * sh + (msaa == 2u ? (s >> 1) : (msaa == 1u ? (s & 1u) : 0u));
    uint pitchTiles = (pitch * sw + 79u) / 80u * (wide ? 2u : 1u);
    uint tw = wide ? 40u : 80u;
    uint tile = (base + (sy / 16u) * pitchTiles + sx / tw) % 2048u;
    return tile * 1280u + (sy % 16u) * 80u + (sx % tw) * (wide ? 2u : 1u);
}
uint edramSamples(uint msaa) { return msaa == 2u ? 4u : msaa == 1u ? 2u : 1u; }
bool is64bpp(uint fmt) { return fmt == 5u || fmt == 7u || fmt == 15u; }

float f7e3(uint v)
{
    uint m = v & 0x7Fu, e = (v >> 7) & 7u;
    return e != 0u ? ldexp(float(m | 0x80u), int(e) - 10) : ldexp(float(m), -9);
}
uint to7e3(float f)
{
    if (!(f > 0.0)) return 0u;
    if (f >= 31.875) return 0x3FFu;
    if (f < 0.25) { precise float d = f * 512.0; return lroundU(d); }
    int e;
    float m = frexp(f, e);
    precise float t = (m * 2.0 - 1.0) * 128.0;
    uint mant = lroundU(t);
    uint ex = uint(e - 1 + 3);
    if (mant == 128u) { mant = 0u; ++ex; }
    return ex > 7u ? 0x3FFu : (ex << 7) | mant;
}
float pwlToLinear(float g)
{
    precise float v = g * 255.0;
    precise float l = v < 64.0 ? v : v < 96.0 ? v * 2.0 - 64.0 : v < 192.0 ? v * 4.0 - 256.0 : v * 8.0 - 1024.0;
    return stdmin(1.0, xdiv(l, 1023.0));
}
float linearToPwl(float l)
{
    precise float v = xsat(l) * 1023.0;
    precise float g = v < 64.0 ? trunc(v) : v < 128.0 ? trunc(v * 0.5) + 32.0
                    : v < 512.0 ? trunc(v * 0.25) + 64.0 : trunc(v * 0.125) + 128.0;
    return xdiv(g, 255.0);
}
float fixed16(uint v)
{
    int s = bitfieldExtract(int(v), 0, 16);
    precise float f = stdmax(-1.0, xdiv(float(s), 32767.0)) * 32.0;
    return f;
}
uint toFixed16(float f)
{
    precise float t = stdmin(32.0, stdmax(-32.0, f)) / 32.0 * 32767.0;
    return uint(lroundI(t)) & 0xFFFFu;
}
float halfToFloat(uint h)
{
    uint e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float f;
    if (e == 0u) f = ldexp(float(m), -24);
    else if (e == 31u) f = m != 0u ? uintBitsToFloat(0x7FC00000u) : uintBitsToFloat(0x7F800000u);
    else f = ldexp(float(m | 0x400u), int(e) - 25);
    return (h & 0x8000u) != 0u ? -f : f;
}
uint floatToHalf(float f)
{
    uint sign = (floatBitsToUint(f) >> 31) != 0u ? 0x8000u : 0u;
    float a = abs(f);
    if (isnan(a)) return sign | 0x7E00u;
    if (a >= 65520.0) return sign | 0x7C00u;
    if (a < ldexp(1.0, -14)) { precise float d = a * ldexp(1.0, 24); return sign | lroundU(d); }
    int e;
    float m = frexp(a, e);
    precise float t = (m * 2.0 - 1.0) * 1024.0;
    uint mant = lroundU(t);
    int ex = e - 1 + 15;
    if (mant == 1024u) { mant = 0u; ++ex; }
    if (ex >= 31) return sign | 0x7C00u;
    return sign | (uint(ex) << 10) | mant;
}

// Sample -> RGBA. `linear`: 8888_GAMMA converted to linear (for blending); raw otherwise (resolves).
vec4 decodeRt(uint fmt, uint p0, uint p1, bool linear)
{
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    switch (fmt)
    {
    case 0u: case 1u:
        for (int i = 0; i < 4; ++i) c[i] = uintBitsToFloat(kU8Bits[(p0 >> (8 * i)) & 0xFFu]);
        if (fmt == 1u && linear) for (int i = 0; i < 3; ++i) c[i] = pwlToLinear(c[i]);
        break;
    case 2u: case 10u:
        for (int i = 0; i < 3; ++i) c[i] = uintBitsToFloat(kU10Bits[(p0 >> (10 * i)) & 0x3FFu]);
        c.w = uintBitsToFloat(kU2Bits[p0 >> 30]);
        break;
    case 3u: case 12u:
        for (int i = 0; i < 3; ++i) c[i] = f7e3(p0 >> (10 * i));
        c.w = uintBitsToFloat(kU2Bits[p0 >> 30]);
        break;
    case 4u: c = vec4(fixed16(p0), fixed16(p0 >> 16), 0.0, 1.0); break;
    case 5u: c = vec4(fixed16(p0), fixed16(p0 >> 16), fixed16(p1), fixed16(p1 >> 16)); break;
    case 6u: c = vec4(halfToFloat(p0 & 0xFFFFu), halfToFloat(p0 >> 16), 0.0, 1.0); break;
    case 7u: c = vec4(halfToFloat(p0 & 0xFFFFu), halfToFloat(p0 >> 16), halfToFloat(p1 & 0xFFFFu), halfToFloat(p1 >> 16)); break;
    case 14u: c = vec4(uintBitsToFloat(p0), 0.0, 0.0, 1.0); break;
    case 15u: c = vec4(uintBitsToFloat(p0), uintBitsToFloat(p1), 0.0, 1.0); break;
    default: break;
    }
    return c;
}
uvec2 encodeRt(uint fmt, vec4 c, bool linear)
{
    uvec2 p = uvec2(0u);
    switch (fmt)
    {
    case 0u: case 1u:
        for (int i = 0; i < 4; ++i)
        {
            float v = (fmt == 1u && linear && i < 3) ? linearToPwl(c[i]) : c[i];
            p.x |= unorm(v, 8u) << (8 * i);
        }
        break;
    case 2u: case 10u: p.x = unorm(c.x, 10u) | (unorm(c.y, 10u) << 10) | (unorm(c.z, 10u) << 20) | (unorm(c.w, 2u) << 30); break;
    case 3u: case 12u: p.x = to7e3(c.x) | (to7e3(c.y) << 10) | (to7e3(c.z) << 20) | (unorm(c.w, 2u) << 30); break;
    case 4u: p.x = toFixed16(c.x) | (toFixed16(c.y) << 16); break;
    case 5u: p.x = toFixed16(c.x) | (toFixed16(c.y) << 16); p.y = toFixed16(c.z) | (toFixed16(c.w) << 16); break;
    case 6u: p.x = floatToHalf(c.x) | (floatToHalf(c.y) << 16); break;
    case 7u: p.x = floatToHalf(c.x) | (floatToHalf(c.y) << 16); p.y = floatToHalf(c.z) | (floatToHalf(c.w) << 16); break;
    case 14u: p.x = floatBitsToUint(c.x); break;
    case 15u: p.x = floatBitsToUint(c.x); p.y = floatBitsToUint(c.y); break;
    default: break;
    }
    return p;
}
vec4 clampRt(uint fmt, vec4 c)
{
    switch (fmt)
    {
    case 0u: case 1u: case 2u: case 10u: for (int i = 0; i < 4; ++i) c[i] = xsat(c[i]); break;
    case 3u: case 12u: for (int i = 0; i < 3; ++i) c[i] = stdmin(31.875, stdmax(0.0, c[i])); c.w = xsat(c.w); break;
    case 4u: case 5u: for (int i = 0; i < 4; ++i) c[i] = stdmin(32.0, stdmax(-32.0, c[i])); break;
    default: break;
    }
    return c;
}
)GLSL"
R"GLSL(
float decodeDepth(uint d24, bool isFloat)
{
    if (!isFloat) return xdiv(float(d24), 16777215.0);
    uint m = d24 & 0xFFFFFu, e = d24 >> 20;
    return e != 0u ? ldexp(float(m | 0x100000u), int(e) - 35) : ldexp(float(m), -34);
}
uint encodeDepth(float z, bool isFloat)
{
    z = xsat(z);
    if (!isFloat) { precise float d = z * 16777215.0; return uint(d); }
    if (z < ldexp(1.0, -14)) return lroundU(ldexp(z, 34));
    int e;
    float m = frexp(z, e);
    precise float t = (m * 2.0 - 1.0) * 1048576.0;
    uint mant = lroundU(t);
    uint ex = uint(e - 1 + 15);
    if (mant == 1048576u) { mant = 0u; ++ex; }
    return ex > 15u ? 0xFFFFFFu : (ex << 20) | mant;
}
bool xcompare(uint func, float a, float b)
{
    switch (func)
    {
    case 0u: return false; case 1u: return a < b; case 2u: return a == b; case 3u: return a <= b;
    case 4u: return a > b; case 5u: return a != b; case 6u: return a >= b; default: return true;
    }
}
uint stencilOp(uint op, uint s, uint ref)
{
    switch (op)
    {
    case 0u: return s;
    case 1u: return 0u;
    case 2u: return ref;
    case 3u: return s < 255u ? s + 1u : 255u;
    case 4u: return s > 0u ? s - 1u : 0u;
    case 5u: return ~s & 0xFFu;
    case 6u: return (s + 1u) & 0xFFu;
    default: return (s - 1u) & 0xFFu;
    }
}
float blendFactor(uint f, vec4 s, vec4 d, vec4 k, int i)
{
    switch (f)
    {
    case 0u: return 0.0; case 1u: return 1.0;
    case 4u: return s[i]; case 5u: return 1.0 - s[i];
    case 6u: return s.w; case 7u: return 1.0 - s.w;
    case 8u: return d[i]; case 9u: return 1.0 - d[i];
    case 10u: return d.w; case 11u: return 1.0 - d.w;
    case 12u: return k[i]; case 13u: return 1.0 - k[i];
    case 14u: return k.w; case 15u: return 1.0 - k.w;
    case 16u: return i == 3 ? 1.0 : stdmin(s.w, 1.0 - d.w);
    default: return 0.0;
    }
}
float blendOp(uint op, float a, float b)
{
    switch (op) { case 1u: return a - b; case 2u: return stdmin(a, b); case 3u: return stdmax(a, b); case 4u: return b - a; default: return a + b; }
}
// One blended component (RB_BLENDCONTROL `blend`), as RasterDraw.
float blendChannel(uint blend, vec4 src, vec4 dst, vec4 k, int i)
{
    bool a = i == 3;
    uint sf = bitfieldExtract(blend, a ? 16 : 0, 5), op = bitfieldExtract(blend, a ? 21 : 5, 3);
    uint df = bitfieldExtract(blend, a ? 24 : 8, 5);
    precise float sv = src[i] * blendFactor(sf, src, dst, k, i);
    precise float dv = dst[i] * blendFactor(df, src, dst, k, i);
    return blendOp(op, sv, dv);
}
// Resolve packing (xenos_raster.cpp PackResolve).
uvec2 packResolve(uint destFmt, uint srcFmt, vec4 c)
{
    uvec2 v = uvec2(0u);
    switch (destFmt)
    {
    case 2u: v.x = unorm(c.x, 8u); break;
    case 4u: v.x = unorm(c.x, 5u) | (unorm(c.y, 6u) << 5) | (unorm(c.z, 5u) << 11); break;
    case 10u: v.x = unorm(c.x, 8u) | (unorm(c.y, 8u) << 8); break;
    case 24u: v.x = (srcFmt == 4u || srcFmt == 5u) ? toFixed16(c.x) : unorm(c.x, 16u); break;
    case 6u: for (int i = 0; i < 4; ++i) v.x |= unorm(c[i], 8u) << (8 * i); break;
    case 7u: v.x = unorm(c.x, 10u) | (unorm(c.y, 10u) << 10) | (unorm(c.z, 10u) << 20) | (unorm(c.w, 2u) << 30); break;
    case 25u: v.x = toFixed16(c.x) | (toFixed16(c.y) << 16); break;
    case 26u: v.x = toFixed16(c.x) | (toFixed16(c.y) << 16); v.y = toFixed16(c.z) | (toFixed16(c.w) << 16); break;
    case 31u: v.x = floatToHalf(c.x) | (floatToHalf(c.y) << 16); break;
    case 32u: v.x = floatToHalf(c.x) | (floatToHalf(c.y) << 16); v.y = floatToHalf(c.z) | (floatToHalf(c.w) << 16); break;
    case 36u: v.x = floatBitsToUint(c.x); break;
    case 37u: v.x = floatBitsToUint(c.x); v.y = floatBitsToUint(c.y); break;
    default: break;
    }
    return v;
}
)GLSL";

// Texture sampling of the software rasterizer (xenos_raster.cpp QuadSampler),
// operation for operation, on the RGBA8_UINT views of the same decoded
// textures: texelFetch, byte / 255 as the CPU computes it (table), the
// software's wrap modes and float bilinear weights, LOD from the quad's coarse
// derivatives (lane 1 - lane 0, lane 2 - lane 0, as the software's record
// pass), its mip selection and blending, 3D as two bilinear slices + linear
// between, cube faces as layers. The hardware samplers' fixed-point weights
// and LOD differ from it by +-1 in most filtered texels (HC09_GPU_HWSAMPLER=1).
const char* kExactSamplerA = R"GLSL(
int xwrap(int v, int n, uint mode)
{
    if (mode == 0u) return v >= 0 ? v % n : n - 1 - ((-v - 1) % n);
    if (mode == 1u) { int p = 2 * n; int m = v >= 0 ? v % p : p - 1 - ((-v - 1) % p); return m < n ? m : p - 1 - m; }
    if (mode == 3u || mode == 5u || mode == 7u) { int m = v < 0 ? -v - 1 : v; return m >= n ? n - 1 : m; }   // mirror once
    if (mode == 6u) return v < 0 || v >= n ? -1 : v;   // border
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
}
vec4 xeTexel(uint kind, int slot, int x, int y, int layer, int level)
{
    if (x < 0 || y < 0 || layer < 0) return (D.fetch[slot * 6 + 5] & 3u) == 1u ? vec4(1.0) : vec4(0.0);   // border colour
    uvec4 t = kind == 1u ? texelFetch(utex3d[slot], ivec3(x, y, layer), 0)
            : kind >= 2u ? texelFetch(utexCube[slot], ivec3(x, y, layer), level) : texelFetch(utex2d[slot], ivec2(x, y), level);
    return uintBitsToFloat(uvec4(kU8Bits[t.r & 255u], kU8Bits[t.g & 255u], kU8Bits[t.b & 255u], kU8Bits[t.a & 255u]));
}
ivec2 xeLevelSize(uint kind, int slot, int level)
{
    return kind == 1u ? textureSize(utex3d[slot], 0).xy : kind >= 2u ? textureSize(utexCube[slot], level).xy : textureSize(utex2d[slot], level);
}
// u * w / W (level size / level-0 size), as the software: exact scaling when
// both are powers of two, else the correctly rounded division.
float xscale(float u, int w, int W)
{
    if (((w & (w - 1)) | (W & (W - 1))) == 0) { precise float r = u * (float(w) / float(W)); return r; }
    precise float t = u * float(w);
    return xdiv(t, float(W));
}
// QuadSampler::SampleLevel: (u, v) in texels of this level.
vec4 xeSampleLevel(uint kind, int slot, int layer, int level, float u, float v, bool linear, uint cx, uint cy)
{
    ivec2 sz = xeLevelSize(kind, slot, level);
    if (!linear) return xeTexel(kind, slot, xwrap(int(floor(u)), sz.x, cx), xwrap(int(floor(v)), sz.y, cy), layer, level);
    precise float fu = u - 0.5, fv = v - 0.5;
    float flx = floor(fu), fly = floor(fv);
    int x0 = int(flx), y0 = int(fly);
    precise float ax = fu - flx, ay = fv - fly;
    int xa = xwrap(x0, sz.x, cx), xb = xwrap(x0 + 1, sz.x, cx), ya = xwrap(y0, sz.y, cy), yb = xwrap(y0 + 1, sz.y, cy);
    vec4 c00 = xeTexel(kind, slot, xa, ya, layer, level), c10 = xeTexel(kind, slot, xb, ya, layer, level);
    vec4 c01 = xeTexel(kind, slot, xa, yb, layer, level), c11 = xeTexel(kind, slot, xb, yb, layer, level);
    precise vec4 o = (c00 * (1.0 - ax) + c10 * ax) * (1.0 - ay) + (c01 * (1.0 - ax) + c11 * ax) * ay;
    return o;
}
// One 2D layer: a 2D texture, a cube face or a layer of a stacked texture (`face`), mipmapped.
// `filt`: the instruction's mag | min << 2 | mip << 4 (3 = from the fetch constant);
// `ofs`: offsets in half texels; `lodOn`: computed LOD (pixel shaders); `ibias`: instruction LOD bias.
// `gm`: bit 0 LOD from the register gradients `gh` / `gv`, bit 1 add the register
// LOD `rl`, bit 2 the instruction's computed-LOD bit (xenos_raster.cpp QuadSampler).
vec4 xeSample2D(int slot, uint kind, int face, vec3 c, bool unnorm, vec2 ofs, uint filt, bool lodOn, float ibias, uint gm, vec3 gh,
                vec3 gv, float rl)
{
    uint d0 = D.fetch[slot * 6], d3 = D.fetch[slot * 6 + 3], d4 = D.fetch[slot * 6 + 4];
    uint cx = (d0 >> 10) & 7u, cy = (d0 >> 13) & 7u;
    uint magF = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
    bool cube = kind == 2u;
    ivec2 s0 = xeLevelSize(kind, slot, 0);
    int last = (kind >= 2u ? textureQueryLevels(utexCube[slot]) : textureQueryLevels(utex2d[slot])) - 1;
    float W = float(s0.x), H = float(s0.y);
    // Unnormalized coordinates, offsets and register gradients are in the guest's texels (D.texSize); at a
    // resolution scale a texture decoded from a scaled resolve is larger (gx = gy = s; else exactly 1).
    float gx = W / D.texSize[slot].x, gy = H / D.texSize[slot].y;
    float cu = cube ? c.x - 1.0 : c.x, cv = cube ? c.y - 1.0 : c.y;
    precise float u = unnorm ? cu * gx : cu * W, v = unnorm ? cv * gy : cv * H;
    u = u + ofs.x * 0.5 * gx;
    v = v + ofs.y * 0.5 * gy;
    uint minF = ((filt >> 2) & 3u) == 3u ? (d3 >> 21) & 3u : (filt >> 2) & 3u;
    uint mipF = ((filt >> 4) & 3u) == 3u ? (d3 >> 23) & 3u : (filt >> 4) & 3u;
    float lod = 0.0;
    bool haveLod = false;
)GLSL";
const char* kExactSamplerB = R"GLSL(
    // The LOD only matters with mips or different min/mag filters (as the
    // software's needLod; otherwise level minLevel with the mag filter either way).
    precise float fetchBias = float(bitfieldExtract(int(d4), 12, 10)) / 32.0;
    if (lodOn && (last > 0 || minF != magF))
    {
        precise float dux = 0.0, dvx = 0.0, duy = 0.0, dvy = 0.0;
        bool haveGrad = false;
        if ((gm & 1u) != 0u && !cube)   // setGradientsH/V, normalized unless the coordinates are not
        {
            float sx = unnorm ? gx : W, sy = unnorm ? gy : H;
            dux = gh.x * sx; dvx = gh.y * sy; duy = gv.x * sx; dvy = gv.y * sy;
            haveGrad = true;
        }
#ifdef XE_FRAGMENT
        else { dux = dFdxCoarse(u); dvx = dFdxCoarse(v); duy = dFdyCoarse(u); dvy = dFdyCoarse(v); haveGrad = true; }
#endif
        precise float rx = dux * dux + dvx * dvx, ry = duy * duy + dvy * dvy;
        float rho2 = stdmax(rx, ry);
        if (haveGrad && !isinf(rho2) && !isnan(rho2))
        {
            precise float l0 = rho2 > 0.0 ? 0.5 * xlog2(rho2) : -16.0;
            precise float bias = fetchBias + ibias;
            lod = l0 + bias;
            haveLod = true;
        }
    }
    if ((gm & 2u) != 0u)   // setTexLOD: added to the computed LOD, or the explicit LOD
    {
        if (haveLod) { precise float l = lod + rl; lod = l; }
        else if ((gm & 4u) == 0u) { precise float l = rl + fetchBias + ibias; lod = l; haveLod = true; }
    }
    int minLevel = min(int((d4 >> 2) & 15u), last), maxLevel = min(int((d4 >> 6) & 15u), last);
    if (maxLevel < minLevel) maxLevel = minLevel;
    bool minify = haveLod && lod > 0.0;
    bool linear = (minify ? minF : magF) == 1u;
    int lA = minLevel, lB = minLevel;
    float f = 0.0;
    if (minify && mipF != 2u && maxLevel != minLevel)
    {
        float l = stdmin(stdmax(lod, float(minLevel)), float(maxLevel));
        if (mipF == 0u) { lA = int(floor(l + 0.5)); lB = lA; }
        else { lA = int(floor(l)); lB = min(lA + 1, maxLevel); f = l - float(lA); }
    }
    ivec2 sA = xeLevelSize(kind, slot, lA);
    precise float uA = xscale(u, sA.x, s0.x), vA = xscale(v, sA.y, s0.y);
    vec4 o = xeSampleLevel(kind, slot, face, lA, uA, vA, linear, cx, cy);
    if (f > 0.0 && lB != lA)
    {
        ivec2 sB = xeLevelSize(kind, slot, lB);
        precise float uB = xscale(u, sB.x, s0.x), vB = xscale(v, sB.y, s0.y);
        vec4 b = xeSampleLevel(kind, slot, face, lB, uB, vB, linear, cx, cy);
        precise vec4 r = o + (b - o) * f;
        o = r;
    }
    return o;
}
// The texture fetch (xenos_raster.cpp QuadSampler::Sample). `vol`: for a 3D fetch the instruction's
// volume magnification filter (3 = from the fetch constant), 4 for other fetches.
vec4 xeSample(int slot, vec3 c, bool unnorm, vec2 ofs, uint filt, bool lodOn, float ibias, uint vol, uint gm, vec3 gh, vec3 gv, float rl)
{
    uint kind = xeKind(slot);
    uint d0 = D.fetch[slot * 6], d3 = D.fetch[slot * 6 + 3], d4 = D.fetch[slot * 6 + 4];
    uint cx = (d0 >> 10) & 7u, cy = (d0 >> 13) & 7u, cz = (d0 >> 16) & 7u;
    uint magF = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
    if (kind == 1u)   // 3D: level 0, magnification filter
    {
        ivec3 s3 = textureSize(utex3d[slot], 0);
        precise float u = unnorm ? c.x : c.x * float(s3.x), v = unnorm ? c.y : c.y * float(s3.y);
        precise float w = unnorm ? c.z : c.z * float(s3.z);
        if (magF != 1u) return xeSampleLevel(1u, slot, xwrap(int(floor(w)), s3.z, cz), 0, u, v, false, cx, cy);
        precise float fw = w - 0.5;
        float fl = floor(fw);
        int z0 = int(fl);
        precise float f = fw - fl;
        vec4 a = xeSampleLevel(1u, slot, xwrap(z0, s3.z, cz), 0, u, v, true, cx, cy);
        vec4 b = xeSampleLevel(1u, slot, xwrap(z0 + 1, s3.z, cz), 0, u, v, true, cx, cy);
        precise vec4 o = a + (b - a) * f;
        return o;
    }
    if (kind == 3u && vol < 4u)   // stacked, 3D fetch: z * depth picks the layer, linear between with the volume filter
    {
        // The layer obeys the Z clamp mode, as a 3D texture's slices: outside the stack with clamp to border it is
        // the border colour (the jersey numbers' missing second digit of a single digit).
        int n = textureSize(utexCube[slot], 0).z;
        precise float w = unnorm ? c.z : c.z * float(n);
        if (isnan(w)) w = 0.0;
        bool volLinear = (vol == 3u ? d4 & 1u : vol) == 1u;
        if (volLinear) w = w - 0.5;
        float fl = floor(w);
        int z0 = int(clamp(fl, -65536.0, 65536.0));
        int la = xwrap(z0, n, cz);
        vec4 a = la < 0 ? xeTexel(3u, slot, -1, -1, -1, 0) : xeSample2D(slot, 3u, la, c, unnorm, ofs, filt, lodOn, ibias, gm, gh, gv, rl);
        if (!volLinear) return a;
        precise float f = w - fl;
        int lb = xwrap(z0 + 1, n, cz);
        vec4 b = lb < 0 ? xeTexel(3u, slot, -1, -1, -1, 0) : xeSample2D(slot, 3u, lb, c, unnorm, ofs, filt, lodOn, ibias, gm, gh, gv, rl);
        precise vec4 o = a + (b - a) * f;
        return o;
    }
    int face = kind == 2u ? int(c.z >= 0.0 ? min(5.0, floor(c.z)) : 0.0) : 0;
    return xeSample2D(slot, kind, face, c, unnorm, ofs, filt, lodOn, ibias, gm, gh, gv, rl);
}
)GLSL";

std::string ExactSampler() { return std::string(kExactSamplerA) + kExactSamplerB; }

// Pixel epilogue of the EDRAM renderer: xenos_raster.cpp RasterDraw's
// per-pixel loop body for a covered pixel. Called inside the interlock.
const char* kEdramPixel = R"GLSL(
void xeEdramPixel(uint x, uint y, bool front, float z, bool killedPs, uint written, vec4 outs[4])
{
    uint pitch = D.surf.x, msaa = D.surf.y, samples = edramSamples(msaa);
    bool depthFloat = D.surf.w != 0u;
    uint dc = D.ds.x;
    bool zEnable = ((dc >> 1) & 1u) != 0u, zWrite = ((dc >> 2) & 1u) != 0u, stencilOn = (dc & 1u) != 0u;
    uint zFunc = (dc >> 4) & 7u;
    bool back = !front && ((dc >> 7) & 1u) != 0u;
    uint rm = back ? D.ds.z : D.ds.y;
    int lo = back ? 20 : 8;
    uint sFunc = bitfieldExtract(dc, lo, 3), sFail = bitfieldExtract(dc, lo + 3, 3);
    uint sZpass = bitfieldExtract(dc, lo + 6, 3), sZfail = bitfieldExtract(dc, lo + 9, 3);
    uint sRef = rm & 0xFFu, sMask = (rm >> 8) & 0xFFu, sWrite = (rm >> 16) & 0xFFu;
    uint di = edramIndex(D.surf.z, pitch, msaa, false, x, y, 0u);
    uint dv = edram[di];
    uint stencil = dv & 0xFFu;
    bool sPass = !stencilOn || xcompare(sFunc, float(sRef & sMask), float(stencil & sMask));
    bool zPass = !zEnable || !sPass || xcompare(zFunc, z, decodeDepth(dv >> 8, depthFloat));
    uint failOp = !sPass ? sFail : sZfail;
    bool failWrites = stencilOn && failOp != 0u && sWrite != 0u;
    if (!(sPass && zPass) && !failWrites) return;
    if ((D.ds.w & 1u) != 0u)   // shade
    {
        if (killedPs) return;
        vec4 out0 = vec4(0.0, 0.0, 0.0, 1.0);
        if ((written & 1u) == 0u) { if (!zWrite && !stencilOn) return; }
        else out0 = outs[0];
        if ((D.alpha & 8u) != 0u && !xcompare(D.alpha & 7u, out0.w, D.alphaRef)) return;
    }
    else written = 0u;
    bool pass = sPass && zPass;
    uint ndv = dv;
    if (stencilOn)
    {
        uint ns = stencilOp(pass ? sZpass : failOp, stencil, sRef);
        ndv = (ndv & ~0xFFu) | ((stencil & ~sWrite) | (ns & sWrite));
    }
    if (pass && zEnable && zWrite) ndv = (encodeDepth(z, depthFloat) << 8) | (ndv & 0xFFu);
    if (ndv != dv)
        for (uint s = 0u; s < samples; ++s) edram[edramIndex(D.surf.z, pitch, msaa, false, x, y, s)] = ndv;
    if (!pass || (D.ds.w & 2u) == 0u) return;
    for (int rt = 0; rt < 4; ++rt)
    {
        uint fw = D.rtFmt[rt];
        if ((fw & 0x100u) == 0u || (written & (1u << rt)) == 0u) continue;
        uint fmt = fw & 0xFFu, blend = D.rtBlend[rt], mask = D.rtMask[rt];
        bool wide = is64bpp(fmt);
        precise vec4 src = outs[rt] * D.rtScale[rt];
        src = clampRt(fmt, src);
        uint k0 = edramIndex(D.rtBase[rt], pitch, msaa, wide, x, y, 0u);
        vec4 dst = decodeRt(fmt, edram[k0], wide ? edram[k0 + 1u] : 0u, true);
        vec4 res;
        for (int i = 0; i < 4; ++i)
            res[i] = (mask & (1u << i)) != 0u ? blendChannel(blend, src, dst, D.blendConst, i) : dst[i];
        res = clampRt(fmt, res);
        uvec2 pv = encodeRt(fmt, res, true);
        for (uint s = 0u; s < samples; ++s)
        {
            uint k = edramIndex(D.rtBase[rt], pitch, msaa, wide, x, y, s);
            edram[k] = pv.x;
            if (wide) edram[k + 1u] = pv.y;
        }
    }
}
)GLSL";

struct Emitter
{
    const ShaderCode& code;
    TranslatedShader& out;
    std::string body;
    bool vs;
    uint32_t maxTemp = 0;
    bool relTemp = false;
    bool usesVfetch = false;
    int indent = 1;

    bool hwSampler = false;   // hardware samplers instead of the software's sampling (xeSample)
    uint32_t relaxed = 0;     // TranslateOptions::relaxed

    Emitter(const ShaderCode& c, TranslatedShader& o) : code(c), out(o), vs(c.type == ShaderType::kVertex) {}

    void Line(const std::string& s) { body.append(size_t(indent) * 4, ' '); body += s; body += '\n'; }

    std::string Temp(uint32_t idx, bool rel)
    {
        if (rel) { relTemp = true; return "r[(" + std::to_string(idx) + " + aL()) & 127]"; }
        maxTemp = std::max(maxTemp, idx);
        return "r[" + std::to_string(idx) + "]";
    }

    // ALU source n (1-3) as a swizzled vec4 expression.
    std::string Source(const uint32_t* in, int n)
    {
        bool temp = Bits(in[2], 31 - (n - 1), 1) != 0;
        uint32_t reg = Bits(in[2], 16 - 8 * (n - 1), 8);
        uint32_t swz = Bits(in[1], 16 - 8 * (n - 1), 8);
        bool neg = Bits(in[1], 26 - (n - 1), 1) != 0;
        std::string e;
        bool absVal;
        if (temp) { e = Temp(reg & 0x3F, reg & 0x40); absVal = reg & 0x80; }
        else { e = Const(in, reg, n); absVal = Bits(in[0], 7, 1); }
        e += ".";
        for (uint32_t j = 0; j < 4; ++j) e += kXyzw[SwizzledComponent(swz, j)];
        if (absVal) e = "abs(" + e + ")";
        if (neg) e = "(-" + e + ")";
        return e;
    }
    bool ConstRel(const uint32_t* in, int n)
    {
        bool c0Rel = Bits(in[1], 31, 1), c1Rel = Bits(in[1], 30, 1);
        auto isTemp = [&](int k) { return Bits(in[2], 31 - (k - 1), 1) != 0; };
        if (n == 1) return c0Rel;
        if (n == 2) return isTemp(1) ? c0Rel : c1Rel;
        return (isTemp(1) && isTemp(2)) ? c0Rel : c1Rel;
    }
    std::string Const(const uint32_t* in, uint32_t reg, int n)
    {
        std::string i = std::to_string(reg);
        bool rel = ConstRel(in, n);
        if (rel) i += Bits(in[1], 29, 1) ? " + a0" : " + aL()";
        if (rel) out.constRelative = true;
        else out.constUsed[(reg >> 6) & 3] |= 1ull << (reg & 63);
        return "cnst(" + i + ")";
    }
    // One scalar operand component (k-th of a two-component op).
    std::string ScalarSource(const uint32_t* in, uint32_t k)
    {
        bool temp = Bits(in[2], 29, 1) != 0;
        uint32_t reg = Bits(in[2], 0, 8), swz = Bits(in[1], 0, 8);
        bool neg = Bits(in[1], 24, 1) != 0;
        std::string e;
        bool absVal;
        if (temp) { e = Temp(reg & 0x3F, reg & 0x40); absVal = reg & 0x80; }
        else { e = Const(in, reg, 3); absVal = Bits(in[0], 7, 1); }
        e += ".";
        e += kXyzw[SwizzledComponent(swz, (3 + k) & 3)];
        if (absVal) e = "abs(" + e + ")";
        if (neg) e = "(-" + e + ")";
        return e;
    }

    void Alu(const uint32_t* in, uint32_t ia)
    {
        uint32_t vop = Bits(in[2], 24, 5), sop = Bits(in[0], 26, 6);
        bool exportData = Bits(in[0], 15, 1);
        uint32_t vmask = Bits(in[0], 16, 4), smask = Bits(in[0], 20, 4);
        uint32_t vResMask = exportData ? (vmask & ~smask) : vmask;
        uint32_t sResMask = exportData ? (smask & ~vmask) : smask;
        bool predicated = Bits(in[1], 28, 1);
        Line("{   // " + std::to_string(ia) + ": vec" + std::to_string(vop) + " sca" + std::to_string(sop));
        ++indent;
        if (predicated) { Line(std::string("if (p == ") + (Bits(in[1], 27, 1) ? "true" : "false") + ") {"); ++indent; }
        Line((relaxed & 2) ? "vec4 v = vec4(0.0);" : "precise vec4 v = vec4(0.0);");
        if (vResMask || (vop >= 20 && vop <= 27) || vop == 29)
        {
            std::string s[3];
            for (int n = 1; n <= 3; ++n)
                if (kVecOperands[vop][n - 1]) { s[n - 1] = "s" + std::to_string(n); Line("vec4 " + s[n - 1] + " = " + Source(in, n) + ";"); }
            const std::string &a = s[0], &b = s[1], &c = s[2];
            bool rep = false;
            switch (vop)
            {
            case 0: Line("v = " + a + " + " + b + ";"); break;
            case 1: Line("v = xmul4(" + a + ", " + b + ");"); break;
            case 2: Line("v = xmax4(" + a + ", " + b + ");"); break;
            case 3: Line("v = xmin4(" + a + ", " + b + ");"); break;
            case 4: Line("v = vec4(equal(" + a + ", " + b + "));"); break;
            case 5: Line("v = vec4(greaterThan(" + a + ", " + b + "));"); break;
            case 6: Line("v = vec4(greaterThanEqual(" + a + ", " + b + "));"); break;
            case 7: Line("v = vec4(notEqual(" + a + ", " + b + "));"); break;
            case 8: Line("v = " + a + " - floor(" + a + ");"); break;
            case 9: Line("v = trunc(" + a + ");"); break;
            case 10: Line("v = floor(" + a + ");"); break;
            case 11: Line("v = xmul4(" + a + ", " + b + ") + " + c + ";"); break;
            case 12: Line("v = sel4(equal(" + a + ", vec4(0.0)), " + b + ", " + c + ");"); break;
            case 13: Line("v = sel4(greaterThanEqual(" + a + ", vec4(0.0)), " + b + ", " + c + ");"); break;
            case 14: Line("v = sel4(greaterThan(" + a + ", vec4(0.0)), " + b + ", " + c + ");"); break;
            case 15: Line("v.x = (((0.0 + xmul(" + a + ".x, " + b + ".x)) + xmul(" + a + ".y, " + b + ".y)) + xmul(" + a + ".z, " + b +
                          ".z)) + xmul(" + a + ".w, " + b + ".w);"); rep = true; break;
            case 16: Line("v.x = ((0.0 + xmul(" + a + ".x, " + b + ".x)) + xmul(" + a + ".y, " + b + ".y)) + xmul(" + a + ".z, " + b +
                          ".z);"); rep = true; break;
            case 17: Line("v.x = (xmul(" + a + ".x, " + b + ".x) + xmul(" + a + ".y, " + b + ".y)) + " + c + ".x;"); rep = true; break;
            case 18:
                Line("{ float x = " + a + ".z, y = " + a + ".w, z = " + a + ".x;");
                Line("  float ax = abs(x), ay = abs(y), az = abs(z);");
                Line("  if (az >= ax && az >= ay) { bool n = z < 0.0; v = vec4(-y, n ? -x : x, z, n ? 5.0 : 4.0); }");
                Line("  else if (ay >= ax) { bool n = y < 0.0; v = vec4(n ? -z : z, x, y, n ? 3.0 : 2.0); }");
                Line("  else { bool n = x < 0.0; v = vec4(-y, n ? z : -z, x, n ? 1.0 : 0.0); }");
                Line("  v.z *= 2.0; }");
                break;
            case 19:
                Line("{ float m = " + a + ".x; if (!(m >= " + a + ".y)) m = " + a + ".y; if (!(m >= " + a + ".z)) m = " + a +
                     ".z; if (!(m >= " + a + ".w)) m = " + a + ".w; v.x = m; }");
                rep = true;
                break;
            case 20: case 21: case 22: case 23:
            {
                const char* cmpW = vop == 20 ? " == 0.0" : vop == 21 ? " != 0.0" : vop == 22 ? " > 0.0" : " >= 0.0";
                Line("p = " + a + ".w == 0.0 && " + b + ".w" + cmpW + ";");
                Line("v.x = (" + a + ".x == 0.0 && " + b + ".x" + cmpW + ") ? 0.0 : " + a + ".x + 1.0;");
                rep = true;
                break;
            }
            case 24: case 25: case 26: case 27:
            {
                const char* f = vop == 24 ? "equal" : vop == 25 ? "greaterThan" : vop == 26 ? "greaterThanEqual" : "notEqual";
                Line(std::string("{ bool k = any(") + f + "(" + a + ", " + b + ")); v.x = float(k); if (k) killed = true; }");
                out.usesKill = true;
                rep = true;
                break;
            }
            case 28: Line("v = vec4(1.0, xmul(" + a + ".y, " + b + ".y), " + a + ".z, " + b + ".w);"); break;
            case 29:
                Line("a0 = int(floor(min(255.0, max(-256.0, " + a + ".w)) + 0.5));");
                Line("v = xmax4(" + a + ", " + b + ");");
                break;
            default: break;
            }
            if (rep) Line("v = v.xxxx;");
        }
        if (Bits(in[0], 24, 1)) Line("v = sat4(v);");

        // Scalar.
        uint32_t nops = sop < 64 ? kScaOperands[sop] : 0;
        if (sop >= 51) nops = 0;
        std::string sa = "0.0", sb = "0.0";
        if (nops == 1)
        {
            Line("float sa = " + ScalarSource(in, 0) + ";");
            sa = "sa";
            if (kScaTwoComponent[sop]) { Line("float sb = " + ScalarSource(in, 1) + ";"); sb = "sb"; }
        }
        else if (nops == 2)
        {
            uint32_t swz = Bits(in[1], 0, 8);
            uint32_t tempReg = (sop & 1) | (Bits(in[2], 29, 1) << 1) | (swz & 0x3C);
            std::string ce = Const(in, Bits(in[2], 0, 8), 3) + "." + kXyzw[SwizzledComponent(swz, 3)];
            std::string te = Temp(tempReg, false) + "." + kXyzw[SwizzledComponent(swz, 0)];
            bool absC = Bits(in[0], 7, 1), neg = Bits(in[1], 24, 1);
            auto wrap = [&](std::string e) { if (absC) e = "abs(" + e + ")"; if (neg) e = "(-" + e + ")"; return e; };
            Line("float sa = " + wrap(ce) + ";");
            Line("float sb = " + wrap(te) + ";");
            sa = "sa"; sb = "sb";
        }
        switch (sop)
        {
        case 0: case 44: case 45: Line("ps = " + sa + " + " + sb + ";"); break;
        case 1: Line("ps = " + sa + " + ps;"); break;
        case 2: case 42: case 43: Line("ps = xmul(" + sa + ", " + sb + ");"); break;
        case 3: Line("ps = xmul(" + sa + ", ps);"); break;
        case 4: Line("ps = (ps == -3.402823466e+38 || isinf(ps) || isnan(ps) || isinf(" + sb + ") || isnan(" + sb + ") || " + sb +
                     " <= 0.0) ? -3.402823466e+38 : xmul(" + sa + ", ps);"); break;
        case 5: Line("ps = xmax(" + sa + ", " + sb + ");"); break;
        case 6: Line("ps = xmin(" + sa + ", " + sb + ");"); break;
        case 7: Line("ps = float(" + sa + " == 0.0);"); break;
        case 8: Line("ps = float(" + sa + " > 0.0);"); break;
        case 9: Line("ps = float(" + sa + " >= 0.0);"); break;
        case 10: Line("ps = float(" + sa + " != 0.0);"); break;
        case 11: Line("ps = " + sa + " - floor(" + sa + ");"); break;
        case 12: Line("ps = trunc(" + sa + ");"); break;
        case 13: Line("ps = floor(" + sa + ");"); break;
        case 14: Line("ps = xexp2(" + sa + ");"); break;
        case 15: Line("ps = xlog2(" + sa + "); if (isinf(ps) && ps < 0.0) ps = -3.402823466e+38;"); break;
        case 16: Line("ps = xlog2(" + sa + ");"); break;
        case 17: Line("ps = xdiv(1.0, " + sa + "); if (isinf(ps)) ps = ps < 0.0 ? -3.402823466e+38 : 3.402823466e+38;"); break;
        case 18: Line("ps = xdiv(1.0, " + sa + "); if (isinf(ps)) ps = ps < 0.0 ? -0.0 : 0.0;"); break;
        case 19: Line("ps = xdiv(1.0, " + sa + ");"); break;
        case 20: Line("ps = xdiv(1.0, xsqrtf(" + sa + ")); if (isinf(ps)) ps = ps < 0.0 ? -3.402823466e+38 : 3.402823466e+38;"); break;
        case 21: Line("ps = xdiv(1.0, xsqrtf(" + sa + ")); if (isinf(ps)) ps = ps < 0.0 ? -0.0 : 0.0;"); break;
        case 22: Line("ps = xdiv(1.0, xsqrtf(" + sa + "));"); break;
        case 23: Line("a0 = int(floor(min(255.0, max(-256.0, " + sa + ")) + 0.5)); ps = xmax(" + sa + ", " + sb + ");"); break;
        case 24: Line("a0 = int(floor(min(255.0, max(-256.0, " + sa + ")))); ps = xmax(" + sa + ", " + sb + ");"); break;
        case 25: case 46: case 47: Line("ps = " + sa + " - " + sb + ";"); break;
        case 26: Line("ps = " + sa + " - ps;"); break;
        case 27: Line("p = " + sa + " == 0.0; ps = float(!p);"); break;
        case 28: Line("p = " + sa + " != 0.0; ps = float(!p);"); break;
        case 29: Line("p = " + sa + " > 0.0; ps = float(!p);"); break;
        case 30: Line("p = " + sa + " >= 0.0; ps = float(!p);"); break;
        case 31: Line("p = " + sa + " == 1.0; ps = p ? 0.0 : (" + sa + " == 0.0 ? 1.0 : " + sa + ");"); break;
        case 32: Line("{ float n = " + sa + " - 1.0; p = n <= 0.0; ps = p ? 0.0 : n; }"); break;
        case 33: Line("p = false; ps = 3.402823466e+38;"); break;
        case 34: Line("p = " + sa + " == 0.0; ps = p ? 0.0 : " + sa + ";"); break;
        case 35: Line("ps = float(" + sa + " == 0.0); if (ps != 0.0) killed = true;"); out.usesKill = true; break;
        case 36: Line("ps = float(" + sa + " > 0.0); if (ps != 0.0) killed = true;"); out.usesKill = true; break;
        case 37: Line("ps = float(" + sa + " >= 0.0); if (ps != 0.0) killed = true;"); out.usesKill = true; break;
        case 38: Line("ps = float(" + sa + " != 0.0); if (ps != 0.0) killed = true;"); out.usesKill = true; break;
        case 39: Line("ps = float(" + sa + " == 1.0); if (ps != 0.0) killed = true;"); out.usesKill = true; break;
        case 40: Line("ps = xsqrtf(" + sa + ");"); break;
        case 48: Line("ps = xsin(" + sa + ");"); break;
        case 49: Line("ps = xcos(" + sa + ");"); break;
        default: break;   // retain_prev
        }
        Line(std::string("float sr = ") + (Bits(in[0], 25, 1) ? "sat(ps);" : "ps;"));

        if (exportData)
        {
            uint32_t reg = Bits(in[0], 0, 6);
            uint32_t c0Mask = Bits(in[0], 14, 1) ? (15 & ~(vmask | smask)) : 0;
            uint32_t c1Mask = vmask & smask;
            uint32_t mask = vResMask | sResMask | c0Mask | c1Mask;
            std::string dst;
            if (vs) { if (reg == 62) dst = "pos"; else if (reg < 16) { dst = "o[" + std::to_string(reg) + "]"; out.exportMask |= 1u << reg; } }
            else if (reg < 4) { dst = "oc" + std::to_string(reg); out.exportMask |= 1u << reg; }
            if (!vs && reg < 4 && mask) Line("ocWritten |= " + std::to_string(1u << reg) + "u;");
            if (!dst.empty() && mask)
                for (uint32_t i = 0; i < 4; ++i)
                {
                    uint32_t b = 1u << i;
                    if (!(mask & b)) continue;
                    std::string val = (vResMask & b) ? std::string("v.") + kXyzw[i] : (sResMask & b) ? "sr" : (c1Mask & b) ? "1.0" : "0.0";
                    Line(dst + "." + kXyzw[i] + " = " + val + ";");
                }
        }
        else
        {
            if (vResMask)
            {
                std::string d = Temp(Bits(in[0], 0, 6), Bits(in[0], 6, 1));
                for (uint32_t i = 0; i < 4; ++i)
                    if (vResMask & (1u << i)) Line(d + "." + kXyzw[i] + " = v." + kXyzw[i] + ";");
            }
            if (sResMask)
            {
                std::string d = Temp(Bits(in[0], 8, 6), Bits(in[0], 14, 1));
                for (uint32_t i = 0; i < 4; ++i)
                    if (sResMask & (1u << i)) Line(d + "." + kXyzw[i] + " = sr;");
            }
        }
        if (predicated) { --indent; Line("}"); }
        --indent;
        Line("}");
    }

    void StoreFetch(uint32_t dst, bool rel, uint32_t dswz, const char* value)
    {
        std::string d = Temp(dst, rel);
        for (uint32_t i = 0; i < 4; ++i)
        {
            uint32_t s = (dswz >> (3 * i)) & 7;
            if (s <= 3) Line(d + "." + kXyzw[i] + " = " + value + "." + kXyzw[s] + ";");
            else if (s == 4) Line(d + "." + kXyzw[i] + " = 0.0;");
            else if (s == 5) Line(d + "." + kXyzw[i] + " = 1.0;");
        }
    }

    void Fetch(const uint32_t* in, uint32_t ia)
    {
        uint32_t op = Bits(in[0], 0, 5);
        uint32_t dst = Bits(in[0], 12, 6), src = Bits(in[0], 5, 6), dswz = Bits(in[1], 0, 12);
        bool dstRel = Bits(in[0], 18, 1), srcRel = Bits(in[0], 11, 1);
        bool predicated = Bits(in[1], 31, 1);
        Line("{   // " + std::to_string(ia) + ": fetch op" + std::to_string(op));
        ++indent;
        if (predicated) { Line(std::string("if (p == ") + (Bits(in[2], 31, 1) ? "true" : "false") + ") {"); ++indent; }
        if (op == 0)
        {
            usesVfetch = true;
            if (!vs) out.unsupported |= 2;   // vertex fetch in a pixel shader: not supported
            bool mini = Bits(in[1], 30, 1);
            if (!mini && vs)
            {
                uint32_t vc = Bits(in[0], 20, 5) * 3 + Bits(in[0], 25, 2), stride = Bits(in[2], 0, 8);
                std::string idx = Temp(src, srcRel) + "." + kXyzw[Bits(in[0], 30, 2)];
                if (Bits(in[1], 15, 1)) idx = "(" + idx + " + 0.5)";
                Line("vfConst = " + std::to_string(vc) + "u; vfStride = " + std::to_string(stride) + "u;");
                Line("vfAddr = vfStride * uint(int(floor(" + idx + "))) + (D.fetch[vfConst * 2u] >> 2);");
            }
            uint32_t fmt = Bits(in[1], 16, 6);
            static const uint32_t kKnown[] = { 6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57 };
            if (std::find(std::begin(kKnown), std::end(kKnown), fmt) == std::end(kKnown)) out.unsupported |= 2;
            char b[200];
            snprintf(b, sizeof(b), "vec4 f = %s;",
                     vs ? ("vfetch(uint(int(vfAddr) + (" + std::to_string(SBits(in[2], 8, 23)) + ")), " + std::to_string(fmt) + "u, " +
                           (Bits(in[1], 12, 1) ? "true" : "false") + ", " + (!Bits(in[1], 13, 1) ? "true" : "false") + ", " +
                           (Bits(in[1], 14, 1) ? "true" : "false") + ", " + std::to_string(SBits(in[1], 24, 6)) + ")").c_str()
                        : "vec4(0.0)");
            Line(b);
            StoreFetch(dst, dstRel, dswz, "f");
        }
        else if (op == 1)
        {
            // The texture kind comes from the fetch constant at draw time (as the
            // software sampler: D.texSize[slot].z 0 2D, 1 3D, 2 cube, 3 stacked),
            // not from the instruction's dimension.
            uint32_t slot = Bits(in[0], 20, 5);
            uint32_t sswz = Bits(in[0], 26, 6);
            std::string s = Temp(src, srcRel);
            std::string sl = std::to_string(slot);
            out.textureSlots |= 1u << slot;
            uint8_t filt = uint8_t(Bits(in[1], 12, 2) | (Bits(in[1], 14, 2) << 2) | (Bits(in[1], 16, 2) << 4));
            if (out.slotFilter[slot] == 0xFF) out.slotFilter[slot] = filt;
            bool unnorm = Bits(in[0], 25, 1);
            Line(std::string("vec3 sc = vec3(") + s + "." + kXyzw[sswz & 3] + ", " + s + "." + kXyzw[(sswz >> 2) & 3] + ", " + s + "." +
                 kXyzw[(sswz >> 4) & 3] + ");");
            if (!hwSampler)
            {
                char b[200];
                // Register gradients also give vertex shaders a LOD (as the software sampler).
                uint32_t gm = Bits(in[2], 0, 1) | (Bits(in[1], 29, 1) << 1) | (Bits(in[1], 28, 1) << 2);
                bool lodOn = Bits(in[1], 28, 1) && (!vs || Bits(in[2], 0, 1));
                snprintf(b, sizeof(b), "vec4 f = xeSample(%u, sc, %s, vec2(%d.0, %d.0), %uu, %s, %.6f, %uu, %uu, xeGradH, xeGradV, xeRegLod);", slot,
                         unnorm ? "true" : "false", SBits(in[2], 16, 5), SBits(in[2], 21, 5), unsigned(filt), lodOn ? "true" : "false",
                         double(SBits(in[2], 2, 7)) / 16.0, Bits(in[2], 14, 2) == 2 ? Bits(in[1], 24, 2) : 4u, gm);
                Line(b);
                StoreFetch(dst, dstRel, dswz, "f");
                if (predicated) { --indent; Line("}"); }
                --indent;
                Line("}");
                return;
            }
            Line("vec4 f;");
            Line("uint kind = xeKind(" + sl + ");");
            Line("if (kind == 1u) {   // 3D: level 0, magnification filter (sampler)");
            Line("    vec3 uvw = sc;");
            if (unnorm) Line("    uvw /= vec3(D.texSize[" + sl + "].xy, D.texSize[" + sl + "].w);");
            Line("    f = textureLod(tex3d[" + sl + "], uvw, 0.0);");
            Line("} else {   // 2D, a cube face (coordinates in [1, 2), face = floor(z)) or a stacked layer (floor(z * depth))");
            Line("    vec2 uv = kind == 2u ? sc.xy - 1.0 : sc.xy;");
            Line("    float face = sc.z >= 0.0 ? min(5.0, floor(sc.z)) : 0.0;");
            // A stacked layer obeys the fetch's Z clamp mode (xwrap): outside the stack with clamp to border it
            // is the border colour. The jersey numbers: a 10-layer stack of digits, a single digit's missing
            // second digit is a layer outside it.
            bool stackedLayer = Bits(in[2], 14, 2) == 2;
            if (stackedLayer)
            {
                Line("    bool outside = false;");
                Line(std::string("    if (kind == 3u) { int l = xwrap(int(clamp(floor(sc.z") + (unnorm ? "" : " * D.texSize[" + sl + "].w") +
                     "), -65536.0, 65536.0)), int(D.texSize[" + sl + "].w), (D.fetch[" + std::to_string(slot * 6) +
                     "] >> 16) & 7u); outside = l < 0; face = float(max(l, 0)); }");
            }
            else
                Line("    if (kind == 3u) face = 0.0;");
            if (unnorm) Line("    uv /= D.texSize[" + sl + "].xy;");
            int32_t ox = SBits(in[2], 16, 5), oy = SBits(in[2], 21, 5);
            if (ox || oy)
                Line("    uv += vec2(" + std::to_string(ox) + ".0, " + std::to_string(oy) + ".0) * 0.5 / D.texSize[" + sl + "].xy;");
            char bias[32];
            snprintf(bias, sizeof(bias), "%.6f", double(SBits(in[2], 2, 7)) / 16.0);
            bool computed = Bits(in[1], 28, 1), regGrad = Bits(in[2], 0, 1), regLod = Bits(in[1], 29, 1);
            std::string lodAdd = regLod ? std::string(" + xeRegLod") : std::string();
            if (computed && regGrad)   // setGradientsH/V; the LOD biases scale them
            {
                std::string norm = unnorm ? " / D.texSize[" + sl + "].xy" : "";
                Line("    float gs = exp2(" + std::string(bias) + lodAdd + ");");
                Line("    vec2 gx = xeGradH.xy" + norm + " * gs, gy = xeGradV.xy" + norm + " * gs;");
                Line("    f = kind >= 2u ? textureGrad(texCube[" + sl + "], vec3(uv, face), gx, gy) : textureGrad(tex2d[" + sl + "], uv, gx, gy);");
            }
            else if (!vs && computed)
                Line("    f = kind >= 2u ? texture(texCube[" + sl + "], vec3(uv, face), " + bias + lodAdd + ") : texture(tex2d[" + sl + "], uv, " +
                     bias + lodAdd + ");");
            else if (!computed && regLod)   // setTexLOD: explicit LOD
                Line("    f = kind >= 2u ? textureLod(texCube[" + sl + "], vec3(uv, face), xeRegLod + " + bias + ") : textureLod(tex2d[" + sl +
                     "], uv, xeRegLod + " + bias + ");");
            else
                Line("    f = kind >= 2u ? textureLod(texCube[" + sl + "], vec3(uv, face), 0.0) : textureLod(tex2d[" + sl + "], uv, 0.0);");
            if (stackedLayer)
                Line("    if (outside) f = (D.fetch[" + std::to_string(slot * 6 + 5) + "] & 3u) == 1u ? vec4(1.0) : vec4(0.0);");
            Line("}");
            StoreFetch(dst, dstRel, dswz, "f");
        }
        else if (op >= 24 && op <= 26)   // setTexLOD / setGradientsH / setGradientsV: register state, no result
        {
            std::string s = Temp(src, srcRel);
            uint32_t sswz = Bits(in[0], 26, 6);
            std::string v = "vec3(" + s + "." + kXyzw[sswz & 3] + ", " + s + "." + kXyzw[(sswz >> 2) & 3] + ", " + s + "." + kXyzw[(sswz >> 4) & 3] + ")";
            Line(op == 24 ? "xeRegLod = " + v + ".x;" : (op == 25 ? "xeGradH = " : "xeGradV = ") + v + ";");
        }
        else if (op == 18)   // getGradients: coarse quad derivatives of the source's x and y
        {
            std::string s = Temp(src, srcRel);
            uint32_t sswz = Bits(in[0], 26, 6);
            std::string gx = s + "." + kXyzw[sswz & 3], gy = s + "." + kXyzw[(sswz >> 2) & 3];
            if (vs)
                StoreFetch(dst, dstRel, dswz, "vec4(0.0)");
            else
            {
                Line("precise vec4 f = vec4(dFdxCoarse(" + gx + "), dFdyCoarse(" + gx + "), dFdxCoarse(" + gy + "), dFdyCoarse(" + gy + "));");
                StoreFetch(dst, dstRel, dswz, "f");
            }
        }
        else
            StoreFetch(dst, dstRel, dswz, "vec4(0.0)");   // getCompTexLOD / getWeights / getBCF: zeros, as the interpreter
        if (predicated) { --indent; Line("}"); }
        --indent;
        Line("}");
    }

    void Exec(const Cf& cf)
    {
        const auto& dw = code.dw;
        uint32_t addr = Bits(cf.d0, 0, 12), count = Bits(cf.d0, 12, 3), seq = Bits(cf.d0, 16, 12);
        for (uint32_t k = 0; k < count; ++k)
        {
            uint32_t ia = addr + k;
            if (3 * ia + 2 >= dw.size()) break;
            if ((seq >> (2 * k)) & 1) Fetch(&dw[3 * ia], ia); else Alu(&dw[3 * ia], ia);
        }
    }
    std::string ExecCondition(const Cf& cf)
    {
        uint32_t op = cf.op();
        std::string want = Bits(cf.d1, 10, 1) ? "1u" : "0u";
        if (op == 3 || op == 4 || op == 13 || op == 14) return "boolBit(" + std::to_string(Bits(cf.d1, 2, 8)) + "u) == " + want;
        if (op == 5 || op == 6) return std::string("p == ") + (Bits(cf.d1, 10, 1) ? "true" : "false");
        return "";
    }

    void Program()
    {
        const auto& dw = code.dw;
        uint32_t n = CfCount(dw);
        bool linear = true;
        for (uint32_t cfi = 0; cfi < n; ++cfi)
        {
            uint32_t op = GetCf(dw, cfi).op();
            if (op >= 7 && op <= 11) linear = false;
        }
        out.dispatcher = !linear;
        if (linear)
        {
            for (uint32_t cfi = 0; cfi < n; ++cfi)
            {
                Cf cf = GetCf(dw, cfi);
                if (!CfIsExec(cf.op())) continue;
                std::string cond = ExecCondition(cf);
                if (!cond.empty()) { Line("if (" + cond + ") {"); ++indent; }
                Exec(cf);
                if (!cond.empty()) { --indent; Line("}"); }
                if (CfEnds(cf.op())) break;
            }
            return;
        }
        Line("int pc = 0;");
        Line("int callStack[4];");
        Line("int callDepth = 0;");
        Line("for (int guard = 0; guard < 100000 && pc >= 0; ++guard) {");
        ++indent;
        Line("switch (pc) {");
        for (uint32_t cfi = 0; cfi < n; ++cfi)
        {
            Cf cf = GetCf(dw, cfi);
            uint32_t op = cf.op();
            std::string next = std::to_string(cfi + 1);
            Line("case " + std::to_string(cfi) + ": {");
            ++indent;
            if (CfIsExec(op))
            {
                std::string cond = ExecCondition(cf);
                if (!cond.empty()) { Line("if (" + cond + ") {"); ++indent; }
                Exec(cf);
                if (!cond.empty()) { --indent; Line("}"); }
                Line("pc = " + std::string(CfEnds(op) ? "-1" : next) + ";");
            }
            else switch (op)
            {
            case 7:
            {
                std::string target = std::to_string(Bits(cf.d0, 0, 13));
                Line("uint lc = D.loops[" + std::to_string(Bits(cf.d0, 16, 5)) + "];");
                Line("if (loopDepth >= 4) pc = " + target + ";");
                Line("else {");
                if (!Bits(cf.d0, 13, 1)) Line("    loopIter[loopDepth] = 0u;");
                Line("    if (loopIter[loopDepth] >= (lc & 0xFFu)) pc = " + target + ";");
                Line("    else { loopConst[loopDepth] = lc; ++loopDepth; pc = " + next + "; }");
                Line("}");
                break;
            }
            case 8:
            {
                std::string target = std::to_string(Bits(cf.d0, 0, 13));
                Line("pc = " + next + ";");
                Line("if (loopDepth > 0) {");
                Line("    uint it = ++loopIter[loopDepth - 1];");
                std::string brk = Bits(cf.d0, 21, 1) ? std::string(" && p != ") + (Bits(cf.d1, 10, 1) ? "true" : "false") : "";
                Line("    if (it < (loopConst[loopDepth - 1] & 0xFFu)" + brk + ") pc = " + target + "; else --loopDepth;");
                Line("}");
                break;
            }
            case 9: case 11:
            {
                std::string target = std::to_string(Bits(cf.d0, 0, 13));
                std::string take = Bits(cf.d0, 13, 1) ? "true"
                                 : Bits(cf.d0, 14, 1) ? std::string("p == ") + (Bits(cf.d1, 10, 1) ? "true" : "false")
                                 : "boolBit(" + std::to_string(Bits(cf.d1, 2, 8)) + "u) == " + (Bits(cf.d1, 10, 1) ? "1u" : "0u");
                Line("pc = " + next + ";");
                if (op == 9) Line("if ((" + take + ") && callDepth < 4) { callStack[callDepth++] = " + next + "; pc = " + target + "; }");
                else Line("if (" + take + ") pc = " + target + ";");
                break;
            }
            case 10: Line("pc = callDepth > 0 ? callStack[--callDepth] : " + next + ";"); break;
            default: Line("pc = " + next + ";"); break;
            }
            Line("break;");
            --indent;
            Line("}");
        }
        Line("default: pc = -1; break;");
        Line("}");
        --indent;
        Line("}");
    }
};

}  // namespace

namespace {

const char* kInterlockHeader = "#version 450\n#extension GL_ARB_fragment_shader_interlock : require\n";

// The prelude without its #version line (the EDRAM pixel shader puts the
// interlock extension right after #version).
std::string PreludeBody(bool fast)
{
    std::string p = Prelude(fast);
    return p.substr(p.find('\n') + 1);
}

// Pixel shader of the EDRAM renderer around a translated body.
std::string EdramPixelShader(const std::string& body, uint32_t temps, bool fast)
{
    std::string g = kInterlockHeader;
    g += PreludeBody(fast);
    g += "precise vec4 r[" + std::to_string(temps) + "];\n";
    g += R"GLSL(layout(pixel_interlock_ordered) in;
layout(std430, set = 0, binding = 3) coherent buffer EdramBlock { uint edram[]; };
layout(location = 0) in vec4 vin[16];
layout(location = 16) noperspective in float xeZ;
vec4 oc0, oc1, oc2, oc3;
uint ocWritten = 0u;
)GLSL";
    g += kEdramLib;
    g += "#define XE_FRAGMENT 1\n";
    g += ExactSampler();
    g += kEdramPixel;
    g += "void main()\n{\n    cBase = D.psBase; cSize = D.psSize;\n";
    g += "    for (int k = 0; k < " + std::to_string(temps) + "; ++k) r[k] = vec4(0.0);\n";
    g += "    for (int k = 0; k < 16; ++k) if (uint(k) < D.interpCount) r[k] = vin[k];\n";
    g += "    if ((D.paramGen >> 31) != 0u && (D.paramGen & 0xFFu) < " + std::to_string(temps) +
         "u) r[D.paramGen & 0xFFu] = vec4(gl_FragCoord.xy, 0.0, 0.0);\n";
    g += "    oc0 = vec4(0.0, 0.0, 0.0, 1.0); oc1 = vec4(0.0); oc2 = vec4(0.0); oc3 = vec4(0.0);\n";
    g += body;
    g += R"GLSL(    vec4 outs[4] = vec4[4](oc0, oc1, oc2, oc3);
    uint px = uint(gl_FragCoord.x), py = uint(gl_FragCoord.y);
    bool front = gl_FrontFacing;
    float z = xeZ;
    bool k = killed;
    uint w = ocWritten;
    beginInvocationInterlockARB();
    xeEdramPixel(px, py, front, z, k, w, outs);
    endInvocationInterlockARB();
}
)GLSL";
    return g;
}

// Pixel shader of the native renderer (host render targets) around a translated
// body: colour exports to the attachments, alpha test and kill by discard; depth,
// stencil and blending are the pipeline's fixed function.
std::string HostPixelShader(const std::string& body, uint32_t temps, bool fast, uint32_t exportMask, uint32_t relaxed)
{
    std::string g = Prelude(fast);
    g += std::string((relaxed & 2) ? "" : "precise ") + "vec4 r[" + std::to_string(temps) + "];\n";
    g += "layout(location = 0) in vec4 vin[16];\n";
    g += "layout(location = 16) noperspective in float xeZ;   // unused: matches the vertex shader's interface\n";
    g += "layout(location = 17) in vec3 xeMcur;\nlayout(location = 18) in vec3 xeMprev;\nlayout(location = 4) out vec2 xeMotion;\n";
    for (int i = 0; i < 4; ++i)
        g += ((exportMask >> i) & 1 ? "layout(location = " + std::to_string(i) + ") out vec4 oc" : std::string("vec4 oc")) +
             std::to_string(i) + ";\n";
    g += "uint ocWritten = 0u;\n";
    g += kEdramLib;
    g += "#define XE_FRAGMENT 1\n";
    g += ExactSampler();
    g += R"GLSL(bool alphaPass(uint f, float a, float ref)
{
    switch (f) { case 0u: return false; case 1u: return a < ref; case 2u: return a == ref; case 3u: return a <= ref;
                 case 4u: return a > ref; case 5u: return a != ref; case 6u: return a >= ref; default: return true; }
}
void main()
{
    cBase = D.psBase; cSize = D.psSize;
)GLSL";
    g += "    for (int k = 0; k < " + std::to_string(temps) + "; ++k) r[k] = vec4(0.0);\n";
    g += "    for (int k = 0; k < 16; ++k) if (uint(k) < D.interpCount) r[k] = vin[k];\n";
    // The pixel position in the game's pixels (resolution scale: D.fragScale = 1 / s).
    // Constant indices only, so r[] can stay in registers: a dynamic index put the whole array in scratch memory on
    // Intel (draws of a main-menu frame 168 -> 28 ms on a UHD 630, M12 step 4); the same result.
    if (temps <= 16)
        g += "    if ((D.paramGen >> 31) != 0u) for (int k = 0; k < " + std::to_string(temps) +
             "; ++k) if (uint(k) == (D.paramGen & 0xFFu)) r[k] = vec4(gl_FragCoord.xy * D.fragScale, 0.0, 0.0);\n";
    else
        g += "    if ((D.paramGen >> 31) != 0u && (D.paramGen & 0xFFu) < " + std::to_string(temps) +
             "u) r[D.paramGen & 0xFFu] = vec4(gl_FragCoord.xy * D.fragScale, 0.0, 0.0);\n";
    g += "    oc0 = vec4(0.0, 0.0, 0.0, 1.0); oc1 = vec4(0.0); oc2 = vec4(0.0); oc3 = vec4(0.0);\n";
    g += body;
    g += R"GLSL(    if (killed) discard;
    if ((D.alpha & 8u) != 0u && !alphaPass(D.alpha & 7u, oc0.w, D.alphaRef)) discard;
    oc0 *= D.rtScale.x; oc1 *= D.rtScale.y; oc2 *= D.rtScale.z; oc3 *= D.rtScale.w;
    // hc09: where the pixel was a frame before, in the target's pixels (none known: 32768, the depth's instead).
    if (kMotion != 0u)
        xeMotion = floatBitsToUint(D.winOfs.w) != 0u && xeMcur.z > 0.0 && xeMprev.z > 0.0
                       ? (xeMprev.xy / xeMprev.z - xeMcur.xy / xeMcur.z) / D.fragScale : vec2(32768.0);
}
)GLSL";
    return g;
}

}  // namespace

std::string HostNullPixelShader() { return "#version 450\nlayout(location = 16) noperspective in float xeZ;\nvoid main() {}\n"; }

// The native renderer (host render targets): the per-draw constants are one array in a plain storage buffer, the
// draw's element picked by a push constant, instead of a dynamic buffer offset. A set layout with no dynamic buffer
// can be an update-after-bind one, which Intel's driver needs for the 192 texture bindings (its ordinary limit is 64
// samplers per stage; HANDOFF_M12.md step 4). The block's members are unchanged, so `D.x` still works.
std::string DrawIndexedConsts(std::string g)
{
    const std::string head = "layout(std430, set = 0, binding = 1) readonly buffer DrawConstsBlock {";
    const std::string tail = "\n} D;\n";
    size_t h = g.find(head);
    size_t e = h == std::string::npos ? h : g.find(tail, h);
    if (e == std::string::npos) return g;
    g.replace(e, tail.size(), "\n};\nlayout(std430, set = 0, binding = 1) readonly buffer DrawConstsBlock { DrawConstsT dcs[]; };\n"
                              "layout(push_constant) uniform XePush { uint draw; } xePush;\n#define D dcs[xePush.draw]\n");
    g.replace(h, head.size(), "struct DrawConstsT {");
    const std::string read = ": D.c[cBase + uint(i)]; }";
    if (size_t r = g.find(read); r != std::string::npos) g.replace(r, read.size(), ": dcs[xePush.draw + cPrev].c[cBase + uint(i)]; }");
    return g;
}

TranslatedShader TranslateShader(const ShaderCode& code, const TranslateOptions& opt)
{
    TranslatedShader t;
    memset(t.slotFilter, 0xFF, sizeof(t.slotFilter));
    if (code.dw.size() < 3) { t.error = "empty shader"; return t; }
    Emitter e(code, t);
    e.hwSampler = opt.hwSampler;
    e.relaxed = opt.relaxed;
    e.Program();
    uint32_t temps = e.relTemp ? 128 : std::max<uint32_t>(16, e.maxTemp + 1);
    if (!e.vs)
    {
        t.glsl = opt.hostRt ? DrawIndexedConsts(HostPixelShader(e.body, temps, opt.fastMath, t.exportMask, opt.relaxed))
                            : EdramPixelShader(e.body, temps, opt.fastMath);
        return t;
    }
    std::string g = Prelude(opt.fastMath);
    g += "precise vec4 r[" + std::to_string(temps) + "];\n";
    g += kVertexFetch;
    g += "layout(location = 0) out vec4 o[16];\n";
    g += "layout(location = 16) noperspective out float xeZ;\n";
    g += "layout(location = 17) out vec3 xeMcur;\nlayout(location = 18) out vec3 xeMprev;\n";
    g += "out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; };\n";
    // Host render targets: the hardware depth test (z from gl_Position); EDRAM: the
    // pixel shader's epilogue tests xeZ itself.
    g += opt.hostRt ? "#define XE_HOST_Z (scr.z * cw)\n" : "#define XE_HOST_Z 0.0\n";
    g += kEdramLib;
    g += ExactSampler();
    // hc09: the position global: temporal upscaling runs the translated program twice (kMotion), inline both times (a
    // function holding it crashed NVIDIA's pipeline compiler, 616.92).
    g += "vec4 pos;\n";
    g += R"GLSL(// hc09: temporal upscaling: a position's place in the window (the game's pixels, without the jitter) times its w,
// with its w: linear across a triangle, so interpolated as it is and divided in the pixel shader.
vec3 xeScreenH(vec4 pp)
{
    bool xyFmt = (D.vte & 1u) != 0u, w0Fmt = (D.vte & 4u) != 0u;
    if (D.wideClip != 0.0) pp.x *= D.wideClip;
    float invW = w0Fmt ? (pp.w != 0.0 ? 1.0 / pp.w : 1.0) : pp.w;
    vec2 q = xyFmt ? pp.xy : pp.xy * invW;
    vec2 s = q * D.vpScale.xy + D.vpOffset.xy + D.winOfs.z;
    float cw = invW != 0.0 ? 1.0 / invW : 1.0;
    return vec3(s * cw, cw);
}
)GLSL";
    const std::string clearR = "    for (int k = 0; k < " + std::to_string(temps) + "; ++k) r[k] = vec4(0.0);\n";
    g += "void main()\n{\n";
    g += "    cBase = D.vsBase; cSize = D.vsSize;\n";
    g += clearR;
    g += "    for (int k = 0; k < 16; ++k) o[k] = vec4(0.0);\n";
    g += "    pos = vec4(0.0, 0.0, 0.0, 1.0);\n";
    g += "    r[0].x = float(gl_VertexIndex);\n";
    g += e.body;
    g += "    xeMcur = vec3(0.0); xeMprev = vec3(0.0);\n";
    g += "    if (kMotion != 0u)\n    {\n";
    g += "        vec4 posNow = pos, posPrev = pos;\n";
    g += "        uint prevOfs = floatBitsToUint(D.winOfs.w);\n";
    g += "        if (prevOfs != 0u)\n        {\n";
    g += "            vec4 oNow[16];\n            for (int k = 0; k < 16; ++k) oNow[k] = o[k];\n";
    g += "    " + clearR;
    g += "            pos = vec4(0.0, 0.0, 0.0, 1.0);\n            r[0].x = float(gl_VertexIndex);\n";
    g += "            loopDepth = 0; p = false; a0 = 0; ps = 0.0; xeRegLod = 0.0; xeGradH = vec3(0.0); xeGradV = vec3(0.0);\n";
    g += "            cPrev = prevOfs;\n            {\n" + e.body + "            }\n            cPrev = 0u;\n";
    g += "            posPrev = pos;\n            for (int k = 0; k < 16; ++k) o[k] = oNow[k];\n            pos = posNow;\n";
    g += "        }\n";
    g += "        xeMcur = xeScreenH(posNow);\n        xeMprev = xeScreenH(posPrev);\n";
    g += "    }\n";
    g += R"GLSL(    // Xenos viewport transform (PA_CL_VTE_CNTL) in the software rasterizer's
    // order of operations; the viewport maps clip space 1:1 to pixels
    // (x_window = x_ndc * K, K = 1 / rtSize.z), so no rounding is added.
    bool xyFmt = (D.vte & 1u) != 0u, zFmt = (D.vte & 2u) != 0u, w0Fmt = (D.vte & 4u) != 0u;
    if (D.wideClip != 0.0) pos.x *= D.wideClip;   // hc09: widescreen: a 3D draw sees wider (widescreen.h)
    float invW = w0Fmt ? (pos.w != 0.0 ? xdiv(1.0, pos.w) : 1.0) : pos.w;
    precise vec3 q = vec3(xyFmt ? pos.x : pos.x * invW, xyFmt ? pos.y : pos.y * invW, zFmt ? pos.z : pos.z * invW);
    precise vec3 scr;
    scr.x = ((q.x * D.vpScale.x + D.vpOffset.x) + D.winOfs.x) + D.winOfs.z;
    scr.y = ((q.y * D.vpScale.y + D.vpOffset.y) + D.winOfs.y) + D.winOfs.z;
    scr.z = q.z * D.vpScale.z + D.vpOffset.z;
    // hc09: widescreen (widescreen.h): the draw moves toward the centre by the squeeze factor (D.vpOffset.w is the
    // centre). Vertices past the screen's left and right edges stay (what the game keeps off-screen stays there), and
    // so do those on the edges of a plain draw (negative factor: no texture over 32x32; fades and bars keep the width).
    {
        float edge = D.vpScale.w < 0.0 ? 1.0 : -0.5;
        if (D.vpScale.w != 0.0 && scr.x > edge && scr.x < 2.0 * D.vpOffset.w - edge)
            scr.x = D.vpOffset.w + (scr.x - D.vpOffset.w) * abs(D.vpScale.w);
    }
    float cw = invW != 0.0 ? xdiv(1.0, invW) : 1.0;
    gl_Position = vec4(scr.xy * D.rtSize.zw * cw, XE_HOST_Z, cw);
    xeZ = scr.z;
    // Homogeneous clipping against w > 0 and the near/far planes, before the
    // divide, as the software rasterizer (PA_CL_CLIP_CNTL).
    bool clipOn = (D.clip.x & 1u) != 0u;
    gl_ClipDistance[0] = clipOn ? pos.w - 1e-6 : 1.0;
    gl_ClipDistance[1] = clipOn ? ((D.clip.x & 2u) != 0u ? pos.z : pos.z + pos.w) : 1.0;
    gl_ClipDistance[2] = clipOn ? pos.w - pos.z : 1.0;
}
)GLSL";
    t.glsl = opt.hostRt ? DrawIndexedConsts(std::move(g)) : std::move(g);
    return t;
}

std::string TextureDecodeShader(bool depthFloat)
{
    std::string s = R"GLSL(#version 450
// k_8_8_8_8 or k_24_8 (depth as grey) level 0, 2D: xenos_texture.cpp DecodeTextureLayer (tiled or linear
// block address, endian swap of the 4 bytes, fetch swizzle) into an RGBA8_UINT
// storage image. Byte moves only, so identical to the CPU decode.
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer GuestMemory { uint mem[]; };
layout(set = 0, binding = 1, rgba8ui) uniform writeonly uimage2D dst;
layout(push_constant) uniform Params { uint base, pitchBlocks, rowBytes, tiled, w, h, endian, swz, fmt, ox, oy; } P;
uint tiledOffset2D(uint x, uint y, uint pitchBlocks, uint bppLog2)
{
    uint outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
    uint innerBlocks = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outerBlocks | innerBlocks) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
           (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= P.w || id.y >= P.h) return;
    uvec2 b = id + uvec2(P.ox, P.oy);   // packed-mip base offset (xenos_texture.cpp PackedBaseOffset)
    uint off = P.tiled != 0u ? tiledOffset2D(b.x, b.y, P.pitchBlocks, 2u) : b.y * P.rowBytes + b.x * 4u;
    uint v = mem[((P.base + off) & 0x1FFFFFFFu) >> 2];
    if (P.endian == 1u) v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    else if (P.endian == 2u) v = (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    else if (P.endian == 3u) v = (v << 16) | (v >> 16);
    if (P.fmt == 22u) { imageStore(dst, ivec2(id), uvec4(v >> 24, v >> 24, v >> 24, 255u)); return; }   // depth: high byte as grey
    uint c[4] = uint[4](v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24);
    uvec4 o;
    for (int i = 0; i < 4; ++i)
    {
        uint s = (P.swz >> (3 * i)) & 7u;
        o[i] = s < 4u ? c[s] : s == 4u ? 0u : s == 5u ? 255u : c[i];
    }
    imageStore(dst, ivec2(id), o);
}
)GLSL";
    if (depthFloat)
    {
        // k_24_8: depth in the word's high 24 bits (D24S8), stencil in the low 8.
        auto rep = [&](const std::string& a, const std::string& b) { s.replace(s.find(a), a.size(), b); };
        rep("layout(set = 0, binding = 1, rgba8ui) uniform writeonly uimage2D dst;",
            "layout(set = 0, binding = 1, r32f) uniform writeonly image2D dst;");
        rep("if (P.fmt == 22u) { imageStore(dst, ivec2(id), uvec4(v >> 24, v >> 24, v >> 24, 255u)); return; }   // depth: high byte as grey",
            "imageStore(dst, ivec2(id), vec4(float(v >> 8) * (1.0 / 16777215.0), 0.0, 0.0, 1.0)); return;   // depth, 24 bits");
    }
    return s;
}

std::string TextureMipShader()
{
    return R"GLSL(#version 450
// One box-filtered mip level from the one above (xenos_raster.cpp GetTexture /
// vk_edram MakeMips): (a + b + c + d + 2) / 4 per channel, edge texels clamped.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8ui) uniform readonly uimage2D src;
layout(set = 0, binding = 1, rgba8ui) uniform writeonly uimage2D dst;
layout(push_constant) uniform Params { uint w, h, sw, sh; } P;
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= P.w || id.y >= P.h) return;
    int x0 = int(min(2u * id.x, P.sw - 1u)), x1 = int(min(2u * id.x + 1u, P.sw - 1u));
    int y0 = int(min(2u * id.y, P.sh - 1u)), y1 = int(min(2u * id.y + 1u, P.sh - 1u));
    uvec4 s = imageLoad(src, ivec2(x0, y0)) + imageLoad(src, ivec2(x1, y0)) + imageLoad(src, ivec2(x0, y1)) + imageLoad(src, ivec2(x1, y1));
    imageStore(dst, ivec2(id), (s + 2u) / 4u);
}
)GLSL";
}

std::string EdramGlslLibrary() { return ExactMath() + kEdramLib; }

std::string EdramNullPixelShader(bool fastMath) { return EdramPixelShader("", 16, fastMath); }

std::string EdramResolveShader()
{
    std::string g = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) buffer GuestMemory { uint mem[]; };
layout(std430, set = 0, binding = 3) buffer EdramBlock { uint edram[]; };
layout(push_constant) uniform ResolveConsts {
    int x0, y0; uint w, h;
    uint base, pitch, msaa, wide;
    uint src, srcFmt, sampleSel, pass;
    uint destBase, destPitch, destFmt, destBytes;
    uint endian, swapRB, depthBase, colorClear;
    uint colorClearLo, depthClear; float destScale; uint pad;
} R;
)GLSL";
    g += ExactMath();
    g += kEdramLib;
    g += R"GLSL(
uint tiledOffset2D(uint x, uint y, uint pitchBlocks, uint bppLog2)
{
    uint outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
    uint innerBlocks = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outerBlocks | innerBlocks) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
           (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}
// Endian swap of one GPU word (8in16, 8in32, 16in32) as the software's byte loop.
uint swapWord(uint v, uint endian, uint bytes)
{
    if (endian == 1u && bytes >= 2u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2u && bytes >= 4u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (endian == 3u && bytes >= 4u) return (v << 16) | (v >> 16);
    return v;
}
// Store the low `n` bytes of v at physical address `addr` (n = 1, 2: within one word).
void writeBytes(uint addr, uint v, uint n)
{
    uint wi = (addr >> 2) & 0x7FFFFFFu;
    if (n >= 4u) { mem[wi] = v; return; }
    uint sh = (addr & 3u) * 8u, m = (n == 1u ? 0xFFu : 0xFFFFu) << sh;
    atomicAnd(mem[wi], ~m);
    atomicOr(mem[wi], (v << sh) & m);
}
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= R.w || id.y >= R.h) return;
    uint x = uint(R.x0) + id.x, y = uint(R.y0) + id.y;
    bool wide = R.wide != 0u;
    uint samples = edramSamples(R.msaa);
    if (R.pass == 0u)
    {
        uvec2 v = uvec2(0u);
        if (R.src >= 4u) v.x = edram[edramIndex(R.base, R.pitch, R.msaa, false, x, y, 0u)];   // depth: the 24:8 sample
        else
        {
            uint first = R.sampleSel <= 3u ? R.sampleSel : R.sampleSel == 5u ? 2u : 0u;
            uint count = R.sampleSel <= 3u ? 1u : R.sampleSel == 6u ? 4u : 2u;
            if (first >= samples) first = 0u;
            if (first + count > samples) count = samples - first;
            precise vec4 acc = vec4(0.0);
            for (uint s = first; s < first + count; ++s)
            {
                uint k = edramIndex(R.base, R.pitch, R.msaa, wide, x, y, s);
                vec4 c = decodeRt(R.srcFmt, edram[k], wide ? edram[k + 1u] : 0u, false);
                for (int i = 0; i < 4; ++i) acc[i] += xdiv(c[i], float(count));
            }
            acc *= R.destScale;
            if (R.swapRB != 0u) acc = acc.zyxw;
            v = packResolve(R.destFmt, R.srcFmt, acc);
        }
        uint bpp = R.destBytes == 1u ? 0u : R.destBytes == 2u ? 1u : R.destBytes == 4u ? 2u : 3u;
        uint addr = R.destBase + tiledOffset2D(x, y, R.destPitch, bpp);
        uint n = min(R.destBytes, 4u);
        writeBytes(addr, swapWord(v.x, R.endian, n), n);
        if (R.destBytes == 8u) writeBytes(addr + 4u, swapWord(v.y, R.endian, 4u), 4u);
    }
    else if (R.pass == 1u)   // clear colour (in the target's own encoding)
    {
        for (uint s = 0u; s < samples; ++s)
        {
            uint k = edramIndex(R.base, R.pitch, R.msaa, wide, x, y, s);
            edram[k] = R.colorClear;
            if (wide) edram[k + 1u] = R.colorClearLo;
        }
    }
    else   // clear depth
    {
        for (uint s = 0u; s < samples; ++s) edram[edramIndex(R.depthBase, R.pitch, R.msaa, false, x, y, s)] = R.depthClear;
    }
}
)GLSL";
    return g;
}

std::string NativeResolveShader(bool depth, bool ms)
{
    std::string g = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) buffer GuestMemory { uint mem[]; };
layout(push_constant) uniform NativeResolveConsts {
    int x0, y0; uint w, h;
    uint srcFmt, destBase, destPitch, destFmt;
    uint destBytes, endian, swapRB; float destScale;
    uint samples, sampleSel, scale, scaledRows;
    uint scaled, scaledPitch; int destX, destY;
    float srcPerOut;
} R;
layout(std430, set = 0, binding = 3) buffer ScaledCopy { uint smem[]; };
)GLSL";
    std::string sfx = ms ? "MS" : "";
    g += depth ? "layout(set = 0, binding = 1) uniform sampler2D" + sfx + " srcDepth;\nlayout(set = 0, binding = 2) uniform usampler2D" + sfx +
                     " srcStencil;\n#define XE_DEPTH 1\n"
               : "layout(set = 0, binding = 1) uniform sampler2D" + sfx + " srcColor;\n";
    if (ms) g += "#define XE_MS 1\n";
    g += ExactMath();
    g += kEdramLib;
    g += R"GLSL(
uint tiledOffset2D(uint x, uint y, uint pitchBlocks, uint bppLog2)
{
    uint outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
    uint innerBlocks = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outerBlocks | innerBlocks) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
           (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}
uint swapWord(uint v, uint endian, uint bytes)
{
    if (endian == 1u && bytes >= 2u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2u && bytes >= 4u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (endian == 3u && bytes >= 4u) return (v << 16) | (v >> 16);
    return v;
}
void writeBytes(uint addr, uint v, uint n)
{
    uint wi = (addr >> 2) & 0x7FFFFFFu;
    if (n >= 4u) { mem[wi] = v; return; }
    uint sh = (addr & 3u) * 8u, m = (n == 1u ? 0xFFu : 0xFFFFu) << sh;
    atomicAnd(mem[wi], ~m);
    atomicOr(mem[wi], (v << sh) & m);
}
// The scaled copy (resolution scale): source pixel (sx, sy) of the scaled target, as the word the guest copy gets.
void writeScaled(uint sx, uint sy, uint word)
{
    int X = int(sx) + R.destX * int(R.scale), Y = int(sy) + R.destY * int(R.scale);
    if (X >= 0 && Y >= 0 && uint(X) < R.scaledPitch && uint(Y) < R.scaledRows) smem[uint(Y) * R.scaledPitch + uint(X)] = word;
}
#ifndef XE_DEPTH
uint packWord(vec4 c)   // a 4-byte destination's word, as main() packs the guest copy's
{
    precise vec4 d = c * R.destScale;
    if (R.swapRB != 0u) d = d.zyxw;
    return swapWord(packResolve(R.destFmt, R.srcFmt, d).x, R.endian, 4u);
}
#endif
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= R.w || id.y >= R.h) return;
    uint x = uint(R.x0) + id.x, y = uint(R.y0) + id.y;
    uvec2 v = uvec2(0u);
    // Resolution scale S: the guest pixel (x, y) is the S x S block from (S x, S y) of the source.
    uint S = R.scale;
#ifdef XE_DEPTH
    if (R.scaled != 0u)
        for (uint j = 0u; j < S; ++j)
            for (uint i = 0u; i < S; ++i)
            {
                ivec2 q = ivec2(x * S + i, y * S + j);
                ivec2 qs = R.srcPerOut == 1.0 ? q : ivec2((vec2(q) + 0.5) * R.srcPerOut);
                float dq = texelFetch(srcDepth, qs, 0).r;
                uint sq = texelFetch(srcStencil, qs, 0).r;
                writeScaled(uint(q.x), uint(q.y), swapWord((uint(clamp(dq, 0.0, 1.0) * 16777215.0 + 0.5) << 8) | (sq & 0xFFu), R.endian, 4u));
            }
    ivec2 p = ivec2(x * S + S / 2u, y * S + S / 2u);   // the block's centre
    if (R.srcPerOut != 1.0) p = ivec2((vec2(x * S, y * S) + 0.5 * float(S)) * R.srcPerOut);
    float d = texelFetch(srcDepth, p, 0).r;   // MS: sample 0, as the EDRAM copy
    uint s = texelFetch(srcStencil, p, 0).r;
    v.x = (uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5) << 8) | (s & 0xFFu);
#else
#ifdef XE_MS
    uint first = R.sampleSel <= 3u ? R.sampleSel : R.sampleSel == 5u ? 2u : 0u;
    uint count = R.sampleSel <= 3u ? 1u : R.sampleSel == 6u ? 4u : 2u;
    if (first >= R.samples) first = 0u;
    if (first + count > R.samples) count = R.samples - first;
    precise vec4 c = vec4(0.0);
    float nb = float(count * S * S);
    for (uint j = 0u; j < S; ++j)
        for (uint i = 0u; i < S; ++i)
        {
            precise vec4 cs = vec4(0.0);   // the scaled pixel: its selected samples
            for (uint k = first; k < first + count; ++k)
            {
                vec4 t = texelFetch(srcColor, ivec2(x * S + i, y * S + j), int(k));
                c += t / nb;
                cs += t / float(count);
            }
            if (R.scaled != 0u) writeScaled(x * S + i, y * S + j, packWord(cs));
        }
#else
    precise vec4 c = texelFetch(srcColor, ivec2(x * S, y * S), 0);
    if (S > 1u)
    {
        if (R.scaled != 0u) writeScaled(x * S, y * S, packWord(c));
        float nb = float(S * S);
        c = c / nb;
        for (uint j = 0u; j < S; ++j)
            for (uint i = 0u; i < S; ++i)
                if ((i | j) != 0u)
                {
                    vec4 t = texelFetch(srcColor, ivec2(x * S + i, y * S + j), 0);
                    c += t / nb;
                    if (R.scaled != 0u) writeScaled(x * S + i, y * S + j, packWord(t));
                }
    }
#endif
    c *= R.destScale;
    if (R.swapRB != 0u) c = c.zyxw;
    v = packResolve(R.destFmt, R.srcFmt, c);
#endif
    uint bpp = R.destBytes == 1u ? 0u : R.destBytes == 2u ? 1u : R.destBytes == 4u ? 2u : 3u;
    uint addr = R.destBase + tiledOffset2D(x, y, R.destPitch, bpp);
    uint n = min(R.destBytes, 4u);
    writeBytes(addr, swapWord(v.x, R.endian, n), n);
    if (R.destBytes == 8u) writeBytes(addr + 4u, swapWord(v.y, R.endian, 4u), 4u);
}
)GLSL";
    return g;
}

const char* RectListGeometryShader()
{
    return R"GLSL(#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 gi[][16];
layout(location = 16) noperspective in float giz[];
layout(location = 0) out vec4 go[16];
layout(location = 16) noperspective out float goz;
layout(location = 17) out vec3 goMcur;   // hc09: temporal upscaling's motion (none for rectangles)
layout(location = 18) out vec3 goMprev;
in gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
layout(std430, set = 0, binding = 1) readonly buffer DrawConstsBlock {
    vec4 c[512]; uint bools[8]; uint loops[32]; uint fetch[192];
    vec4 vpScale; vec4 vpOffset; vec4 rtSize;
} D;
void Emit(vec4 pos, int src)
{
    gl_Position = pos;
    for (int k = 0; k < 16; ++k) go[k] = gi[src][k];
    goz = giz[src];
    goMcur = vec3(0.0); goMprev = vec3(0.0);
    EmitVertex();
}
void main()
{
    // Rectangle: the vertex opposite the longest edge (in pixels) is mirrored
    // across it (xenos_raster.cpp: interpolators and screen z as p + q - o).
    vec2 s[3];
    for (int i = 0; i < 3; ++i) s[i] = gl_in[i].gl_Position.xy / gl_in[i].gl_Position.w / D.rtSize.zw;
    float lab = dot(s[1] - s[0], s[1] - s[0]), lbc = dot(s[2] - s[1], s[2] - s[1]), lca = dot(s[0] - s[2], s[0] - s[2]);
    int o = 0, pp = 1, qq = 2;
    if (lab >= lbc && lab >= lca) { o = 2; pp = 0; qq = 1; }
    else if (lca >= lbc) { o = 1; pp = 2; qq = 0; }
    for (int i = 0; i < 3; ++i) Emit(gl_in[i].gl_Position, i);
    EndPrimitive();
    vec4 P = gl_in[pp].gl_Position, Q = gl_in[qq].gl_Position, O = gl_in[o].gl_Position;
    float w = P.w;
    vec4 R = vec4((P.xy / P.w + Q.xy / Q.w - O.xy / O.w) * w, 0.0, w);
    Emit(P, pp);
    Emit(Q, qq);
    gl_Position = R;
    for (int k = 0; k < 16; ++k) go[k] = gi[pp][k] + gi[qq][k] - gi[o][k];
    goz = giz[pp] + giz[qq] - giz[o];
    EmitVertex();
    EndPrimitive();
}
)GLSL";
}

std::vector<uint32_t> VertexFetchSlots(const xenos::ShaderCode& code)
{
    std::vector<uint32_t> slots;
    const auto& dw = code.dw;
    uint32_t n = CfCount(dw);
    for (uint32_t cfi = 0; cfi < n; ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        if (!CfIsExec(cf.op())) continue;
        uint32_t addr = Bits(cf.d0, 0, 12), count = Bits(cf.d0, 12, 3), seq = Bits(cf.d0, 16, 12);
        for (uint32_t k = 0; k < count; ++k)
        {
            uint32_t ia = addr + k;
            if (3 * ia + 2 >= dw.size()) break;
            const uint32_t* in = &dw[3 * ia];
            if (!((seq >> (2 * k)) & 1) || Bits(in[0], 0, 5) != 0 || Bits(in[1], 30, 1)) continue;   // vfetch, not mini
            uint32_t vc = Bits(in[0], 20, 5) * 3 + Bits(in[0], 25, 2);
            if (std::find(slots.begin(), slots.end(), vc) == slots.end()) slots.push_back(vc);
        }
    }
    return slots;
}

}  // namespace hc09vk
