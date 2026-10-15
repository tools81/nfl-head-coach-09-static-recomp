#pragma once

// XEX2 helpers for the installer (a C++ port of tools/xexdecrypt.py): title check, and decryption
// and decompression of a retail executable into the flat PE image HC09.exe loads.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hc09::setup
{
    constexpr uint32_t kHeadCoach09TitleId = 0x45410841;

    // EXECUTION_INFO title ID, or nothing when the data is not an XEX2 executable.
    std::optional<uint32_t> XexTitleId(const std::vector<uint8_t>& xex);

    // The retail XEX key is not shipped with this project; it is read from the XenonRecomp checkout
    // (XenonUtils/xex.h, Xex2RetailKey).
    std::optional<std::array<uint8_t, 16>> LoadRetailKey(const std::filesystem::path& xexHeader, std::wstring& error);

    bool DecryptXex(const std::vector<uint8_t>& xex, const std::array<uint8_t, 16>& retailKey, std::vector<uint8_t>& image,
        std::wstring& error);
}
