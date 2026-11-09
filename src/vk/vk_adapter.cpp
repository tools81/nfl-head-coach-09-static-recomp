// Adapter (this project): the runtime services the native Vulkan renderer from NFL Head Coach 09 for PC expects
// (its runtime.cpp, gpu.cpp, kernel_vars.cpp and imports_threads.cpp), implemented over this project's runtime,
// and the choice between it and the D3D12 renderer (vk_select.h). The page write tracking and the resolve access
// watch below are its runtime.cpp's (MIT, see LICENSE), adapted to this project's views of physical memory.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <realtimeapiset.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime.h"
#include "kernel.h"
#include "gpu.h"
#include "native_live.h"
#include "xenos/xenos.h"
#include "vk_select.h"
#include "volk.h"

#include "gpu/command_processor.h"
#include "kernel/heap.h"
#include "memory.h"
#include "kernel/threads.h"

namespace hc09
{
    uint8_t* PhysicalAlias();  // memory.cpp
}

void GpuLiveInit(uint8_t* physicalMemory);  // gpu_live.cpp

namespace hc09::vk
{
    void InstallReplacements();  // vk_replacements.cpp
}

// ---- Which renderer --------------------------------------------------------------------------------------------

namespace hc09::vk
{
    namespace
    {
        // Whether this PC can run the renderer: a Vulkan driver with a device for Vulkan 1.3.
        bool VulkanUsable()
        {
            if (volkInitialize() != VK_SUCCESS)
                return false;
            VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            info.pApplicationInfo = &app;
            VkInstance instance = VK_NULL_HANDLE;
            if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
                return false;
            volkLoadInstanceOnly(instance);
            uint32_t count = 0;
            vkEnumeratePhysicalDevices(instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(instance, &count, devices.data());
            bool usable = false;
            for (VkPhysicalDevice device : devices)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                usable |= properties.apiVersion >= VK_API_VERSION_1_3;
            }
            vkDestroyInstance(instance, nullptr);
            return usable;
        }
    }

