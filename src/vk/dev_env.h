#pragma once
// Hc09Env(name): the runtime's getenv for its HC09_* switches.
//
// The development build (CMake HC09_DEV_TOOLS=ON, the default) reads every switch. The release build
// (HC09_DEV_TOOLS=OFF, docs/HANDOFF_M11.md step 1) reads only the settings hc09.ini and the launcher set
// (settings.cpp kKnown) and HC09_CONFIG: for any other name Hc09Env is a constant nullptr, so the code behind the
// switch and the name itself are left out of hc09.exe (not merely off). The name must be a string literal: the
// release build checks it at compile time.
#include <cstdlib>
#include <string_view>
#include <type_traits>

#ifndef HC09_DEV_TOOLS
#define HC09_DEV_TOOLS 1
#endif

namespace hc09env {

// The switches of the release build: hc09.ini's settings (settings.cpp kKnown) and the ini's own path.
constexpr std::string_view kRelease[] = {
    "HC09_CONFIG",     "HC09_CACHE_DIR",  "HC09_GPU_LIVE",   "HC09_FULLSCREEN", "HC09_VSYNC",
    "HC09_GPU_EXACT",  "HC09_FRAMERATE",  "HC09_RENDERER",   "HC09_SCALE",      "HC09_NATIVE_MSAA",
    "HC09_AUDIO_OUT",  "HC09_AUDIO_VOLUME", "HC09_HOST_INPUT", "HC09_PAD_API",  "HC09_GLYPHS",
    "HC09_PAD_NAME",   "HC09_SAVES_DIR",  "HC09_GPU",        "HC09_ANISOTROPY", "HC09_SSAO",       "HC09_SSR",
    "HC09_UPSCALE",    "HC09_SHARPEN",    "HC09_ASPECT",     "HC09_SMOOTH_MOTION", "HC09_BLOOM",
    "HC09_GRADE",      "HC09_DOF",        "HC09_HDR",        "HC09_HDR_PEAK",   "HC09_HDR_PAPER_WHITE",
    "HC09_CROWD",      "HC09_TURF",       "HC09_PLAYERS",    "HC09_SHARP_TEXT",
};

constexpr bool Release(std::string_view name)
{
    for (std::string_view r : kRelease)
        if (r == name) return true;
    return false;
}

}  // namespace hc09env

#if HC09_DEV_TOOLS
#define Hc09Env(name) getenv(name)
#else
// A constant condition: clang emits only the taken arm, so a left-out name is not even a string in the binary.
#define Hc09Env(name) \
    (std::bool_constant<::hc09env::Release(name)>::value ? getenv(name) : static_cast<const char*>(nullptr))
#endif
