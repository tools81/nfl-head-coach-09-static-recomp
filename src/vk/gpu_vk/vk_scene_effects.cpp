#include "../dev_env.h"
#include "vk_scene_effects.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vk_shader_compile.h"

namespace hc09vk {

namespace {

// Push constants, laid out as the shaders' block (vec4 rows).
struct Constants
{
    float size[2], invSize[2];
    float depthA, depthB, invProj[2];
    float center[2], proj[2];
    float up[3], radius;
    float occlusion, reflections, debug, pad0;
    float blurDirection[2], origin[2];
    float targetInvSize[2], pad1[2];
};
static_assert(sizeof(Constants) <= 128, "push constants");

const char* const kVertex = R"GLSL(#version 450
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// src/gpu/scene_effects.cpp's shaders. Pixel coordinates are the effect's rectangle (the scene's viewport):
// the passes into the scene's own target are offset by its origin, and so are depth reads.
const char* const kFragment = R"GLSL(
layout(push_constant) uniform Effects { vec4 r0, r1, r2, r3, r4, r5, r6; } E;
#define size E.r0.xy
#define invSize E.r0.zw
#define depthA E.r1.x
#define depthB E.r1.y
#define invProj E.r1.zw
#define center E.r2.xy
#define proj E.r2.zw
#define upView E.r3.xyz
#define radius E.r3.w
#define occlusionStrength E.r4.x
#define reflectionStrength E.r4.y
#define debugView E.r4.z
#define blurDirection E.r5.xy
#define origin E.r5.zw
#define targetInvSize E.r6.xy

#if MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTexture;
#endif
layout(set = 0, binding = 1) uniform sampler2D input0;
layout(location = 0) out vec4 outColor;

float RawDepth(ivec2 p) { return texelFetch(depthTexture, p + ivec2(origin), 0).r; }

#if IN_TARGET
vec2 FragPixel() { return gl_FragCoord.xy - origin; }
#else
vec2 FragPixel() { return gl_FragCoord.xy; }
#endif

const float kSky = 0.999999;

// Distance along the view direction (clip w) at a pixel.
float ViewDepth(ivec2 p)
{
    p = clamp(p, ivec2(0), ivec2(size) - 1);
    float denominator = RawDepth(p) - depthA;
    return depthB / (abs(denominator) < 1e-9 ? -1e-9 : denominator);
}

// View-space position (x right, y up, z forward) of a point at pixel coordinates (pixel centres at .5) and view
// distance w.
vec3 ViewPosition(vec2 pixel, float w)
{
    vec2 ndc = vec2(pixel.x * invSize.x * 2.0 - 1.0, 1.0 - pixel.y * invSize.y * 2.0);
    return vec3((ndc - center) * invProj * w, w);
}

vec2 ProjectToPixel(vec3 position)
{
    vec2 ndc = position.xy * proj / position.z + center;
    return vec2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
}

vec3 PositionAt(ivec2 p)
{
    return ViewPosition(vec2(p) + 0.5, ViewDepth(p));
}

// The surface normal from the depth of the neighbours, taking on each axis the side that continues the surface
// (not the one across an edge). Faces the camera.
vec3 NormalAt(ivec2 p, vec3 position)
{
    vec3 left = PositionAt(p + ivec2(-1, 0)), right = PositionAt(p + ivec2(1, 0));
    vec3 above = PositionAt(p + ivec2(0, -1)), below = PositionAt(p + ivec2(0, 1));
    vec3 dx = abs(right.z - position.z) < abs(position.z - left.z) ? right - position : position - left;
    vec3 dy = abs(below.z - position.z) < abs(position.z - above.z) ? below - position : position - above;
    vec3 normal = normalize(cross(dy, dx));
    return dot(normal, position) > 0.0 ? -normal : normal;
}

float Noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

#if PASS_OCCLUSION
// Ambient occlusion: how much of the hemisphere over the surface nearby geometry covers, from 16 depth samples in
// a disc of `radius` (view units) around the pixel. Output: occlusion (1 open, 0 covered) and the view distance,
// for the blur.
void main()
{
    vec2 pixel = FragPixel();
    ivec2 p = ivec2(pixel);
    float raw = RawDepth(p);
    vec3 center3 = PositionAt(p);
    outColor = vec4(1.0, center3.z, 0.0, 0.0);
    if (raw >= kSky)
        return;
    vec3 normal = NormalAt(p, center3);
    float pixels = radius * proj.y * 0.5 * size.y / center3.z;
    if (pixels < 2.0)
        return;
    pixels = min(pixels, size.y * 0.15);
    const int kSamples = 16;
    float angle = Noise(pixel) * 6.2831853;
    float covered = 0.0;
    for (int i = 0; i < kSamples; i++)
    {
        float t = (float(i) + 0.5) / float(kSamples);
        float a = angle + float(i) * 2.3999632;  // golden angle
        vec2 q = pixel + vec2(cos(a), sin(a)) * sqrt(t) * pixels;
        vec3 sample3 = PositionAt(ivec2(q));
        vec3 v = sample3 - center3;
        float distance2 = dot(v, v);
        float cosine = dot(v, normal) * inversesqrt(distance2 + 1e-6);
        float falloff = clamp(1.0 - distance2 / (radius * radius * 4.0), 0.0, 1.0);
        covered += clamp((cosine - 0.15) / 0.85, 0.0, 1.0) * falloff;
    }
    outColor = vec4(clamp(1.0 - covered * 2.0 / float(kSamples), 0.0, 1.0), center3.z, 0.0, 0.0);
}
#endif

#if PASS_BLUR || PASS_COMPOSITE
// One direction of a depth-aware blur of the occlusion (9 taps).
vec2 Blur(ivec2 p)
{
    vec2 middle = texelFetch(input0, p, 0).rg;
    float sum = middle.r, weights = 1.0;
    for (int i = -4; i <= 4; i++)
    {
        if (i == 0)
            continue;
        ivec2 q = clamp(p + ivec2(blurDirection * float(i)), ivec2(0), ivec2(size) - 1);
        vec2 sample2 = texelFetch(input0, q, 0).rg;
        float weight = exp(-float(i * i) / 18.0) * exp(-abs(sample2.g - middle.g) / (middle.g * 0.02 + 1e-3));
        sum += sample2.r * weight;
        weights += weight;
    }
    return vec2(sum / weights, middle.g);
}
#endif

#if PASS_BLUR
void main()
{
    outColor = vec4(Blur(ivec2(FragPixel())), 0.0, 0.0);
}
#endif

#if PASS_COMPOSITE
// The second blur direction, multiplied into the scene (blend: destination * source).
void main()
{
    float blurred = Blur(ivec2(FragPixel())).r;
    float occlusion = debugView == 1.0 ? blurred : mix(1.0, blurred, occlusionStrength);
    outColor = vec4(occlusion, occlusion, occlusion, 1.0);
}
#endif

#if PASS_REFLECTIONS
// Reflections on surfaces facing up: a ray marched through the depth buffer from the surface along the mirrored
// view direction; where it passes behind the scene's depth it hits, fading with the distance the ray travelled,
// so what stands on the surface shows near where it stands. Grass is no mirror: the reflected scene (input0, a
// copy of the whole target) is drawn dark and with strong contrast, so it reads as a shadow cast by the player
// rather than an image of him. Output premultiplied by its opacity, for the reflection composite.
void main()
{
    vec2 fragPixel = FragPixel();
    ivec2 p = ivec2(fragPixel);
    outColor = vec4(0.0);
    if (RawDepth(p) >= kSky)
        return;
    vec3 origin3 = PositionAt(p);
    vec3 normal = NormalAt(p, origin3);
    float facing = clamp((dot(normal, upView) - 0.9) / 0.08, 0.0, 1.0);
    if (facing <= 0.0)
        return;
    vec3 view = normalize(origin3);
    vec3 ray = reflect(view, normal);
    // Stronger at low camera angles, where the reflected shape is longest.
    float grazing = mix(0.6, 1.0, clamp(1.0 - dot(-view, normal), 0.0, 1.0));

    const int kSteps = 48;
    float stepLength = origin3.z * 0.01;
    vec3 previous = origin3, current = origin3 + ray * stepLength * Noise(fragPixel);
    vec3 hit = vec3(0.0);
    bool found = false;
    for (int step = 0; step < kSteps; step++)
    {
        current += ray * stepLength;
        stepLength *= 1.06;
        if (current.z <= 0.0)
            break;
        vec2 q = ProjectToPixel(current);
        if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, size)))
            break;
        float behind = current.z - ViewDepth(ivec2(q));
        if (behind > 0.0 && behind < max(stepLength * 2.0, current.z * 0.01))
        {
            // Refine between the last point in front and this one.
            vec3 front = previous, back = current;
            for (int i = 0; i < 5; i++)
            {
                vec3 middle = (front + back) * 0.5;
                if (middle.z - ViewDepth(ivec2(ProjectToPixel(middle))) > 0.0)
                    back = middle;
                else
                    front = middle;
            }
            hit = back;
            found = true;
            break;
        }
        previous = current;
    }
    if (!found)
        return;
    vec2 pixel = ProjectToPixel(hit);
    if (RawDepth(ivec2(pixel)) >= kSky)
        return;
    vec2 edges = min(pixel, size - pixel) / (size * 0.08);
    float fade = clamp(min(edges.x, edges.y), 0.0, 1.0) * clamp(1.0 - length(hit - origin3) / (radius * 5.0), 0.0, 1.0);
    // The reflected colour's luminance, its contrast deepened and brought down to a dark tone.
    vec3 color = textureLod(input0, (pixel + origin) * targetInvSize, 0.0).rgb;
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    float tone = clamp((luminance - 0.5) * 2.0 + 0.5, 0.0, 1.0) * 0.12;
    float alpha = clamp(0.45 * reflectionStrength * grazing, 0.0, 1.0) * facing * fade;
    outColor = vec4(tone * alpha, tone * alpha, tone * alpha, alpha);
}
#endif

