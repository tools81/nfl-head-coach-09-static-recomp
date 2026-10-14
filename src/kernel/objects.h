#pragma once

// Kernel objects: handle table, guest dispatcher-object mapping, and waiting.
//
// Every object is a host KernelObject. It can be reached by handle (Nt* APIs) and/or by
// the guest address of its dispatcher structure (Ke* APIs, ObReferenceObjectByHandle).
// All waitable state is protected by one global mutex, and every state change wakes all
// waiters, which re-check their conditions.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hc09
{
    struct GuestThread;

    enum class ObjectType
    {
        Event,
        Semaphore,
        Mutant,
        Timer,
        Thread,
        File,
        Other,
    };

    // DISPATCHER_HEADER.type values
    namespace dispatcher
    {
        constexpr uint8_t kNotificationEvent = 0;
        constexpr uint8_t kSynchronizationEvent = 1;
        constexpr uint8_t kMutant = 2;
        constexpr uint8_t kSemaphore = 5;
        constexpr uint8_t kThread = 6;
        constexpr uint8_t kNotificationTimer = 8;
        constexpr uint8_t kSynchronizationTimer = 9;
    }

    class KernelObject
    {
    public:
        explicit KernelObject(ObjectType type) : type(type) {}
        virtual ~KernelObject() = default;

        const ObjectType type;
        std::string name;

        // Waiting. Called with WaitMutex() held.
        virtual bool IsWaitable() const { return false; }
        virtual bool IsSignaled(uint32_t /*threadId*/) const { return false; }
        virtual void Acquire(uint32_t /*threadId*/) {}

        // DISPATCHER_HEADER.type used when a guest structure has to be created for it.
        virtual uint8_t DispatcherType() const { return 0xFF; }
        virtual uint32_t GuestStructSize() const { return 0x10; }

        // DISPATCHER_HEADER.signal_state mirroring for objects that live in guest memory:
        // game code reads (and sometimes writes) it inline. Called with WaitMutex() held.
        virtual void SyncToGuest() {}
        virtual void SyncFromGuest() {}

        uint32_t guestObject = 0;  // guest address of the object's structure, 0 until needed
    };

    class EventObject : public KernelObject
    {
    public:
        EventObject(bool manualReset, bool signaled)
            : KernelObject(ObjectType::Event), manualReset(manualReset), signaled(signaled) {}
        bool IsWaitable() const override { return true; }
        bool IsSignaled(uint32_t) const override { return signaled; }
        void Acquire(uint32_t) override
        {
            if (!manualReset)
            {
                signaled = false;
                SyncToGuest();
            }
        }
        void SyncToGuest() override;
        void SyncFromGuest() override;
        uint8_t DispatcherType() const override
        {
            return manualReset ? dispatcher::kNotificationEvent : dispatcher::kSynchronizationEvent;
        }

        bool manualReset;
        bool signaled;
    };

    class SemaphoreObject : public KernelObject
    {
    public:
        SemaphoreObject(int32_t count, int32_t limit)
            : KernelObject(ObjectType::Semaphore), count(count), limit(limit) {}
        bool IsWaitable() const override { return true; }
        bool IsSignaled(uint32_t) const override { return count > 0; }
        void Acquire(uint32_t) override
        {
            count--;
            SyncToGuest();
        }
        void SyncToGuest() override;
        uint8_t DispatcherType() const override { return dispatcher::kSemaphore; }
        uint32_t GuestStructSize() const override { return 0x14; }

        int32_t count;
        int32_t limit;
    };

    class MutantObject : public KernelObject
    {
    public:
        MutantObject() : KernelObject(ObjectType::Mutant) {}
        bool IsWaitable() const override { return true; }
        bool IsSignaled(uint32_t threadId) const override { return owner == 0 || owner == threadId; }
        void Acquire(uint32_t threadId) override { owner = threadId; recursion++; }
        uint8_t DispatcherType() const override { return dispatcher::kMutant; }
        uint32_t GuestStructSize() const override { return 0x20; }

        uint32_t owner = 0;
        uint32_t recursion = 0;
    };

    class TimerObject : public KernelObject
    {
    public:
        explicit TimerObject(bool manualReset) : KernelObject(ObjectType::Timer), manualReset(manualReset) {}
        bool IsWaitable() const override { return true; }
        bool IsSignaled(uint32_t) const override { return signaled; }
        void Acquire(uint32_t) override { if (!manualReset) signaled = false; }
        uint8_t DispatcherType() const override
        {
            return manualReset ? dispatcher::kNotificationTimer : dispatcher::kSynchronizationTimer;
        }
        uint32_t GuestStructSize() const override { return 0x28; }

        bool manualReset;
        bool signaled = false;
        uint64_t generation = 0;  // bumped on every set/cancel; stale timer threads exit
    };

    class ThreadObject : public KernelObject
    {
    public:
        explicit ThreadObject(GuestThread* thread) : KernelObject(ObjectType::Thread), thread(thread) {}
        bool IsWaitable() const override { return true; }
        bool IsSignaled(uint32_t) const override { return exited; }
        uint8_t DispatcherType() const override { return dispatcher::kThread; }

        GuestThread* thread;
        bool exited = false;
        uint32_t exitCode = 0;
        int32_t suspendCount = 0;
        int32_t priority = 0;
        uint32_t affinity = 0x3F;
    };

    // Global lock for all waitable state; NotifyWaiters must be called after changing it.
    std::mutex& WaitMutex();
    void NotifyWaiters();

    // Handles. Pseudo-handles: 0xFFFFFFFE = current thread, 0xFFFFFFFF = current process.
    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;

    uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
    bool CloseGuestHandle(uint32_t handle);

    template <typename T>
    std::shared_ptr<T> LookupHandleAs(uint32_t handle, ObjectType type)
    {
        auto object = LookupHandle(handle);
        return object && object->type == type ? std::static_pointer_cast<T>(object) : nullptr;
    }

    // Guest dispatcher structures (KEVENT*, KSEMAPHORE*, KTHREAD*, ...).
    void RegisterGuestObject(uint32_t address, std::shared_ptr<KernelObject> object);
    // Returns the object for a guest structure, creating it from its DISPATCHER_HEADER the
    // first time it is seen (structures initialized inline by guest code).
    std::shared_ptr<KernelObject> ObjectFromGuest(uint32_t address);
    // Guest structure for an object, allocating and registering one if it has none yet.
    uint32_t GuestStructFor(const std::shared_ptr<KernelObject>& object);

    // Named objects (OBJECT_ATTRIBUTES with a name). Returns the existing object, if any.
    std::shared_ptr<KernelObject> FindNamedObject(const std::string& name);
    void AddNamedObject(const std::string& name, std::shared_ptr<KernelObject> object);
    // Name from an OBJECT_ATTRIBUTES pointer (empty when unnamed or null).
    std::string ObjectAttributesName(uint32_t objectAttributes);

    // Timeouts from a guest LARGE_INTEGER pointer: null = infinite, negative = relative
    // (100 ns units), positive = absolute system time.
    std::optional<std::chrono::steady_clock::time_point> GuestTimeout(uint32_t timeoutPtr);

    // Waits until one (waitAll = false) or all objects are signaled, then acquires them.
    // Returns STATUS_WAIT_0 + index, STATUS_WAIT_0 for wait-all, or STATUS_TIMEOUT.
    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
        std::optional<std::chrono::steady_clock::time_point> deadline);
}
