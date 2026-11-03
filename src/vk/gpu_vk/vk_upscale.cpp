#include "vk_upscale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "vk_shader_compile.h"

namespace hc09vk {

namespace {

// Push constants: FsrEasuCon's four constants (as floats), RCAS's sharpness, the area filter's footprint, and the
// output rectangle's origin in the target.
struct Constants
{
    float con0[4], con1[4], con2[4], con3[4];
    float origin[2], size[2];
};
static_assert(sizeof(Constants) <= 128, "push constants");

const char* const kVertex = R"GLSL(#version 450
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// EASU and RCAS: the 32-bit float paths of AMD FidelityFX FSR 1 (ffx_fsr1.h, ffx_a.h), ported to plain GLSL.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
const char* const kFragment = R"GLSL(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { vec4 con0, con1, con2, con3; vec4 rect; } C;
layout(location = 0) out vec4 outColor;

float APrxLoRcpF1(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float APrxMedRcpF1(float a) { float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a)); return b * (-b * a + 2.0); }
float APrxLoRsqF1(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
vec3 AMin3F3(vec3 x, vec3 y, vec3 z) { return min(x, min(y, z)); }
vec3 AMax3F3(vec3 x, vec3 y, vec3 z) { return max(x, max(y, z)); }

// The output pixel, relative to the rectangle.
uvec2 OutputPixel() { return uvec2(max(floor(gl_FragCoord.xy - C.rect.xy), vec2(0.0))); }

#if PASS_EASU
void FsrEasuTapF(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v;
    v.x = (off.x * (dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + -1.0;
    float wA = lob * d2 + -1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + -(25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void main()
{
    vec2 pp = vec2(OutputPixel()) * C.con0.xy + C.con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 p0 = fp * C.con1.xy + C.con1.zw;
    vec2 p1 = p0 + C.con2.xy;
    vec2 p2 = p0 + C.con2.zw;
    vec2 p3 = p0 + C.con3.xy;
    vec4 bczzR = textureGather(src, p0, 0), bczzG = textureGather(src, p0, 1), bczzB = textureGather(src, p0, 2);
    vec4 ijfeR = textureGather(src, p1, 0), ijfeG = textureGather(src, p1, 1), ijfeB = textureGather(src, p1, 2);
    vec4 klhgR = textureGather(src, p2, 0), klhgG = textureGather(src, p2, 1), klhgB = textureGather(src, p2, 2);
    vec4 zzonR = textureGather(src, p3, 0), zzonG = textureGather(src, p3, 1), zzonB = textureGather(src, p3, 2);
    vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y, iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w, oL = zzonL.z, nL = zzonL.w;
    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = APrxLoRcpF1(lob);
    vec3 min4 = min(AMin3F3(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w), vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                    vec3(klhgR.x, klhgG.x, klhgB.x));
    vec3 max4 = max(AMax3F3(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w), vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                    vec3(klhgR.x, klhgG.x, klhgB.x));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    FsrEasuTapF(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));   // b
    FsrEasuTapF(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));   // c
    FsrEasuTapF(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));   // i
    FsrEasuTapF(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.y, ijfeG.y, ijfeB.y));    // j
    FsrEasuTapF(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.z, ijfeG.z, ijfeB.z));    // f
    FsrEasuTapF(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));   // e
    FsrEasuTapF(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.x, klhgG.x, klhgB.x));    // k
    FsrEasuTapF(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));    // l
    FsrEasuTapF(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));    // h
    FsrEasuTapF(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.w, klhgG.w, klhgB.w));    // g
    FsrEasuTapF(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));    // o
    FsrEasuTapF(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));    // n
    outColor = vec4(min(max4, max(min4, aC * vec3(1.0 / aW))), 1.0);
}
#endif

#if PASS_AREA
// Downscaling (hc09): the average of a grid of bilinear samples over the output pixel's footprint in the source
// (con0.xy: source pixels per output pixel, con0.zw: the grid, con1.xy: 1 / source size).
void main()
{
    vec2 start = vec2(OutputPixel()) * C.con0.xy;
    int nx = int(C.con0.z), ny = int(C.con0.w);
    vec3 sum = vec3(0.0);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
        {
            vec2 at = start + (vec2(x, y) + 0.5) * C.con0.xy / vec2(nx, ny);
            sum += textureLod(src, at * C.con1.xy, 0.0).rgb;
        }
    outColor = vec4(sum / float(nx * ny), 1.0);
}
#endif

