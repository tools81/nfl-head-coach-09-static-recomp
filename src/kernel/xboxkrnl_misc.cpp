// Process, TLS, time, IRQL/spinlock, debug and fatal-error functions.

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "kernel/guest.h"
#include "kernel/threads.h"

namespace hc09
{
    [[noreturn]] void GuestFatal(const PPCContext& ctx, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        std::fprintf(stderr, "[fatal] ");
        std::vfprintf(stderr, format, args);
        va_end(args);
        std::fprintf(stderr, "\n        lr=0x%08X r1=0x%08X thread=0x%X\n",
            uint32_t(ctx.lr), ctx.r1.u32, CurrentThreadId());
        std::fflush(stderr);
        std::_Exit(3);
    }

    // ---- TLS ----------------------------------------------------------------------------------

    namespace
    {
        std::mutex g_tlsMutex;
        uint64_t g_tlsUsed = 0;  // bit per slot
    }

    // ---- Time -------------------------------------------------------------------------------

    uint64_t HostTimestampFrequency()
    {
        // mftb is recompiled to __rdtsc(), so guest timing must use the TSC rate.
        static const uint64_t frequency = [] {
            LARGE_INTEGER qpcFrequency, start, end;
            QueryPerformanceFrequency(&qpcFrequency);
            QueryPerformanceCounter(&start);
            const uint64_t tscStart = __rdtsc();
            do
                QueryPerformanceCounter(&end);
            while (end.QuadPart - start.QuadPart < qpcFrequency.QuadPart / 20);  // 50 ms
            const uint64_t tscEnd = __rdtsc();
            return (tscEnd - tscStart) * uint64_t(qpcFrequency.QuadPart) / uint64_t(end.QuadPart - start.QuadPart);
        }();
        return frequency;
    }

    uint64_t SystemTime()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    // ---- IRQL, spinlocks, critical regions -----------------------------------------------------
    //
    // There is no real IRQL; spinlocks are guest words holding the owner's thread id.

    namespace
    {
        std::atomic_ref<uint32_t> GuestWord(uint32_t address)
        {
            return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(GuestToHost(address)));
        }

        void AcquireSpinLock(uint32_t lock)
        {
            const uint32_t owner = ByteSwap(CurrentThreadId());
            auto word = GuestWord(lock);
            uint32_t expected = 0;
            while (!word.compare_exchange_weak(expected, owner, std::memory_order_acquire))
            {
                expected = 0;
                YieldProcessor();
            }
        }

        void ReleaseSpinLock(uint32_t lock)
        {
            GuestWord(lock).store(0, std::memory_order_release);
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    Return(ctx, 1);  // title process
}

PPC_FUNC(__imp__HalReturnToFirmware)
{
    GuestFatal(ctx, "HalReturnToFirmware(%u): the game requested a reboot/shutdown", Arg32(ctx, 0));
}

PPC_FUNC(__imp__KeBugCheck)
{
    GuestFatal(ctx, "KeBugCheck(0x%08X)", Arg32(ctx, 0));
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    GuestFatal(ctx, "KeBugCheckEx(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X)",
        Arg32(ctx, 0), Arg32(ctx, 1), Arg32(ctx, 2), Arg32(ctx, 3), Arg32(ctx, 4));
}

PPC_FUNC(__imp__DbgBreakPoint)
{
    std::fprintf(stderr, "[debug] DbgBreakPoint from 0x%08X\n", uint32_t(ctx.lr));
}

PPC_FUNC(__imp__KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t slot = 0; slot < kTlsSlotCount; slot++)
    {
        if (!(g_tlsUsed & (1ull << slot)))
        {
            g_tlsUsed |= 1ull << slot;
            return Return(ctx, slot);
        }
    }
    Return(ctx, 0xFFFFFFFF);
}

PPC_FUNC(__imp__KeTlsFree)
{
    const uint32_t slot = Arg32(ctx, 0);
    std::lock_guard lock(g_tlsMutex);
    if (slot >= kTlsSlotCount)
        return Return(ctx, 0);
    g_tlsUsed &= ~(1ull << slot);
    Return(ctx, 1);
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    const uint32_t slot = Arg32(ctx, 0);
    Return(ctx, slot < kTlsSlotCount ? CurrentThread()->tls[slot] : 0);
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    const uint32_t slot = Arg32(ctx, 0);
    if (slot >= kTlsSlotCount)
        return Return(ctx, 0);
    CurrentThread()->tls[slot] = Arg32(ctx, 1);
    Return(ctx, 1);
}

PPC_FUNC(__imp__KeQuerySystemTime)
{
    Store<uint64_t>(Arg32(ctx, 0), SystemTime());
}

PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    Return(ctx, HostTimestampFrequency());
}

PPC_FUNC(__imp__KfAcquireSpinLock)
{
    AcquireSpinLock(Arg32(ctx, 0));
    Return(ctx, 0);  // previous IRQL
}

PPC_FUNC(__imp__KfReleaseSpinLock)
{
    ReleaseSpinLock(Arg32(ctx, 0));
}

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpinLock(Arg32(ctx, 0));
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseSpinLock(Arg32(ctx, 0));
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__KfLowerIrql)
{
}

PPC_FUNC(__imp__KeEnterCriticalRegion)
{
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
}

PPC_FUNC(__imp__KeEnableFpuExceptions)
{
}

PPC_FUNC(__imp__KeLockL2)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__KeUnlockL2)
{
}
