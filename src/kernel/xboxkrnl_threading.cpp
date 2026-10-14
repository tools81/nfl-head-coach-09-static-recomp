// Threads, waits, and synchronization objects (events, semaphores, mutants, timers).

#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <thread>

#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/threads.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kCreateSuspended = 0x1;
        constexpr uint32_t kDispatcherSignalState = 0x04;
        constexpr uint32_t kKthreadExitStatus = 0x140;

        void RunGuestThread(GuestThread* thread, uint32_t startup, uint32_t start, uint32_t context)
        {
            PPCContext ctx{};
            EnterGuestThread(ctx, thread);
            auto& object = *thread->object;
            {
                // Created suspended: poll until KeResumeThread/NtResumeThread drops the count.
                std::unique_lock lock(WaitMutex());
                while (object.suspendCount > 0)
                {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    lock.lock();
                }
            }

            uint8_t* base = g_base;
            uint32_t exitCode = 0;
            try
            {
                if (startup)
                {
                    // XAPI thread startup: startup(start_address, start_context)
                    ctx.r3.u64 = start;
                    ctx.r4.u64 = context;
                    PPC_CALL_INDIRECT_FUNC(startup);
                }
                else
                {
                    ctx.r3.u64 = context;
                    PPC_CALL_INDIRECT_FUNC(start);
                }
                exitCode = ctx.r3.u32;
            }
            catch (const GuestThreadExit& exit)
            {
                exitCode = exit.exitCode;
            }

            std::fprintf(stderr, "[thread] 0x%X exited with code 0x%X\n", thread->id, exitCode);
            std::lock_guard lock(WaitMutex());
            // XAPI's GetExitCodeThread reads the KTHREAD directly: the dispatcher header's
            // signal state says whether the thread has terminated, +0x140 holds the status.
            Store<uint32_t>(thread->kthread + kKthreadExitStatus, exitCode);
            Store<int32_t>(thread->kthread + kDispatcherSignalState, 1);
            object.exited = true;
            object.exitCode = exitCode;
            NotifyWaiters();
        }

        std::shared_ptr<ThreadObject> ThreadFromGuest(uint32_t kthread)
        {
            auto object = ObjectFromGuest(kthread);
            return object->type == ObjectType::Thread ? std::static_pointer_cast<ThreadObject>(object) : nullptr;
        }

        template <typename T, typename... Args>
        uint32_t CreateObject(PPCContext& ctx, uint32_t handlePtr, uint32_t objectAttributes, Args&&... args)
        {
            const std::string name = ObjectAttributesName(objectAttributes);
            if (!name.empty())
            {
                if (auto existing = FindNamedObject(name))
                {
                    Store<uint32_t>(handlePtr, CreateHandle(existing));
                    return status::kObjectNameExists;
                }
            }
            auto object = std::make_shared<T>(std::forward<Args>(args)...);
            if (!name.empty())
                AddNamedObject(name, object);
            Store<uint32_t>(handlePtr, CreateHandle(object));
            return status::kSuccess;
        }
    }
}

using namespace hc09;

// ---- Threads ---------------------------------------------------------------------------------

PPC_FUNC(__imp__ExCreateThread)
{
    // (out_handle, stack_size, out_thread_id, xapi_startup, start_address, start_context, flags)
    const uint32_t handlePtr = Arg32(ctx, 0), stackSize = Arg32(ctx, 1), threadIdPtr = Arg32(ctx, 2);
    const uint32_t startup = Arg32(ctx, 3), start = Arg32(ctx, 4), context = Arg32(ctx, 5);
    const uint32_t flags = Arg32(ctx, 6);

    GuestThread* thread = CreateGuestThread(stackSize ? stackSize : 0x10000);
    thread->object->suspendCount = (flags & kCreateSuspended) ? 1 : 0;
    // The top byte of the creation flags is a processor affinity mask.
    if (const uint32_t mask = flags >> 24)
    {
        thread->object->affinity = mask;
        Store<uint8_t>(thread->pcr + 0x10C, uint8_t(__builtin_ctz(mask)));
    }
    if (handlePtr)
        Store<uint32_t>(handlePtr, CreateHandle(thread->object));
    if (threadIdPtr)
        Store<uint32_t>(threadIdPtr, thread->id);

    std::fprintf(stderr, "[thread] create 0x%X start=0x%08X startup=0x%08X ctx=0x%08X flags=0x%X kthread=0x%08X%s\n",
        thread->id, start, startup, context, flags, thread->kthread, (flags & kCreateSuspended) ? " (suspended)" : "");
    std::thread(RunGuestThread, thread, startup, start, context).detach();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__ExTerminateThread)
{
    throw GuestThreadExit{ Arg32(ctx, 0) };
}

