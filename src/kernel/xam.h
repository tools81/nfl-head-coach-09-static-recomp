#pragma once

// Shared XAM helpers.

#include <cstdint>

namespace hc09
{
    // XOVERLAPPED: be32 InternalLow (result), InternalHigh (length), InternalContext,
    // hEvent, pCompletionRoutine, dwCompletionContext, dwExtendedError.
    // Completes an asynchronous XAM operation: stores the result and signals the event.
    void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0, uint32_t extendedError = 0);

    // Completes synchronously, or through the overlapped structure when one is given.
    // Returns what the API should return: the result itself, or ERROR_IO_PENDING.
    uint32_t CompleteRequest(uint32_t overlapped, uint32_t result, uint32_t length = 0);

    // System notifications (XN_*). Queued to every listener whose area mask matches.
    void BroadcastNotification(uint32_t id, uint32_t param);

    constexpr uint32_t kXnSysUi = 0x00000009;
    constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
    constexpr uint32_t kXnSysStorageDevicesChanged = 0x0000000B;
    constexpr uint32_t kXnSysInputDevicesChanged = 0x00000012;

    // The single local profile.
    constexpr uint64_t kLocalXuid = 0xE000000000000001ull;
    constexpr const char* kLocalGamertag = "Player";
}
