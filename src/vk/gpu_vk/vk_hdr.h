#pragma once
// hc09: HDR output (hc09.ini [Graphics] HDR=on, as HC09_HDR). The window's picture is made in SDR as before (the
// upscaler, the overlay), then this pass writes it to an HDR10 swapchain (BT.2020 primaries, PQ): SDR white at
// HDRPaperWhite nits (200 the default, HC09_HDR_PAPER_WHITE), and the 3D scene's highlights above it, up to
// HDRPeak nits (1000 the default, HC09_HDR_PEAK).
//
// The highlights come from the scene's post-processing (vk_scene_effects.h): per pixel, how much brighter than
// SDR it should be, and the brightness it was given. A pixel of the game's final frame takes the gain only where
// it still shows that brightness, so the menus and on-field graphics drawn over the scene stay at paper white.
// Frames without a 3D scene get no gain.
//
// HC09_HDR_DEBUG=1 (dev): the same pass into an SDR window, the pixels that would be brighter tinted red, so
// screenshots show it without an HDR display.
#include <cstdint>

#include "vk_device.h"

namespace hc09vk {

// The scene's HDR gain for the frame being presented: `image` (R16G16: gain, the brightness shown; GENERAL
// layout) covers the rectangle (x, y, width, height) of the front buffer's pixels. No image: no gain.
struct HdrGain
{
    const Image* image = nullptr;
    float x = 0, y = 0, width = 0, height = 0;
};

class HdrOutput
{
public:
    float paperWhite = 200.0f, peak = 1000.0f;   // nits
    int debug = 0;   // 1: highlights tinted red in SDR; 2: the same without the front buffer test

    // Records the pass into `dstView` (format `dstFormat`, `extent`, COLOR_ATTACHMENT_OPTIMAL, left so): `sdr` (the
    // window's picture, SHADER_READ_ONLY_OPTIMAL), with the gain of `gain` where `src` (the front buffer,
    // TRANSFER_SRC_OPTIMAL, left so), shown at the rectangle (x, y, w, h) of the window, still matches it. `pq`: an
    // HDR10 target, else the debug view. False: nothing was recorded.
    bool Record(Device& dev, VkCommandBuffer cb, const Image& sdr, const Image& src, int32_t x, int32_t y, uint32_t w, uint32_t h,
                const HdrGain& gain, VkImageView dstView, VkFormat dstFormat, VkExtent2D extent, bool pq);
    void Destroy();

private:
    bool Setup();
    VkPipeline Pipeline(VkFormat format, bool pq);

    Device* dev_ = nullptr;
    bool failed_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkFormat madeFormat_ = VK_FORMAT_UNDEFINED;
    VkPipeline pipe_ = VK_NULL_HANDLE;
};

}  // namespace hc09vk