PPC_FUNC(__imp__KeResumeThread)
{
    auto thread = ThreadFromGuest(Arg32(ctx, 0));
    std::lock_guard lock(WaitMutex());
    const int32_t previous = thread ? thread->suspendCount : 0;
    if (thread && thread->suspendCount > 0)
        thread->suspendCount--;
    Return(ctx, uint32_t(previous));
}

PPC_FUNC(__imp__NtResumeThread)
{
    auto thread = LookupHandleAs<ThreadObject>(Arg32(ctx, 0), ObjectType::Thread);
    if (!thread)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    if (Arg32(ctx, 1))
        Store<uint32_t>(Arg32(ctx, 1), uint32_t(thread->suspendCount));
    if (thread->suspendCount > 0)
        thread->suspendCount--;
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtSuspendThread)
{
    // Suspending a running host thread is not supported; only the count is tracked.
    auto thread = LookupHandleAs<ThreadObject>(Arg32(ctx, 0), ObjectType::Thread);
    if (!thread)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    if (Arg32(ctx, 1))
        Store<uint32_t>(Arg32(ctx, 1), uint32_t(thread->suspendCount));
    thread->suspendCount++;
    std::fprintf(stderr, "[thread] NtSuspendThread(0x%X) not enforced\n", thread->thread->id);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__KeSetAffinityThread)
{
    // (thread, affinity mask). Each guest thread has its own PCR, so the PCR's current-CPU
    // byte stands for the hardware thread it is pinned to: code uses it to index per-CPU
    // data (XAudio's workers rendezvous through one byte per CPU).
    auto thread = ThreadFromGuest(Arg32(ctx, 0));
    uint32_t previous = 0;
    if (thread)
    {
        const uint32_t mask = Arg32(ctx, 1);
        previous = thread->affinity;
        thread->affinity = mask;
        if (mask != 0)
        {
            const uint32_t cpu = uint32_t(__builtin_ctz(mask));
            Store<uint8_t>(thread->thread->pcr + 0x10C, uint8_t(cpu));
            std::fprintf(stderr, "[thread] 0x%X pinned to CPU %u (mask 0x%X)\n", thread->thread->id, cpu, mask);
        }
    }
    Return(ctx, previous);
}

PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    auto thread = ThreadFromGuest(Arg32(ctx, 0));
    int32_t previous = 0;
    if (thread)
    {
        previous = thread->priority;
        thread->priority = int32_t(Arg32(ctx, 1));
    }
    Return(ctx, uint32_t(previous));
}

PPC_FUNC(__imp__KeDelayExecutionThread)
{
    // (processor_mode, alertable, interval)
    const auto deadline = GuestTimeout(Arg32(ctx, 2));
    if (deadline)
        std::this_thread::sleep_until(*deadline);
    else
        std::this_thread::yield();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtYieldExecution)
{
    std::this_thread::yield();
    Return(ctx, 0);
}

// ---- Waits -----------------------------------------------------------------------------------

PPC_FUNC(__imp__KeWaitForSingleObject)
{
    // (object, wait_reason, processor_mode, alertable, timeout)
    auto object = ObjectFromGuest(Arg32(ctx, 0));
    Return(ctx, WaitForObjects({ object }, false, GuestTimeout(Arg32(ctx, 4))));
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    // (handle, processor_mode, alertable, timeout)
    auto object = LookupHandle(Arg32(ctx, 0));
    if (!object || !object->IsWaitable())
        return Return(ctx, status::kInvalidHandle);
    Return(ctx, WaitForObjects({ object }, false, GuestTimeout(Arg32(ctx, 3))));
}

PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    // (count, objects[], wait_type (0 = all, 1 = any), wait_reason, mode, alertable, timeout, wait_blocks)
    const uint32_t count = Arg32(ctx, 0), objectsPtr = Arg32(ctx, 1);
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < count; i++)
        objects.push_back(ObjectFromGuest(Load<uint32_t>(objectsPtr + i * 4)));
    Return(ctx, WaitForObjects(objects, Arg32(ctx, 2) == 0, GuestTimeout(Arg32(ctx, 6))));
}

// ---- Events ----------------------------------------------------------------------------------

