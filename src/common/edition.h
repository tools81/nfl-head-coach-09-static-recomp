#pragma once

// Which game an executable runs. HC09.exe is the original; HC27.exe (built with
// HC09_EDITION_HC27) is the same game with a modern roster and coaching staffs, built by
// tools/roster_import into the hc27 folder: hc27\ROSTER replaces the disc's default roster, and
// replacement images in hc27\textures take precedence over textures\. HC27 keeps its own saves.

#include <string>

namespace hc09::edition
{
#ifdef HC09_EDITION_HC27
    inline constexpr const char* kName = "HC27";
    inline constexpr const char* kTitle = "NFL Head Coach 27";
    inline constexpr const wchar_t* kWindowTitle = L"NFL Head Coach 27";
    inline constexpr const char* kUserData = "build/userdata-hc27";
    inline constexpr const wchar_t* kModFolder = L"hc27";
#else
    inline constexpr const char* kName = "HC09";
    inline constexpr const char* kTitle = "NFL Head Coach 09";
    inline constexpr const wchar_t* kWindowTitle = L"NFL Head Coach 09";
    inline constexpr const char* kUserData = "build/userdata";
    inline constexpr const wchar_t* kModFolder = nullptr;
#endif

    // Installs the edition's disc overrides (HC27: the default roster). Call after the VFS is
    // initialized; on failure returns false with a message for the user.
    bool Apply(std::string& error);

#ifdef HC09_EDITION_HC27
    // An asset name holding a team's name the roster changed (e.g. "CAMPUS_COMMANDERS"; not the
    // name alone): rewritten in place with the team's name on the disc instead. Returns the
    // string as it was, to put back afterwards (empty when nothing changed).
    std::string RenameTeamAsset(char* name);
#endif
}
