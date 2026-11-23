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
// HC09_SSAO and HC09_SSR; EffectsResolution (auto, half or full), as HC09_EFFECTS_RES: both are worked out at half
// the scene's resolution above about 1080p (a quarter of the cost; the occlusion enlarged along the depth's edges).
//
// With post-processing and neither depth of field nor HDR, the passes are merged: one copy of the scene, the
// occlusion, reflections and bloom worked out from it, and one pass that puts them on it and grades it into the
// target (instead of two composites into the multisampled target and two copies).
//
// Post-processing, last, on scenes in 8-bit targets (the menus and HUD are drawn later, into other targets, so it
// never touches them): bloom (a soft-threshold bright pass blurred over five halvings, added in linear light),
// colour grading (a filmic shoulder that rolls the bloom off into white, a mild S-curve, saturation, a faint
// vignette), and depth of field (what lies past the middle of the picture blurs; focus eased over frames). hc09.ini Bloom,
// ColorGrading and DepthOfField (strength, 0 off; DepthOfField off by default), as HC09_BLOOM, HC09_GRADE, HC09_DOF.
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
    bool Enabled() const { return occlusion_ > 0 || reflections_ > 0 || Post(); }

    // Records the effects into `cb` (outside a render pass). `color`: the scene's colour target, `depthView` its
    // depth buffer's depth aspect; both GENERAL, `samples` per pixel, and left GENERAL. `allocate` gives a
    // descriptor set of a layout for this frame's work. Images replaced at a new size go to `retired` (the GPU may
    // still be using them).
    // `scene` (hc09, ray-traced lighting, vk_raytrace.h): the scene's acceleration structure, in the view space the
    // depth buffer gives: the occlusion is traced against it (with contact shadows) instead of read from the depth.
    void Apply(VkCommandBuffer cb, const Image& color, VkSampleCountFlagBits samples, VkImageView depthView, const SceneView& view,
               const std::function<VkDescriptorSet(VkDescriptorSetLayout)>& allocate, std::vector<Image>& retired,
               VkAccelerationStructureKHR scene = VK_NULL_HANDLE, const float* sun = nullptr,
               uint32_t glossyFrom = ~0u);
    // `glossyFrom` (with `scene`): the scene's triangles from this one on are mirror-like (the helmets' shells): the
    // traced reflections show the scene in them (~0: none).
    // `sun` (with `scene`; 8 values): the world's x axis in view space, how dark the lights' shadows are (0..1), the
    // direction toward the sun in the world's axes and 1 by day (0: the stadium's floodlights instead): traced
    // shadows from them, in the occlusion.

    // HDR output (HC09_HDR): the gain image the last Apply made (R16G16: gain, the brightness shown; GENERAL) and
    // the rectangle of the scene's target it covers (x, y, width, height), or nullptr.
    const Image* HdrGain(float rect[4]) const;
    // hc09: whether the last Apply traced the lights' shadows (so the game's own are not needed).
    bool TracedShadows() const { return tracedShadows_; }
    // hc09: rain or snow over the next Apply's scene (roadmap 16; 8 values: the world's x axis in view space, the
    // time in seconds, the kind (1 rain, 2 snow), how heavy (0..1), the wind along the world's x and z in yards per
    // second), or nullptr: none.
    void SetPrecipitation(const float* values)
    {
        precipitationOn_ = values != nullptr;
        if (values)
            for (int i = 0; i < 8; ++i) precipitation_[i] = values[i];
    }
    // hc09: footprints in the snow (roadmap 16): the next Apply stamps where something stands on the field into the
    // footprint map. `world` (12 values): the world's x, y and z axes and its origin in view space (this frame's
    // field camera), or nullptr: no stamping this frame. `game`: a new value starts a clean map.
    void SetFootprints(const float* world, uint32_t game)
    {
        footprintStamp_ = world != nullptr;
        if (world)
            for (int i = 0; i < 12; ++i) footprintWorld_[i] = world[i];
        if (game != footprintGame_) { footprintGame_ = game; footprintClear_ = true; }
    }
    // hc09: breath in the cold (vk_breath.h) over the next Apply's scene: `count` puffs (8 floats each, as
    // Breath::Update writes them), lit by `light` (1 by day). Every frame (0 or nullptr: none).
    void SetBreath(const float* puffs, uint32_t count, float light);
    // hc09: the whole scene's tint in the post-processing (linear light; vk_sunset.h), every frame.
    void SetSceneTint(const float* rgb)
    {
        for (int i = 0; i < 3; ++i) sceneTint_[i] = rgb ? rgb[i] : 1.0f;
    }
    // The footprint map for the field's shader (R16F, 60 x 120 yards from the field's middle, 20 texels a yard;
    // SHADER_READ_ONLY_OPTIMAL between frames) and its sampler, once it exists.
    bool Footprints(VkImageView& view, VkSampler& sampler) const
    {
        if (!footprints_.image || footprintClear_) return false;
        view = footprints_.view;
        sampler = linear_;
        return true;
    }

