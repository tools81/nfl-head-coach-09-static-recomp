// Guest address space: 4 GiB of host memory where guest address X lives at g_base + X.
//
// The Xbox 360 exposes its 512 MiB of physical memory through three virtual windows:
// 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages) and 0xE0000000 (4 KiB pages).
// Code freely converts between them (Direct3D, for example, reads shader microcode through
// the 0xC0000000 view of memory allocated in the 0xA0000000 view), so the three ranges
// must alias the same memory. They are mapped as three views of one pagefile-backed
// section. On hardware the 0xE0000000 window is offset by 4 KiB; host views must be
// 64 KiB-aligned, so that offset is not reproduced (allocations always return 0xA0000000
// addresses, so this only matters for code that builds 0xE0000000 addresses itself).

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "memory.h"

namespace hc09
{
    namespace
    {
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

        constexpr uint64_t kPhysicalSize = 0x20000000;
        constexpr uint64_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };
        HANDLE g_physicalSection = nullptr;
    }

    uint8_t* PhysicalAlias()
    {
        static uint8_t* const alias = [] {
            void* view = g_physicalSection ? MapViewOfFile(g_physicalSection, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize) : nullptr;
            if (!view)
                std::fprintf(stderr, "[memory] cannot map another view of physical memory (%lu)\n", GetLastError());
            return static_cast<uint8_t*>(view);
        }();
        return alias;
    }

    namespace
    {
        // The same layout without placeholders, for Wine versions whose placeholder support falls short: a free
        // 4 GiB range is found by reserving it and letting it go, then taken by the low memory and the three views
        // at fixed addresses (retried if anything else takes part of it in between).
        bool AllocateWithoutPlaceholders(bool skipEWindow)
        {
            HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPhysicalSize), nullptr);
            if (!section)
                return false;
            for (int attempt = 0; attempt < 16; attempt++)
            {
                auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS));
                if (!base)
                    break;
                VirtualFree(base, 0, MEM_RELEASE);
                if (!VirtualAlloc(base, kPhysicalViews[0], MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                    continue;
                void* views[3] = {};
                bool ok = true;
                for (int i = 0; i < 3 && ok; i++)
                {
                    if (skipEWindow && kPhysicalViews[i] == 0xE0000000)
                        continue;
                    views[i] = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, base + kPhysicalViews[i]);
                    ok = views[i] != nullptr;
                }
                if (ok)
                {
                    g_physicalSection = section;
                    g_base = base;
                    std::fprintf(stderr, "[memory] guest memory mapped without placeholders\n");
                    return true;
                }
                for (void* view : views)
                    if (view)
                        UnmapViewOfFile(view);
                VirtualFree(base, 0, MEM_RELEASE);
            }
            std::fprintf(stderr, "[memory] mapping guest memory without placeholders failed (%lu)\n", GetLastError());
            CloseHandle(section);
            return false;
        }
    }

    bool AllocateGuestMemory()
    {
        // HC09_NO_E_WINDOW=1 leaves 0xE0000000 unmapped, so any use of it faults (debugging).
        const bool skipEWindow = std::getenv("HC09_NO_E_WINDOW") != nullptr;
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
        if (!virtualAlloc2 || !mapViewOfFile3 || std::getenv("HC09_NO_PLACEHOLDERS"))
            return AllocateWithoutPlaceholders(skipEWindow);

        // Reserve the whole space as a placeholder, then split it into the ordinary region
        // below 0xA0000000 and one placeholder per physical view.
        auto* base = static_cast<uint8_t*>(virtualAlloc2(nullptr, nullptr, PPC_MEMORY_SIZE,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base)
        {
            std::fprintf(stderr, "[memory] reserving a placeholder failed (%lu)\n", GetLastError());
            return AllocateWithoutPlaceholders(skipEWindow);
        }
        // Splitting off the first two windows leaves the last one (which runs to the end of
        // the reservation) as its own placeholder already.
        for (uint64_t view : { kPhysicalViews[0], kPhysicalViews[1] })
        {
            if (!VirtualFree(base + view, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            {
                std::fprintf(stderr, "[memory] splitting placeholder at 0x%llX failed (%lu)\n", (unsigned long long)view, GetLastError());
                return AllocateWithoutPlaceholders(skipEWindow);
            }
        }

        // Ordinary guest memory: everything below the physical windows.
        if (!virtualAlloc2(nullptr, base, kPhysicalViews[0], MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                PAGE_READWRITE, nullptr, 0))
        {
            std::fprintf(stderr, "[memory] committing low guest memory failed (%lu)\n", GetLastError());
            return AllocateWithoutPlaceholders(skipEWindow);
        }

        // Physical memory: one section, mapped into each window.
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPhysicalSize), nullptr);
        if (!section)
            return false;
        for (uint64_t view : kPhysicalViews)
        {
            if (skipEWindow && view == 0xE0000000)
                continue;
            if (!mapViewOfFile3(section, GetCurrentProcess(), base + view, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER,
                    PAGE_READWRITE, nullptr, 0))
            {
                std::fprintf(stderr, "[memory] mapping physical view at 0x%llX failed (%lu)\n", (unsigned long long)view, GetLastError());
                CloseHandle(section);
                return AllocateWithoutPlaceholders(skipEWindow);
            }
        }
        g_physicalSection = section;
        g_base = base;
        return true;
    }
}
