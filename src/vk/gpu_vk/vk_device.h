#pragma once
// Vulkan device for the real-time backend (docs/BACKEND_PLAN.md section 2):
// instance (+ validation with HC09_VK_VALIDATION=1), one physical device
// (discrete GPU preferred), one graphics+compute queue that can present, and
// small helpers for buffers, images and one-shot command buffers.
// Portable: no platform headers (the window lives in vk_platform_*.cpp).
#include <cstdint>
#include <string>
#include <vector>

// Vulkan through volk (this project): vulkan-1.dll is loaded at run time, so no Vulkan SDK is needed to build.
#include "volk.h"

namespace hc09vk {

const char* ResultName(VkResult r);

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;      // host-visible buffers stay mapped
    VkDeviceSize size = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView altView = VK_NULL_HANDLE;   // optional view with another format (CreateImage altViewFormat)
    std::vector<VkImageView> levelViews;     // optional single-level views (owner-created), destroyed with the image
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mipLevels = 1, layers = 1, depth = 1;
};

struct Device
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    bool validation = false;
    uint32_t validationMessages = 0;   // warnings/errors reported by the layer
    bool hostImport = false;           // VK_EXT_external_memory_host (live renderer: guest memory in place)
    VkDeviceSize hostImportAlignment = 0;
    bool pixelInterlock = false;       // fragment shader pixel interlock + fragment stores + clip distances + fp64 (EDRAM renderer)
    float anisotropy = 1.0f;           // the most sampler anisotropy the device allows (1: none)
    bool geometryShader = false;       // hc09: the feature is on (gl_PrimitiveID in fragment shaders: the crowd)

    // Step 1: the instance, with the window's surface extensions.
    bool CreateInstance(const std::vector<const char*>& extensions, std::string* err);
    // Step 2: pick a device whose queue can present to `surface` (may be null) and create it.
    bool CreateDevice(VkSurfaceKHR surface, std::string* err);
    void Destroy();

    uint32_t MemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) const;
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, Buffer& out);
    void DestroyBuffer(Buffer& b);
    // A buffer over existing host memory (VK_EXT_external_memory_host): `ptr` and
    // `size` aligned to hostImportAlignment; the memory stays the caller's.
    bool ImportHostBuffer(void* ptr, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out, std::string* err);
    bool CreateImage2D(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out,
                       uint32_t mipLevels = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    // Any sampled image: 2D, 2D array (`depthOrLayers` layers) or 3D (`depthOrLayers` slices), with a view of that type.
    // `altViewFormat`: also create altView with that (size-compatible) format.
    bool CreateImage(VkImageViewType viewType, uint32_t width, uint32_t height, uint32_t depthOrLayers, VkFormat format,
                     VkImageUsageFlags usage, Image& out, uint32_t mipLevels = 1,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkFormat altViewFormat = VK_FORMAT_UNDEFINED,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void DestroyImage(Image& img);
    // Record into a fresh command buffer, then EndOneShot submits it and waits.
    VkCommandBuffer BeginOneShot();
    bool EndOneShot(VkCommandBuffer cb);
};

// Image layout transition with explicit stages/access masks.
void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

}  // namespace hc09vk
