#pragma once
// Native renderer (milestone 9, docs/NATIVE_RENDERER.md): the game's D3D calls
// rendered with host render targets — one Vulkan image per D3D surface, at the
// surface's full size (no EDRAM, no predicated tiling: a tiled frame is drawn
// once), fixed-function blending, depth and stencil, draws translated by xe_glsl
// (TranslateOptions::hostRt). Resolves are copies from those images into the
// destination texture's guest memory in the GPU's tiled layout, so texture
// fetch, the CPU and the front buffer see exactly what a resolve on the
// console leaves there (step 1; keeping resolved textures as host images,
// which higher resolutions need, comes later).
//
// Input: draws with the register file the GPU would see (xenos::Reg on the
// calling thread: xenos::SetThreadRegisterFile), the bound surfaces, clears and
// resolves at the D3D level (runtime/xenos/d3d_state.h).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "d3d_state.h"
#include "vk_device.h"
#include "vk_edram.h"   // RendererStats
#include "xenos.h"
#include "xenos_shader.h"
#include "xenos_texture.h"

namespace hc09vk {

// HC09_NATIVE_PROF=1: where the renderer thread's time goes (seconds, summed
// since Init; the reporter takes differences). Sections exclude the GPU waits
// inside them, which are counted under their reason instead.
struct NativeProfile
{
    enum Section
    {
        kDrawState,      // Draw: state checks, shader lookup/translation
        kDrawTextures,   // texture cache lookups, decodes, uploads
        kDrawConsts,     // DrawConsts from the register file
        kDrawIndices,    // index assembly
        kDrawDesc,       // samplers, descriptor sets
        kDrawPipeline,   // pipeline lookup / creation
        kDrawRecord,     // vkCmd* of the draw
        kResolve,        // resolves (CPU side)
        kClear,          // clears and target binding
        kFront,          // front buffer decode
        kSubmit,         // EndFrame: ending and submitting the command buffers
        kWaitFrame,      // EndFrame: waiting for the GPU
        kWaitRing,       // mid-frame waits: constant/index rings or descriptor sets full
        kWaitTexture,    // mid-frame waits: a texture's memory is pending a resolve write
        kWaitCompute,    // mid-frame waits: compute descriptor sets used up
        kWaitResolve,    // mid-frame waits: after copy resolves (offline replays)
        kWaitFence,      // mid-frame submits a D3D fence wait asked for (live: native fences)
        kSections
    };
    double sec[kSections] = {};
    uint64_t waits[kSections] = {};   // number of waits by reason
    double gpuSeconds = 0;            // GPU time of the frame command buffers (timestamps)
    uint64_t submits = 0;
    // GPU time by kind of work (timestamps where the kind changes).
    enum GpuKind { kGpuDraws, kGpuResolves, kGpuDecodes, kGpuFront, kGpuOther, kGpuKinds };
    double gpuKind[kGpuKinds] = {};
};
inline constexpr const char* kNativeGpuKindNames[NativeProfile::kGpuKinds] = { "draws", "resolves", "texture decodes", "front buffer", "other" };
inline constexpr const char* kNativeSectionNames[NativeProfile::kSections] = {
    "state", "textures", "consts", "indices", "descriptors", "pipeline", "record", "resolve", "clear",
    "front", "submit", "wait-frame", "wait-ring", "wait-texture", "wait-compute", "wait-resolve", "wait-fence" };

class NativeRenderer
{
public:
    // `guestMemory`: the 512 MiB of guest physical memory (vertex fetch reads it,
    // resolves write it). `exact`: shaders with the software's math and sampling.
    bool Init(Device& dev, const Buffer& guestMemory, std::string* err, bool exact = true);
    void Shutdown();
    // The bound colour targets 0-3 and depth/stencil surface (D3D's dev+12440..12456).
    void SetTargets(const d3dn::Surface rt[4], const d3dn::Surface& depth);
    // ClearF: `flags` bits 0-3 colour target n, 0x10 depth, 0x20 stencil; `rect`
    // (x0, y0, x1, y1, already clipped to the viewport) or nullptr = the whole targets.
    void Clear(uint32_t flags, const int32_t* rect, const float color[4], float z, uint32_t stencil);
    // A draw with the current register file.
    void Draw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash, const xenos::ShaderCode* ps);
    // Copy (and clears) from the bound targets.
    void Resolve(const d3dn::ResolveOp& r);
    // Frame boundary: submit the frame without waiting for it; the GPU draws it while
    // the next one is recorded (two frames in flight; textures persist across frames
    // as in vk_edram).
    void EndFrame();
    // Wait until the GPU finished everything submitted.
    void Finish();
    // Before the CPU reads [pa, pa + bytes) of guest memory: resolved data the GPU
    // keeps (HC09_NATIVE_RESOLVE_GPU) is written back, and everything waited for.
    void SyncToGuest(uint32_t pa, uint32_t bytes);
    // Called (renderer thread) for each range the GPU starts to keep: the live game
    // makes the CPU's accesses to it write it back first (runtime.cpp GuestWatchAccess).
    void SetOwnedCallback(void (*fn)(uint32_t pa, uint32_t bytes));
    // The GPU timeline: the value of the last submit (EndFrame's, after it), and a wait
    // for a value — any thread may wait.
    uint64_t LastSubmit() const;
    void WaitSubmit(uint64_t value) const;
    // Work recorded since the last submit (it signals LastSubmit() + 1 when submitted).
    bool Unsubmitted() const;
    // Submit the work recorded so far in the middle of a frame, without waiting (D3D's
    // fences follow the GPU: the game waits for work the frame's end would submit).
    void SubmitNow();
    // The XE_SWAP-style front buffer (a tiled 8888 texture in guest memory) decoded
    // into an image owned by the renderer (as EdramRenderer::DecodeFrontBuffer).
    // At resolution scale s > 1 the image is s times the size when the front buffer's last
    // resolve left a scaled copy (NATIVE_RENDERER.md 7.11), else the guest size.
    const Image* DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height);
    // Resolution scale (HC09_SCALE, hc09.ini [display] scale).
    uint32_t Scale() const;
    // The image DecodeFrontBuffer made last, read back (RGBA8, row-major; waits for the GPU): dumps.
    bool ReadFront(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height);
    // Wait for the GPU after copy resolves (default: yes, offline replays read them).
    void SetResolveWaits(bool wait);
    const RendererStats& Stats() const { return stats_; }
    // HC09_NATIVE_PROF=1 (nullptr when off).
    const NativeProfile* Profile() const;

    NativeRenderer();
    ~NativeRenderer();
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    RendererStats stats_;
};

// Texture override (hc09: the button icons, runtime/glyphs.h; none in the replay
// tools). `set` gives a value for the texture (0 = as the game has it) that joins
// the cache key; `replace` may then change the decoded mip 0 (single-layer 2D).
// Set once before rendering.
struct TextureOverride
{
    uint32_t (*set)(const xenos::TextureFetch& tf) = nullptr;
    bool (*replace)(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img) = nullptr;
    // hc09: whether `replace` has an image for the texture (it is then decoded on the CPU, not by DecodeOnGpu).
    bool (*has)(const xenos::TextureFetch& tf, uint32_t set) = nullptr;
};
void SetTextureOverride(const TextureOverride& o);

}  // namespace hc09vk
