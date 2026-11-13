// hc09: temporal upscaling with FSR 3.1; see vk_fsr3.h.

#include "vk_fsr3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../dev_env.h"

#ifdef HC09_HAVE_FFX
#include <windows.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace hc09vk {

int TemporalUpscaler::Mode()
{
    static const int mode = [] {
        const char* e = Hc09Env("HC09_TEMPORAL_UPSCALING");
        if (!e || !*e) return 0;
        const std::string v = e;
        if (v == "quality" || v == "1") return 1;
        if (v == "balanced" || v == "2") return 2;
        if (v == "performance" || v == "3") return 3;
        return 0;
    }();
    return mode;
}

float TemporalUpscaler::Ratio(int mode) { return mode == 1 ? 1.5f : mode == 2 ? 1.7f : mode == 3 ? 2.0f : 1.0f; }

namespace {
    // The Halton sequence's value at `index` (from 1) in `base`: FSR's jitter pattern.
    float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f, r = 0.0f;
        for (uint32_t i = index; i > 0; i /= base)
        {
            f /= float(base);
            r += f * float(i % base);
        }
        return r;
    }
}

void TemporalUpscaler::Jitter(uint64_t frame, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y)
{
    // As FSR's ffxFsr3GetJitterPhaseCount: 8 times the square of the ratio, so each output pixel is covered.
    const float ratio = renderWidth ? float(outputWidth) / float(renderWidth) : 1.0f;
    const uint32_t phases = std::max<uint32_t>(1, uint32_t(8.0f * ratio * ratio));
    const uint32_t i = uint32_t(frame % phases) + 1;
    x = Halton(i, 2) - 0.5f;
    y = Halton(i, 3) - 0.5f;
}

#ifdef HC09_HAVE_FFX
namespace {
    struct Api
    {
        PfnFfxCreateContext create = nullptr;
        PfnFfxDestroyContext destroy = nullptr;
        PfnFfxDispatch dispatch = nullptr;
        bool ok = false;
    };

