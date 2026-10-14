// XAM notifications (XamNotifyCreateListener / XNotifyGetNext), XOVERLAPPED completion,
// and XMsg app-message requests.

#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/xam.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kErrorNotFound = 1168;  // ERROR_NOT_FOUND

        // Waitable while it has queued notifications.
        class NotifyListener : public KernelObject
        {
        public:
            explicit NotifyListener(uint64_t mask) : KernelObject(ObjectType::Other), mask(mask) {}
            bool IsWaitable() const override { return true; }
            bool IsSignaled(uint32_t) const override { return !queue.empty(); }

            uint64_t mask;
            std::deque<std::pair<uint32_t, uint32_t>> queue;  // id, param (guarded by WaitMutex)
        };

        std::vector<std::weak_ptr<NotifyListener>> g_listeners;  // guarded by WaitMutex

        uint32_t NotificationArea(uint32_t id)
        {
            return (id >> 25) & 0x3F;
        }

        void Enqueue(NotifyListener& listener, uint32_t id, uint32_t param)
        {
            if (listener.mask & (1ull << NotificationArea(id)))
                listener.queue.emplace_back(id, param);
        }
    }

    void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t extendedError)
    {
        Store<uint32_t>(overlapped + 0x00, result);
        Store<uint32_t>(overlapped + 0x04, length);
        Store<uint32_t>(overlapped + 0x18, extendedError);
        const uint32_t event = Load<uint32_t>(overlapped + 0x0C);
        if (event)
        {
            if (auto object = LookupHandleAs<EventObject>(event, ObjectType::Event))
            {
                std::lock_guard lock(WaitMutex());
                object->signaled = true;
                NotifyWaiters();
            }
        }
        if (Load<uint32_t>(overlapped + 0x10))
            std::fprintf(stderr, "[xam] overlapped completion routine 0x%08X not called\n", Load<uint32_t>(overlapped + 0x10));
    }

    uint32_t CompleteRequest(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        if (overlapped == 0)
            return result;
        CompleteOverlapped(overlapped, result, length, result);
        return error::kIoPending;
    }

    void BroadcastNotification(uint32_t id, uint32_t param)
    {
        std::lock_guard lock(WaitMutex());
        for (auto& weak : g_listeners)
            if (auto listener = weak.lock())
                Enqueue(*listener, id, param);
        NotifyWaiters();
    }
}

using namespace hc09;

PPC_FUNC(__imp__XamNotifyCreateListener)
{
    // (mask (64-bit), max_version)
    auto listener = std::make_shared<NotifyListener>(Arg64(ctx, 0));
    {
        std::lock_guard lock(WaitMutex());
        // State a freshly started title would observe: user 0 signed in, devices present.
        Enqueue(*listener, kXnSysSigninChanged, 1);
        Enqueue(*listener, kXnSysStorageDevicesChanged, 0);
        Enqueue(*listener, kXnSysInputDevicesChanged, 0);
        g_listeners.push_back(listener);
    }
    std::fprintf(stderr, "[xam] notification listener, mask 0x%llX\n", (unsigned long long)Arg64(ctx, 0));
    Return(ctx, CreateHandle(listener));
}

PPC_FUNC(__imp__XNotifyGetNext)
{
    // (handle, match_id, out_id, out_param) -> TRUE if a notification was returned
    auto object = LookupHandle(Arg32(ctx, 0));
    auto* listener = dynamic_cast<NotifyListener*>(object.get());
    if (!listener)
        return Return(ctx, 0);
    const uint32_t match = Arg32(ctx, 1);
    std::lock_guard lock(WaitMutex());
    for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
    {
        if (match != 0 && it->first != match)
            continue;
        Store<uint32_t>(Arg32(ctx, 2), it->first);
        if (Arg32(ctx, 3))
            Store<uint32_t>(Arg32(ctx, 3), it->second);
        std::fprintf(stderr, "[xam] notification 0x%08X (param 0x%X) delivered\n", it->first, it->second);
        listener->queue.erase(it);
        return Return(ctx, 1);
    }
    Return(ctx, 0);
}

