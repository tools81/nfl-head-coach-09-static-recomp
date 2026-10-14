// Imported kernel data variables (XboxHardwareInfo, KeTimeStampBundle, ...) and the
// import-table slots that point at them.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "kernel/guest.h"
#include "kernel/heap.h"
#include "kernel/variables.h"
#include "kernel/xex.h"

namespace hc09
{
    uint64_t SystemTime();

    namespace
    {
        uint32_t g_timeStampBundle = 0;

        uint32_t AllocateZeroed(uint32_t size)
        {
            return PoolHeap().Allocate(size, 0x10);
        }

        uint32_t CreateVariable(const char* name)
        {
            const std::string_view n(name);
            if (n == "XboxHardwareInfo")
            {
                // { be32 flags; u8 cpu_count; u8 pad[3]; ... }
                const uint32_t address = AllocateZeroed(0x10);
                Store<uint8_t>(address + 4, 6);
                return address;
            }
            if (n == "XboxKrnlVersion")
            {
                // { be16 major, minor, build, qfe }
                const uint32_t address = AllocateZeroed(8);
                Store<uint16_t>(address + 0, 2);
                Store<uint16_t>(address + 2, 0);
                Store<uint16_t>(address + 4, 6534);
                Store<uint16_t>(address + 6, 0);
                return address;
            }
            if (n == "XexExecutableModuleHandle")
            {
                // A pointer to the module's LDR_DATA_TABLE_ENTRY
                const uint32_t address = AllocateZeroed(4);
                Store<uint32_t>(address, XexModuleHandle());
                return address;
            }
            if (n == "ExLoadedCommandLine")
            {
                const uint32_t address = AllocateZeroed(0x400);
                std::strcpy(reinterpret_cast<char*>(GuestToHost(address)), "\"default.xex\"");
                return address;
            }
            if (n == "KeTimeStampBundle")
            {
                // { be64 interrupt_time; be64 system_time; be32 tick_count; be32 padding; }
                g_timeStampBundle = AllocateZeroed(0x18);
                return g_timeStampBundle;
            }
            if (n == "VdGpuClockInMHz")
            {
                const uint32_t address = AllocateZeroed(4);
                Store<uint32_t>(address, 500);
                return address;
            }
            if (n == "VdHSIOCalibrationLock")
            {
                // An initialized, unowned RTL_CRITICAL_SECTION: lock_count must start at -1.
                const uint32_t address = AllocateZeroed(0x1C);
                Store<uint8_t>(address, 1);
                Store<int32_t>(address + 0x10, -1);
                return address;
            }
            // Object type descriptors and monitor/device blocks: identity matters, contents don't.
            return AllocateZeroed(0x40);
        }

        void UpdateTimeStampBundle()
        {
            const uint64_t ticks = GetTickCount64();
            Store<uint64_t>(g_timeStampBundle + 0x00, ticks * 10000);  // 100 ns units
            Store<uint64_t>(g_timeStampBundle + 0x08, SystemTime());
            Store<uint32_t>(g_timeStampBundle + 0x10, uint32_t(ticks));
        }
    }

    void PatchImportSlots()
    {
        int variables = 0;
#define HC09_FUNCTION_SLOT(slot, thunk) Store<uint32_t>(slot, thunk);
#define HC09_VARIABLE_SLOT(name, slot) Store<uint32_t>(slot, CreateVariable(#name)); variables++;
#include "kernel/import_slots.inc"
#undef HC09_FUNCTION_SLOT
#undef HC09_VARIABLE_SLOT
        std::fprintf(stderr, "[boot] patched import slots (%d kernel variables)\n", variables);

        if (g_timeStampBundle)
        {
            UpdateTimeStampBundle();
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    UpdateTimeStampBundle();
                }
            }).detach();
        }
    }
}
