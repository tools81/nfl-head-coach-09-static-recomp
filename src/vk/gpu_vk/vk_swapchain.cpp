#include "vk_swapchain.h"
#include "vk_shader_compile.h"
#include "../dev_env.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hc09vk {

bool Swapchain::Create(Device& device, VkSurfaceKHR surf, uint32_t width, uint32_t height, std::string* err)
{
    dev = &device;
    surface = surf;
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = dev->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkCreateSemaphore(dev->device, &si, nullptr, &acquired) != VK_SUCCESS ||
        vkCreateFence(dev->device, &fi, nullptr, &fence) != VK_SUCCESS ||
        vkAllocateCommandBuffers(dev->device, &ai, &cb) != VK_SUCCESS)
    {
        if (err) *err = "swapchain sync objects";
        return false;
    }
    return Build(width, height, err);
}

bool Swapchain::Build(uint32_t width, uint32_t height, std::string* err)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, surface, &n, fmts.data());
    // UNORM, not _SRGB: the Xenos front buffer already holds display-encoded
    // values; the bytes must reach the screen unchanged.
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
    for (const auto& f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    format = chosen.format;
    extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{ width, height };
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    transferSrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount + 1, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    colorAttachment = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (transferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
                    (colorAttachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync)
    {
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, surface, &n, modes.data());
        for (VkPresentModeKHR m : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) { ci.presentMode = m; break; }
    }
    ci.clipped = VK_TRUE;
    VkResult r = vkCreateSwapchainKHR(dev->device, &ci, nullptr, &swapchain);
    if (r != VK_SUCCESS) { if (err) *err = std::string("vkCreateSwapchainKHR: ") + ResultName(r); return false; }
    vkGetSwapchainImagesKHR(dev->device, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(dev->device, swapchain, &n, images.data());
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    renderDone.resize(n);
    for (auto& s : renderDone) vkCreateSemaphore(dev->device, &si, nullptr, &s);
    views.assign(n, VK_NULL_HANDLE);
    if (colorAttachment)
        for (uint32_t i = 0; i < n; ++i)
        {
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCreateImageView(dev->device, &vi, nullptr, &views[i]);
        }
    return true;
}

void Swapchain::DestroySwapchain()
{
    if (!dev || !dev->device) return;
    vkDeviceWaitIdle(dev->device);
    for (auto s : renderDone) vkDestroySemaphore(dev->device, s, nullptr);
    renderDone.clear();
    for (auto v : views) if (v) vkDestroyImageView(dev->device, v, nullptr);
    views.clear();
    images.clear();
    if (swapchain) vkDestroySwapchainKHR(dev->device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

bool Swapchain::Resize(uint32_t width, uint32_t height, std::string* err)
{
    if (!width || !height) return true;
    DestroySwapchain();
    return Build(width, height, err);
}

void Swapchain::Destroy()
{
    if (!dev || !dev->device) return;
    DestroySwapchain();
    DestroyOverlay();
    dev->DestroyBuffer(readback);
    if (acquired) vkDestroySemaphore(dev->device, acquired, nullptr);
    if (fence) vkDestroyFence(dev->device, fence, nullptr);
    if (cb) vkFreeCommandBuffers(dev->device, dev->pool, 1, &cb);
    acquired = VK_NULL_HANDLE; fence = VK_NULL_HANDLE; cb = VK_NULL_HANDLE;
}

VkResult Swapchain::Present(const Image& src, std::vector<uint32_t>* presented)
{
    VkDevice d = dev->device;
    vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(d, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return r;
    vkResetFences(d, 1, &fence);
    bool wantReadback = presented && transferSrc;
    VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
    if (wantReadback && readback.size < bytes)
    {
        dev->DestroyBuffer(readback);
        if (!dev->CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback))
            wantReadback = false;
    }

    VkImage dst = images[index];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    ImageBarrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    black.float32[3] = 1.0f;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    ImageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    // Fit the source into the window, keeping its aspect ratio.
    uint32_t w = extent.width, h = uint32_t(uint64_t(extent.width) * src.height / src.width);
    if (h > extent.height) { h = extent.height; w = uint32_t(uint64_t(extent.height) * src.width / src.height); }
    int32_t x0 = int32_t(extent.width - w) / 2, y0 = int32_t(extent.height - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int32_t(src.width), int32_t(src.height), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { x0, y0, 0 };
    blit.dstOffsets[1] = { x0 + int32_t(w), y0 + int32_t(h), 1 };
    vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   (w == src.width && h == src.height) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    // The overlay: alpha-blended triangles over the blitted image (the live tweaker's; the release build,
    // HC09_DEV_TOOLS=0, has none and leaves the overlay pipeline out).
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags access = VK_ACCESS_TRANSFER_WRITE_BIT;
    if (HC09_DEV_TOOLS && !overlay.empty() && index < views.size() && views[index] && EnsureOverlayPipeline())
    {
        const VkDeviceSize vbytes = overlay.size() * sizeof(OverlayVtx);
        if (ovVerts.size < vbytes)
        {
            dev->DestroyBuffer(ovVerts);
            dev->CreateBuffer(vbytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ovVerts);
        }
        if (ovVerts.mapped)
        {
            memcpy(ovVerts.mapped, overlay.data(), size_t(vbytes));
            ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, stage, access,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = views[index];
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, extent };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(cb, &ri);
            VkViewport vp{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
            VkRect2D sc{ { 0, 0 }, extent };
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ovPipeline);
            const float size[2] = { float(extent.width), float(extent.height) };
            vkCmdPushConstants(cb, ovLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof size, size);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &ovVerts.buffer, &off);
            vkCmdDraw(cb, uint32_t(overlay.size()), 1, 0, 0);
            vkCmdEndRendering(cb);
            layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        }
    }
    overlay.clear();
    if (wantReadback)
    {
        ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        ImageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    else
        ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, stage, access, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[index];
    r = vkQueueSubmit(dev->queue, 1, &si, fence);
    if (r != VK_SUCCESS) return r;
    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(dev->queue, &pi);

    if (wantReadback)
    {
        vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
        presented->resize(size_t(extent.width) * extent.height);
        memcpy(presented->data(), readback.mapped, size_t(bytes));
        if (format == VK_FORMAT_B8G8R8A8_UNORM)   // B,G,R,A -> R,G,B,A
            for (uint32_t& p : *presented) p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
    }
    return r;
}

// The overlay pipeline: 2-D triangles in pixels, vertex colour, alpha blending, dynamic rendering into the
// swapchain image. Made on first use (and again if the swapchain format changes).
bool Swapchain::EnsureOverlayPipeline()
{
    if (ovPipeline && ovFormat == format) return true;
    if (ovFailed) return false;
    DestroyOverlay();
    static const char* kVs =
        "#version 450\n"
        "layout(location = 0) in vec2 pos;\n"
        "layout(location = 1) in vec4 col;\n"
        "layout(push_constant) uniform PC { vec2 size; } pc;\n"
        "layout(location = 0) out vec4 vcol;\n"
        "void main() { gl_Position = vec4(pos / pc.size * 2.0 - 1.0, 0.0, 1.0); vcol = col; }\n";
    static const char* kFs =
        "#version 450\n"
        "layout(location = 0) in vec4 vcol;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = vcol; }\n";
    std::vector<uint32_t> vs, fs;
    std::string log;
    if (!CompileGlsl(kVs, Stage::kVertex, "overlay.vert", vs, &log) ||
        !CompileGlsl(kFs, Stage::kFragment, "overlay.frag", fs, &log))
    {
        fprintf(stderr, "[overlay] shader: %s\n", log.c_str());
        ovFailed = true;
        return false;
    }
    VkDevice d = dev->device;
    VkShaderModule mv = VK_NULL_HANDLE, mf = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = vs.size() * 4;
    mi.pCode = vs.data();
    vkCreateShaderModule(d, &mi, nullptr, &mv);
    mi.codeSize = fs.size() * 4;
    mi.pCode = fs.data();
    vkCreateShaderModule(d, &mi, nullptr, &mf);
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo li{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(d, &li, nullptr, &ovLayout);
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = mv;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = mf;
    st[1].pName = "main";
    VkVertexInputBindingDescription bind{ 0, sizeof(OverlayVtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr[2] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8 },
    };
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = attr;
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
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
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
    pi.layout = ovLayout;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, nullptr, &ovPipeline);
    vkDestroyShaderModule(d, mv, nullptr);
    vkDestroyShaderModule(d, mf, nullptr);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "[overlay] pipeline: %s\n", ResultName(r));
        ovFailed = true;
        return false;
    }
    ovFormat = format;
    return true;
}

void Swapchain::DestroyOverlay()
{
    if (!dev || !dev->device) return;
    if (ovPipeline) vkDestroyPipeline(dev->device, ovPipeline, nullptr);
    if (ovLayout) vkDestroyPipelineLayout(dev->device, ovLayout, nullptr);
    ovPipeline = VK_NULL_HANDLE;
    ovLayout = VK_NULL_HANDLE;
    dev->DestroyBuffer(ovVerts);
}

bool UploadRGBA8(Device& dev, const uint32_t* rgba, uint32_t width, uint32_t height, Image& out)
{
    VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    Buffer staging;
    if (!dev.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
        return false;
    memcpy(staging.mapped, rgba, size_t(bytes));
    if (!dev.CreateImage2D(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, out))
    {
        dev.DestroyBuffer(staging);
        return false;
    }
    VkCommandBuffer cb = dev.BeginOneShot();
    ImageBarrier(cb, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cb, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cb, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT);
    bool ok = dev.EndOneShot(cb);
    dev.DestroyBuffer(staging);
    return ok;
}

}  // namespace hc09vk
