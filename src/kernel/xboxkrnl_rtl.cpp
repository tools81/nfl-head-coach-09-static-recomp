// Rtl*: critical sections, strings, memory helpers, time conversion.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "kernel/guest.h"
#include "kernel/threads.h"

namespace hc09
{
    std::u16string LoadU16String(uint32_t address, int count)
    {
        std::u16string result;
        for (int i = 0; count < 0 || i < count; i++)
        {
            const char16_t c = char16_t(Load<uint16_t>(address + i * 2));
            if (count < 0 && c == 0)
                break;
            result.push_back(c);
        }
        return result;
    }

    std::string LoadAnsiString(uint32_t stringStruct)
    {
        const uint16_t length = Load<uint16_t>(stringStruct);
        const uint32_t buffer = Load<uint32_t>(stringStruct + 4);
        return buffer ? std::string(GuestCString(buffer), length) : std::string();
    }

    // ---- Critical sections ----------------------------------------------------------------
    //
    // X_RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10), be32 lock_count (0x10, -1 when free),
    // be32 recursion_count (0x14), be32 owning_thread (0x18). Same algorithm as the kernel:
    // lock_count is adjusted atomically in guest memory, so an uncontended enter or leave is
    // a single atomic operation. Contended waiters block on a sharded host wait table.

    namespace
    {
        constexpr uint32_t kLockCount = 0x10;
        constexpr uint32_t kRecursion = 0x14;
        constexpr uint32_t kOwner = 0x18;
        constexpr size_t kWaitShards = 64;

        std::atomic_ref<uint32_t> GuestWord(uint32_t address)
        {
            return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(GuestToHost(address)));
        }

        // Atomically adds `delta` to a big-endian guest int32; returns the new value.
        int32_t AtomicAdd(uint32_t address, int32_t delta)
        {
            auto word = GuestWord(address);
            uint32_t observed = word.load();
            for (;;)
            {
                const int32_t next = int32_t(ByteSwap(observed)) + delta;
                if (word.compare_exchange_weak(observed, ByteSwap(uint32_t(next))))
                    return next;
            }
        }

        // Ownership hand-offs to waiting threads, counted per critical section.
        struct WaitShard
        {
            std::mutex mutex;
            std::condition_variable condition;
            std::unordered_map<uint32_t, uint32_t> handoffs;
        };
        WaitShard g_waitShards[kWaitShards];

        WaitShard& ShardFor(uint32_t cs)
        {
            return g_waitShards[(cs >> 4) % kWaitShards];
        }

        void WaitForHandoff(uint32_t cs)
        {
            auto& shard = ShardFor(cs);
            std::unique_lock lock(shard.mutex);
            if (!shard.condition.wait_for(lock, std::chrono::seconds(3), [&] { return shard.handoffs.contains(cs); }))
            {
                // Diagnostic for deadlocks: who holds it, and who has waited for 3 s.
                std::fprintf(stderr, "[cs] thread 0x%X (kthread 0x%08X) waiting >3 s for 0x%08X: owner kthread 0x%08X, "
                    "recursion %d, lock count %d\n", CurrentThread()->id, CurrentThread()->kthread, cs,
                    Load<uint32_t>(cs + kOwner), int32_t(Load<uint32_t>(cs + kRecursion)), int32_t(Load<uint32_t>(cs + kLockCount)));
                shard.condition.wait(lock, [&] { return shard.handoffs.contains(cs); });
            }
            if (--shard.handoffs[cs] == 0)
                shard.handoffs.erase(cs);
        }

        void SignalHandoff(uint32_t cs)
        {
            auto& shard = ShardFor(cs);
            {
                std::lock_guard lock(shard.mutex);
                shard.handoffs[cs]++;
            }
            shard.condition.notify_all();
        }

        void TakeOwnership(uint32_t cs, uint32_t self)
        {
            Store<uint32_t>(cs + kOwner, self);
            Store<uint32_t>(cs + kRecursion, 1);
        }