// ---- XMsg: requests to XAM "apps" (xmp, xlive, xcontent, ...) -------------------------------

namespace
{
    constexpr uint32_t kXmpApp = 0xFA;           // the music player (custom soundtracks)
    constexpr uint32_t kXmpGetStatus = 0x00070009;
    constexpr uint32_t kXmpSetPlaybackController = 0x0007001A;  // (client, controller, playback_client)
    constexpr uint32_t kXmpGetPlaybackController = 0x0007001B;  // (client, out_controller, out_locked)

    // XMP with no user soundtrack: always idle, so the title plays its own music. Control
    // messages (play, stop, volume, playback behaviour, ...) are accepted and ignored.
    uint32_t XmpMessage(uint32_t message, uint32_t args)
    {
        if (message == kXmpGetStatus)
        {
            // (xmp_client, out_state): 0 idle, 1 playing, 2 paused
            Store<uint32_t>(Load<uint32_t>(args + 4), 0);
            return status::kSuccess;
        }
        if (message == kXmpGetPlaybackController)
        {
            // The title controls playback (0) and nothing is locked: it plays its own music.
            if (const uint32_t controller = Load<uint32_t>(args + 4))
                Store<uint32_t>(controller, 0);
            if (const uint32_t locked = Load<uint32_t>(args + 8))
                Store<uint32_t>(locked, 0);
            return status::kSuccess;
        }
        if (message == kXmpSetPlaybackController)
            return status::kSuccess;
        static std::mutex loggedMutex;
        static std::set<uint32_t> logged;
        std::lock_guard lock(loggedMutex);
        if (logged.insert(message).second)
            std::fprintf(stderr, "[xmp] message 0x%08X accepted (ignored)\n", message);
        return status::kSuccess;
    }
}

PPC_FUNC(__imp__XMsgInProcessCall)
{
    // (app, message, args, args_size)
    if (Arg32(ctx, 0) == kXmpApp)
        return Return(ctx, XmpMessage(Arg32(ctx, 1), Arg32(ctx, 2)));
    std::fprintf(stderr, "[xmsg] in-process app 0x%08X message 0x%08X (unhandled)\n", Arg32(ctx, 0), Arg32(ctx, 1));
    Return(ctx, status::kUnsuccessful);
}

PPC_FUNC(__imp__XMsgStartIORequest)
{
    // (app, message, overlapped, buffer, buffer_length)
    if (Arg32(ctx, 0) == kXmpApp)
    {
        const uint32_t result = XmpMessage(Arg32(ctx, 1), Arg32(ctx, 3));
        return Return(ctx, CompleteRequest(Arg32(ctx, 2), result));
    }
    std::fprintf(stderr, "[xmsg] app 0x%08X message 0x%08X (unhandled)\n", Arg32(ctx, 0), Arg32(ctx, 1));
    if (Arg32(ctx, 2))
        CompleteOverlapped(Arg32(ctx, 2), status::kUnsuccessful, 0, kErrorNotFound);
    Return(ctx, status::kUnsuccessful);
}

PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    // (app, message, overlapped, buffer, buffer_length, unknown)
    if (Arg32(ctx, 0) == kXmpApp)
    {
        const uint32_t result = XmpMessage(Arg32(ctx, 1), Arg32(ctx, 3));
        return Return(ctx, CompleteRequest(Arg32(ctx, 2), result));
    }
    std::fprintf(stderr, "[xmsg] app 0x%08X message 0x%08X (unhandled)\n", Arg32(ctx, 0), Arg32(ctx, 1));
    if (Arg32(ctx, 2))
        CompleteOverlapped(Arg32(ctx, 2), status::kUnsuccessful, 0, kErrorNotFound);
    Return(ctx, status::kUnsuccessful);
}

PPC_FUNC(__imp__XMsgCancelIORequest)
{
    Return(ctx, status::kSuccess);
}
