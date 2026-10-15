#include "xex.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <regex>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace hc09::setup
{
    namespace
    {
        constexpr uint32_t kFileFormatInfo = 0x000003FF;
        constexpr uint32_t kExecutionInfo = 0x00040006;

        uint32_t Be32(const std::vector<uint8_t>& d, size_t offset)
        {
            return offset + 4 > d.size() ? 0
                : (uint32_t(d[offset]) << 24) | (uint32_t(d[offset + 1]) << 16) | (uint32_t(d[offset + 2]) << 8) | d[offset + 3];
        }

        uint16_t Be16(const std::vector<uint8_t>& d, size_t offset)
        {
            return offset + 2 > d.size() ? 0 : uint16_t((d[offset] << 8) | d[offset + 1]);
        }

        std::optional<uint32_t> FindHeader(const std::vector<uint8_t>& d, uint32_t key)
        {
            const uint32_t count = Be32(d, 0x14);
            for (uint32_t i = 0; i < count && i < 256; i++)
                if (Be32(d, 0x18 + i * 8) == key)
                    return Be32(d, 0x18 + i * 8 + 4);
            return std::nullopt;
        }

        // AES-128 decryption in ECB (no IV) or CBC (zero IV) mode with Windows CNG.
        bool Aes(const std::array<uint8_t, 16>& key, bool cbc, const uint8_t* input, size_t size, uint8_t* output)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_KEY_HANDLE handle = nullptr;
            const wchar_t* mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
            const ULONG modeBytes = ULONG((wcslen(mode) + 1) * sizeof(wchar_t));
            bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
                BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                    reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)), modeBytes, 0)) &&
                BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, const_cast<uint8_t*>(key.data()), 16, 0));
            if (ok)
            {
                uint8_t iv[16] = {};
                ULONG written = 0;
                ok = BCRYPT_SUCCESS(BCryptDecrypt(handle, const_cast<uint8_t*>(input), ULONG(size), nullptr, cbc ? iv : nullptr,
                    cbc ? 16 : 0, output, ULONG(size), &written, 0));
            }
            if (handle)
                BCryptDestroyKey(handle);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
            return ok;
        }
    }

    std::optional<uint32_t> XexTitleId(const std::vector<uint8_t>& xex)
    {
        if (xex.size() < 0x18 || std::string(xex.begin(), xex.begin() + 4) != "XEX2")
            return std::nullopt;
        const auto info = FindHeader(xex, kExecutionInfo);
        if (!info)
            return std::nullopt;
        // media id, version, base version, title id, ...
        return Be32(xex, *info + 12);
    }

    std::optional<std::array<uint8_t, 16>> LoadRetailKey(const std::filesystem::path& xexHeader, std::wstring& error)
    {
        std::ifstream file(xexHeader);
        if (!file)
        {
            error = xexHeader.wstring() + L" not found (XenonRecomp must be cloned first).";
            return std::nullopt;
        }
        std::stringstream text;
        text << file.rdbuf();
        const std::string source = text.str();
        std::smatch match;
        if (!std::regex_search(source, match, std::regex(R"(Xex2RetailKey\[16\]\s*=\s*\{([^}]*)\})")))
        {
            error = L"Xex2RetailKey not found in " + xexHeader.wstring();
            return std::nullopt;
        }
        std::array<uint8_t, 16> key{};
        const std::string body = match[1].str();
        size_t count = 0;
        const std::regex hexByte(R"(0x([0-9A-Fa-f]{2}))");
        for (std::sregex_iterator it(body.begin(), body.end(), hexByte), end; it != end && count < 16; ++it)
            key[count++] = uint8_t(std::stoul((*it)[1].str(), nullptr, 16));
        if (count != 16)
        {
            error = L"Xex2RetailKey in " + xexHeader.wstring() + L" is malformed.";
            return std::nullopt;
        }
        return key;
    }

    bool DecryptXex(const std::vector<uint8_t>& xex, const std::array<uint8_t, 16>& retailKey, std::vector<uint8_t>& image,
        std::wstring& error)
    {
        const uint32_t peOffset = Be32(xex, 8), securityOffset = Be32(xex, 0x10);
        const auto formatInfo = FindHeader(xex, kFileFormatInfo);
        if (!formatInfo || peOffset >= xex.size())
        {
            error = L"The executable's XEX headers are invalid.";
            return false;
        }
        const uint32_t infoSize = Be32(xex, *formatInfo);
        const uint16_t encryption = Be16(xex, *formatInfo + 4), compression = Be16(xex, *formatInfo + 6);
        std::vector<uint8_t> payload(xex.begin() + peOffset, xex.end());

        if (encryption)
        {
            if (securityOffset + 0x160 > xex.size())
            {
                error = L"The executable's security info is truncated.";
                return false;
            }
            const uint8_t* wrapped = xex.data() + securityOffset + 0x150;
            const std::array<uint8_t, 16> devkitKey{};
            const size_t usable = payload.size() / 16 * 16;
            std::vector<uint8_t> plain(payload.size());
            bool decrypted = false;
            for (const auto& master : { retailKey, devkitKey })
            {
                std::array<uint8_t, 16> session{};
                if (!Aes(master, false, wrapped, 16, session.data()) || !Aes(session, true, payload.data(), usable, plain.data()))
                    continue;
                std::copy(payload.begin() + usable, payload.end(), plain.begin() + usable);
                decrypted = true;
                if (compression != 1 || (plain.size() >= 2 && plain[0] == 'M' && plain[1] == 'Z'))
                    break;
            }
            if (!decrypted)
            {
                error = L"AES decryption failed.";
                return false;
            }
            payload = std::move(plain);
        }

        if (compression == 0)
            image = std::move(payload);
        else if (compression == 1)
        {
            // Basic compression: (data size, zero size) block pairs follow the format info.
            image.clear();
            size_t source = 0;
            for (uint32_t i = 0; i < (infoSize - 8) / 8; i++)
            {
                const uint32_t dataSize = Be32(xex, *formatInfo + 8 + i * 8), zeroSize = Be32(xex, *formatInfo + 12 + i * 8);
                if (source + dataSize > payload.size())
                {
                    error = L"The executable's compressed data is truncated.";
                    return false;
                }
                image.insert(image.end(), payload.begin() + source, payload.begin() + source + dataSize);
                image.insert(image.end(), zeroSize, 0);
                source += dataSize;
            }
        }
        else
        {
            error = L"Unsupported XEX compression (only none and basic are supported).";
            return false;
        }
        if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z')
        {
            error = L"Decryption failed: the result is not a PE image. Is this a retail NFL Head Coach 09 disc?";
            return false;
        }
        return true;
    }
}
