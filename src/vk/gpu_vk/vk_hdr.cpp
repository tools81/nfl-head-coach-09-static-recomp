#include "vk_hdr.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "vk_shader_compile.h"

namespace hc09vk {

namespace {

// Push constants, laid out as the shader's block.
struct Constants
{
    float pictureOrigin[2], pictureInvSize[2];   // the front buffer's rectangle in the window
    float gainOffset[2], gainInvScale[2];        // the gain image's rectangle in the front buffer's (0..1) coordinates
    float paperWhite, peak, gainOn, pq;
};
static_assert(sizeof(Constants) <= 128, "push constants");

const char* const kVertex = R"GLSL(#version 450
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* const kFragment = R"GLSL(#version 450
layout(push_constant) uniform Hdr { vec4 picture, gainRect, params; } P;
layout(set = 0, binding = 0) uniform sampler2D sdrImage;
layout(set = 0, binding = 1) uniform sampler2D frontImage;
layout(set = 0, binding = 2) uniform sampler2D gainImage;
layout(location = 0) out vec4 outColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

// SMPTE ST 2084 (PQ) of linear light in nits.
vec3 Pq(vec3 nits)
{
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

void main()
{
    vec3 sdr = texelFetch(sdrImage, ivec2(gl_FragCoord.xy), 0).rgb;
    float gain = 1.0;
    if (P.params.z > 0.0)
    {
        vec2 uv = (gl_FragCoord.xy - P.picture.xy) * P.picture.zw;
        vec2 g = (uv - P.gainRect.xy) * P.gainRect.zw;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))) && all(greaterThanEqual(g, vec2(0.0))) &&
            all(lessThanEqual(g, vec2(1.0))))
        {
            // The scene's gain where the front buffer still shows the scene's brightness (not a menu over it).
            vec2 s = textureLod(gainImage, g, 0.0).rg;
            float shown = dot(textureLod(frontImage, uv, 0.0).rgb, kLuma);
            gain = P.params.z > 1.5 ? max(s.r, 1.0) : mix(1.0, max(s.r, 1.0), 1.0 - smoothstep(0.02, 0.05, abs(shown - s.g)));
            if (P.params.z > 2.5) { outColor = vec4(vec3(clamp(abs(shown - s.g) * 10.0, 0.0, 1.0)), 1.0); outColor.r = s.g; outColor.g = shown; return; }
        }
    }
    if (P.params.w == 0.0)
    {
        outColor = vec4(mix(sdr, vec3(1.0, 0.0, 0.0), clamp((gain - 1.0) / max(P.params.y / P.params.x - 1.0, 1e-3), 0.0, 1.0) * 0.8), 1.0);
        return;
    }
    // SDR (gamma 2.2, BT.709) to nits, BT.2020, PQ.
    vec3 nits = pow(max(sdr, vec3(0.0)), vec3(2.2)) * (gain * P.params.x);
    const mat3 k709To2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
    outColor = vec4(Pq(max(k709To2020 * nits, vec3(0.0))), 1.0);
}
)GLSL";

VkShaderModule Module(VkDevice device, const char* source, Stage stage, const char* name)
{
    std::vector<uint32_t> spirv;
    std::string log;
    if (!CompileGlsl(source, stage, name, spirv, &log))
    {
        std::fprintf(stderr, "[hdr] shader %s failed: %s\n", name, log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

void Transition(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

bool HdrOutput::Setup()
{
    VkDevice d = dev_->device;
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) b[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants) };
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &setLayout_;
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pci, nullptr, &pipeLayout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return false;
    vs_ = Module(d, kVertex, Stage::kVertex, "hdr_vs");
    fs_ = Module(d, kFragment, Stage::kFragment, "hdr_fs");
    return vs_ && fs_;
}

VkPipeline HdrOutput::Pipeline(VkFormat format, bool)
{
    if (pipe_ && madeFormat_ == format) return pipe_;
    if (pipe_) { vkDestroyPipeline(dev_->device, pipe_, nullptr); pipe_ = VK_NULL_HANDLE; }   // the previous frame's work is done
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs_;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs_;
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachmentFormats = &format;
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
    if (vkCreateGraphicsPipelines(dev_->device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe_) != VK_SUCCESS) pipe_ = VK_NULL_HANDLE;
    madeFormat_ = format;
    return pipe_;
}

bool HdrOutput::Record(Device& dev, VkCommandBuffer cb, const Image& sdr, const Image& src, int32_t x, int32_t y, uint32_t w, uint32_t h,
                       const HdrGain& gain, VkImageView dstView, VkFormat dstFormat, VkExtent2D extent, bool pq)
{
    if (failed_ || !dstView || !w || !h) return false;
    if (!dev_)
    {
        dev_ = &dev;
        if (!Setup())
        {
            std::fprintf(stderr, "[hdr] setup failed: the picture is shown in SDR\n");
            failed_ = true;
            return false;
        }
    }
    VkPipeline pipe = Pipeline(dstFormat, pq);
    if (!pipe) { failed_ = true; return false; }
    const bool gainOn = gain.image && gain.image->view && gain.width > 0 && gain.height > 0;
    static bool loggedGain = false;
    if (gainOn && !loggedGain)
    {
        loggedGain = true;
        std::fprintf(stderr, "[hdr] the 3D scene's highlights: gain %.0fx%.0f at %.0f,%.0f of the %ux%u picture\n", gain.width,
                     gain.height, gain.x, gain.y, src.width, src.height);
    }
    vkResetDescriptorPool(dev.device, pool_, 0);
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS) { failed_ = true; return false; }
    VkDescriptorImageInfo images[3] = { { linear_, sdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { linear_, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        gainOn ? VkDescriptorImageInfo{ linear_, gain.image->view, VK_IMAGE_LAYOUT_GENERAL }
                                               : VkDescriptorImageInfo{ linear_, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet wr[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(dev.device, 3, wr, 0, nullptr);

    Constants c{};
    c.pictureOrigin[0] = float(x);
    c.pictureOrigin[1] = float(y);
    c.pictureInvSize[0] = 1.0f / float(w);
    c.pictureInvSize[1] = 1.0f / float(h);
    if (gainOn)
    {
        c.gainOffset[0] = gain.x / float(src.width);
        c.gainOffset[1] = gain.y / float(src.height);
        c.gainInvScale[0] = float(src.width) / gain.width;
        c.gainInvScale[1] = float(src.height) / gain.height;
    }
    c.paperWhite = paperWhite;
    c.peak = std::max(peak, paperWhite);
    c.gainOn = gainOn ? (debug > 1 ? float(debug) : 1.0f) : 0.0f;   // debug 2: no test against the front buffer; 3: r the scene's brightness, g the front buffer's, b their difference x10
    c.pq = pq ? 1.0f : 0.0f;

    // The renderer's writes (the gain image, the front buffer) were submitted before this command buffer.
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    Transition(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = dstView;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRect2D area{ { 0, 0 }, extent };
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = area;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp{ 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, pipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    Transition(cb, src.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return true;
}

void HdrOutput::Destroy()
{
    if (!dev_ || !dev_->device) return;
    VkDevice d = dev_->device;
    if (pipe_) vkDestroyPipeline(d, pipe_, nullptr);
    if (fs_) vkDestroyShaderModule(d, fs_, nullptr);
    if (vs_) vkDestroyShaderModule(d, vs_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    pipe_ = VK_NULL_HANDLE;
    fs_ = vs_ = VK_NULL_HANDLE;
    linear_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    dev_ = nullptr;
}

}  // namespace hc09vk