    bool Selected()
    {
        // Decided once: Configure then sets HC09_RENDERER to the Vulkan renderer's own name for it.
        static const bool selected = [] {
            std::string choice;
            if (const char* value = std::getenv("HC09_RENDERER"))
                choice = value;
            else
            {
                wchar_t text[32] = {};
                const std::wstring file = (std::filesystem::current_path() / L"hc09.ini").wstring();
                GetPrivateProfileStringW(L"Graphics", L"Renderer", L"", text, DWORD(std::size(text)), file.c_str());
                for (const wchar_t* c = text; *c; c++)
                    choice.push_back(char(*c));
            }
            std::transform(choice.begin(), choice.end(), choice.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
            // The default (no setting, or one this does not know) is Vulkan; Renderer=d3d12 picks the D3D12 renderer.
            if (choice == "d3d12" || choice == "d3d" || choice == "directx")
                return false;
            if (!VulkanUsable())
            {
                std::fprintf(stderr, "[vk] the Vulkan renderer needs a graphics driver with Vulkan 1.3: using the D3D12 renderer\n");
                return false;
            }
            return true;
        }();
        return selected;
    }

    void Configure()
    {
        if (!Selected())
            return;
        // The renderer's switches (gpu_live.cpp): the live renderer on, its native (D3D-level) renderer.
        _putenv_s("HC09_GPU_LIVE", "1");
        _putenv_s("HC09_RENDERER", "native");
        // Resolves are written to guest memory: with them kept on the GPU (HC09_NATIVE_RESOLVE_GPU=1, the
        // original's default) game threads wait seconds on each other during loads here.
        if (!std::getenv("HC09_NATIVE_RESOLVE_GPU"))
            _putenv_s("HC09_NATIVE_RESOLVE_GPU", "0");
        // Its pipeline caches, beside the D3D12 renderer's shader cache (the folder must exist).
        if (!std::getenv("HC09_CACHE_DIR"))
        {
            std::error_code error;
            std::filesystem::create_directories("build/shadercache/vulkan", error);
            _putenv_s("HC09_CACHE_DIR", "build/shadercache/vulkan");
        }
        // hc09.ini [Graphics] settings of the Vulkan renderer, as the switches it reads (an environment variable
        // already set wins): FrameRate 35 (the game's own, the default) .. 240; Scale 1 (720p) .. 4, 2 = 1440p;
        // VSync on/off; Fullscreen on/off (Alt+Enter or F11 toggles it); MSAA 1 (as the game asks), 2 (at most
        // 2x) or 0; GPU discrete, integrated or part of the card's name; Anisotropy 16 (the default) .. 2, or off;
        // AmbientOcclusion and Reflections: strength (1 the default, 0 off); Upscaling fsr (the default) or bilinear;
        // Sharpening 0 (off) .. 100 (75 the default); SmoothMotion on (the default) or off; HDTextures on or off (the
        // default; on: collect the game's textures, HC09_DUMP_HASHED, for tools/hdpack); Bloom, ColorGrading and
        // DepthOfField: strength (1 the default, DepthOfField 0; 0 off); HDR on or off (the default), HDRPeak and
        // HDRPaperWhite in nits (1000, 200); Crowd on (the default) or off; Turf and Players: strength (1 the default, 0 off);
        // AspectRatio (below).
        static const std::pair<const wchar_t*, const char*> kSettings[] = {
            { L"FrameRate", "HC09_FRAMERATE" }, { L"Scale", "HC09_SCALE" },       { L"VSync", "HC09_VSYNC" },
            { L"Fullscreen", "HC09_FULLSCREEN" }, { L"MSAA", "HC09_NATIVE_MSAA" }, { L"GPU", "HC09_GPU" },
            { L"Anisotropy", "HC09_ANISOTROPY" }, { L"AmbientOcclusion", "HC09_SSAO" }, { L"Reflections", "HC09_SSR" },
            { L"Upscaling", "HC09_UPSCALE" }, { L"Sharpening", "HC09_SHARPEN" }, { L"SmoothMotion", "HC09_SMOOTH_MOTION" },
            { L"HDTextures", "HC09_HD_TEXTURES" }, { L"Bloom", "HC09_BLOOM" }, { L"ColorGrading", "HC09_GRADE" },
            { L"DepthOfField", "HC09_DOF" }, { L"HDR", "HC09_HDR" }, { L"HDRPeak", "HC09_HDR_PEAK" },
            { L"HDRPaperWhite", "HC09_HDR_PAPER_WHITE" }, { L"Crowd", "HC09_CROWD" }, { L"Turf", "HC09_TURF" },
            { L"Players", "HC09_PLAYERS" },
        };
        const std::wstring file = (std::filesystem::current_path() / L"hc09.ini").wstring();
        for (const auto& [key, variable] : kSettings)
        {
            wchar_t text[64] = {};
            if (std::getenv(variable) || !GetPrivateProfileStringW(L"Graphics", key, L"", text, DWORD(std::size(text)), file.c_str()))
                continue;
            std::string value;
            for (const wchar_t* c = text; *c; c++)
                value.push_back(char(std::tolower(*c & 0x7F)));
            if (value == "auto")
                continue;
            if (value == "off" || value == "no" || value == "false")
                value = "0";
            else if (value == "on" || value == "yes" || value == "true")
                value = "1";
            _putenv_s(variable, value.c_str());
        }
        if (const char* hd = std::getenv("HC09_HD_TEXTURES"); hd && *hd && strcmp(hd, "0") && !std::getenv("HC09_DUMP_HASHED"))
            _putenv_s("HC09_DUMP_HASHED", "1");
        // AspectRatio: 16:9 (the game's own, the default), 21:9, 32:9, <w>:<h>, a number, or auto (the screen's shape)
        // -> HC09_ASPECT, the picture's width / height (widescreen.h).
        if (!std::getenv("HC09_ASPECT"))
        {
            wchar_t text[32] = {};
            GetPrivateProfileStringW(L"Graphics", L"AspectRatio", L"", text, DWORD(std::size(text)), file.c_str());
            double aspect = 0.0, w = 0.0, h = 0.0;
            if (!_wcsicmp(text, L"auto"))
            {
                const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                aspect = sh > 0 ? double(sw) / double(sh) : 0.0;
            }
            else if (swscanf(text, L"%lf:%lf", &w, &h) == 2 && h > 0.0)
                aspect = w / h;
            else
                aspect = std::wcstod(text, nullptr);
            if (aspect > 16.0 / 9.0 + 0.01)
            {
                const std::string value = std::to_string(aspect);
                _putenv_s("HC09_ASPECT", value.c_str());
            }
        }
        std::fprintf(stderr, "[vk] renderer: Vulkan (native)\n");
    }

    void ReserveMemory()
    {
        if (!Selected())
            return;
        // gpu_live.cpp copies each frame's dynamic draw data into physical 0x01000000-0x01FFFFFF, which the
        // original's memory layout never hands out; ours allocates physical memory upward from 0, so without
        // this the copies overwrite the game's own data.
        if (!PhysicalHeap().AllocateAt(kPhysicalHeapBase + 0x01000000, 0x01000000))
            std::fprintf(stderr, "[vk] physical 0x01000000-0x01FFFFFF is not free: draw-data snapshots will corrupt game memory\n");
    }

    namespace
    {
        LONG CALLBACK OnException(EXCEPTION_POINTERS* info);
    }

    void Start()
    {
        static bool started = false;
        if (!Selected() || started)
            return;
        started = true;
        uint8_t* physical = PhysicalAlias();
        if (!physical)
        {
            std::fprintf(stderr, "[vk] no view of physical memory for the renderer: the game runs without graphics\n");
            return;
        }
        // Before the guest fault reports (registered first, called last).
        AddVectoredExceptionHandler(1, OnException);
        InstallReplacements();
        GpuLiveInit(physical);
    }
}

// ---- Registers, physical memory, frames ------------------------------------------------------------------------

namespace xenos
{
    uint32_t HostReg(uint32_t index) { return hc09::gpu::ReadRegister(index); }