#if PASS_REFLECTION_COMPOSITE
// The reflections, softened (a 5x5 tent over every other pixel), over the scene (blend: source premultiplied).
void main()
{
    ivec2 p = ivec2(FragPixel());
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int y = -2; y <= 2; y++)
    {
        for (int x = -2; x <= 2; x++)
        {
            float weight = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
            sum += texelFetch(input0, clamp(p + ivec2(x, y) * 2, ivec2(0), ivec2(size) - 1), 0) * weight;
            weights += weight;
        }
    }
    sum /= weights;
    outColor = debugView == 2.0 ? vec4(sum.rgb, 1.0) : sum;
}
#endif
)GLSL";

constexpr VkFormat kOcclusionFormat = VK_FORMAT_R16G16_SFLOAT;          // occlusion, view distance
constexpr VkFormat kReflectionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;   // premultiplied colour, opacity

float Strength(const char* value, float fallback)
{
    return value && *value ? std::max(0.0f, float(std::atof(value))) : fallback;
}

void Barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

VkShaderModule Module(VkDevice device, const std::string& source, Stage stage, const char* name)
{
    std::vector<uint32_t> spirv;
    std::string log;
    if (!CompileGlsl(source, stage, name, spirv, &log))
    {
        std::fprintf(stderr, "[effects] shader %s failed: %s\n", name, log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

}  // namespace

bool SceneEffects::Init(Device& dev)
{
    dev_ = &dev;
    occlusion_ = Strength(Hc09Env("HC09_SSAO"), 1.0f);
    reflections_ = Strength(Hc09Env("HC09_SSR"), 1.0f);
    // HC09_EFFECTS_DEBUG=1 shows the occlusion alone, 2 the reflections alone (as the D3D12 renderer).
    if (const char* e = Hc09Env("HC09_EFFECTS_DEBUG")) debug_ = std::atoi(e);
    if (debug_ == 1) reflections_ = 0;
    else if (debug_ == 2) occlusion_ = 0;
    if (!Enabled()) return true;

    VkDevice d = dev.device;
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 2;
    lci.pBindings = b;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants) };
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &setLayout_;
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges = &pcr;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    VkSamplerCreateInfo lin = sci;
    lin.magFilter = lin.minFilter = VK_FILTER_LINEAR;
    vs_ = Module(d, kVertex, Stage::kVertex, "effects_vs");
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_) != VK_SUCCESS ||
        vkCreatePipelineLayout(d, &pci, nullptr, &pipeLayout_) != VK_SUCCESS || vkCreateSampler(d, &sci, nullptr, &nearest_) != VK_SUCCESS ||
        vkCreateSampler(d, &lin, nullptr, &linear_) != VK_SUCCESS || !vs_)
    {
        std::fprintf(stderr, "[effects] setup failed; ambient occlusion and reflections are off\n");
        occlusion_ = reflections_ = 0;
        return false;
    }
    std::fprintf(stderr, "[effects] ambient occlusion %.2f, reflections %.2f\n", occlusion_, reflections_);
    return true;
}

