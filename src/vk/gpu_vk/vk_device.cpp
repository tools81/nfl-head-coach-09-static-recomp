#include "../dev_env.h"
#include "vk_device.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>

namespace hc09vk {

const char* ResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_ERROR_(other)";
    }
}

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void* user)
{
    auto* dev = static_cast<Device*>(user);
    ++dev->validationMessages;
    fprintf(stderr, "[vk] %s: %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning",
            data->pMessage);
    return VK_FALSE;
}

bool Check(VkResult r, const char* what, std::string* err)
{
    if (r == VK_SUCCESS) return true;
    if (err) *err = std::string(what) + " failed: " + ResultName(r);
    return false;
}
}  // namespace

bool Device::CreateInstance(const std::vector<const char*>& extensions, std::string* err)
{
    // volk (this project): load vulkan-1.dll and the global functions first.
    if (volkGetInstanceVersion() == 0 && volkInitialize() != VK_SUCCESS)
    {
        if (err) *err = "no Vulkan driver (vulkan-1.dll could not be loaded)";
        return false;
    }
    std::vector<const char*> exts = extensions;
    std::vector<const char*> layers;
    const char* v = Hc09Env("HC09_VK_VALIDATION");
    validation = v && atoi(v) != 0;
    if (validation)
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        bool have = false;
        for (auto& p : props) have |= !strcmp(p.layerName, "VK_LAYER_KHRONOS_validation");
        if (have)
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        else
        {
            fprintf(stderr, "[vk] HC09_VK_VALIDATION=1 but VK_LAYER_KHRONOS_validation is not installed\n");
            validation = false;
        }
    }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "hc09";
    app.pEngineName = "hc09-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (!Check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance", err)) return false;
    volkLoadInstance(instance);
    if (validation)
    {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = OnValidation;
        mi.pUserData = this;
        if (create) create(instance, &mi, nullptr, &messenger);
    }
    return true;
}

bool Device::CreateDevice(VkSurfaceKHR surface, std::string* err)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    // HC09_GPU (hc09.ini [display] gpu): auto (a discrete GPU, else an integrated one), discrete, integrated, or
    // part of the device's name (any case). A choice that matches no device that can present falls back to auto.
    std::string want;
    if (const char* e = Hc09Env("HC09_GPU"))
        for (const char* c = e; *c; ++c) want += char(tolower((unsigned char)*c));
    auto wanted = [&](const VkPhysicalDeviceProperties& p)
    {
        if (want.empty() || want == "auto") return false;
        if (want == "discrete") return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (want == "integrated") return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        std::string name;
        for (const char* c = p.deviceName; *c; ++c) name += char(tolower((unsigned char)*c));
        return name.find(want) != std::string::npos;
    };
    int best = -1;
    uint32_t bestFamily = 0;
    int bestScore = -1;
    for (uint32_t i = 0; i < n; ++i)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> q(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, q.data());
        for (uint32_t f = 0; f < qn; ++f)
        {
            if ((q[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                continue;
            VkBool32 present = VK_TRUE;
            if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, surface, &present);
            if (!present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (wanted(p)) score += 10;
            if (score > bestScore) { bestScore = score; best = int(i); bestFamily = f; }
            break;
        }
    }
    if (best < 0) { if (err) *err = "no Vulkan device with a graphics+compute queue that can present"; return false; }
    physical = devs[best];
    queueFamily = bestFamily;
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    std::vector<const char*> exts;
    if (surface) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // Rendering features the backend uses: dynamic rendering (1.3), geometry
    // shaders (rectangle lists), depth clamp (clipping disabled by the game).
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    // Timeline semaphores (1.2): the native renderer's frames in flight.
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &f13;
    // EDRAM renderer (vk_edram): fragment shader interlock, stores from fragment
    // shaders, clip distances for the near/far planes.
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, avail.data());
        for (auto& e : avail)
            if (!strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
            {
                exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                hostImport = true;
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
                VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                p2.pNext = &hp;
                vkGetPhysicalDeviceProperties2(physical, &p2);
                hostImportAlignment = hp.minImportedHostPointerAlignment;
            }
        for (auto& e : avail)
            if (!strcmp(e.extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))
            {
                VkPhysicalDeviceFeatures2 q{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
                VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT qf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
                q.pNext = &qf;
                vkGetPhysicalDeviceFeatures2(physical, &q);
                if (qf.fragmentShaderPixelInterlock)
                {
                    fsi.fragmentShaderPixelInterlock = VK_TRUE;
                    exts.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
                }
            }
    }
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    if (fsi.fragmentShaderPixelInterlock) f13.pNext = &fsi;
    f2.features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    f2.features.shaderClipDistance = supported.shaderClipDistance;
    f2.features.shaderFloat64 = supported.shaderFloat64;   // exact divisions in the EDRAM arithmetic
    f2.features.geometryShader = supported.geometryShader;
    f2.features.depthClamp = supported.depthClamp;
    f2.features.independentBlend = supported.independentBlend;   // MRT targets blend differently
    f2.features.shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;   // hc09: the native renderer's anisotropic filtering
    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (!Check(vkCreateDevice(physical, &ci, nullptr, &device), "vkCreateDevice", err)) return false;
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    pixelInterlock = fsi.fragmentShaderPixelInterlock && supported.fragmentStoresAndAtomics && supported.shaderClipDistance &&
                     supported.shaderFloat64;
    anisotropy = supported.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
    VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = queueFamily;
    if (!Check(vkCreateCommandPool(device, &pi, nullptr, &pool), "vkCreateCommandPool", err)) return false;
    fprintf(stderr, "[vk] device: %s (driver %u.%u, Vulkan %u.%u.%u)%s\n", props.deviceName,
            VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
            validation ? ", validation on" : "");
    return true;
}

void Device::Destroy()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (messenger)
    {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    *this = Device{};
}

uint32_t Device::MemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

bool Device::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, Buffer& out)
{
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(device, &ci, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, flags);
    if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    out.size = size;
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    return true;
}

