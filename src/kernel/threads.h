#pragma once

#include <ppc_recomp_shared.h>

#include <array>
#include <cstdint>
#include <memory>

namespace hc09
{
    constexpr uint32_t kTlsSlotCount = 64;

    class ThreadObject;

    // One guest thread. Each runs on its own host thread with its own PPCContext,
    // guest stack and processor control region (r13).
    struct GuestThread
    {
        uint32_t id = 0;           // guest thread id (also stored as CS owner)
        uint32_t pcr = 0;          // guest address r13 points at
        uint32_t stackBase = 0;    // lowest guest address of the stack
        uint32_t stackSize = 0;
        uint32_t kthread = 0;      // guest KTHREAD stand-in
        std::array<uint32_t, kTlsSlotCount> tls{};
        std::shared_ptr<ThreadObject> object;  // waitable kernel object for this thread
    };

    GuestThread* CurrentThread();
    uint32_t CurrentThreadId();

    // Allocates a guest thread's stack, PCR and KTHREAD and registers its kernel object.
    // The thread does not run until a host thread calls EnterGuestThread on it.
    GuestThread* CreateGuestThread(uint32_t stackSize);

    // Binds `thread` to the calling host thread and points ctx.r1/r13 at its stack and PCR.
    void EnterGuestThread(PPCContext& ctx, GuestThread* thread);

    // Thrown by ExTerminateThread to unwind out of guest code back to the thread's entry.
    struct GuestThreadExit
    {
        uint32_t exitCode;
    };
}
