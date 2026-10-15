#pragma once

// Per-install settings shared by HC09.exe and HC09Setup.exe, stored as hc09.ini in the project
// folder (written by the installer).

#include <filesystem>
#include <string>

namespace hc09
{
    struct Settings
    {
        std::wstring isoPath;     // the user's NFL Head Coach 09 disc image
        bool autoUpdate = false;  // check GitHub for a newer release at launch
    };

    std::filesystem::path SettingsPath(const std::filesystem::path& projectFolder);
    Settings LoadSettings(const std::filesystem::path& projectFolder);
    bool SaveSettings(const std::filesystem::path& projectFolder, const Settings& settings);
}
