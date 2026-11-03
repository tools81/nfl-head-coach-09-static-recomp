#pragma once
// Software rasterizer for Xenos draws: EDRAM model, vertex/pixel shading via
// the interpreter, blending, and resolves (EDRAM -> memory). Used offline to
// render captured frames to PNG. Portable C++17 (see xenos.h).
#include <cstdint>
#include <string>

#include "xenos.h"
#include "xenos_shader.h"

namespace xenos {

struct RasterStats { uint32_t draws = 0, triangles = 0, clipped = 0, culled = 0, pixels = 0, resolves = 0, skipped = 0; };

// Execute one draw (edram_mode 4/5) or resolve (edram_mode 6).
// `debugDir`: if non-empty, every resolve is also written there as PNG.
void RasterDraw(const DrawPacket& d, const ShaderCode* vs, const ShaderCode* ps, const std::string& debugDir,
                std::string* note);
RasterStats& RasterGetStats();
// Number of the draw about to be rasterized in the capture's numbering (for
// HC09_PICK_VERTEX=<draw>:<index>, which traces that vertex's shader run).
void RasterSetDrawNumber(uint32_t n);
// Start of a captured frame: zero EDRAM and the stats. Frames between captures
// are not rendered, so EDRAM contents left from an older capture would be stale
// (and would make a live capture differ from its replay).
void RasterBeginFrame();
// Between frames rendered without capture output: drop cached textures (the
// CPU may have rewritten them), keep EDRAM.
void RasterNewFrame();

// The EDRAM model (2048 tiles of 80x16 dwords) and the dword index of sample
// `s` of pixel (x, y) of a surface — for a renderer that keeps its own copy
// of render targets and hands them back before a resolve.
uint32_t* RasterEdram();
uint32_t RasterEdramIndex(uint32_t base, uint32_t pitch, uint32_t msaa, bool wide, uint32_t x, uint32_t y, uint32_t s);

// A resolve (edram_mode 6: RB_COPY_* registers) decoded: the EDRAM rectangle
// and surface it reads, the destination in memory, and the clears that follow
// the copy. Shared by the software Resolve and a GPU renderer.
struct ResolveInfo
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // pixel rectangle, multiples of 8 (window offset applied)
    uint32_t src = 0;                            // 0-3 colour target, 4+ depth
    uint32_t base = 0, pitch = 0, msaa = 0;      // source surface (EDRAM tile, pixels, 0/1/2 = 1x/2x/4x)
    bool wide = false;                           // source is 64 bpp
    uint32_t srcFmt = 0, sampleSel = 0;          // colour format; sample select (0-3 one, 4 = 0+1, 5 = 2+3, 6 = all)
    bool copy = false;                           // copy to memory (cmd != 3 and a known destination format)
    uint32_t destBase = 0, destPitch = 0, destHeight = 0, destFmt = 0, destBytes = 0, endian = 0;
    bool swapRB = false;
    float destScale = 1.0f;                      // 2^copy_dest_exp_bias
    bool clearColor = false, clearDepth = false; // after the copy
    uint32_t colorClear = 0, colorClearLo = 0, depthClear = 0;
    uint32_t depthBase = 0;                      // depth surface (same pitch and MSAA) for the depth clear
};
// Decode the current resolve. False (and `note` set) when there is nothing to do.
bool RasterDecodeResolve(ResolveInfo& r, std::string* note);
// Bookkeeping after an observer performed resolve `r` in its own EDRAM: counts
// it, writes the note and, if `debugDir` is set, resolve_NN_<dest>.png from memory.
void RasterResolveDone(const ResolveInfo& r, const std::string& debugDir, std::string* note);

// The software rasterizer's render-target arithmetic, exposed so another
// implementation (the GPU renderer's GLSL, tools/xe_edram_test.cpp) can be
// checked against it bit for bit. `p`/`out`: one dword, two for 64 bpp.
void RasterDecodeRt(uint32_t fmt, const uint32_t* p, float c[4], bool linear);
void RasterEncodeRt(uint32_t fmt, const float c[4], uint32_t* out, bool linear);
void RasterClampRt(uint32_t fmt, float c[4]);
float RasterDecodeDepth(uint32_t d24, bool isFloat);
uint32_t RasterEncodeDepth(float z, bool isFloat);
// Blend of one component (i = 0..3) with RB_BLENDCONTROL `blend`, as RasterDraw.
float RasterBlend(uint32_t blend, const float src[4], const float dst[4], const float k[4], int i);
// Resolve packing of a colour into destination format `destFmt` (source format `srcFmt`).
void RasterPackResolve(uint32_t destFmt, uint32_t srcFmt, const float c[4], uint32_t out[2]);

// A second renderer fed with every draw of a captured frame (the Vulkan
// backend, tools/replay_gpu.cpp). Called before each draw or resolve with the
// register file describing it. Unless the observer performs resolves itself
// (PerformsResolves, with softwareRaster off), resolves (edram_mode 6) run in
// the software path on RasterEdram(), so the observer must have written the
// render targets it holds back to EDRAM when OnDraw returns for one;
// OnResolved follows (the resolve may have cleared EDRAM and written memory).
struct DrawObserver
{
    virtual ~DrawObserver() = default;
    virtual void OnDraw(const DrawPacket& d, uint64_t vsHash, const ShaderCode* vs, uint64_t psHash, const ShaderCode* ps) = 0;
    virtual void OnResolved() = 0;
    // An observer with its own EDRAM: OnResolve must leave the resolve's result
    // in memory (copy) and in its EDRAM (clears) when it returns.
    virtual bool PerformsResolves() const { return false; }
    virtual void OnResolve(const ResolveInfo&) {}
    // XE_SWAP: the frame is complete; `frontBuffer` is what is presented.
    virtual void OnFrameEnd(uint32_t frontBuffer, uint32_t width, uint32_t height) { (void)frontBuffer; (void)width; (void)height; }
};
// `softwareRaster` false: only the observer renders draws (resolves stay in software).
void SetDrawObserver(DrawObserver* observer, bool softwareRaster);
// A renderer fed with every draw, resolve (it must perform them: PerformsResolves)
// and frame end of the running game, captured or not (hc09.exe HC09_GPU_LIVE).
void SetLiveObserver(DrawObserver* observer);

}  // namespace xenos
