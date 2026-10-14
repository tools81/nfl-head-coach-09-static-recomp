// Guest thread environment: the processor control region (KPCR) r13 points at.
//
// Field offsets follow Xenia's X_KPCR / X_KPRCB definitions. The game declares no static
// TLS data (XEX TLS_INFO: 64 slots, raw data size 0), so dynamic slots are reached only
// through KeTlsGetValue/KeTlsSetValue (kept host-side in GuestThread::tls) and tls_ptr
// just needs to point at valid memory.

#include <atomic>

#include "debug/stack_dump.h"
#include "kernel/guest.h"
#include "kernel/heap.h"
#include "kernel/objects.h"
#include "kernel/threads.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kPcrBlockSize = 0x3000;
        constexpr uint32_t kTlsOffset = 0x1000;     // relative to the PCR
        constexpr uint32_t kThreadOffset = 0x2000;  // KTHREAD stand-in, relative to the PCR
        constexpr uint8_t kThreadObjectType = 6;    // DISPATCHER_HEADER type for threads

        // X_KPCR
        constexpr uint32_t kPcrTlsPtr = 0x000;
        constexpr uint32_t kPcrSelf = 0x030;
        constexpr uint32_t kPcrStackBase = 0x070;
        constexpr uint32_t kPcrStackEnd = 0x074;
        // X_KPRCB, embedded at 0x100
        constexpr uint32_t kPrcbCurrentThread = 0x100;
        constexpr uint32_t kPrcbCurrentCpu = 0x10C;

        std::atomic<uint32_t> g_nextThreadId{ 0x100 };
        thread_local GuestThread* t_current = nullptr;
    }

    GuestThread* CurrentThread()
    {
        return t_current;
    }

    uint32_t CurrentThreadId()
    {
        return t_current ? t_current->id : 0;
    }

    GuestThread* CreateGuestThread(uint32_t stackSize)
    {
        auto* thread = new GuestThread();  // guest threads are never destroyed
        thread->id = g_nextThreadId.fetch_add(4);
        thread->stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
        thread->stackBase = VirtualHeap().Allocate(thread->stackSize, 0x10000, true);
        thread->pcr = PoolHeap().Allocate(kPcrBlockSize, 0x1000);
        thread->kthread = thread->pcr + kThreadOffset;

        const uint32_t pcr = thread->pcr;
        Store<uint32_t>(pcr + kPcrTlsPtr, pcr + kTlsOffset);
        Store<uint32_t>(pcr + kPcrSelf, pcr);
        Store<uint32_t>(pcr + kPcrStackBase, thread->stackBase + thread->stackSize);
        Store<uint32_t>(pcr + kPcrStackEnd, thread->stackBase);
        Store<uint32_t>(pcr + kPrcbCurrentThread, thread->kthread);
        Store<uint8_t>(pcr + kPrcbCurrentCpu, 0);
        Store<uint8_t>(thread->kthread, kThreadObjectType);

        thread->object = std::make_shared<ThreadObject>(thread);
        RegisterGuestObject(thread->kthread, thread->object);
        return thread;
    }

    void EnterGuestThread(PPCContext& ctx, GuestThread* thread)
    {
        ctx.r1.u64 = thread->stackBase + thread->stackSize - 0x80;
        ctx.r13.u64 = thread->pcr;
        // fpscr caches the host MXCSR and writes it back on every mode change; seeding it
        // with 0 would unmask every floating-point exception.
        ctx.fpscr.loadFromHost();
        t_current = thread;
        debug::RegisterThread(thread->id);
    }
}