PPC_FUNC(__imp__NtCreateEvent)
{
    // (out_handle, object_attributes, event_type (0 = notification, 1 = synchronization), initial_state)
    Return(ctx, CreateObject<EventObject>(ctx, Arg32(ctx, 0), Arg32(ctx, 1), Arg32(ctx, 2) == 0, Arg32(ctx, 3) != 0));
}

PPC_FUNC(__imp__NtSetEvent)
{
    auto event = LookupHandleAs<EventObject>(Arg32(ctx, 0), ObjectType::Event);
    if (!event)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    if (Arg32(ctx, 1))
        Store<uint32_t>(Arg32(ctx, 1), event->signaled);
    event->signaled = true;
    event->SyncToGuest();
    NotifyWaiters();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtClearEvent)
{
    auto event = LookupHandleAs<EventObject>(Arg32(ctx, 0), ObjectType::Event);
    if (!event)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    event->signaled = false;
    event->SyncToGuest();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__KeSetEvent)
{
    // (event, increment, wait) -> previous state
    auto object = ObjectFromGuest(Arg32(ctx, 0));
    std::lock_guard lock(WaitMutex());
    auto* event = object->type == ObjectType::Event ? static_cast<EventObject*>(object.get()) : nullptr;
    const bool previous = event && event->signaled;
    if (event)
    {
        event->signaled = true;
        event->SyncToGuest();
    }
    NotifyWaiters();
    Return(ctx, previous);
}

PPC_FUNC(__imp__KeResetEvent)
{
    auto object = ObjectFromGuest(Arg32(ctx, 0));
    std::lock_guard lock(WaitMutex());
    auto* event = object->type == ObjectType::Event ? static_cast<EventObject*>(object.get()) : nullptr;
    const bool previous = event && event->signaled;
    if (event)
    {
        event->signaled = false;
        event->SyncToGuest();
    }
    Return(ctx, previous);
}

// ---- Semaphores ------------------------------------------------------------------------------

PPC_FUNC(__imp__NtCreateSemaphore)
{
    // (out_handle, object_attributes, initial_count, maximum_count)
    Return(ctx, CreateObject<SemaphoreObject>(ctx, Arg32(ctx, 0), Arg32(ctx, 1), int32_t(Arg32(ctx, 2)), int32_t(Arg32(ctx, 3))));
}

PPC_FUNC(__imp__NtReleaseSemaphore)
{
    // (handle, release_count, out_previous_count)
    auto semaphore = LookupHandleAs<SemaphoreObject>(Arg32(ctx, 0), ObjectType::Semaphore);
    if (!semaphore)
        return Return(ctx, status::kInvalidHandle);
    const int32_t release = int32_t(Arg32(ctx, 1));
    std::lock_guard lock(WaitMutex());
    if (semaphore->count + release > semaphore->limit)
        return Return(ctx, status::kSemaphoreLimitExceeded);
    if (Arg32(ctx, 2))
        Store<int32_t>(Arg32(ctx, 2), semaphore->count);
    semaphore->count += release;
    semaphore->SyncToGuest();
    NotifyWaiters();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__KeInitializeSemaphore)
{
    // (semaphore, count, limit): KSEMAPHORE = DISPATCHER_HEADER + be32 limit
    const uint32_t address = Arg32(ctx, 0);
    const int32_t count = int32_t(Arg32(ctx, 1)), limit = int32_t(Arg32(ctx, 2));
    std::memset(GuestToHost(address), 0, 0x14);
    Store<uint8_t>(address, dispatcher::kSemaphore);
    Store<uint8_t>(address + 2, 0x14 / 4);
    Store<int32_t>(address + 4, count);
    Store<int32_t>(address + 0x10, limit);
    RegisterGuestObject(address, std::make_shared<SemaphoreObject>(count, limit));
}

PPC_FUNC(__imp__KeReleaseSemaphore)
{
    // (semaphore, increment, adjustment, wait) -> previous count
    auto object = ObjectFromGuest(Arg32(ctx, 0));
    std::lock_guard lock(WaitMutex());
    if (object->type != ObjectType::Semaphore)
        return Return(ctx, 0);
    auto* semaphore = static_cast<SemaphoreObject*>(object.get());
    const int32_t previous = semaphore->count;
    semaphore->count = std::min(semaphore->count + int32_t(Arg32(ctx, 2)), semaphore->limit);
    semaphore->SyncToGuest();
    NotifyWaiters();
    Return(ctx, uint32_t(previous));
}

// ---- Mutants ---------------------------------------------------------------------------------

