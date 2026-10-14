#pragma once

#include <ppc_recomp_shared.h>

#include <cstdint>
#include <string>

namespace hc09
{
    // Sequential reader over guest variadic arguments. Each argument occupies one 8-byte
    // big-endian slot: integers/pointers in the low word, doubles in the whole slot.
    class GuestArgs
    {
    public:
        // Arguments passed in registers starting at r(3 + firstIndex), continuing onto the
        // caller's stack (parameter save area at r1 + 0x50) past r10.
        static GuestArgs FromRegisters(const PPCContext& ctx, int firstIndex);
        // A guest va_list: a pointer to consecutive 8-byte slots.
        static GuestArgs FromVaList(uint32_t vaList);

        uint32_t Next32() { return uint32_t(Next64()); }
        uint64_t Next64();
        double NextDouble();

    private:
        const PPCContext* ctx_ = nullptr;
        uint32_t vaList_ = 0;
        int index_ = 0;
    };

    // MSVC-compatible printf formatting of a guest format string and arguments.
    std::string FormatGuestString(const char* format, GuestArgs args);
}
