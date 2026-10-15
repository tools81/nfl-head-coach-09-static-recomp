#pragma once

// Helpers for kernel/XAM implementations: big-endian guest memory access, call
// arguments, and NT status codes.

#include <ppc_recomp_shared.h>

#include <cstring>
#include <string>

#include "memory.h"

namespace hc09
{
    template <typename T>
    T ByteSwap(T value)
    {
        static_assert(std::is_integral_v<T>);
        if constexpr (sizeof(T) == 1)
            return value;
        else if constexpr (sizeof(T) == 2)
            return T(__builtin_bswap16(uint16_t(value)));
        else if constexpr (sizeof(T) == 4)
            return T(__builtin_bswap32(uint32_t(value)));
        else
            return T(__builtin_bswap64(uint64_t(value)));
    }

    template <typename T>
    T Load(uint32_t address)
    {
        T value;
        std::memcpy(&value, GuestToHost(address), sizeof(T));
        return ByteSwap(value);
    }

    template <typename T>
    void Store(uint32_t address, T value)
    {
        value = ByteSwap(value);
        std::memcpy(GuestToHost(address), &value, sizeof(T));
    }

    inline const char* GuestCString(uint32_t address)
    {
        return address ? reinterpret_cast<const char*>(GuestToHost(address)) : nullptr;
    }

    // Reads a big-endian UTF-16 string of `count` characters (or up to NUL when count < 0).
    std::u16string LoadU16String(uint32_t address, int count = -1);

    // ANSI_STRING / UNICODE_STRING: { be16 Length; be16 MaximumLength; be32 Buffer; }
    std::string LoadAnsiString(uint32_t stringStruct);

    // Index of the first argument register is 3; arguments are r3..r10.
    inline uint32_t Arg32(const PPCContext& ctx, int index)
    {
        const PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        return regs[index]->u32;
    }

    inline uint64_t Arg64(const PPCContext& ctx, int index)
    {
        const PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        return regs[index]->u64;
    }

    inline void Return(PPCContext& ctx, uint64_t value) { ctx.r3.u64 = value; }

    // Aborts with a report of the guest call site. Used for unrecoverable guest states.
    [[noreturn]] void GuestFatal(const PPCContext& ctx, const char* format, ...);

    namespace status
    {
        constexpr uint32_t kSuccess = 0x00000000;
        constexpr uint32_t kTimeout = 0x00000102;
        constexpr uint32_t kPending = 0x00000103;
        constexpr uint32_t kObjectNameExists = 0x40000000;
        constexpr uint32_t kNoMoreFiles = 0x80000006;
        constexpr uint32_t kUnsuccessful = 0xC0000001;
        constexpr uint32_t kNotImplemented = 0xC0000002;
        constexpr uint32_t kInvalidHandle = 0xC0000008;
        constexpr uint32_t kInvalidParameter = 0xC000000D;
        constexpr uint32_t kNoSuchFile = 0xC000000F;
        constexpr uint32_t kEndOfFile = 0xC0000011;
        constexpr uint32_t kNoMemory = 0xC0000017;
        constexpr uint32_t kAccessDenied = 0xC0000022;
        constexpr uint32_t kBufferTooSmall = 0xC0000023;
        constexpr uint32_t kObjectTypeMismatch = 0xC0000024;
        constexpr uint32_t kObjectNameInvalid = 0xC0000033;
        constexpr uint32_t kObjectNameNotFound = 0xC0000034;
        constexpr uint32_t kObjectNameCollision = 0xC0000035;
        constexpr uint32_t kObjectPathNotFound = 0xC000003A;
        constexpr uint32_t kMutantNotOwned = 0xC0000046;
        constexpr uint32_t kSemaphoreLimitExceeded = 0xC0000047;
        constexpr uint32_t kNotSupported = 0xC00000BB;
        constexpr uint32_t kDirectoryNotEmpty = 0xC0000101;
        constexpr uint32_t kCannotDelete = 0xC0000121;
    }

    // Win32 error codes returned by XAM
    namespace error
    {
        constexpr uint32_t kSuccess = 0;
        constexpr uint32_t kFileNotFound = 2;
        constexpr uint32_t kAccessDenied = 5;
        constexpr uint32_t kInvalidHandle = 6;
        constexpr uint32_t kNoMoreFiles = 18;
        constexpr uint32_t kInvalidParameter = 87;
        constexpr uint32_t kInsufficientBuffer = 122;
        constexpr uint32_t kAlreadyExists = 183;
        constexpr uint32_t kIoPending = 997;
        constexpr uint32_t kFunctionFailed = 1627;
        constexpr uint32_t kNotLoggedOn = 1245;
        constexpr uint32_t kDeviceNotConnected = 1167;
        constexpr uint32_t kNoSuchUser = 1317;
        constexpr uint32_t kCancelled = 1223;
    }
}