        void EnterCriticalSection(uint32_t cs)
        {
            const uint32_t self = CurrentThread()->kthread;  // owning_thread holds a PKTHREAD
            if (AtomicAdd(cs + kLockCount, 1) != 0)
            {
                // Only this thread ever stores its own id as owner, so equality is reliable.
                if (Load<uint32_t>(cs + kOwner) == self)
                {
                    Store<uint32_t>(cs + kRecursion, Load<uint32_t>(cs + kRecursion) + 1);
                    return;
                }
                WaitForHandoff(cs);
            }
            TakeOwnership(cs, self);
        }

        bool TryEnterCriticalSection(uint32_t cs)
        {
            const uint32_t self = CurrentThread()->kthread;  // owning_thread holds a PKTHREAD
            uint32_t expected = ByteSwap(uint32_t(-1));
            if (GuestWord(cs + kLockCount).compare_exchange_strong(expected, 0))
            {
                TakeOwnership(cs, self);
                return true;
            }
            if (Load<uint32_t>(cs + kOwner) == self)
            {
                AtomicAdd(cs + kLockCount, 1);
                Store<uint32_t>(cs + kRecursion, Load<uint32_t>(cs + kRecursion) + 1);
                return true;
            }
            return false;
        }

        void LeaveCriticalSection(uint32_t cs)
        {
            const uint32_t recursion = Load<uint32_t>(cs + kRecursion) - 1;
            Store<uint32_t>(cs + kRecursion, recursion);
            if (recursion != 0)
            {
                AtomicAdd(cs + kLockCount, -1);
                return;
            }
            Store<uint32_t>(cs + kOwner, 0);
            if (AtomicAdd(cs + kLockCount, -1) >= 0)
                SignalHandoff(cs);  // someone incremented lock_count and is waiting
        }

        void InitializeCriticalSection(uint32_t cs)
        {
            std::memset(GuestToHost(cs), 0, 0x1C);
            GuestToHost(cs)[0] = 1;  // SynchronizationEvent-type header, as the kernel does
            Store<int32_t>(cs + kLockCount, -1);
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    InitializeCriticalSection(Arg32(ctx, 0));
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    InitializeCriticalSection(Arg32(ctx, 0));
    Return(ctx, 0);
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    EnterCriticalSection(Arg32(ctx, 0));
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    Return(ctx, TryEnterCriticalSection(Arg32(ctx, 0)) ? 1 : 0);
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    LeaveCriticalSection(Arg32(ctx, 0));
}

PPC_FUNC(__imp__RtlInitAnsiString)
{
    const uint32_t dest = Arg32(ctx, 0), source = Arg32(ctx, 1);
    const uint16_t length = source ? uint16_t(std::strlen(GuestCString(source))) : 0;
    Store<uint16_t>(dest, length);
    Store<uint16_t>(dest + 2, source ? length + 1 : 0);
    Store<uint32_t>(dest + 4, source);
}

PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    // (dest, dest_bytes, out_bytes_written, source, source_bytes): plain byte widening
    const uint32_t dest = Arg32(ctx, 0), destBytes = Arg32(ctx, 1), written = Arg32(ctx, 2);
    const uint32_t source = Arg32(ctx, 3), sourceBytes = Arg32(ctx, 4);
    const uint32_t count = std::min(destBytes / 2, sourceBytes);
    for (uint32_t i = 0; i < count; i++)
        Store<uint16_t>(dest + i * 2, uint8_t(GuestToHost(source)[i]));
    if (written)
        Store<uint32_t>(written, count * 2);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    // (dest, dest_bytes, out_bytes_written, source, source_bytes)
    const uint32_t dest = Arg32(ctx, 0), destBytes = Arg32(ctx, 1), written = Arg32(ctx, 2);
    const uint32_t source = Arg32(ctx, 3), sourceBytes = Arg32(ctx, 4);
    const uint32_t count = std::min(destBytes, sourceBytes / 2);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint16_t c = Load<uint16_t>(source + i * 2);
        GuestToHost(dest)[i] = c < 0x100 ? uint8_t(c) : '?';
    }
    if (written)
        Store<uint32_t>(written, count);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__RtlUpcaseUnicodeChar)
{
    Return(ctx, uint16_t(std::towupper(wint_t(Arg32(ctx, 0) & 0xFFFF))));
}

