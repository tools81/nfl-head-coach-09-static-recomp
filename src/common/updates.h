#pragma once

// Release checks against the project's GitHub repository (used by HC09.exe at launch and by the
// installer to pick the version to install).

#include <optional>
#include <string>

namespace hc09
{
    // owner/name on GitHub.
    constexpr char kRepository[] = "tools81/nfl-head-coach-09-static-recomp";

    // Tag of the latest published release (e.g. "v0.6.0"), or nothing when it cannot be fetched
    // within `timeoutMs` (offline, no release yet, rate limited).
    std::optional<std::string> LatestReleaseTag(int timeoutMs);

    // True when `tag` ("v1.2.3" or "1.2.3") is a newer version than `current` ("1.2.0").
    bool IsNewerVersion(const std::string& tag, const std::string& current);
}