    const uint32_t* HostRegFile(bool* bigEndian)
    {
        *bigEndian = true;  // the register file is the guest's MMIO window, stored big-endian
        return reinterpret_cast<const uint32_t*>(g_base + hc09::gpu::kRegisterBase);
    }

    const uint8_t* HostPhys(uint32_t physicalAddress) { return hc09::PhysicalAlias() + (physicalAddress & 0x1FFFFFFF); }
    uint64_t HostSwapCount() { return hc09::gpu::SwapCount(); }
    void HostWatchWrites(uint32_t, uint32_t) {}  // diagnostics (HC09_WATCH_VFETCH): not here
}

uint64_t GpuSwapCount() { return hc09::gpu::SwapCount(); }

void GpuCpIdle(double* ringEmptySeconds, double* memWaitSeconds)
{
    *ringEmptySeconds = 0;  // HC09_GPU_STATS reports this project's command processor's idle time instead
    *memWaitSeconds = 0;
}

void GpuRedirectFenceWord(uint32_t pa, void (*sink)(uint32_t value)) { hc09::gpu::RedirectFenceWord(pa, sink); }
void GpuStoreFenceWord(uint32_t pa, uint32_t value) { hc09::gpu::StoreFenceWord(pa, value); }

void GpuCopyRegisters(uint32_t* out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        out[i] = hc09::gpu::ReadRegister(i);
}

// The emulated renderer's register log (it renders from the command stream): the native renderer does not use it.
void GpuLogRegisterWrites(bool) {}
void GpuTakeRegisterWrites(std::vector<std::pair<uint32_t, uint32_t>>& out) { out.clear(); }

// ---- Kernel and host threads -----------------------------------------------------------------------------------

uint64_t KernelCurrentTid() { return hc09::CurrentThreadId(); }

uint64_t KernelInterruptTimeNs()
{
    // The time base of the game's tick count (KeTimeStampBundle, GetTickCount64), in nanoseconds: the interrupt
    // time (kernelbase.dll's, so no OneCore import library is needed).
    using QueryInterruptTimePreciseFn = VOID(WINAPI*)(PULONGLONG);
    static const auto query = reinterpret_cast<QueryInterruptTimePreciseFn>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryInterruptTimePrecise"));
    ULONGLONG time = 0;
    if (query)
        query(&time);
    else
        time = GetTickCount64() * 10000;
    return uint64_t(time) * 100;
}

void HostSetThreadName(void* thread, const char* name)
{
    std::wstring wide(name, name + std::strlen(name));
    SetThreadDescription(thread ? HANDLE(thread) : GetCurrentThread(), wide.c_str());
}

// glslang's release package is built with a newer Visual C++ library, whose vectorized find_first_of it calls. This
// stands in for it only when the library being linked lacks it: newer ones define it, and a second definition fails
// the link.
extern "C" size_t __stdcall hc09_find_first_of_trivial_pos_1(const void* first1, size_t count1, const void* first2, size_t count2) noexcept
{
    const auto* haystack = static_cast<const uint8_t*>(first1);
    const auto* needles = static_cast<const uint8_t*>(first2);
    for (size_t i = 0; i < count1; i++)
        if (std::memchr(needles, haystack[i], count2))
            return i;
    return size_t(-1);
}
#pragma comment(linker, "/alternatename:__std_find_first_of_trivial_pos_1=hc09_find_first_of_trivial_pos_1")

// ---- Page write tracking for decoded-data caches (xenos::HostTrackWrites) --------------------------------------
// The live renderer's texture cache: a decoded texture's pages are made read-only in the guest's views of physical
// memory (the renderer's alias, PhysicalAlias, stays writable). The first CPU write to such a page faults; the
// handler stamps the page with a new write epoch and makes it writable again (the store re-executes). A cache
// entry is stale once one of its pages carries an epoch newer than the one returned when it was decoded. File
// reads into guest memory go through a host buffer (xboxkrnl_io.cpp), so the OS never writes a protected page.

namespace
{
    constexpr uint32_t kTrackPages = 0x20000000u >> 12;
    constexpr uint8_t kPageProtected = 1, kPageTracked = 2;  // tracked: ever protected by HostTrackWrites
    std::atomic<uint64_t> g_trackEpoch{ 1 };
    std::atomic<uint64_t>* g_pageEpoch = nullptr;  // per 4 KiB physical page: epoch of its last tracked write
    std::atomic<uint8_t>* g_pageState = nullptr;
    std::mutex g_trackMu;

