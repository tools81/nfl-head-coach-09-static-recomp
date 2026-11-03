#pragma once
// hc09: the picture's last step to the window, in place of a bilinear blit: AMD FidelityFX FSR 1 (EASU) when the
// window is larger than the image, an area filter when it is smaller (Scale 2 on a 1080p screen), then RCAS
// sharpening. The same image size as the window is copied unchanged.
//
// Settings: hc09.ini [Graphics] Upscaling=fsr (the default) or bilinear, Sharpening=0 (off) .. 100 (75 the
// default), as the switches HC09_UPSCALE and HC09_SHARPEN.
#include <cstdint>

#include "vk_device.h"

namespace hc09vk {

class Upscaler
{
public:
    bool enabled = true;      // false: the caller blits
    float sharpening = 0.75f; // 0 off .. 1 the most (RCAS's 0 stops)

    // Records the scaling of `src` (TRANSFER_SRC_OPTIMAL, left so; must have SAMPLED usage) into the rectangle
    // (x, y, w, h) of `dst` (`dstView` of format `dstFormat`, COLOR_ATTACHMENT_OPTIMAL, left so). False: nothing
    // was recorded (the caller blits instead). Call once per frame, after the previous frame's work is done.
    bool Record(Device& dev, VkCommandBuffer cb, const Image& src, VkImageView dstView, VkFormat dstFormat, int32_t x, int32_t y,
                uint32_t w, uint32_t h);
    void Destroy();

private:
    enum Pass { kEasu, kArea, kRcas, kPasses };
    VkPipeline Pipeline(Pass pass, VkFormat format);
    bool Setup();

    Device* dev_ = nullptr;
    bool failed_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_[kPasses] = {};
    struct Made { Pass pass; VkFormat format; VkPipeline pipe; };
    Made made_[8] = {};
    uint32_t madeCount_ = 0;
    Image middle_;   // the scaled image before sharpening (RGBA8, w x h)
};

}  // namespace hc09vk
