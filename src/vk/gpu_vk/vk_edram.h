#pragma once
// Vulkan renderer with EDRAM in a storage buffer (docs/BACKEND_PLAN.md §2.3,
// milestone 4). It replaced the first renderer (host render targets synced
// with the software EDRAM model at every target switch, git 5cb6b00..a6f9572).
//
// EDRAM is one device-local buffer with the console's layout (2048 tiles of
// 80x16 dwords), addressed exactly as runtime/xenos/xenos_raster.cpp's
// Surface::Index, so every surface aliases every other as on the hardware.
// Draws rasterize without attachments; the translated pixel shader ends with
// an epilogue that, inside fragment shader interlock (pixel, ordered), does
// the software rasterizer's per-pixel work on that buffer: depth/stencil test
// and update, alpha test, and per colour target decode -> blend -> clamp ->
// encode -> store to every sample (the software MSAA model). Resolves and
// clears are compute shaders over the same buffer writing into the guest
// memory buffer. A frame is recorded once; the only waits are resolves that
// copy to memory (the CPU decodes textures from it, and the capture writes
// each resolve as PNG).
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "vk_device.h"
#include "xenos_raster.h"

namespace hc09vk {

struct RendererStats
{
    uint32_t draws = 0, skipped = 0, syncs = 0, pipelines = 0, textures = 0, shaders = 0, resolves = 0, barriers = 0;
    uint32_t texturesRedecoded = 0;   // decoded again because the CPU rewrote their memory
    uint32_t texturesOnGpu = 0;       // decoded by a compute shader (written by a pending resolve)
    uint32_t resolveAcquires = 0, resolveWritebacks = 0;   // vk_native: ranges resolved data moved to / back from the GPU
    double compileSeconds = 0, pipelineSeconds = 0, textureSeconds = 0, waitSeconds = 0;
    double gpuSeconds = 0;            // GPU execution time of the frame command buffers (timestamps)
    double rendererSeconds = 0;       // time spent in OnDraw/OnResolve/EndFrame (CPU work and GPU waits)   // GLSL -> SPIR-V, pipelines, texture decode + mips, fence waits
    std::map<std::string, uint32_t> skipReasons;
    // HC09_GPU_CPUPROF=1: CPU seconds per section of a draw (kDrawSectionNames).
    static constexpr int kDrawSections = 8;
    double drawSections[kDrawSections] = {};
};
inline constexpr const char* kDrawSectionNames[RendererStats::kDrawSections] = {
    "state+shaders", "indices", "textures", "constants", "descriptors", "pipeline", "record", "ring flush" };

class EdramRenderer : public xenos::DrawObserver
{
public:
    // `guestMemory`: host-visible buffer holding the 512 MiB of guest physical
    // memory (vertex fetch reads it, resolves write it).
    // `exact`: shaders reproduce the software rasterizer bit for bit (correctly
    // rounded math, the software's texture sampling) — replays and validation;
    // false: the GPU's fp32 math and hardware samplers, about half the GPU time
    // (live play; +-1 differences, edges of mip/alpha-test decisions).
    bool Init(Device& dev, const Buffer& guestMemory, std::string* err, bool exact = true);
    void Shutdown();
    // Finish all GPU work.
    void Flush();
    // Frame boundary of a live game: submit and wait; evict textures unused for
    // 600 frames. Decoded textures persist across frames: each records the
    // write epoch of its memory (xenos::HostTrackWrites) and is decoded again
    // when the CPU has written it since; resolves invalidate what they overwrite.
    void EndFrame();
    // Live presentation: record, into the frame being built, the decode of the
    // XE_SWAP front buffer (a tiled k_8_8_8_8 surface in guest memory, R/B
    // swapped by the resolve; scanout ignores alpha) into an R8G8B8A8_UNORM
    // image owned by the renderer — the same pixels as xenos::DecodeFrontBuffer.
    // The image is valid after the next EndFrame, in TRANSFER_SRC_OPTIMAL
    // layout (ready for Swapchain::Present), until the next call.
    const Image* DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height);
    // Copy resolves: wait for the GPU after each (default: the CPU may read
    // what it wrote, e.g. the capture's resolve PNGs), or not (live: textures
    // written by a pending resolve are decoded on the GPU or wait then).
    void SetResolveWaits(bool wait);
    const RendererStats& Stats() const { return stats_; }

    void OnDraw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash,
                const xenos::ShaderCode* ps) override;
    void OnResolved() override;
    bool PerformsResolves() const override { return true; }
    void OnResolve(const xenos::ResolveInfo& r) override;
    void OnFrameEnd(uint32_t frontBuffer, uint32_t width, uint32_t height) override;   // EndFrame

    EdramRenderer();
    ~EdramRenderer() override;
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    RendererStats stats_;
};

}  // namespace hc09vk
