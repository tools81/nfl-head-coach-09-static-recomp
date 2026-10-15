#include "disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>

namespace hc09::setup
{
    namespace
    {
        constexpr uint64_t kSector = 0x800;
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
        // Game-partition offsets: raw partition, XGD1, XGD2, XGD3.
        constexpr uint64_t kPartitionOffsets[] = { 0, 0x18300000, 0xFD90000, 0x2080000 };

        uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        bool ReadAt(std::ifstream& file, uint64_t offset, void* buffer, size_t size)
        {
            file.clear();
            file.seekg(std::streamoff(offset));
            file.read(static_cast<char*>(buffer), std::streamsize(size));
            return size_t(file.gcount()) == size;
        }

        // Directory tables are binary trees of entries: left/right child offsets (in dwords),
        // start sector, size, attributes, name length, name.
        std::optional<DiscFile> Search(std::ifstream& file, uint64_t base, uint32_t sector, uint32_t size,
            const std::string& prefix, const std::string& wanted, int depth)
        {
            if (size == 0 || depth > 32)
                return std::nullopt;
            std::vector<uint8_t> table(size);
            if (!ReadAt(file, base + sector * kSector, table.data(), size))
                return std::nullopt;
            std::vector<uint32_t> stack{ 0 };
            std::set<uint32_t> seen;
            while (!stack.empty())
            {
                const uint32_t offset = stack.back();
                stack.pop_back();
                if (!seen.insert(offset).second || offset + 14 > table.size())
                    continue;
                const uint8_t* entry = table.data() + offset;
                const uint16_t left = Le16(entry), right = Le16(entry + 2);
                if (left == 0xFFFF)  // padding
                    continue;
                const uint32_t start = Le32(entry + 4), length = Le32(entry + 8);
                const uint8_t attributes = entry[12], nameLength = entry[13];
                if (offset + 14 + nameLength > table.size())
                    continue;
                const std::string name(reinterpret_cast<const char*>(entry + 14), nameLength);
                const std::string path = Lower(prefix.empty() ? name : prefix + "/" + name);
                if (attributes & 0x10)
                {
                    if (wanted.compare(0, path.size() + 1, path + "/") == 0)
                        if (auto found = Search(file, base, start, length, path, wanted, depth + 1))
                            return found;
                }
                else if (path == wanted)
                {
                    return DiscFile{ base + start * kSector, length };
                }
                if (left)
                    stack.push_back(uint32_t(left) * 4);
                if (right)
                    stack.push_back(uint32_t(right) * 4);
            }
            return std::nullopt;
        }
    }

    std::optional<DiscFile> FindDiscFile(const std::filesystem::path& iso, const std::string& name, std::wstring& error)
    {
        std::ifstream file(iso, std::ios::binary);
        if (!file)
        {
            error = L"The file cannot be opened.";
            return std::nullopt;
        }
        for (const uint64_t base : kPartitionOffsets)
        {
            uint8_t descriptor[28];
            if (!ReadAt(file, base + 32 * kSector, descriptor, sizeof(descriptor)) ||
                std::memcmp(descriptor, kMagic, 20) != 0)
                continue;
            if (auto found = Search(file, base, Le32(descriptor + 20), Le32(descriptor + 24), "", Lower(name), 0))
                return found;
            error = L"The disc image does not contain " + std::wstring(name.begin(), name.end()) + L".";
            return std::nullopt;
        }
        error = L"This is not an Xbox 360 game disc image (no XDVDFS volume found).";
        return std::nullopt;
    }

    bool ExtractDiscFile(const std::filesystem::path& iso, const DiscFile& file, const std::filesystem::path& destination,
        std::wstring& error)
    {
        std::ifstream input(iso, std::ios::binary);
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!input || !output)
        {
            error = L"Cannot open " + (input ? destination.wstring() : iso.wstring());
            return false;
        }
        std::vector<char> buffer(1 << 20);
        input.seekg(std::streamoff(file.offset));
        for (uint64_t remaining = file.size; remaining > 0;)
        {
            const size_t chunk = size_t(std::min<uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), std::streamsize(chunk));
            if (size_t(input.gcount()) != chunk)
            {
                error = L"The disc image is truncated.";
                return false;
            }
            output.write(buffer.data(), std::streamsize(chunk));
            remaining -= chunk;
        }
        return bool(output);
    }
}