PPC_FUNC(__imp__RtlCompareStringN)
{
    // (string1, length1, string2, length2, case_insensitive)
    const char* a = GuestCString(Arg32(ctx, 0));
    const char* b = GuestCString(Arg32(ctx, 2));
    const uint32_t lengthA = Arg32(ctx, 1), lengthB = Arg32(ctx, 3);
    const bool insensitive = Arg32(ctx, 4) != 0;
    const uint32_t n = std::min(lengthA, lengthB);
    for (uint32_t i = 0; i < n; i++)
    {
        int ca = uint8_t(a[i]), cb = uint8_t(b[i]);
        if (insensitive)
            ca = std::toupper(ca), cb = std::toupper(cb);
        if (ca != cb)
            return Return(ctx, uint32_t(ca - cb));
    }
    Return(ctx, uint32_t(int32_t(lengthA) - int32_t(lengthB)));
}

PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    // (source, length, pattern) -> number of bytes that matched
    const uint32_t source = Arg32(ctx, 0), length = Arg32(ctx, 1), pattern = Arg32(ctx, 2);
    uint32_t matched = 0;
    while (matched + 4 <= length && Load<uint32_t>(source + matched) == pattern)
        matched += 4;
    Return(ctx, matched);
}

PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    const uint32_t dest = Arg32(ctx, 0), length = Arg32(ctx, 1), pattern = Arg32(ctx, 2);
    for (uint32_t offset = 0; offset + 4 <= length; offset += 4)
        Store<uint32_t>(dest + offset, pattern);
}

PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    const uint64_t time = Load<uint64_t>(Arg32(ctx, 0));
    const uint32_t fields = Arg32(ctx, 1);
    FILETIME ft{ DWORD(time), DWORD(time >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    const uint16_t values[] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, st.wDayOfWeek };
    for (int i = 0; i < 8; i++)
        Store<uint16_t>(fields + i * 2, values[i]);
}

PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    const uint32_t fields = Arg32(ctx, 0), out = Arg32(ctx, 1);
    SYSTEMTIME st{};
    st.wYear = Load<uint16_t>(fields + 0);
    st.wMonth = Load<uint16_t>(fields + 2);
    st.wDay = Load<uint16_t>(fields + 4);
    st.wHour = Load<uint16_t>(fields + 6);
    st.wMinute = Load<uint16_t>(fields + 8);
    st.wSecond = Load<uint16_t>(fields + 10);
    st.wMilliseconds = Load<uint16_t>(fields + 12);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft))
        return Return(ctx, 0);
    Store<uint64_t>(out, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    Return(ctx, 1);
}

PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    const uint32_t code = Arg32(ctx, 0);
    uint32_t result;
    switch (code)
    {
    case status::kSuccess: result = error::kSuccess; break;
    case status::kPending: result = error::kIoPending; break;
    case status::kNoSuchFile:
    case status::kObjectNameNotFound: result = error::kFileNotFound; break;
    case status::kObjectPathNotFound: result = 3; break;  // ERROR_PATH_NOT_FOUND
    case status::kAccessDenied: result = error::kAccessDenied; break;
    case status::kInvalidHandle: result = error::kInvalidHandle; break;
    case status::kInvalidParameter: result = error::kInvalidParameter; break;
    case status::kNoMemory: result = 8; break;  // ERROR_NOT_ENOUGH_MEMORY
    case status::kEndOfFile: result = 38; break;  // ERROR_HANDLE_EOF
    case status::kNoMoreFiles: result = error::kNoMoreFiles; break;
    case status::kObjectNameCollision: result = error::kAlreadyExists; break;
    case status::kBufferTooSmall: result = error::kInsufficientBuffer; break;
    case status::kTimeout: result = 1460; break;  // ERROR_TIMEOUT
    default: result = (code & 0xFFFF0000) == 0x80070000 ? (code & 0xFFFF) : 317; break;  // ERROR_MR_MID_NOT_FOUND
    }
    Return(ctx, result);
}
