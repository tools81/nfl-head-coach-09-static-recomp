// Kernel-exported C runtime string formatting: sprintf, _snprintf, _vsnprintf, DbgPrint.

#include <cstdio>

#include "kernel/format.h"
#include "kernel/guest.h"

namespace hc09
{
    namespace
    {
        // MSVC _snprintf semantics: returns -1 (and does not terminate) when the output
        // does not fit; otherwise writes the terminator if there is room.
        int32_t WriteBounded(uint32_t buffer, uint32_t count, const std::string& text)
        {
            char* dest = reinterpret_cast<char*>(GuestToHost(buffer));
            if (text.size() > count)
            {
                std::memcpy(dest, text.data(), count);
                return -1;
            }
            std::memcpy(dest, text.data(), text.size());
            if (text.size() < count)
                dest[text.size()] = '\0';
            return int32_t(text.size());
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__sprintf)
{
    // (buffer, format, ...)
    const std::string text = FormatGuestString(GuestCString(Arg32(ctx, 1)), GuestArgs::FromRegisters(ctx, 2));
    char* dest = reinterpret_cast<char*>(GuestToHost(Arg32(ctx, 0)));
    std::memcpy(dest, text.c_str(), text.size() + 1);
    Return(ctx, uint32_t(text.size()));
}

PPC_FUNC(__imp___snprintf)
{
    // (buffer, count, format, ...)
    const std::string text = FormatGuestString(GuestCString(Arg32(ctx, 2)), GuestArgs::FromRegisters(ctx, 3));
    Return(ctx, uint32_t(WriteBounded(Arg32(ctx, 0), Arg32(ctx, 1), text)));
}

PPC_FUNC(__imp___vsnprintf)
{
    // (buffer, count, format, va_list)
    const std::string text = FormatGuestString(GuestCString(Arg32(ctx, 2)), GuestArgs::FromVaList(Arg32(ctx, 3)));
    Return(ctx, uint32_t(WriteBounded(Arg32(ctx, 0), Arg32(ctx, 1), text)));
}

PPC_FUNC(__imp__DbgPrint)
{
    // (format, ...)
    const std::string text = FormatGuestString(GuestCString(Arg32(ctx, 0)), GuestArgs::FromRegisters(ctx, 1));
    std::fprintf(stderr, "[DbgPrint] %s%s", text.c_str(), !text.empty() && text.back() == '\n' ? "" : "\n");
    Return(ctx, 0);
}
