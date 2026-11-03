#pragma once
// Xenos shader microcode -> GLSL 4.50 (Vulkan), for the GPU backend.
// A code-generating mirror of xenos::ShaderInterpreter (runtime/xenos/
// xenos_shader.cpp): same ALU semantics (0 * x = 0 multiplies, predicates,
// a0/aL relative addressing, loops, calls), vertex fetch from guest memory
// in the shader, texture fetch through combined image samplers. Portable:
// no Vulkan headers; the GLSL is compiled to SPIR-V by the caller.
//
// Resource interface (set 0) shared by every translated shader:
//   binding 0: storage buffer, guest physical memory as uint[] (vertex fetch)
//   binding 1: storage buffer (dynamic offset), struct DrawConsts below
//   binding 2: sampler2D tex2d[32] (texture fetch constant slots)
//   binding 3: storage buffer, EDRAM as uint[] (pixel shader epilogue)
//   binding 4: sampler3D tex3d[32], binding 5: sampler2DArray texCube[32]
//   (cube faces / stacked layers); which of 2/4/5 a slot uses: DrawConsts::texSize[slot].z
//   bindings 6-8: the same textures as usampler2D/3D/2DArray (RGBA8_UINT views)
// Specialization constants 0/1: the texture kind of slots 0-15 / 16-31, 2 bits each.
// Vertex shader outputs: location 0..15 = export registers 0..15
// (interpolators), 16 = screen-space z; position from export 62 with the
// Xenos viewport transform applied in the shader. Pixel shader inputs:
// location 0..16; no outputs (colours go to EDRAM).
#include <cstdint>
#include <string>

#include "xenos_shader.h"

