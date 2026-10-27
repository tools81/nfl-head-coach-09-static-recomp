// Networking (NetDll_*): an Xbox 360 with no network cable.
//
// Every NetDll export takes an extra leading "caller" argument, so WinSock-style
// parameters start at argument 1. WSA events are real kernel events (so waits block);
// sockets cannot be created, and address/link queries report no network.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <memory>
#include <vector>

#include "kernel/guest.h"
#include "kernel/objects.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kXnAddrNone = 0x00000001;     // XNET_GET_XNADDR_NONE: no network
        constexpr uint32_t kSocketError = 0xFFFFFFFF;    // SOCKET_ERROR / INVALID_SOCKET
        constexpr uint32_t kWsaENetDown = 10050;
        constexpr uint32_t kWsaWaitTimeout = 258;
        constexpr uint32_t kWsaInvalidHandle = 6;

        thread_local uint32_t t_lastError = 0;

        uint32_t SocketFailure(PPCContext& ctx)
        {
            t_lastError = kWsaENetDown;
            ctx.r3.u64 = kSocketError;
            return kSocketError;
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__NetDll_XNetStartup)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__NetDll_XNetCleanup)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__NetDll_WSAStartup)
{
    // (caller, version, out WSADATA)
    const uint32_t data = Arg32(ctx, 2);
    if (data)
    {
        std::memset(GuestToHost(data), 0, 0x190);
        Store<uint16_t>(data, uint16_t(Arg32(ctx, 1)));
        Store<uint16_t>(data + 2, 0x0202);
    }
    Return(ctx, 0);
}

PPC_FUNC(__imp__NetDll_WSACleanup)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__NetDll_WSAGetLastError)
{
    Return(ctx, t_lastError);
}

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    // (caller, out XNADDR) -> status flags
    const uint32_t address = Arg32(ctx, 1);
    if (address)
        std::memset(GuestToHost(address), 0, 0x24);
    Return(ctx, kXnAddrNone);
}

PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    Return(ctx, 0);  // no link
}

PPC_FUNC(__imp__NetDll_XNetGetConnectStatus)
{
    Return(ctx, 0);  // XNET_CONNECT_STATUS_IDLE
}

// ---- WSA events ------------------------------------------------------------------------------

PPC_FUNC(__imp__NetDll_WSACreateEvent)
{
    // Manual-reset, initially non-signaled.
    Return(ctx, CreateHandle(std::make_shared<EventObject>(true, false)));
}

PPC_FUNC(__imp__NetDll_WSACloseEvent)
{
    Return(ctx, CloseGuestHandle(Arg32(ctx, 1)) ? 1 : 0);
}

PPC_FUNC(__imp__NetDll_WSASetEvent)
{
    auto event = LookupHandleAs<EventObject>(Arg32(ctx, 1), ObjectType::Event);
    if (!event)
        return Return(ctx, 0);
    std::lock_guard lock(WaitMutex());
    event->signaled = true;
    event->SyncToGuest();
    NotifyWaiters();
    Return(ctx, 1);
}

PPC_FUNC(__imp__NetDll_WSAResetEvent)
{
    auto event = LookupHandleAs<EventObject>(Arg32(ctx, 1), ObjectType::Event);
    if (!event)
        return Return(ctx, 0);
    std::lock_guard lock(WaitMutex());
    event->signaled = false;
    event->SyncToGuest();
    Return(ctx, 1);
}

PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents)
{
    // (caller, count, events[], wait_all, timeout_ms, alertable) -> WSA_WAIT_EVENT_0 + i
    const uint32_t count = Arg32(ctx, 1), events = Arg32(ctx, 2);
    const bool waitAll = Arg32(ctx, 3) != 0;
    const uint32_t timeoutMs = Arg32(ctx, 4);
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < count; i++)
    {
        auto object = LookupHandle(Load<uint32_t>(events + i * 4));
        if (!object || !object->IsWaitable())
        {
            t_lastError = kWsaInvalidHandle;
            return Return(ctx, 0xFFFFFFFF);  // WSA_WAIT_FAILED
        }
        objects.push_back(std::move(object));
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (timeoutMs != 0xFFFFFFFF)
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const uint32_t result = WaitForObjects(objects, waitAll, deadline);
    Return(ctx, result == status::kTimeout ? kWsaWaitTimeout : result);
}

PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult)
{
    t_lastError = kWsaENetDown;
    Return(ctx, 0);
}

// ---- Sockets: nothing can be opened -----------------------------------------------------------

#define HC09_SOCKET_FAILURE(name) \
    PPC_FUNC(__imp__NetDll_##name) { SocketFailure(ctx); }

HC09_SOCKET_FAILURE(socket)
HC09_SOCKET_FAILURE(closesocket)
HC09_SOCKET_FAILURE(bind)
HC09_SOCKET_FAILURE(connect)
HC09_SOCKET_FAILURE(listen)
HC09_SOCKET_FAILURE(accept)
HC09_SOCKET_FAILURE(send)
HC09_SOCKET_FAILURE(sendto)
HC09_SOCKET_FAILURE(recv)
HC09_SOCKET_FAILURE(recvfrom)
HC09_SOCKET_FAILURE(select)
HC09_SOCKET_FAILURE(shutdown)
HC09_SOCKET_FAILURE(setsockopt)
HC09_SOCKET_FAILURE(getsockopt)
HC09_SOCKET_FAILURE(getsockname)
HC09_SOCKET_FAILURE(getpeername)
HC09_SOCKET_FAILURE(ioctlsocket)
HC09_SOCKET_FAILURE(WSARecv)
HC09_SOCKET_FAILURE(WSARecvFrom)

// ---- XNet services (matchmaking, QoS, keys, DNS): all unavailable offline -------------------

#define HC09_XNET_FAILURE(name) \
    PPC_FUNC(__imp__NetDll_##name) { ctx.r3.u64 = kWsaENetDown; }

HC09_XNET_FAILURE(XNetConnect)
HC09_XNET_FAILURE(XNetCreateKey)
HC09_XNET_FAILURE(XNetRegisterKey)
HC09_XNET_FAILURE(XNetUnregisterKey)
HC09_XNET_FAILURE(XNetXnAddrToInAddr)
HC09_XNET_FAILURE(XNetServerToInAddr)
HC09_XNET_FAILURE(XNetDnsLookup)
HC09_XNET_FAILURE(XNetDnsRelease)
HC09_XNET_FAILURE(XNetQosListen)
HC09_XNET_FAILURE(XNetQosLookup)
HC09_XNET_FAILURE(XNetQosRelease)
HC09_XNET_FAILURE(XNetQosServiceLookup)

PPC_FUNC(__imp__NetDll_XNetRandom)
{
    // (caller, buffer, length): fill with host randomness
    const uint32_t buffer = Arg32(ctx, 1), length = Arg32(ctx, 2);
    for (uint32_t i = 0; i < length; i++)
        GuestToHost(buffer)[i] = uint8_t(std::rand());
    Return(ctx, 0);
}
