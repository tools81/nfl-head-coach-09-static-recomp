#include "common/settings.h"

#include <windows.h>

namespace hc09
{
    namespace
    {
        constexpr wchar_t kSection[] = L"HC09";
    }

    std::filesystem::path SettingsPath(const std::filesystem::path& projectFolder)
    {
        return projectFolder / L"hc09.ini";
    }

    Settings LoadSettings(const std::filesystem::path& projectFolder)
    {
        const std::wstring file = SettingsPath(projectFolder).wstring();
        Settings settings;
        wchar_t iso[MAX_PATH * 2] = {};
        GetPrivateProfileStringW(kSection, L"IsoPath", L"", iso, DWORD(std::size(iso)), file.c_str());
        settings.isoPath = iso;
        settings.autoUpdate = GetPrivateProfileIntW(kSection, L"AutoUpdate", 0, file.c_str()) != 0;
        return settings;
    }

    bool SaveSettings(const std::filesystem::path& projectFolder, const Settings& settings)
    {
        const std::wstring file = SettingsPath(projectFolder).wstring();
        return WritePrivateProfileStringW(kSection, L"IsoPath", settings.isoPath.c_str(), file.c_str()) &&
            WritePrivateProfileStringW(kSection, L"AutoUpdate", settings.autoUpdate ? L"1" : L"0", file.c_str());
    }
}
