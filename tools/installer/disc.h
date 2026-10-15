#pragma once

// Minimal Xbox 360 XDVDFS (XGD) reader: finds and extracts files from a game-partition ISO
// (a C++ port of tools/xdvdfs.py).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace hc09::setup
{
    struct DiscFile
    {
        uint64_t offset = 0;  // byte offset in the ISO
        uint64_t size = 0;
    };

    // `name` is a path inside the disc, '/'-separated and case-insensitive (e.g. "default.xex").
    std::optional<DiscFile> FindDiscFile(const std::filesystem::path& iso, const std::string& name, std::wstring& error);
    bool ExtractDiscFile(const std::filesystem::path& iso, const DiscFile& file, const std::filesystem::path& destination,
        std::wstring& error);
}
