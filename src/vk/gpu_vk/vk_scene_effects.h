#pragma once
// hc09: screen-space ambient occlusion and reflections for the native renderer's 3D scenes, as the D3D12
// renderer's (src/gpu/scene_effects.h, the same shaders in GLSL).
//
// The native renderer draws a frame's 3D scene into a multisampled colour target and depth buffer, then
// resolves it. Apply works on those targets in place before that first resolve: ambient occlusion darkens the
// colour from the depth buffer, and reflections blend in a copy of the scene on surfaces that face up (the
// field). The camera comes from the scene's draws (gpu/scene_camera.h).
//
// Settings: hc09.ini [Graphics] AmbientOcclusion and Reflections (strength, 0 turns it off), as the switches
// HC09_SSAO and HC09_SSR.
#include <cstdint>
#include <functional>
#include <vector>

#include "gpu/scene_camera.h"
#include "vk_device.h"

namespace hc09vk {

// Where the scene is in its targets: the camera, and the viewport's rectangle in the target's (host) pixels.
struct SceneView
{
    hc09::gpu::SceneCamera camera;
    float x = 0, y = 0, width = 0, height = 0;
};

class SceneEffects
{
public:
    bool Init(Device& dev);
    void Shutdown();
    bool Enabled() const { return occlusion_ > 0 || reflections_ > 0; }

    // Records the effects into `cb` (outside a render pass). `color`: the scene's colour target, `depthView` its
    // depth buffer's depth aspect; both GENERAL, `samples` per pixel, and left GENERAL. `allocate` gives a
    // descriptor set of a layout for this frame's work. Images replaced at a new size go to `retired` (the GPU may
    // still be using them).
    void Apply(VkCommandBuffer cb, const Image& color, VkSampleCountFlagBits samples, VkImageView depthView, const SceneView& view,
               const std::function<VkDescriptorSet(VkDescriptorSetLayout)>& allocate, std::vector<Image>& retired);

private:
    struct Pipelines
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool failed = false;
        VkPipeline occlusion = VK_NULL_HANDLE, blur = VK_NULL_HANDLE, composite = VK_NULL_HANDLE, reflections = VK_NULL_HANDLE,
                   reflectionComposite = VK_NULL_HANDLE;
    };
    Pipelines* GetPipelines(VkFormat format, VkSampleCountFlagBits samples);
    VkPipeline CreatePipeline(VkShaderModule fs, VkFormat target, VkSampleCountFlagBits samples, int blend);
    bool Prepare(Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, std::vector<Image>& retired);

    Device* dev_ = nullptr;
    float occlusion_ = 0, reflections_ = 0;
    int debug_ = 0;
    bool loggedCamera_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    std::vector<Pipelines> pipelines_;
    Image occlusionRaw_, occlusionBlurred_, sceneCopy_, reflectionTarget_;
};

}  // namespace hc09vk