PPC_FUNC(__imp__NtCreateMutant)
{
    // (out_handle, object_attributes, initial_owner)
    const uint32_t result = CreateObject<MutantObject>(ctx, Arg32(ctx, 0), Arg32(ctx, 1));
    if (result == status::kSuccess && Arg32(ctx, 2))
    {
        auto mutant = LookupHandleAs<MutantObject>(Load<uint32_t>(Arg32(ctx, 0)), ObjectType::Mutant);
        std::lock_guard lock(WaitMutex());
        mutant->Acquire(CurrentThreadId());
    }
    Return(ctx, result);
}

PPC_FUNC(__imp__NtReleaseMutant)
{
    // (handle, out_previous_count)
    auto mutant = LookupHandleAs<MutantObject>(Arg32(ctx, 0), ObjectType::Mutant);
    if (!mutant)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    if (mutant->owner != CurrentThreadId())
        return Return(ctx, status::kMutantNotOwned);
    if (Arg32(ctx, 1))
        Store<int32_t>(Arg32(ctx, 1), int32_t(mutant->recursion));
    if (--mutant->recursion == 0)
        mutant->owner = 0;
    NotifyWaiters();
    Return(ctx, status::kSuccess);
}

// ---- Timers ----------------------------------------------------------------------------------

PPC_FUNC(__imp__NtCreateTimer)
{
    // (out_handle, object_attributes, timer_type (0 = notification, 1 = synchronization))
    Return(ctx, CreateObject<TimerObject>(ctx, Arg32(ctx, 0), Arg32(ctx, 1), Arg32(ctx, 2) == 0));
}

PPC_FUNC(__imp__NtSetTimerEx)
{
    // (handle, due_time, apc_routine, apc_mode, apc_context, resume, period_ms, out_previous_state)
    auto timer = LookupHandleAs<TimerObject>(Arg32(ctx, 0), ObjectType::Timer);
    if (!timer)
        return Return(ctx, status::kInvalidHandle);
    if (Arg32(ctx, 2))
        std::fprintf(stderr, "[timer] APC routine 0x%08X ignored (APCs not implemented)\n", Arg32(ctx, 2));

    const auto due = GuestTimeout(Arg32(ctx, 1)).value_or(std::chrono::steady_clock::now());
    const auto period = std::chrono::milliseconds(Arg32(ctx, 6));
    uint64_t generation;
    {
        std::lock_guard lock(WaitMutex());
        if (Arg32(ctx, 7))
            Store<uint32_t>(Arg32(ctx, 7), timer->signaled);
        timer->signaled = false;
        generation = ++timer->generation;
    }
    std::thread([timer, due, period, generation] {
        auto next = due;
        for (;;)
        {
            std::this_thread::sleep_until(next);
            std::lock_guard lock(WaitMutex());
            if (timer->generation != generation)
                return;
            timer->signaled = true;
            NotifyWaiters();
            if (period.count() == 0)
                return;
            next += period;
        }
    }).detach();
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtCancelTimer)
{
    auto timer = LookupHandleAs<TimerObject>(Arg32(ctx, 0), ObjectType::Timer);
    if (!timer)
        return Return(ctx, status::kInvalidHandle);
    std::lock_guard lock(WaitMutex());
    if (Arg32(ctx, 1))
        Store<uint32_t>(Arg32(ctx, 1), timer->signaled);
    timer->generation++;
    Return(ctx, status::kSuccess);
}

// ---- Handles and object references -----------------------------------------------------------

PPC_FUNC(__imp__NtClose)
{
    Return(ctx, CloseGuestHandle(Arg32(ctx, 0)) ? status::kSuccess : status::kInvalidHandle);
}

PPC_FUNC(__imp__NtDuplicateObject)
{
    // (source_handle, out_handle, options); DUPLICATE_CLOSE_SOURCE = 1
    auto object = LookupHandle(Arg32(ctx, 0));
    if (!object)
        return Return(ctx, status::kInvalidHandle);
    Store<uint32_t>(Arg32(ctx, 1), CreateHandle(object));
    if (Arg32(ctx, 2) & 1)
        CloseGuestHandle(Arg32(ctx, 0));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    // (handle, object_type, out_object)
    auto object = LookupHandle(Arg32(ctx, 0));
    if (!object)
        return Return(ctx, status::kInvalidHandle);
    Store<uint32_t>(Arg32(ctx, 2), GuestStructFor(object));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__ObReferenceObject)
{
}

PPC_FUNC(__imp__ObDereferenceObject)
{
}

PPC_FUNC(__imp__ObIsTitleObject)
{
    Return(ctx, 1);
}

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
}

PPC_FUNC(__imp__KiApcNormalRoutineNop)
{
}
