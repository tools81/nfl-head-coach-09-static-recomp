#include "vk_swapchain.h"
#include "vk_shader_compile.h"
#include "vk_replay.h"
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
    // hc09: HDR10 (10-bit PQ, BT.2020) when asked for and offered (Windows offers it with HDR on in its settings).
    hdrActive = false;
    if (hdrRequested && (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        for (const auto& f : fmts)
            if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) { chosen = f; hdrActive = true; break; }
    static bool logged = false;
    if (hdrRequested && !logged)
    {
        logged = true;
        if (hdrActive)
            fprintf(stderr, "[hdr] HDR10 output: paper white %.0f nits, peak %.0f nits\n", hdr.paperWhite, hdr.peak);
        else
            fprintf(stderr, "[hdr] the display offers no HDR10 output (is HDR on in Windows' display settings?); SDR it is\n");
    }
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
    upscaler.Destroy();
    hdr.Destroy();
    if (sdr_.image) dev->DestroyImage(sdr_);
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

    // hc09: HDR (or its debug view): the picture is made in sdr_ as it would be in the window, then the HDR pass
    // writes the window from it.
    const VkImage swapImage = images[index];
    const VkImageView swapView = index < views.size() ? views[index] : VK_NULL_HANDLE;
    bool viaHdr = (hdrActive || hdr.debug) && swapView;
    if (viaHdr && (sdr_.width != extent.width || sdr_.height != extent.height))
    {
        if (sdr_.image) dev->DestroyImage(sdr_);   // the fence: the last frame's work is done
        viaHdr = dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, extent.width, extent.height, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT,
                                  sdr_);
        if (!viaHdr) sdr_ = Image{};
    }
    const VkImage dst = viaHdr ? sdr_.image : swapImage;
    const VkImageView dstView = viaHdr ? sdr_.view : swapView;
    const VkFormat dstFormat = viaHdr ? VK_FORMAT_R8G8B8A8_UNORM : format;
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
    // Fit the source into the window, keeping its aspect ratio (hc09: `stretch` times wider).
    const double shown = double(src.width) * stretch / double(src.height);
    uint32_t w = extent.width, h = uint32_t(double(extent.width) / shown + 0.5);
    if (h > extent.height) { h = extent.height; w = std::min(extent.width, uint32_t(double(extent.height) * shown + 0.5)); }
    int32_t x0 = int32_t(extent.width - w) / 2, y0 = int32_t(extent.height - h) / 2;
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags access = VK_ACCESS_TRANSFER_WRITE_BIT;
    // hc09: scaled with FSR 1 (or the area filter) and sharpened where the swapchain can be rendered to, else (and
    // at the image's own size) blitted.
    bool scaled = false;
    if (upscaler.enabled && !(w == src.width && h == src.height) && dstView)
    {
        ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, stage, access, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        scaled = upscaler.Record(*dev, cb, src, dstView, dstFormat, x0, y0, w, h);
        if (!scaled)
        {
            ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, stage, access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
    }
    if (!scaled)
    {
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { int32_t(src.width), int32_t(src.height), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0] = { x0, y0, 0 };
        blit.dstOffsets[1] = { x0 + int32_t(w), y0 + int32_t(h), 1 };
        vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       (w == src.width && h == src.height) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    // The overlay: alpha-blended triangles over the scaled image (the score bar's, and the live tweaker's).
    if (!overlay.empty() && dstView && EnsureOverlayPipeline(dstFormat) && UpdateOverlayAtlas() && UpdateOverlayReplay())
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
            // The replay's layers (written by the native renderer's earlier work on this queue) read by the overlay.
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            ImageBarrier(cb, dst, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, stage, access,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = dstView;
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
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ovLayout, 0, 1, &ovSet_, 0, nullptr);
            const float pc[4] = { float(extent.width), float(extent.height), std::max(0.0f, overlayReplayLayer), 0 };
            vkCmdPushConstants(cb, ovLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
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
    // What is read back: the window's image, or with HDR the SDR picture it was made from (the debug view: the
    // window's image).
    VkImage readImage = dst;
    VkFormat readFormat = dstFormat;
    bool copiedSdr = false;
    if (viaHdr)
    {
        ImageBarrier(cb, sdr_.image, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, access, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
        ImageBarrier(cb, swapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        if (!hdr.Record(*dev, cb, sdr_, src, x0, y0, w, h, gain, swapView, format, extent, hdrActive))
        {
            // No HDR pass: the SDR picture as it is (wrong colours on an HDR10 swapchain, but a picture).
            ImageBarrier(cb, sdr_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            ImageBarrier(cb, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = blit.dstOffsets[1] = { int32_t(extent.width), int32_t(extent.height), 1 };
            vkCmdBlitImage(cb, sdr_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            ImageBarrier(cb, sdr_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
            ImageBarrier(cb, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
        }
        if (wantReadback && hdrActive)
        {
            ImageBarrier(cb, sdr_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { extent.width, extent.height, 1 };
            vkCmdCopyImageToBuffer(cb, sdr_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
            wantReadback = false;   // the copy is recorded; the pixels are read below
            copiedSdr = true;
        }
        else
        {
            readImage = swapImage;
            readFormat = format;
        }
        layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    gain = {};
    if (wantReadback)
    {
        ImageBarrier(cb, readImage, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(cb, readImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        ImageBarrier(cb, readImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    else
        ImageBarrier(cb, swapImage, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, stage, access, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    if (copiedSdr) wantReadback = true;
    vkEndCommandBuffer(cb);

    // The image is first written by a transfer (the clear) or, with HDR, as a colour attachment.
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
        if (readFormat == VK_FORMAT_B8G8R8A8_UNORM)   // B,G,R,A -> R,G,B,A
            for (uint32_t& p : *presented) p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
    }
    return r;
}

// The overlay pipeline: 2-D triangles in pixels, vertex colour (times the atlas's texel where the vertices have
// atlas coordinates), premultiplied alpha blending, dynamic rendering into the swapchain image. Made on first use (and again if the
// swapchain format changes).
bool Swapchain::EnsureOverlayPipeline(VkFormat target)
{
    if (ovPipeline && ovFormat == target) return true;
    if (ovFailed) return false;
    DestroyOverlay();
    static const char* kVs =
        "#version 450\n"
        "layout(location = 0) in vec2 pos;\n"
        "layout(location = 1) in vec4 col;\n"
        "layout(location = 2) in vec2 uv;\n"
        "layout(push_constant) uniform PC { vec2 size; float layer; } pc;\n"
        "layout(location = 0) out vec4 vcol;\n"
        "layout(location = 1) out vec2 vuv;\n"
        "void main() { gl_Position = vec4(pos / pc.size * 2.0 - 1.0, 0.0, 1.0); vcol = col; vuv = uv; }\n";
    static const char* kFs =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D atlas;\n"
        "layout(set = 0, binding = 1) uniform sampler2DArray replay;\n"
        "layout(push_constant) uniform PC { vec2 size; float layer; } pc;\n"
        "layout(location = 0) in vec4 vcol;\n"
        "layout(location = 1) in vec2 vuv;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() {\n"
        "    vec4 t = vuv.x < 0.0 ? vec4(1.0) : vuv.x >= 2.0 ? vec4(texture(replay, vec3(vuv.x - 2.0, vuv.y, pc.layer)).rgb, 1.0)\n"
        "                                  : texture(atlas, vuv);\n"   // premultiplied; u from 2: the replay's layer
        "    o = vec4(vcol.rgb * t.rgb * vcol.a, vcol.a * t.a);\n"          // premultiplied out
        "}\n";
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
    // The atlas: one combined image sampler (bilinear, clamped).
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    vkCreateSampler(d, &si, nullptr, &ovSampler_);
    VkDescriptorSetLayoutBinding binding[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                                                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo sli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sli.bindingCount = 2;
    sli.pBindings = binding;
    vkCreateDescriptorSetLayout(d, &sli, nullptr, &ovSetLayout_);
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pli{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pli.maxSets = 1;
    pli.poolSizeCount = 1;
    pli.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(d, &pli, nullptr, &ovPool_);
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = ovPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &ovSetLayout_;
    if (!ovSampler_ || !ovSetLayout_ || !ovPool_ || vkAllocateDescriptorSets(d, &ai, &ovSet_) != VK_SUCCESS)
    {
        fprintf(stderr, "[overlay] the atlas's descriptor set could not be made\n");
        vkDestroyShaderModule(d, mv, nullptr);
        vkDestroyShaderModule(d, mf, nullptr);
        ovFailed = true;
        return false;
    }
    ovAtlasVersion_ = ~0ull;   // a new set: the atlas and the replay are bound again
    ovReplayVersion_ = ~0ull;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo li{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1;
    li.pSetLayouts = &ovSetLayout_;
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
    VkVertexInputAttributeDescription attr[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
    };
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
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
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;   // premultiplied
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
    ovFormat = target;
    return true;
}

// The atlas to the GPU when overlayAtlasVersion has changed (Present has waited for the last frame, so the old one is
// free); a 1x1 of full coverage until there is one.
bool Swapchain::UpdateOverlayAtlas()
{
    if (ovAtlasVersion_ == overlayAtlasVersion && ovAtlas_.image) return true;
    const bool given = overlayAtlasW && overlayAtlasH && overlayAtlas.size() >= size_t(overlayAtlasW) * overlayAtlasH;
    const uint32_t full = 0xFFFFFFFFu;
    const uint32_t* pixels = given ? overlayAtlas.data() : &full;
    const uint32_t w = given ? overlayAtlasW : 1, h = given ? overlayAtlasH : 1;
    if (ovAtlas_.image) dev->DestroyImage(ovAtlas_);
    Buffer staging;
    if (!dev->CreateBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
        return false;
    memcpy(staging.mapped, pixels, size_t(w) * h * 4);
    if (!dev->CreateImage2D(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ovAtlas_))
    {
        dev->DestroyBuffer(staging);
        return false;
    }
    VkCommandBuffer ocb = dev->BeginOneShot();
    ImageBarrier(ocb, ovAtlas_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(ocb, staging.buffer, ovAtlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(ocb, ovAtlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    const bool ok = dev->EndOneShot(ocb);
    dev->DestroyBuffer(staging);
    if (!ok) return false;
    VkDescriptorImageInfo ii{ ovSampler_, ovAtlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ovSet_;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev->device, 1, &wr, 0, nullptr);
    ovAtlasVersion_ = overlayAtlasVersion;
    return true;
}

// The replay's layers (vk_replay.h) bound to the overlay's set when they change; a black one-layer stand-in until
// there are any.
bool Swapchain::UpdateOverlayReplay()
{
    const ReplayFrames frames = ReplayView();
    if (ovReplayVersion_ == frames.version && (frames.view || ovReplayDummy_.image)) return true;
    VkImageView view = frames.view;
    if (!view)
    {
        if (!ovReplayDummy_.image)
        {
            if (!dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ovReplayDummy_))
                return false;
            VkCommandBuffer ocb = dev->BeginOneShot();
            ImageBarrier(ocb, ovReplayDummy_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue black{};
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(ocb, ovReplayDummy_.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
            ImageBarrier(ocb, ovReplayDummy_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            if (!dev->EndOneShot(ocb)) return false;
        }
        view = ovReplayDummy_.view;
    }
    VkDescriptorImageInfo ii{ ovSampler_, view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ovSet_;
    wr.dstBinding = 1;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev->device, 1, &wr, 0, nullptr);
    ovReplayVersion_ = frames.version;
    return true;
}

void Swapchain::DestroyOverlay()
{
    if (!dev || !dev->device) return;
    if (ovPipeline) vkDestroyPipeline(dev->device, ovPipeline, nullptr);
    if (ovLayout) vkDestroyPipelineLayout(dev->device, ovLayout, nullptr);
    if (ovPool_) vkDestroyDescriptorPool(dev->device, ovPool_, nullptr);
    if (ovSetLayout_) vkDestroyDescriptorSetLayout(dev->device, ovSetLayout_, nullptr);
    if (ovSampler_) vkDestroySampler(dev->device, ovSampler_, nullptr);
    if (ovAtlas_.image) dev->DestroyImage(ovAtlas_);
    if (ovReplayDummy_.image) dev->DestroyImage(ovReplayDummy_);
    ovReplayVersion_ = ~0ull;
    ovPipeline = VK_NULL_HANDLE;
    ovLayout = VK_NULL_HANDLE;
    ovPool_ = VK_NULL_HANDLE;
    ovSet_ = VK_NULL_HANDLE;
    ovSetLayout_ = VK_NULL_HANDLE;
    ovSampler_ = VK_NULL_HANDLE;
    ovAtlasVersion_ = ~0ull;
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
