#include "kernel/objects.h"

#include <condition_variable>
#include <cstdio>
#include <unordered_map>

#include "kernel/guest.h"
#include "kernel/heap.h"
#include "kernel/threads.h"

namespace hc09
{
    uint64_t SystemTime();

    namespace
    {
        std::mutex g_waitMutex;
        std::condition_variable g_waitCondition;

        std::mutex g_tableMutex;
        uint32_t g_nextHandle = 0xF8000004;
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_guestObjects;
        std::unordered_map<std::string, std::weak_ptr<KernelObject>> g_names;

        // Written into DISPATCHER_HEADER.wait_list_head.flink of guest objects we mirror. The
        // guest's KeInitialize* (often inlined) points that list at itself, so a missing marker
        // means the structure was re-initialized and the mirror is stale.
        constexpr uint32_t kMirrorMarker = 0x48433039;  // 'HC09'

        bool IsMirroredType(uint8_t type)
        {
            return type == dispatcher::kNotificationEvent || type == dispatcher::kSynchronizationEvent ||
                type == dispatcher::kSemaphore;
        }

        std::shared_ptr<KernelObject> CreateFromHeader(uint32_t address)
        {
            const uint8_t type = Load<uint8_t>(address);
            const int32_t signalState = Load<int32_t>(address + 4);
            switch (type)
            {
            case dispatcher::kNotificationEvent:
            case dispatcher::kSynchronizationEvent:
                return std::make_shared<EventObject>(type == dispatcher::kNotificationEvent, signalState != 0);
            case dispatcher::kSemaphore:
                return std::make_shared<SemaphoreObject>(signalState, Load<int32_t>(address + 0x10));
            case dispatcher::kMutant:
                return std::make_shared<MutantObject>();
            case dispatcher::kNotificationTimer:
            case dispatcher::kSynchronizationTimer:
                return std::make_shared<TimerObject>(type == dispatcher::kNotificationTimer);
            default:
                std::fprintf(stderr, "[objects] unknown dispatcher type %u at 0x%08X\n", type, address);
                return std::make_shared<EventObject>(true, false);
            }
        }
    }

    std::mutex& WaitMutex()
    {
        return g_waitMutex;
    }

    void NotifyWaiters()
    {
        g_waitCondition.notify_all();
    }

    uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
    {
        std::lock_guard lock(g_tableMutex);
        const uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles.emplace(handle, std::move(object));
        return handle;
    }

    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThread() ? CurrentThread()->object : nullptr;
        std::lock_guard lock(g_tableMutex);
        auto it = g_handles.find(handle);
        return it != g_handles.end() ? it->second : nullptr;
    }

    bool CloseGuestHandle(uint32_t handle)
    {
        std::lock_guard lock(g_tableMutex);
        return g_handles.erase(handle) != 0;
    }

    void RegisterGuestObject(uint32_t address, std::shared_ptr<KernelObject> object)
    {
        std::lock_guard lock(g_tableMutex);
        object->guestObject = address;
        if (IsMirroredType(Load<uint8_t>(address)))
            Store<uint32_t>(address + 8, kMirrorMarker);
        g_guestObjects[address] = std::move(object);
    }

    void EventObject::SyncToGuest()
    {
        if (guestObject)
            Store<int32_t>(guestObject + 4, signaled ? 1 : 0);
    }

    void EventObject::SyncFromGuest()
    {
        if (guestObject)
            signaled = Load<int32_t>(guestObject + 4) != 0;
    }

    void SemaphoreObject::SyncToGuest()
    {
        if (guestObject)
            Store<int32_t>(guestObject + 4, count);
    }

    std::shared_ptr<KernelObject> ObjectFromGuest(uint32_t address)
    {
        const uint8_t type = Load<uint8_t>(address);
        const bool mirrored = IsMirroredType(type);
        std::shared_ptr<KernelObject> existing;
        {
            std::lock_guard lock(g_tableMutex);
            auto it = g_guestObjects.find(address);
            if (it != g_guestObjects.end() && (!mirrored || Load<uint32_t>(address + 8) == kMirrorMarker))
                existing = it->second;
        }
        if (existing)
        {
            // Adopt a signal state the guest changed inline (ours is always written back).
            std::lock_guard waitLock(g_waitMutex);
            existing->SyncFromGuest();
            return existing;
        }
        // New, or re-initialized by the guest since we mirrored it: mirror it afresh.
        auto object = CreateFromHeader(address);
        RegisterGuestObject(address, object);
        return object;
    }

    uint32_t GuestStructFor(const std::shared_ptr<KernelObject>& object)
    {
        if (object->guestObject)
            return object->guestObject;
        const uint32_t address = PoolHeap().Allocate(object->GuestStructSize());
        Store<uint8_t>(address, object->DispatcherType());
        RegisterGuestObject(address, object);
        return address;
    }

    std::shared_ptr<KernelObject> FindNamedObject(const std::string& name)
    {
        std::lock_guard lock(g_tableMutex);
        auto it = g_names.find(name);
        return it != g_names.end() ? it->second.lock() : nullptr;
    }

    void AddNamedObject(const std::string& name, std::shared_ptr<KernelObject> object)
    {
        std::lock_guard lock(g_tableMutex);
        object->name = name;
        g_names[name] = object;
    }

    std::string ObjectAttributesName(uint32_t objectAttributes)
    {
        // OBJECT_ATTRIBUTES: be32 root_directory; be32 object_name (ANSI_STRING*); be32 attributes
        if (objectAttributes == 0)
            return {};
        const uint32_t name = Load<uint32_t>(objectAttributes + 4);
        return name ? LoadAnsiString(name) : std::string();
    }

    std::optional<std::chrono::steady_clock::time_point> GuestTimeout(uint32_t timeoutPtr)
    {
        if (timeoutPtr == 0)
            return std::nullopt;
        int64_t value = Load<int64_t>(timeoutPtr);
        if (value > 0)  // absolute system time
            value = int64_t(SystemTime()) - value;
        // value is now a non-positive relative interval in 100 ns units
        return std::chrono::steady_clock::now() + std::chrono::nanoseconds(-value * 100);
    }

    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
        std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        const uint32_t threadId = CurrentThreadId();
        std::unique_lock lock(g_waitMutex);
        for (;;)
        {
            if (waitAll)
            {
                bool all = true;
                for (auto& object : objects)
                    all = all && object->IsSignaled(threadId);
                if (all)
                {
                    for (auto& object : objects)
                        object->Acquire(threadId);
                    return status::kSuccess;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(threadId))
                    {
                        objects[i]->Acquire(threadId);
                        return status::kSuccess + uint32_t(i);
                    }
                }
            }

            if (!deadline)
                g_waitCondition.wait(lock);
            else if (g_waitCondition.wait_until(lock, *deadline) == std::cv_status::timeout)
            {
                // One last check happens on the next loop iteration only if time remains.
                if (std::chrono::steady_clock::now() >= *deadline)
                    return status::kTimeout;
            }
        }
    }
}