bool Device::ImportHostBuffer(void* ptr, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out, std::string* err)
{
    auto fail = [&](const char* what) { if (err) *err = what; DestroyBuffer(out); return false; };
    if (!hostImport) return fail("VK_EXT_external_memory_host not supported");
    if ((reinterpret_cast<uintptr_t>(ptr) | size) & (hostImportAlignment - 1)) return fail("host pointer/size not aligned for import");
    auto getProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
    VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (!getProps || getProps(device, type, ptr, &hp) != VK_SUCCESS || !hp.memoryTypeBits)
    {
        type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;   // e.g. a file mapping
        hp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        if (!getProps || getProps(device, type, ptr, &hp) != VK_SUCCESS || !hp.memoryTypeBits) return fail("host pointer not importable");
    }
    VkExternalMemoryBufferCreateInfo eb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    eb.handleTypes = type;
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.pNext = &eb;
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(device, &ci, nullptr, &out.buffer) != VK_SUCCESS) return fail("vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits, index = ~0u;
    for (VkMemoryPropertyFlags want : { VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                        VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), VkMemoryPropertyFlags(0) })
        if ((index = MemoryType(bits, want)) != ~0u) break;
    if (index == ~0u) return fail("no memory type for the imported pointer");
    VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = type;
    imp.pHostPointer = ptr;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = index;
    if (vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) return fail("vkAllocateMemory (import)");
    if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) return fail("vkBindBufferMemory (import)");
    out.size = size;
    out.mapped = ptr;
    return true;
}

void Device::DestroyBuffer(Buffer& b)
{
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer{};
}

bool Device::CreateImage2D(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out,
                           uint32_t mipLevels, VkImageAspectFlags aspect)
{
    return CreateImage(VK_IMAGE_VIEW_TYPE_2D, width, height, 1, format, usage, out, mipLevels, aspect);
}

bool Device::CreateImage(VkImageViewType viewType, uint32_t width, uint32_t height, uint32_t depthOrLayers, VkFormat format,
                         VkImageUsageFlags usage, Image& out, uint32_t mipLevels, VkImageAspectFlags aspect, VkFormat altViewFormat,
                         VkSampleCountFlagBits samples)
{
    bool is3D = viewType == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t layers = is3D ? 1 : depthOrLayers, depth = is3D ? depthOrLayers : 1;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    if (altViewFormat != VK_FORMAT_UNDEFINED) ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.format = format;
    ci.extent = { width, height, depth };
    ci.mipLevels = mipLevels;
    ci.arrayLayers = layers;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, out.image, out.memory, 0);
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
    {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out.image;
        vi.viewType = viewType;
        vi.format = format;
        vi.subresourceRange = { aspect, 0, mipLevels, 0, layers };
        if (vkCreateImageView(device, &vi, nullptr, &out.view) != VK_SUCCESS) return false;
        if (altViewFormat != VK_FORMAT_UNDEFINED)
        {
            vi.format = altViewFormat;
            if (vkCreateImageView(device, &vi, nullptr, &out.altView) != VK_SUCCESS) return false;
        }
    }
    out.format = format;
    out.mipLevels = mipLevels;
    out.layers = layers;
    out.depth = depth;
    out.width = width;
    out.height = height;
    return true;
}

void Device::DestroyImage(Image& img)
{
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.altView) vkDestroyImageView(device, img.altView, nullptr);
    for (VkImageView v : img.levelViews) vkDestroyImageView(device, v, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = Image{};
}

VkCommandBuffer Device::BeginOneShot()
{
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

bool Device::EndOneShot(VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    bool ok = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
    vkFreeCommandBuffers(device, pool, 1, &cb);
    return ok;
}

void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace hc09vk