    // The DLL from the game's own folder (loaded once).
    const Api& Load()
    {
        static const Api api = [] {
            Api a;
            wchar_t path[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring dll(path, n);
            dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_vk.dll";
            HMODULE m = LoadLibraryW(dll.c_str());
            if (!m)
            {
                std::fprintf(stderr, "[fsr3] amd_fidelityfx_vk.dll could not be loaded: no temporal upscaling\n");
                return a;
            }
            a.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
            a.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
            a.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
            a.ok = a.create && a.destroy && a.dispatch;
            return a;
        }();
        return api;
    }

    // FSR's device function lookup: the device's, but for what it asks for that our device does not have: the core
    // function for a KHR name (core since Vulkan 1.1/1.2), and no-ops for debug names and labels and AMD's markers.
    VkResult VKAPI_CALL NoName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
    void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
    void VKAPI_CALL NoLabelEnd(VkCommandBuffer) {}
    void VKAPI_CALL NoMarker(VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer, VkDeviceSize, uint32_t) {}
    void VKAPI_CALL NoMarker2(VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize, uint32_t) {}
    VkResult VKAPI_CALL NoHdr(VkDevice, uint32_t, const VkSwapchainKHR*, const VkHdrMetadataEXT*) { return VK_SUCCESS; }

    PFN_vkVoidFunction VKAPI_CALL DeviceProcAddr(VkDevice device, const char* name)
    {
        if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name)) return f;
        std::string n = name;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, "KHR") == 0)
            if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, n.substr(0, n.size() - 3).c_str())) return f;
        if (n == "vkSetDebugUtilsObjectNameEXT") return reinterpret_cast<PFN_vkVoidFunction>(NoName);
        if (n == "vkCmdBeginDebugUtilsLabelEXT") return reinterpret_cast<PFN_vkVoidFunction>(NoLabel);
        if (n == "vkCmdEndDebugUtilsLabelEXT") return reinterpret_cast<PFN_vkVoidFunction>(NoLabelEnd);
        if (n == "vkCmdWriteBufferMarkerAMD") return reinterpret_cast<PFN_vkVoidFunction>(NoMarker);
        if (n == "vkCmdWriteBufferMarker2AMD") return reinterpret_cast<PFN_vkVoidFunction>(NoMarker2);
        if (n == "vkSetHdrMetadataEXT") return reinterpret_cast<PFN_vkVoidFunction>(NoHdr);
        static int missing = 0;
        if (missing++ < 10) std::fprintf(stderr, "[fsr3] %s is not on this device\n", name);
        return nullptr;
    }

    void Message(uint32_t type, const wchar_t* message)
    {
        std::fprintf(stderr, "[fsr3] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
    }

    FfxApiResource Resource(const Image& img, uint32_t format, uint32_t usage)
    {
        FfxApiResourceDescription d{};
        d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        d.format = format;
        d.width = img.width;
        d.height = img.height;
        d.depth = 1;
        d.mipCount = 1;
        d.flags = FFX_API_RESOURCE_FLAGS_NONE;
        d.usage = usage;
        return ffxApiGetResourceVK(reinterpret_cast<void*>(img.image), d, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

bool TemporalUpscaler::Create(Device& dev, uint32_t maxRenderWidth, uint32_t maxRenderHeight, uint32_t outputWidth, uint32_t outputHeight)
{
    Destroy();
    const Api& api = Load();
    if (!api.ok) return false;
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = dev.device;
    backend.vkPhysicalDevice = dev.physical;
    backend.vkDeviceProcAddr = DeviceProcAddr;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = { maxRenderWidth, maxRenderHeight };
    desc.maxUpscaleSize = { outputWidth, outputHeight };
    desc.fpMessage = Message;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t r = api.create(&ctx, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        std::fprintf(stderr, "[fsr3] the upscaler could not be made (%u): no temporal upscaling\n", r);
        return false;
    }
    context_ = ctx;
    outW_ = outputWidth, outH_ = outputHeight;
    std::fprintf(stderr, "[fsr3] FSR 3.1: scenes up to %ux%u upscaled to %ux%u\n", maxRenderWidth, maxRenderHeight, outputWidth, outputHeight);
    return true;
}

void TemporalUpscaler::Destroy()
{
    if (!context_) return;
    ffxContext ctx = context_;
    Load().destroy(&ctx, nullptr);
    context_ = nullptr;
}

bool TemporalUpscaler::Dispatch(VkCommandBuffer cb, const Frame& f)
{
    if (!context_ || !f.color || !f.depth || !f.motion || !f.output) return false;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = reinterpret_cast<void*>(cb);
    d.color = Resource(*f.color, ffxApiGetSurfaceFormatVK(f.color->format), FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = Resource(*f.depth, ffxApiGetSurfaceFormatVK(f.depth->format), FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = Resource(*f.motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = Resource(*f.output, ffxApiGetSurfaceFormatVK(f.output->format), FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = { f.jitterX, f.jitterY };
    d.motionVectorScale = { 1.0f, 1.0f };
    d.renderSize = { f.renderWidth, f.renderHeight };
    d.upscaleSize = { std::min(outW_, f.output->width), std::min(outH_, f.output->height) };
    d.enableSharpening = false;   // the picture's own sharpening (RCAS) follows at the present
    d.frameTimeDelta = f.frameMs;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.depthInverted ? f.farZ : f.nearZ;
    d.cameraFar = f.depthInverted ? f.nearZ : f.farZ;
    d.cameraFovAngleVertical = f.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;
    ffxContext ctx = context_;
    const ffxReturnCode_t r = Load().dispatch(&ctx, &d.header);
    if (r != FFX_API_RETURN_OK)
    {
        static int n = 0;
        if (n++ < 5) std::fprintf(stderr, "[fsr3] dispatch failed (%u)\n", r);
        return false;
    }
    return true;
}
#else
bool TemporalUpscaler::Create(Device&, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
void TemporalUpscaler::Destroy() {}
bool TemporalUpscaler::Dispatch(VkCommandBuffer, const Frame&) { return false; }
#endif

}  // namespace hc09vk
