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
    }

    bool AllocateGuestMemory()
    {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
        if (!virtualAlloc2 || !mapViewOfFile3)
        {
            std::fprintf(stderr, "[memory] VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803+ required)\n");
            return false;
        }

        // Reserve the whole space as a placeholder, then split it into the ordinary region
        // below 0xA0000000 and one placeholder per physical view.
        auto* base = static_cast<uint8_t*>(virtualAlloc2(nullptr, nullptr, PPC_MEMORY_SIZE,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base)
            return false;
        // Splitting off the first two windows leaves the last one (which runs to the end of
        // the reservation) as its own placeholder already.
        for (uint64_t view : { kPhysicalViews[0], kPhysicalViews[1] })
        {
            if (!VirtualFree(base + view, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            {
                std::fprintf(stderr, "[memory] splitting placeholder at 0x%llX failed (%lu)\n", (unsigned long long)view, GetLastError());
                return false;
            }
        }

        // Ordinary guest memory: everything below the physical windows.
        if (!virtualAlloc2(nullptr, base, kPhysicalViews[0], MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                PAGE_READWRITE, nullptr, 0))
        {
            std::fprintf(stderr, "[memory] committing low guest memory failed (%lu)\n", GetLastError());
            return false;
        }

        // Physical memory: one section, mapped into each window.
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPhysicalSize), nullptr);
        if (!section)
            return false;
        // HC09_NO_E_WINDOW=1 leaves 0xE0000000 unmapped, so any use of it faults (debugging).
        const bool skipEWindow = std::getenv("HC09_NO_E_WINDOW") != nullptr;
        for (uint64_t view : kPhysicalViews)
        {
            if (skipEWindow && view == 0xE0000000)
                continue;
            if (!mapViewOfFile3(section, GetCurrentProcess(), base + view, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER,
                    PAGE_READWRITE, nullptr, 0))
            {
                std::fprintf(stderr, "[memory] mapping physical view at 0x%llX failed (%lu)\n", (unsigned long long)view, GetLastError());
                return false;
            }
        }
        g_base = base;
        return true;
    }
}
