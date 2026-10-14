#include "kernel/format.h"

#include <bit>
#include <cstdio>

#include "kernel/guest.h"

namespace hc09
{
    GuestArgs GuestArgs::FromRegisters(const PPCContext& ctx, int firstIndex)
    {
        GuestArgs args;
        args.ctx_ = &ctx;
        args.index_ = firstIndex;
        return args;
    }

    GuestArgs GuestArgs::FromVaList(uint32_t vaList)
    {
        GuestArgs args;
        args.vaList_ = vaList;
        return args;
    }

    uint64_t GuestArgs::Next64()
    {
        const int index = index_++;
        if (ctx_ == nullptr)
            return Load<uint64_t>(vaList_ + index * 8);
        if (index < 8)
            return Arg64(*ctx_, index);
        return Load<uint64_t>(ctx_->r1.u32 + 0x50 + (index - 8) * 8);
    }

    double GuestArgs::NextDouble()
    {
        return std::bit_cast<double>(Next64());
    }

    std::string FormatGuestString(const char* format, GuestArgs args)
    {
        std::string out;
        if (format == nullptr)
            return out;

        for (const char* p = format; *p; )
        {
            if (*p != '%')
            {
                out.push_back(*p++);
                continue;
            }
            const char* specStart = p++;
            if (*p == '%')
            {
                out.push_back('%');
                p++;
                continue;
            }

            // flags, width, precision ('*' takes an argument)
            std::string spec = "%";
            while (*p && std::strchr("-+ #0", *p))
                spec.push_back(*p++);
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next32()));
                p++;
            }
            while (*p >= '0' && *p <= '9')
                spec.push_back(*p++);
            if (*p == '.')
            {
                spec.push_back(*p++);
                if (*p == '*')
                {
                    spec += std::to_string(int32_t(args.Next32()));
                    p++;
                }
                while (*p >= '0' && *p <= '9')
                    spec.push_back(*p++);
            }

            // length modifiers
            bool wide64 = false, wideChar = false, narrowChar = false;
            for (;;)
            {
                if (*p == 'l' && p[1] == 'l') { wide64 = true; p += 2; }
                else if (*p == 'I' && p[1] == '6' && p[2] == '4') { wide64 = true; p += 3; }
                else if (*p == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
                else if (*p == 'l' || *p == 'w') { wideChar = true; p++; }
                else if (*p == 'h') { narrowChar = true; p++; }
                else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'L') { p++; }
                else break;
            }

            const char conversion = *p ? *p++ : 0;
            char buffer[512];
            switch (conversion)
            {
            case 'd':
            case 'i':
                if (wide64)
                    std::snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)args.Next64());
                else
                    std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int32_t(args.Next32()));
                out += buffer;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                if (wide64)
                    std::snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), (unsigned long long)args.Next64());
                else
                    std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), args.Next32());
                out += buffer;
                break;
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "%08X", args.Next32());
                out += buffer;
                break;
            case 'c':
            case 'C':
                out.push_back(char(args.Next32()));
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
                std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), args.NextDouble());
                out += buffer;
                break;
            case 's':
            case 'S':
            {
                // %s is narrow unless 'l'/'w'; %S is wide unless 'h' (MSVC semantics).
                const bool wide = (conversion == 's') ? wideChar : !narrowChar;
                const uint32_t address = args.Next32();
                std::string text;
                if (address == 0)
                    text = "(null)";
                else if (wide)
                    for (char16_t c : LoadU16String(address))
                        text.push_back(c < 0x80 ? char(c) : '?');
                else
                    text = GuestCString(address);
                // Precision limits the characters taken; width pads.
                std::string textSpec = spec + "s";
                const size_t needed = std::snprintf(nullptr, 0, textSpec.c_str(), text.c_str());
                std::string formatted(needed, '\0');
                std::snprintf(formatted.data(), needed + 1, textSpec.c_str(), text.c_str());
                out += formatted;
                break;
            }
            case 'n':
                Store<uint32_t>(args.Next32(), uint32_t(out.size()));
                break;
            default:
                out.append(specStart, p);  // unknown conversion: copy it through
                break;
            }
        }
        return out;
    }
}