#if PASS_RCAS
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))
vec3 FsrRcasLoadF(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).rgb; }

void main()
{
    ivec2 sp = ivec2(OutputPixel());
    vec3 b = FsrRcasLoadF(sp + ivec2(0, -1));
    vec3 d = FsrRcasLoadF(sp + ivec2(-1, 0));
    vec3 e = FsrRcasLoadF(sp);
    vec3 f = FsrRcasLoadF(sp + ivec2(1, 0));
    vec3 h = FsrRcasLoadF(sp + ivec2(0, 1));
    float bR = b.r, bG = b.g, bB = b.b, dR = d.r, dG = d.g, dB = d.b, eR = e.r, eG = e.g, eB = e.b;
    float fR = f.r, fG = f.g, fB = f.b, hR = h.r, hG = h.g, hB = h.b;
    float mn4R = min(AMin3F1(bR, dR, fR), hR);
    float mn4G = min(AMin3F1(bG, dG, fG), hG);
    float mn4B = min(AMin3F1(bB, dB, fB), hB);
    float mx4R = max(AMax3F1(bR, dR, fR), hR);
    float mx4G = max(AMax3F1(bG, dG, fG), hG);
    float mx4B = max(AMax3F1(bB, dB, fB), hB);
    vec2 peakC = vec2(1.0, -1.0 * 4.0);
    float hitMinR = min(mn4R, eR) * (1.0 / (4.0 * mx4R));
    float hitMinG = min(mn4G, eG) * (1.0 / (4.0 * mx4G));
    float hitMinB = min(mn4B, eB) * (1.0 / (4.0 * mx4B));
    float hitMaxR = (peakC.x - max(mx4R, eR)) * (1.0 / (4.0 * mn4R + peakC.y));
    float hitMaxG = (peakC.x - max(mx4G, eG)) * (1.0 / (4.0 * mn4G + peakC.y));
    float hitMaxB = (peakC.x - max(mx4B, eB)) * (1.0 / (4.0 * mn4B + peakC.y));
    float lobeR = max(-hitMinR, hitMaxR);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobeB = max(-hitMinB, hitMaxB);
    float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeR, lobeG, lobeB), 0.0)) * C.con0.x;
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    outColor = vec4((lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL,
                    (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL,
                    (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL, 1.0);
}
#endif
)GLSL";

VkShaderModule Module(VkDevice device, const std::string& source, Stage stage, const char* name)
{
    std::vector<uint32_t> spirv;
    std::string log;
    if (!CompileGlsl(source, stage, name, spirv, &log))
    {
        std::fprintf(stderr, "[upscale] shader %s failed: %s\n", name, log.c_str());
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

bool Upscaler::Setup()
{
    VkDevice d = dev_->device;
    VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants) };
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &setLayout_;
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pci, nullptr, &pipeLayout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return false;
    vs_ = Module(d, kVertex, Stage::kVertex, "upscale_vs");
    static const char* const kDefines[kPasses] = { "PASS_EASU", "PASS_AREA", "PASS_RCAS" };
    for (int p = 0; p < kPasses; ++p)
        fs_[p] = Module(d, std::string("#version 450\n#define ") + kDefines[p] + " 1\n" + kFragment, Stage::kFragment, kDefines[p]);
    return vs_ && fs_[kEasu] && fs_[kArea] && fs_[kRcas];
}

VkPipeline Upscaler::Pipeline(Pass pass, VkFormat format)
{
    for (uint32_t i = 0; i < madeCount_; ++i)
        if (made_[i].pass == pass && made_[i].format == format) return made_[i].pipe;
    if (madeCount_ == std::size(made_)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs_;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs_[pass];
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
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_->device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    made_[madeCount_++] = { pass, format, pipe };
    return pipe;
}

bool Upscaler::Record(Device& dev, VkCommandBuffer cb, const Image& src, VkImageView dstView, VkFormat dstFormat, int32_t x, int32_t y,
                      uint32_t w, uint32_t h)
{
    if (!enabled || failed_ || !dstView || !w || !h || (w == src.width && h == src.height)) return false;
    if (!dev_)
    {
        dev_ = &dev;
        if (!Setup())
        {
            std::fprintf(stderr, "[upscale] setup failed: the picture is scaled with a bilinear blit\n");
            failed_ = true;
            return false;
        }
        std::fprintf(stderr, "[upscale] FSR 1 upscaling, area downscaling, sharpening %.2f\n", sharpening);
    }
    const bool up = w > src.width || h > src.height;
    const bool sharpen = sharpening > 0.0f;
    const Pass scalePass = up ? kEasu : kArea;
    VkPipeline scalePipe = Pipeline(scalePass, sharpen ? VK_FORMAT_R8G8B8A8_UNORM : dstFormat);
    VkPipeline rcasPipe = sharpen ? Pipeline(kRcas, dstFormat) : VK_NULL_HANDLE;
    if (!scalePipe || (sharpen && !rcasPipe)) { failed_ = true; return false; }
    if (sharpen && (middle_.width != w || middle_.height != h))
    {
        if (middle_.image) dev.DestroyImage(middle_);   // the previous frame's work is done (the caller's contract)
        if (!dev.CreateImage(VK_IMAGE_VIEW_TYPE_2D, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, middle_))
        { failed_ = true; return false; }
    }
    vkResetDescriptorPool(dev.device, pool_, 0);
    auto set = [&](VkImageView view) {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev.device, &ai, &s) != VK_SUCCESS) return s;
        VkDescriptorImageInfo ii{ linear_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = s;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev.device, 1, &wr, 0, nullptr);
        return s;
    };
    VkDescriptorSet srcSet = set(src.view), middleSet = sharpen ? set(middle_.view) : VK_NULL_HANDLE;
    if (!srcSet || (sharpen && !middleSet)) { failed_ = true; return false; }

    // One full-screen pass into `view` at the rectangle (ox, oy, w, h).
    auto pass = [&](VkPipeline pipe, VkDescriptorSet s, VkImageView view, VkImageLayout layout, int32_t ox, int32_t oy, const Constants& c) {
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = view;
        ca.imageLayout = layout;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRect2D area{ { ox, oy }, { w, h } };
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp{ float(ox), float(oy), float(w), float(h), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &s, 0, nullptr);
        vkCmdPushConstants(cb, pipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
    };

    Constants c{};
    const float sw = float(src.width), sh = float(src.height), ow = float(w), oh = float(h);
    if (up)
    {
        // FsrEasuCon (input viewport = input size = the source).
        c.con0[0] = sw / ow;
        c.con0[1] = sh / oh;
        c.con0[2] = 0.5f * sw / ow - 0.5f;
        c.con0[3] = 0.5f * sh / oh - 0.5f;
        c.con1[0] = 1.0f / sw;
        c.con1[1] = 1.0f / sh;
        c.con1[2] = 1.0f / sw;
        c.con1[3] = -1.0f / sh;
        c.con2[0] = -1.0f / sw;
        c.con2[1] = 2.0f / sh;
        c.con2[2] = 1.0f / sw;
        c.con2[3] = 2.0f / sh;
        c.con3[0] = 0.0f;
        c.con3[1] = 4.0f / sh;
    }
    else
    {
        c.con0[0] = sw / ow;
        c.con0[1] = sh / oh;
        c.con0[2] = std::min(4.0f, std::ceil(sw / ow));
        c.con0[3] = std::min(4.0f, std::ceil(sh / oh));
        c.con1[0] = 1.0f / sw;
        c.con1[1] = 1.0f / sh;
    }
    Transition(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (sharpen)
    {
        Transition(cb, middle_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        pass(scalePipe, srcSet, middle_.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, c);
        Transition(cb, middle_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
        // FsrRcasCon: sharpness in stops (0 the most, 2 a quarter of it).
        Constants r{};
        r.con0[0] = std::exp2(-2.0f * (1.0f - std::min(sharpening, 1.0f)));
        r.origin[0] = float(x);
        r.origin[1] = float(y);
        pass(rcasPipe, middleSet, dstView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, x, y, r);
    }
    else
    {
        c.origin[0] = float(x);
        c.origin[1] = float(y);
        pass(scalePipe, srcSet, dstView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, x, y, c);
    }
    Transition(cb, src.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return true;
}

void Upscaler::Destroy()
{
    if (!dev_ || !dev_->device) return;
    VkDevice d = dev_->device;
    for (uint32_t i = 0; i < madeCount_; ++i) vkDestroyPipeline(d, made_[i].pipe, nullptr);
    madeCount_ = 0;
    if (middle_.image) dev_->DestroyImage(middle_);
    for (VkShaderModule& m : fs_)
        if (m) { vkDestroyShaderModule(d, m, nullptr); m = VK_NULL_HANDLE; }
    if (vs_) vkDestroyShaderModule(d, vs_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    vs_ = VK_NULL_HANDLE;
    linear_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    dev_ = nullptr;
}

}  // namespace hc09vk