    // The guest's views of physical memory (memory.cpp; 0xE0000000 is left unmapped by HC09_NO_E_WINDOW).
    const std::vector<uint32_t>& WatchViews()
    {
        static const std::vector<uint32_t> views = [] {
            std::vector<uint32_t> list{ 0xA0000000u, 0xC0000000u };
            if (!std::getenv("HC09_NO_E_WINDOW"))
                list.push_back(0xE0000000u);
            return list;
        }();
        return views;
    }

    bool InPhysicalView(uintptr_t faultAddr, uint32_t& page)
    {
        const uintptr_t gb = uintptr_t(g_base);
        if (faultAddr < gb + 0xA0000000ull || faultAddr >= gb + 0x100000000ull)
            return false;
        page = (uint32_t(faultAddr - gb) & 0x1FFFFFFFu) >> 12;
        return true;
    }

    void ProtectGuest(uint32_t va, uint32_t bytes, DWORD mode)
    {
        DWORD old;
        VirtualProtect(g_base + va, bytes, mode, &old);
    }

    void TrackUnprotect(uint32_t page)
    {
        g_pageEpoch[page].store(g_trackEpoch.fetch_add(1) + 1, std::memory_order_release);
        for (uint32_t v : WatchViews())
            ProtectGuest(v + (page << 12), 0x1000, PAGE_READWRITE);
    }

    // Fault handler side (no locks): a write to a tracked page.
    bool TrackHandleFault(uintptr_t faultAddr)
    {
        uint32_t page;
        if (!g_pageState || !InPhysicalView(faultAddr, page))
            return false;
        const uint8_t s = g_pageState[page].load(std::memory_order_acquire);
        if (!(s & kPageTracked))
            return false;
        // Only the thread that clears the flag unprotects; others retry until it has.
        if (g_pageState[page].fetch_and(uint8_t(~kPageProtected)) & kPageProtected)
            TrackUnprotect(page);
        return true;
    }
}

namespace xenos
{
    uint64_t HostTrackWrites(uint32_t pa, uint32_t bytes)
    {
        std::lock_guard<std::mutex> lk(g_trackMu);
        if (!g_pageState)
        {
            g_pageEpoch = new std::atomic<uint64_t>[kTrackPages]();
            g_pageState = new std::atomic<uint8_t>[kTrackPages]();
        }
        const uint64_t epoch = g_trackEpoch.load();
        pa &= 0x1FFFFFFFu;
        if (!bytes)
            return epoch;
        const uint32_t first = pa >> 12, last = std::min(kTrackPages - 1, (pa + bytes - 1) >> 12);
        // Flags first, then the protection: a write in between does not fault and lands before the caller decodes
        // the range.
        for (uint32_t page = first; page <= last;)
        {
            if (g_pageState[page].load() & kPageProtected)
            {
                ++page;
                continue;
            }
            uint32_t run = page;
            while (run <= last && !(g_pageState[run].load() & kPageProtected))
            {
                g_pageState[run].fetch_or(kPageProtected | kPageTracked);
                ++run;
            }
            for (uint32_t v : WatchViews())
                ProtectGuest(v + (page << 12), (run - page) << 12, PAGE_READONLY);
            page = run;
        }
        return epoch;
    }