void SceneEffects::Shutdown()
{
    if (!dev_ || !dev_->device) return;
    VkDevice d = dev_->device;
    for (Pipelines& p : pipelines_)
        for (VkPipeline pipe : { p.occlusion, p.blur, p.composite, p.reflections, p.reflectionComposite })
            if (pipe) vkDestroyPipeline(d, pipe, nullptr);
    pipelines_.clear();
    for (Image* img : { &occlusionRaw_, &occlusionBlurred_, &sceneCopy_, &reflectionTarget_ })
        if (img->image) dev_->DestroyImage(*img);
    if (vs_) vkDestroyShaderModule(d, vs_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    if (nearest_) vkDestroySampler(d, nearest_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    vs_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

VkPipeline SceneEffects::CreatePipeline(VkShaderModule fs, VkFormat target, VkSampleCountFlagBits samples, int blend)
{
    if (!fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs_;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = samples;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    if (blend)
    {
        // 1: destination * source colour; 2: premultiplied source over destination. Alpha kept.
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = blend == 1 ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachmentFormats = &target;
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &rinfo;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &ds;
    pi.layout = pipeLayout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(dev_->device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe);
    return pipe;
}

SceneEffects::Pipelines* SceneEffects::GetPipelines(VkFormat format, VkSampleCountFlagBits samples)
{
    for (Pipelines& p : pipelines_)
        if (p.format == format && p.samples == samples) return p.failed ? nullptr : &p;
    pipelines_.push_back({});
    Pipelines& p = pipelines_.back();
    p.format = format;
    p.samples = samples;
    VkDevice d = dev_->device;
    const std::string head = std::string("#version 450\n#define MSAA ") + (samples != VK_SAMPLE_COUNT_1_BIT ? "1\n" : "0\n");
    auto fragment = [&](const char* pass, bool inTarget) {
        std::string source = head + "#define " + pass + " 1\n#define IN_TARGET " + (inTarget ? "1\n" : "0\n") + kFragment;
        return Module(d, source, Stage::kFragment, pass);
    };
    struct Entry { const char* pass; bool inTarget; VkFormat target; VkSampleCountFlagBits samples; int blend; VkPipeline* out; };
    const Entry entries[] = {
        { "PASS_OCCLUSION", false, kOcclusionFormat, VK_SAMPLE_COUNT_1_BIT, 0, &p.occlusion },
        { "PASS_BLUR", false, kOcclusionFormat, VK_SAMPLE_COUNT_1_BIT, 0, &p.blur },
        { "PASS_COMPOSITE", true, format, samples, debug_ == 1 ? 0 : 1, &p.composite },
        { "PASS_REFLECTIONS", false, kReflectionFormat, VK_SAMPLE_COUNT_1_BIT, 0, &p.reflections },
        { "PASS_REFLECTION_COMPOSITE", true, format, samples, debug_ == 2 ? 0 : 2, &p.reflectionComposite },
    };
    for (const Entry& e : entries)
    {
        VkShaderModule fs = fragment(e.pass, e.inTarget);
        *e.out = CreatePipeline(fs, e.target, e.samples, e.blend);
        if (fs) vkDestroyShaderModule(d, fs, nullptr);
        if (!*e.out) p.failed = true;
    }
    if (p.failed)
    {
        std::fprintf(stderr, "[effects] pipelines for format %d x%d failed; no effects on that target\n", int(format), int(samples));
        return nullptr;
    }
    return &p;
}

bool SceneEffects::Prepare(Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                           std::vector<Image>& retired)
{
    if (image.image && image.width == width && image.height == height && image.format == format) return true;
    if (image.image) { retired.push_back(image); image = Image{}; }
    return dev_->CreateImage(VK_IMAGE_VIEW_TYPE_2D, width, height, 1, format, usage | VK_IMAGE_USAGE_SAMPLED_BIT, image);
}

void SceneEffects::Apply(VkCommandBuffer cb, const Image& color, VkSampleCountFlagBits samples, VkImageView depthView, const SceneView& view,
                         const std::function<VkDescriptorSet(VkDescriptorSetLayout)>& allocate, std::vector<Image>& retired)
{
    const hc09::gpu::SceneCamera& camera = view.camera;
    if (!Enabled() || !camera.valid) return;
    if (color.format != VK_FORMAT_R8G8B8A8_UNORM && color.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
        color.format != VK_FORMAT_R16G16B16A16_SFLOAT)
        return;
    // The rectangle in whole pixels, inside the target.
    int32_t x0 = std::max(0, int32_t(view.x + 0.5f)), y0 = std::max(0, int32_t(view.y + 0.5f));
    int32_t x1 = std::min(int32_t(color.width), int32_t(view.x + view.width + 0.5f));
    int32_t y1 = std::min(int32_t(color.height), int32_t(view.y + view.height + 0.5f));
    if (x1 - x0 < 16 || y1 - y0 < 16) return;
    uint32_t width = uint32_t(x1 - x0), height = uint32_t(y1 - y0);
    Pipelines* pipelines = GetPipelines(color.format, samples);
    std::vector<Image> fresh;
    auto prepare = [&](Image& image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
        bool had = image.image != VK_NULL_HANDLE;
        VkImage before = image.image;
        if (!Prepare(image, format, w, h, usage, retired)) return false;
        if (!had || image.image != before) fresh.push_back(image);
        return true;
    };
    // The blurred occlusion target is also the (unread) input of the occlusion pass.
    if (!pipelines || !prepare(occlusionBlurred_, kOcclusionFormat, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        (occlusion_ > 0 && !prepare(occlusionRaw_, kOcclusionFormat, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
        (reflections_ > 0 && (!prepare(sceneCopy_, color.format, color.width, color.height, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
                              !prepare(reflectionTarget_, kReflectionFormat, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))))
        return;

    Constants c{};
    c.size[0] = float(width);
    c.size[1] = float(height);
    c.invSize[0] = 1.0f / float(width);
    c.invSize[1] = 1.0f / float(height);
    c.depthA = camera.depthA;
    c.depthB = camera.depthB;
    c.invProj[0] = 1.0f / camera.sx;
    c.invProj[1] = 1.0f / camera.sy;
    c.center[0] = camera.cx;
    c.center[1] = camera.cy;
    c.proj[0] = camera.sx;
    c.proj[1] = camera.sy;
    std::memcpy(c.up, camera.up, sizeof(c.up));
    c.radius = 0.6f;   // view units (about yards in the game's scenes)
    c.occlusion = std::min(occlusion_, 1.0f);
    c.reflections = reflections_;
    c.debug = float(debug_);
    c.origin[0] = float(x0);
    c.origin[1] = float(y0);
    c.targetInvSize[0] = 1.0f / float(color.width);
    c.targetInvSize[1] = 1.0f / float(color.height);
    if (!loggedCamera_)
    {
        loggedCamera_ = true;
        std::fprintf(stderr, "[effects] first scene %ux%u at %d,%d x%d: depth a %g b %g, scale %g %g, centre %g %g, up %.3f %.3f %.3f\n",
                     width, height, x0, y0, int(samples), camera.depthA, camera.depthB, camera.sx, camera.sy, camera.cx, camera.cy,
                     camera.up[0], camera.up[1], camera.up[2]);
    }

    // Images made for this frame start in GENERAL, as the renderer's targets.
    for (const Image& img : fresh)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    Barrier(cb);

    // One full-screen pass: `target` (the scene's own colour target is drawn at the rectangle), reading the depth
    // buffer and `input`.
    auto pass = [&](VkPipeline pipe, const Image& target, bool inTarget, const Image& input, VkSampler inputSampler) {
        VkDescriptorSet set = allocate(setLayout_);
        if (!set) return;
        VkDescriptorImageInfo images[2] = { { nearest_, depthView, VK_IMAGE_LAYOUT_GENERAL },
                                            { inputSampler, input.view, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet w[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(dev_->device, 2, w, 0, nullptr);
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = target.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = inTarget ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRect2D area = inTarget ? VkRect2D{ { x0, y0 }, { width, height } } : VkRect2D{ { 0, 0 }, { width, height } };
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp{ float(area.offset.x), float(area.offset.y), float(width), float(height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, pipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        Barrier(cb);
    };

    if (occlusion_ > 0)
    {
        pass(pipelines->occlusion, occlusionRaw_, false, occlusionBlurred_, nearest_);
        c.blurDirection[0] = 1;
        pass(pipelines->blur, occlusionBlurred_, false, occlusionRaw_, nearest_);
        c.blurDirection[0] = 0;
        c.blurDirection[1] = 1;
        pass(pipelines->composite, color, true, occlusionBlurred_, nearest_);
    }
    if (reflections_ > 0)
    {
        // A single-sampled copy of the scene (with its occlusion) to reflect.
        if (samples != VK_SAMPLE_COUNT_1_BIT)
        {
            VkImageResolve r{};
            r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.extent = { color.width, color.height, 1 };
            vkCmdResolveImage(cb, color.image, VK_IMAGE_LAYOUT_GENERAL, sceneCopy_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
        else
        {
            VkImageCopy r{};
            r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.extent = { color.width, color.height, 1 };
            vkCmdCopyImage(cb, color.image, VK_IMAGE_LAYOUT_GENERAL, sceneCopy_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
        Barrier(cb);
        pass(pipelines->reflections, reflectionTarget_, false, sceneCopy_, linear_);
        pass(pipelines->reflectionComposite, color, true, reflectionTarget_, nearest_);
    }
}

}  // namespace hc09vk
