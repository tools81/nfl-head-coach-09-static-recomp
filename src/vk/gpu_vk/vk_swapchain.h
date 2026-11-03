#pragma once
// Swapchain for the backend's window: present an image (scaled to fit,
// aspect kept, black bars) at the display's rate (FIFO).
#include <cstdint>
#include <string>
#include <vector>

#include "vk_device.h"
#include "vk_upscale.h"

namespace hc09vk {

// One overlay vertex: position in swapchain pixels (y down), colour R,G,B,A bytes (A = opacity).
struct OverlayVtx
{
    float x, y;
    uint32_t rgba;
};

struct Swapchain
{
    // Triangles drawn over the presented image (alpha-blended), cleared by Present. The live tweaker's in-game
    // overlay fills this before each Present; empty = nothing is drawn (and no pipeline is made).
    std::vector<OverlayVtx> overlay;
    Device* dev = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone;   // one per swapchain image
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    Buffer readback;                       // host copy of the last presented image (if requested)
    bool transferSrc = false;              // swapchain images can be read back
    bool colorAttachment = false;          // swapchain images can be rendered to (the overlay)
    bool vsync = true;                     // set before Create: false = mailbox/immediate if offered
    Upscaler upscaler;                     // hc09: FSR 1 / area scaling and sharpening in place of the blit
    float stretch = 1.0f;                  // hc09: the picture is shown this much wider than its pixels (widescreen.h)

    bool Create(Device& device, VkSurfaceKHR surf, uint32_t width, uint32_t height, std::string* err);
    // Rebuild for a new window size (no-op for 0x0, e.g. minimized).
    bool Resize(uint32_t width, uint32_t height, std::string* err);
    void Destroy();   // the surface stays (owned by the caller)

    // Blit `src` (in TRANSFER_SRC_OPTIMAL layout; with SAMPLED usage, for the upscaler) to the next image and present.
    // `presented`: if non-null, receives the presented pixels (R,G,B,A bytes,
    // extent.width x extent.height) — what the window shows.
    VkResult Present(const Image& src, std::vector<uint32_t>* presented);

private:
    bool Build(uint32_t width, uint32_t height, std::string* err);
    void DestroySwapchain();
    bool EnsureOverlayPipeline();
    void DestroyOverlay();

    std::vector<VkImageView> views;        // one per swapchain image (overlay rendering)
    VkPipeline ovPipeline = VK_NULL_HANDLE;
    VkPipelineLayout ovLayout = VK_NULL_HANDLE;
    VkFormat ovFormat = VK_FORMAT_UNDEFINED;
    bool ovFailed = false;
    Buffer ovVerts;
};

// Upload R,G,B,A pixels into a new R8G8B8A8_UNORM image left in
// TRANSFER_SRC_OPTIMAL layout (ready for Swapchain::Present).
bool UploadRGBA8(Device& dev, const uint32_t* rgba, uint32_t width, uint32_t height, Image& out);

}  // namespace hc09vk