    bool HostWrittenSince(uint32_t pa, uint32_t bytes, uint64_t& epoch)
    {
        if (!g_pageEpoch || !bytes)
            return false;
        // Global epoch first: a write after this load gets a newer epoch than `now`.
        const uint64_t now = g_trackEpoch.load(std::memory_order_acquire);
        if (now <= epoch)
            return false;  // no tracked write anywhere since
        pa &= 0x1FFFFFFFu;
        const uint32_t first = pa >> 12, last = std::min(kTrackPages - 1, (pa + bytes - 1) >> 12);
        for (uint32_t page = first; page <= last; ++page)
            if (g_pageEpoch[page].load(std::memory_order_acquire) > epoch)
                return true;
        epoch = now;  // nothing of this range was written up to `now`
        return false;
    }
}

// ---- CPU accesses to resolved memory the native renderer keeps on the GPU --------------------------------------
// vk_native keeps resolved data in device memory; guest memory has it only once written back. gpu_live.cpp calls
// GuestWatchAccess for each range the GPU takes: its pages get no access in the guest's views. The game's first
// read or write of such a page faults; the handler calls the hook (GuestSetAccessHook: it blocks until the renderer
// thread has written the range back), restores the page (read-only again where write tracking wants it), and the
// access proceeds on the newest data, as on the console.

namespace
{
    std::atomic<uint8_t>* g_accessWatch = nullptr;  // per 4 KiB physical page: 1 armed, 2 was armed
    std::mutex g_accessMu;                          // arming vs restoring a page
    void (*g_accessHook)(uint32_t pa) = nullptr;

    void AccessProtect(uint32_t page, bool none)
    {
        const bool readOnly = g_pageState && (g_pageState[page].load() & kPageProtected);
        const DWORD mode = none ? PAGE_NOACCESS : readOnly ? PAGE_READONLY : PAGE_READWRITE;
        for (uint32_t v : WatchViews())
            ProtectGuest(v + (page << 12), 0x1000, mode);
    }

    // kind: 0 read, 1 write, 2 unknown.
    bool AccessWatchHandleFault(uintptr_t faultAddr, uint32_t kind)
    {
        uint32_t page;
        if (!g_accessWatch || !InPhysicalView(faultAddr, page))
            return false;
        uint8_t state = 1;
        if (!g_accessWatch[page].compare_exchange_strong(state, 2))
        {
            // Not armed. Another thread is handling this page: retry until it has restored it. A write to a page
            // write tracking protects is that handler's.
            if (state != 2 || kind == 1)
                return false;
            if (kind == 2 && g_pageState && (g_pageState[page].load() & kPageProtected))
                return false;
            return true;
        }
        if (g_accessHook)
            g_accessHook(page << 12);  // the newest data into guest memory
        std::lock_guard<std::mutex> lk(g_accessMu);
        if (g_accessWatch[page].load() == 2)
            AccessProtect(page, false);  // re-armed meanwhile: it faults again
        return true;
    }
}

void GuestSetAccessHook(void (*hook)(uint32_t pa), bool) { g_accessHook = hook; }

void GuestWatchAccess(uint32_t pa, uint32_t bytes)
{
    std::lock_guard<std::mutex> lk(g_accessMu);
    if (!g_accessWatch)
        g_accessWatch = new std::atomic<uint8_t>[kTrackPages]();
    pa &= 0x1FFFFFFFu;
    for (uint32_t page = pa >> 12; page <= (pa + bytes - 1) >> 12 && page < kTrackPages; ++page)
        if (g_accessWatch[page].exchange(1) != 1)
            AccessProtect(page, true);
}

namespace hc09::vk
{
    namespace
    {
        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            const EXCEPTION_RECORD* record = info->ExceptionRecord;
            if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
                return EXCEPTION_CONTINUE_SEARCH;
            const ULONG_PTR operation = record->ExceptionInformation[0];  // 0 read, 1 write, 8 execute
            const uintptr_t address = record->ExceptionInformation[1];
            const uint32_t kind = operation == 0 ? 0 : operation == 1 ? 1 : 2;
            if (AccessWatchHandleFault(address, kind))
                return EXCEPTION_CONTINUE_EXECUTION;
            if (operation == 1 && TrackHandleFault(address))
                return EXCEPTION_CONTINUE_EXECUTION;
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
}
