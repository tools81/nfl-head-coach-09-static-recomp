#pragma once
// Native renderer: the GPU state of a draw, derived from the game's D3D device
// structure (docs/NATIVE_RENDERER.md) instead of from the PM4 packets D3D writes.
//
// The device shadows the Xenos register file (d3dcap::kShadowBlocks, fetch,
// float/bool/loop constants) with 64-bit dirty masks; a draw flushes the dirty
// parts into the ring. A few things bypass the shadow and go straight into the
// ring (constants loaded by GpuBeginShaderConstantF4 / GpuLoadShaderConstantF4Pointer,
// the window scissor, the draw's own index offset): RegisterState keeps a
// register file that follows the same rules, so it equals the register file the
// GPU sees at each draw. Portable C++17 (no OS headers).
#include <cstdint>
#include <utility>
#include <vector>

#include "d3d_stream.h"
#include "xenos.h"

// Kept out of line (the sampling profiler, HC09_PROFILE, then names the part of a hot function).
#if defined(_MSC_VER) && !defined(__clang__)
#define HC09_NOINLINE __declspec(noinline)
#else
#define HC09_NOINLINE __attribute__((noinline))
#endif

namespace d3dn {

inline uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t BE64(const uint8_t* p) { return (uint64_t(BE32(p)) << 32) | BE32(p + 4); }

// Guest memory as the derivation reads it (big-endian words at guest virtual addresses).
struct GuestMemory
{
    virtual ~GuestMemory() = default;
    virtual uint32_t Load32(uint32_t va) const = 0;
    // The bytes at [va, va + n) as one host pointer, or nullptr (then use Load32).
    virtual const uint8_t* Ptr(uint32_t va, uint32_t n) const { (void)va; (void)n; return nullptr; }
};

class RegisterState
{
public:
    RegisterState() : regs_(xenos::kRegisterCount, 0) {}
    // Start from the shadow as it is (the start of a capture): every register,
    // all constants.
    void Reset(const uint8_t* device);
    // A draw: `pre` = the device when the draw was called (its dirty masks say
    // which constants D3D sends now), `post` = after (the state D3D computed
    // while flushing: SQ_PROGRAM_CNTL, RB_MODECONTROL, ...). `indexOffset`: the
    // draw's VGT_INDX_OFFSET. A shader (re)bound by the draw loads its literal
    // constants from its object (`mem`).
    void Draw(const uint8_t* pre, const uint8_t* post, uint32_t indexOffset, const GuestMemory& mem);
    // Constants written into the ring directly (`data`: count float4, big-endian).
    void RingConstants(bool pixel, uint32_t start, uint32_t count, const uint8_t* data);
    // Constants the GPU loads from guest memory when it executes the draw
    // (GpuLoadShaderConstantF4Pointer: LOAD_ALU_CONSTANT). Their values are not
    // known here: External() gives the address until the shadow or a ring write
    // sets them again (static command buffers rely on this: the game rewrites
    // the memory, e.g. bone matrices, every frame and runs the same buffer).
    void PointerConstants(bool pixel, uint32_t start, uint32_t count, uint32_t address);
    // Guest address of float constant k (0-511) if it is loaded from memory, else 0.
    uint32_t External(uint32_t k) const { return k < 512 ? ext_[k] : 0; }
    const uint32_t* Regs() const { return regs_.data(); }
    uint32_t Reg(uint32_t i) const { return i < regs_.size() ? regs_[i] : 0; }
    // The registers this class may have changed since the last call, as ranges
    // [first, last] (the shadowed blocks, fetch and bool/loop constants, and the
    // float constants written since): for cheap deltas.
    void TakeChanged(std::vector<std::pair<uint32_t, uint32_t>>& ranges);
    void MarkAllChanged() { TouchConstants(0, 512); MarkBlocksChanged(); }
    // The shadowed register blocks go out with the next TakeChanged whether they changed or not.
    void MarkBlocksChanged() { blocksChanged_ = ~0u; }
    // D3D's dirty masks (device +16/+24/+32) say which shadow registers a draw flushes: a call
    // the front end sees notes them (`device` before the call), the draw adds its own `pre`'s,
    // and the draw reads only the blocks they cover (NATIVE_RENDERER.md 7.10). A call that
    // flushes state itself (clears, resolves, command buffers, swap) clears the masks inside
    // D3D: FullNext() has the next draw compare every block, as without masks.
    void NoteDirty(const uint8_t* device);
    void FullNext() { full_ = true; }
    // Statistics: draws, draws that compared every block, blocks read; with HC09_NATIVE_DIRTY_CHECK
    // the draws where the masked registers differed from a full compare.
    uint64_t statDraws = 0, statFull = 0, statBlocks = 0, statMismatch = 0;

private:
    // Every shadowed register except the float constants. Blocks whose device bytes are
    // the same as at the last call are not decoded again (most draws change a few).
    // `visit`: the blocks to read (Shadow's numbering), the others keep their registers.
    void Shadow(const uint8_t* device, uint32_t visit = ~0u);
    struct Cache { std::vector<uint8_t> raw; uint8_t window[8] = {}; bool valid = false; };
    // The registers of `visit` blocks into `regs`; the blocks that changed.
    static uint32_t ShadowInto(const uint8_t* device, uint32_t* regs, Cache& cache, uint32_t visit);
    static uint32_t VisitOf(uint64_t m16, uint64_t m24, uint64_t m32);
    void Check(const uint8_t* device, uint32_t visit, uint64_t m16, uint64_t m24, uint64_t m32);
    void Literals(uint32_t header, uint32_t codeBase, const GuestMemory& mem);
    void TouchConstants(uint32_t first, uint32_t count);   // float constants first..first+count-1 (0-511)
    std::vector<uint32_t> regs_;
    uint64_t constTouched_[8] = {};   // float constants written since TakeChanged
    uint32_t ext_[512] = {};          // PointerConstants: address per float constant, 0 = value known
    uint32_t blocksChanged_ = ~0u;    // shadow blocks (Shadow's numbering) changed since TakeChanged
    Cache cache_;                     // the device's shadow bytes as last decoded (by device offset)
    uint64_t pend_[3] = {};           // dirty masks +16/+24/+32 noted since the last draw
    bool full_ = true;                // the next draw compares every block
    // HC09_NATIVE_DIRTY=0: every draw compares every block (as before 7.10).
    // HC09_NATIVE_DIRTY_CHECK=1: every draw also does the full compare into chk_ and reports
    // registers the masked read missed.
    bool masked_ = true, check_ = false;
    std::vector<uint32_t> chk_;
    Cache chkCache_;
};

// The shaders a draw uses (sub_8232C9D0): microcode address and size of the
// vertex shader program `variant` (1 = the variant for draws without a pixel
// shader, when the shader has one) and of the pixel shader. Addresses are guest
// virtual; the microcode of the vertex shader in memory is the version D3D last
// patched in place (capture 'V' records hold patches done this frame).
struct ShaderRefs
{
    uint32_t vs = 0, ps = 0, variant = 0;
    uint32_t vsCode = 0, vsBytes = 0, psCode = 0, psBytes = 0;
};
// `load32(va)`: a big-endian guest word.
template <class Load32>
ShaderRefs Shaders(const uint8_t* device, Load32 load32)
{
    ShaderRefs s;
    s.vs = BE32(device + d3dcap::dev::kVertexShader);
    s.ps = BE32(device + d3dcap::dev::kPixelShader);
    if (s.vs)
    {
        s.variant = (!s.ps && (load32(s.vs + 872) & 0x20)) ? 1 : 0;
        uint32_t rec = s.vs + 872 + load32(s.vs + 896 + 8 * s.variant);
        s.vsCode = load32(s.vs + 32) + load32(rec);
        s.vsBytes = load32(rec + 4);
    }
    if (s.ps)
    {
        uint32_t info = s.ps + 40 + load32(s.ps + 64);
        s.psCode = load32(s.ps + 24) + load32(info);
        s.psBytes = load32(info + 4);
    }
    return s;
}

// A D3D surface (render target or depth/stencil) as the native renderer sees it.
// Object layout (D3DSurface): +24 RB_SURFACE_INFO (EDRAM pitch, MSAA), +28 the
// RB_COLOR_INFO / RB_DEPTH_INFO word (EDRAM base, format), +36 size
// (width - 1 in bits 18-31, height - 1 in bits 3-17), +40 the D3DFORMAT.
struct Surface
{
    uint32_t key = 0;            // guest address of the object (identity of its host image)
    uint32_t width = 0, height = 0;
    uint32_t format = 0;         // colour: RB_COLOR_INFO format (bits 16-19); depth: 1 = D24FS8
    uint32_t msaa = 0;           // 0/1/2 = 1x/2x/4x
    uint32_t edramBase = 0;
    bool depth = false;
    bool valid() const { return key != 0; }
    bool operator==(const Surface& o) const
    {
        return key == o.key && width == o.width && height == o.height && format == o.format && msaa == o.msaa && depth == o.depth;
    }
    bool operator!=(const Surface& o) const { return !(*this == o); }
};
Surface ReadSurface(uint32_t object, bool depth, const GuestMemory& mem);

// A resolve (D3DDevice_Resolve, sub_82322A98) as a native copy: the source
// rectangle of a bound target, written into the destination texture's guest
// memory exactly as the GPU's copy does (tiling, format, endian; the fields mean
// what xenos::ResolveInfo's do), then optional clears of the source.
struct ResolveOp
{
    uint32_t src = 0;                   // 0-3 colour target, 4 depth/stencil
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // source rectangle, pixels (x1/y1 exclusive, multiples of 8)
    uint32_t sampleSel = 0;             // 0-3 one sample, 4 = 0+1, 5 = 2+3, 6 = all
    bool copy = false;
    uint32_t destBase = 0, destPitch = 0, destHeight = 0, destFmt = 0, destBytes = 0, endian = 0;
    bool swapRB = false;
    float destScale = 1.0f;
    bool clearColor = false, clearDepth = false;
    float color[4] = {};                // clear values
    float z = 0;
    uint32_t stencil = 0;
    // The destination texture (its fetch constant): base address, size; source pixel (x, y) lands on its
    // texel (x + destX, y + destY) (destBase above already includes that offset, in whole tiles).
    uint32_t texBase = 0, texWidth = 0, texHeight = 0;
    int32_t destX = 0, destY = 0;
};
// From the call's arguments: flags, the source rectangle (D3DRECT, nullptr =
// the destination's size), the destination texture's fetch constant (6 host-
// order words from the object at +28), destination point (nullptr = 0,0) and
// the clear values. `sourceMsaa`: MSAA mode of the source surface (picks the
// samples when the flags do not). False when there is nothing to do.
bool DecodeResolve(uint32_t flags, const int32_t* rect, const uint32_t destFetch[6], const int32_t* point, const float* color,
                   float z, uint32_t stencil, uint32_t sourceMsaa, ResolveOp& out);

}  // namespace d3dn
