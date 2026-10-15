#include "common/updates.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace hc09
{
    namespace
    {
        struct Handle
        {
            HINTERNET value = nullptr;
            ~Handle()
            {
                if (value)
                    WinHttpCloseHandle(value);
            }
        };

        std::vector<int> ParseVersion(const std::string& text)
        {
            std::vector<int> parts;
            size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
            while (i < text.size())
            {
                int value = 0;
                bool digits = false;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                {
                    value = value * 10 + (text[i++] - '0');
                    digits = true;
                }
                if (!digits)
                    break;
                parts.push_back(value);
                if (i >= text.size() || text[i] != '.')
                    break;
                i++;
            }
            return parts;
        }
    }

    std::optional<std::string> LatestReleaseTag(int timeoutMs)
    {
        Handle session{ WinHttpOpen(L"HC09Recomp-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0) };
        if (!session.value)
            return std::nullopt;
        WinHttpSetTimeouts(session.value, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        Handle connection{ WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) };
        if (!connection.value)
            return std::nullopt;
        wchar_t path[256];
        std::swprintf(path, std::size(path), L"/repos/%hs/releases/latest", kRepository);
        Handle request{ WinHttpOpenRequest(connection.value, L"GET", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
        if (!request.value)
            return std::nullopt;
        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
        if (!WinHttpSendRequest(request.value, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
            return std::nullopt;
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200)
            return std::nullopt;

        std::string body;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.value, &available) || available == 0)
                break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request.value, chunk.data(), available, &read) || read == 0)
                break;
            body.append(chunk.data(), read);
            if (body.size() > (1 << 20))
                break;
        }

        // The response is a JSON object; only "tag_name" is needed.
        const size_t key = body.find("\"tag_name\"");
        if (key == std::string::npos)
            return std::nullopt;
        const size_t open = body.find('"', body.find(':', key) + 1);
        const size_t close = open == std::string::npos ? open : body.find('"', open + 1);
        if (close == std::string::npos)
            return std::nullopt;
        return body.substr(open + 1, close - open - 1);
    }

    bool IsNewerVersion(const std::string& tag, const std::string& current)
    {
        std::vector<int> a = ParseVersion(tag), b = ParseVersion(current);
        if (a.empty())
            return false;
        a.resize(std::max(a.size(), b.size()));
        b.resize(a.size());
        return a > b;
    }
}
