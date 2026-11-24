// hc09: the menus in the Barlow family (roadmap 12, the 2026 TV style).
//
// The game's menus are set in six TrueType fonts packed in qkl_interface.ast (Helvetica Neue cuts: HCHelvN65M,
// NCAAHelvN77BC, ...), which its font engine (FontFusion) rasterizes as it needs glyphs. tools/menu_fonts.py cuts the
// matching Barlow styles (SIL Open Font License, fonts/OFL.txt) down to the characters the game uses, each no larger
// than the font it replaces and named as it is; here they are put in its place in the archive as the game reads it.
// A font is only replaced where the disc has the expected one (its name at its place).
//
// MenuFonts (hc09.ini [Graphics], HC09_MENU_FONTS): on by default; off keeps the game's fonts.
#include "common/menu_fonts.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "kernel/vfs.h"
#include "vk/dev_env.h"

namespace
{
    constexpr const char* kArchive = "qkl_interface.ast";
    constexpr const char* kFolder = "fonts\\menus\\";

    struct Font
    {
        uint64_t offset;    // in the archive
        uint32_t size;      // the game's font's (the most bytes its replacement may take)
        const char* name;   // its name, and the replacement's file
    };
    constexpr Font kFonts[] = {
        { 0x0085A92C, 79942, "HCHelvN107EBC" }, { 0x0086E174, 80290, "HCHelvN65M" },   { 0x00881B1C, 89376, "NCAAHelvN57C" },
        { 0x0089783C, 88120, "NCAAHelvN75B" },  { 0x008AD074, 87348, "NCAAHelvN77BC" }, { 0x008C25AC, 88488, "NCAAHelvN87HC" },
    };

    bool Wanted()
    {
        if (const char* e = Hc09Env("HC09_MENU_FONTS"))
            return std::strcmp(e, "0") != 0 && _stricmp(e, "off") != 0;
        // The D3D12 renderer does not set the [Graphics] switches: read the ini here.
        wchar_t text[16] = {};
        const std::wstring ini = (std::filesystem::current_path() / L"hc09.ini").wstring();
        GetPrivateProfileStringW(L"Graphics", L"MenuFonts", L"", text, DWORD(std::size(text)), ini.c_str());
        return !(_wcsicmp(text, L"off") == 0 || _wcsicmp(text, L"0") == 0 || _wcsicmp(text, L"no") == 0 || _wcsicmp(text, L"false") == 0);
    }
}

namespace hc09::menu_fonts
{
    void Apply()
    {
        if (!Wanted())
            return;
        int replaced = 0;
        for (const Font& font : kFonts)
        {
            // The disc's font: a TrueType font with its name in it.
            std::vector<uint8_t> disc(font.size);
            if (!ReadDiscFile(kArchive, font.offset, disc.data(), font.size) || std::memcmp(disc.data(), "\0\1\0\0", 4) != 0 ||
                std::search(disc.begin(), disc.end(), font.name, font.name + std::strlen(font.name)) == disc.end())
            {
                std::fprintf(stderr, "[fonts] %s is not where it was expected in %s: kept\n", font.name, kArchive);
                continue;
            }
            std::ifstream file(std::string(kFolder) + font.name + ".ttf", std::ios::binary);
            std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
            if (bytes.size() < 12 || bytes.size() > font.size || std::memcmp(bytes.data(), "\0\1\0\0", 4) != 0)
            {
                std::fprintf(stderr, "[fonts] %s%s.ttf is missing or does not fit: the game's font is kept\n", kFolder, font.name);
                continue;
            }
            if (OverlayDiscFile(kArchive, font.offset, std::move(bytes)))
                ++replaced;
        }
        std::fprintf(stderr, "[fonts] %d of %zu menu fonts replaced\n", replaced, std::size(kFonts));
    }
}