private:
    static constexpr int kBloomLevels = 5;
    struct Pipelines
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool failed = false;
        VkPipeline occlusion = VK_NULL_HANDLE, blur = VK_NULL_HANDLE, composite = VK_NULL_HANDLE, reflections = VK_NULL_HANDLE,
                   reflectionComposite = VK_NULL_HANDLE, bloomPrefilter = VK_NULL_HANDLE, bloomDown = VK_NULL_HANDLE,
                   bloomUp = VK_NULL_HANDLE, focus = VK_NULL_HANDLE, depthOfField = VK_NULL_HANDLE, post = VK_NULL_HANDLE,
                   hdrGain = VK_NULL_HANDLE, postMerged = VK_NULL_HANDLE, precipitation = VK_NULL_HANDLE,
                   footprints = VK_NULL_HANDLE, footprintFade = VK_NULL_HANDLE, breath = VK_NULL_HANDLE;
        VkPipeline tracedReflections = VK_NULL_HANDLE;
        VkPipeline tracedOcclusion = VK_NULL_HANDLE;   // hc09: ray-traced (with the device's ray queries)
    };
    bool Post() const { return bloom_ > 0 || grade_ > 0 || dof_ > 0 || hdr_; }
    Pipelines* GetPipelines(VkFormat format, VkSampleCountFlagBits samples);
    VkPipeline CreatePipeline(VkShaderModule fs, VkFormat target, VkSampleCountFlagBits samples, int blend,
                              VkPipelineLayout layout = VK_NULL_HANDLE);
    bool Prepare(Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, std::vector<Image>& retired);

    Device* dev_ = nullptr;
    float occlusion_ = 0, reflections_ = 0, bloom_ = 0, grade_ = 0, dof_ = 0;
    int debug_ = 0;
    // Scene pixels per pixel of the occlusion and reflection passes (HC09_EFFECTS_RES): 0 auto (2 for scenes over
    // 1100 pixels tall, i.e. Scale 2 and up), 1 full, 2 half.
    uint32_t divisor_ = 0;
    uint32_t focusIndex_ = 0;
    bool hdr_ = false;            // HDR output: the gain image (vk_hdr.h)
    float peakRatio_ = 5.0f;      // its peak / paper white
    bool gainMade_ = false;       // the last Apply made the gain image
    bool tracedShadows_ = false;  // and traced the lights' shadows
    float gainRect_[4] = {};
    bool loggedCamera_ = false;
    bool precipitationOn_ = false;
    float precipitation_[8] = {};
    float sceneTint_[3] = { 1.0f, 1.0f, 1.0f };
    Buffer puffBuffer_;            // the breath's puffs: a part a frame (host-visible)
    uint32_t breathPart_ = 0, breathCount_ = 0;
    float breathLight_ = 1.0f;
    Image footprints_;
    bool footprintStamp_ = false, footprintClear_ = true;
    float footprintWorld_[12] = {};
    uint32_t footprintGame_ = ~0u;
    uint32_t footprintFrames_ = 0;   // stamping frames since the last fade
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    // hc09: the ray-traced occlusion's: the same bindings and the acceleration structure (binding 5).
    VkDescriptorSetLayout tracedSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tracedPipeLayout_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    std::vector<Pipelines> pipelines_;
    Image occlusionRaw_, occlusionBlurred_, sceneCopy_, reflectionTarget_;
    Image bloomLevels_[kBloomLevels], focus_[2], depthOfFieldTarget_, hdrGain_;
};

}  // namespace hc09vk