namespace hc09vk {

// Mirrors the GLSL DrawConsts block (std430); filled per draw on the CPU.
struct DrawConsts
{
    float c[512][4];          // ALU constants (VS window at vsBase, PS at psBase)
    uint32_t bools[8];
    uint32_t loops[32];
    uint32_t fetch[192];      // fetch constants: 32 x 6 dwords (= 96 vertex fetch x 2)
    float vpScale[4];         // viewport scale x, y, z (1 where VTE disables it)
    float vpOffset[4];        // offset x, y, z (window and half-pixel offsets: winOfs)
    float rtSize[4];          // K, K, 1/K, 1/K: pixels per clip-space unit (viewport (-K, -K, 2K, 2K));
                              // native at resolution scale s: K/s, K/s, s/K, s/K (window = s * the 1x position)
    uint32_t vte;             // bit 0 xy pre-divided, bit 1 z pre-divided, bit 2 W is W (else 1/W)
    uint32_t interpCount;     // PS inputs taken from interpolators
    uint32_t paramGen;        // bit 31: enabled; low byte: register receiving the pixel position
    uint32_t alpha;           // bits 0-2 alpha func, bit 3 alpha test enabled
    uint32_t vsBase, vsSize, psBase, psSize;   // SQ_VS_CONST / SQ_PS_CONST windows
    float alphaRef;
    float fragScale;          // 1 / resolution scale: gl_FragCoord in the game's (1x) pixels (VPOS)
    float pad0[2];
    float rtScale[4];         // 2^exp_bias per colour target
    float texSize[32][4];     // width, height, kind (0 2D, 1 3D, 2 cube, 3 stacked), depth/layers of the texture at each slot
    // EDRAM renderer (vk_edram): render state read by the pixel shader epilogue.
    uint32_t rtBase[4];       // EDRAM tile of colour target n
    uint32_t rtFmt[4];        // RB_COLORn_INFO colour format; bit 8: target written by this draw
    uint32_t rtBlend[4];      // RB_BLENDCONTROLn
    uint32_t rtMask[4];       // RB_COLOR_MASK nibble of target n
    uint32_t surf[4];         // pitch, MSAA mode, depth base tile, depth is D24FS8
    uint32_t ds[4];           // RB_DEPTHCONTROL, RB_STENCILREFMASK, RB_STENCILREFMASK_BF, flags (kDs*)
    float blendConst[4];      // RB_BLEND_RED..ALPHA
    uint32_t clip[4];         // x: bit 0 near/far/w clipping on, bit 1 D3D clip space (0 <= z <= w)
    float winOfs[4];          // window offset x, y and the half-pixel offset, added as the software does
};
// DrawConsts::ds[3] flags.
constexpr uint32_t kDsShade = 1;      // the pixel shader runs (colour on or alpha test)
constexpr uint32_t kDsColorOn = 2;    // some colour target is written

struct TranslatedShader
{
    std::string glsl;
    std::string error;            // non-empty: translation failed
    uint32_t exportMask = 0;      // VS: interpolator exports 0-15 written; PS: colour exports 0-3 written
    uint32_t textureSlots = 0;    // fetch constant slots sampled
    uint8_t slotFilter[32];       // instruction filter override per slot: mag | min << 2 | mip << 4 (3 = from constant)
    uint32_t unsupported = 0;     // bit 1: unknown vertex format (vertex fetch in a pixel shader included)
    bool usesKill = false;
    bool dispatcher = false;      // non-linear control flow (loops/calls/jumps): pc-dispatch loop
    uint64_t constUsed[4] = {};   // ALU constants of the window read (bit i: constant i), without relative addressing
    bool constRelative = false;   // relative addressing (a0 / aL): any constant of the window
};

// For the EDRAM renderer (vk_edram). The vertex shader also outputs the
// screen-space z (location 16, noperspective) and near/far/w clip distances;
// the pixel shader has no colour outputs and ends with the EDRAM epilogue —
// inside fragment shader interlock, the per-pixel work of the software
// rasterizer (depth/stencil, alpha test, blending, format encoding) on the
// EDRAM storage buffer (binding 3).
// Texture fetches sample like the software rasterizer (texelFetch on the RGBA8_UINT
// views, bindings 6-8, the software's filtering and LOD); `hwSampler`: through
// the hardware samplers instead (bindings 2/4/5; +-1 per filtered texel).
struct TranslateOptions
{
    bool hwSampler = false;   // hardware samplers instead of the software's sampling (xeSample)
    bool fastMath = false;    // the GPU's fp32 division/sqrt/exp2/log2/sin/cos instead of the
                              // correctly rounded ones (about half the GPU time of a 3D frame)
    // Native renderer (vk_native, host render targets): the vertex shader puts the
    // screen-space z into gl_Position.z (hardware depth test); the pixel shader has
    // colour outputs at locations 0-3 (the exported colours, scaled by the targets'
    // exponent bias) and does the alpha test and kill with discard.
    bool hostRt = false;
    // Experiment for weak GPUs (M12 step 4): ALU temporaries without `precise` (the compiler may fuse and reorder).
    uint32_t relaxed = 0;     // bit 1 (2): no `precise` on ALU temporaries (dev: HC09_NATIVE_RELAXED=2; no gain measured)
};
TranslatedShader TranslateShader(const xenos::ShaderCode& code, const TranslateOptions& opt = {});
// The EDRAM library: GLSL ports of the software rasterizer's render-target
// arithmetic (edramIndex, decodeRt/encodeRt/clampRt, decodeDepth/encodeDepth,
// xcompare, stencilOp, blendFactor/blendOp, packResolve) and the correctly
// rounded math they use (xdiv, xsqrtf, xexp2, xlog2, xsin, xcos). Self-contained.
std::string EdramGlslLibrary();
// Native renderer: pixel shader for draws without one (depth/stencil only).
std::string HostNullPixelShader();
// EDRAM renderer: pixel shader for draws without one (depth/stencil only).
std::string EdramNullPixelShader(bool fastMath = false);
// EDRAM renderer: compute shader performing a resolve (copy to memory, then
// clears) — xenos_raster.cpp Resolve on the EDRAM buffer. Push constants:
// struct ResolveConsts below. Bindings: 0 guest memory, 3 EDRAM.
std::string EdramResolveShader();
// Compute shader decoding a k_8_8_8_8 2D texture (level 0) from guest memory into
// an RGBA8_UINT storage image (binding 0 guest memory, 1 the image), exactly as
// the CPU decoder: used for textures a pending resolve wrote. Push constants:
// base, pitchBlocks, rowBytes, tiled, width, height, endian, swizzle, format (9 x uint;
// format 6 k_8_8_8_8 or 22 k_24_8, the depth's high byte as grey).
// depthFloat: the variant for k_24_8 depth: the 24-bit depth as a 0..1 float in R of an R32F image (the
// console samples the whole depth, as Xenia's R32_FLOAT load does; the RGBA8 variant keeps only its high byte).
std::string TextureDecodeShader(bool depthFloat = false);
// Compute shader making one mip level from the level above (box filter as the
// CPU's): bindings 0 source, 1 destination (rgba8ui storage images);
// push constants: width, height, source width, source height.
std::string TextureMipShader();
struct ResolveConsts
{
    int32_t x0, y0;               // rectangle origin (pixels)
    uint32_t w, h;
    uint32_t base, pitch, msaa, wide;
    uint32_t src, srcFmt, sampleSel, pass;   // pass: 0 copy, 1 clear colour, 2 clear depth
    uint32_t destBase, destPitch, destFmt, destBytes;
    uint32_t endian, swapRB, depthBase, colorClear;
    uint32_t colorClearLo, depthClear;
    float destScale;
    uint32_t pad;
};

// Native renderer: compute shader performing a resolve from a host render
// target into guest memory (tiling, format packing, endian as EdramResolveShader).
// `depth`: bindings 1/2 = the depth and stencil aspects of a D24S8 image (the copy
// writes the raw 24:8 word); else binding 1 = the colour image (float view).
// Binding 0 = guest memory. Push constants: struct NativeResolveConsts.
// `ms`: the source is multisampled (sampler2DMS): colour averages the samples
// the resolve selects (sampleSel as xenos::ResolveInfo), depth copies sample 0.
// `scale` s > 1: the source target is s times the guest size; a guest pixel is the
// mean of its s x s block (colour) or the block's centre (depth). `scaled` != 0: every
// pixel of the source also goes to the scaled copy (binding 3: the words the guest
// copy gets, untiled, scaledPitch x scaledRows, at texel (x + destX, y + destY) * s).
std::string NativeResolveShader(bool depth, bool ms = false);
struct NativeResolveConsts
{
    int32_t x0, y0;
    uint32_t w, h;
    uint32_t srcFmt, destBase, destPitch, destFmt;
    uint32_t destBytes, endian, swapRB;
    float destScale;
    uint32_t samples, sampleSel, scale, scaledRows;
    uint32_t scaled, scaledPitch;
    int32_t destX, destY;
};

// GLSL of the fixed geometry shader that expands rectangle lists (3 vertices
// per rectangle; the 4th mirrors the vertex opposite the longest edge; the
// interpolators and the screen-space z likewise).
const char* RectListGeometryShader();
// The native renderer's form of the per-draw constants (an array indexed by a push constant; xe_glsl.cpp).
std::string DrawIndexedConsts(std::string glsl);

// Vertex fetch constant slots (0..95) a vertex shader reads with full vfetch
// instructions (each slot: 2 dwords at FETCH_CONSTANTS; address in dword 0,
// size in dwords at bits 2..25 of dword 1). Diagnostics (HC09_GPU_HAZARD).
std::vector<uint32_t> VertexFetchSlots(const xenos::ShaderCode& code);

}  // namespace hc09vk
