#pragma once
// hc09: temporal upscaling (roadmap 15) with AMD FidelityFX FSR 3.1 (FidelityFX SDK v1.1.4, MIT; thirdparty/fidelityfx,
// fetched by CMake): its signed Vulkan DLL, loaded at run time from the game's folder, through its ffx-api.
//
// The 3D scene is drawn smaller than the picture, with a different sub-pixel camera offset (jitter) each frame; FSR
// gathers those frames, following the scene's motion, into the picture at its full size.
//
// Settings: hc09.ini [Graphics] TemporalUpscaling=off (the default), quality (the scene at 1 / 1.5 of the picture's
// size per side), balanced (1 / 1.7) or performance (1 / 2), as the switch HC09_TEMPORAL_UPSCALING.
#include <cstdint>

#include "vk_device.h"

namespace hc09vk {

class TemporalUpscaler
{
public:
    // The setting: 0 off, else FSR's quality mode (1 quality, 2 balanced, 3 performance).
    static int Mode();
    // The picture's size per side over the scene's for a mode (1 when off).
    static float Ratio(int mode);
    // The jitter for a frame: an offset in the scene's pixels (-0.5 .. 0.5), as FSR's sequence has it for this ratio.
    static void Jitter(uint64_t frame, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y);

    // Loads FSR (once) and makes its context for scenes up to `maxRender` pixels and pictures of `output` pixels.
    // False when FSR is not there (no DLL, or no context): the caller draws the scene at full size instead.
    bool Create(Device& dev, uint32_t maxRenderWidth, uint32_t maxRenderHeight, uint32_t outputWidth, uint32_t outputHeight);
    bool Ready() const { return context_ != nullptr; }
    void Destroy();

    struct Frame
    {
        const Image* color = nullptr;    // the scene (render size; sampled)
        const Image* depth = nullptr;    // its depth (render size; sampled, a depth format)
        const Image* motion = nullptr;   // where each pixel was a frame before, in render pixels (R16G16_SFLOAT)
        const Image* output = nullptr;   // the picture (output size; storage)
        uint32_t renderWidth = 0, renderHeight = 0;
        float jitterX = 0, jitterY = 0;  // as Jitter gave it
        float frameMs = 16.7f;
        bool reset = false;              // a cut: the history is not used
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f;
        bool depthInverted = false;
    };
    // Records the upscaling into `cb`. The images are in GENERAL layout before and after.
    bool Dispatch(VkCommandBuffer cb, const Frame& f);

private:
    void* context_ = nullptr;
    uint32_t outW_ = 0, outH_ = 0;
};

}  // namespace hc09vk
