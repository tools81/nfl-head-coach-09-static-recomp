// hc09: the afternoon wearing on (roadmap 16); see vk_sunset.h.
#include "vk_sunset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../dev_env.h"
#include "common/broadcast_state.h"
#include "common/field_surface.h"
#include "common/weather.h"

namespace hc09vk {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDegrees = kPi / 180.0f;

// "14:03" or ":42" as seconds (-1: not a clock).
int ClockSeconds(const std::string& clock)
{
    int m = 0, s = 0;
    if (clock.empty()) return -1;
    if (clock[0] == ':') return std::sscanf(clock.c_str() + 1, "%d", &s) == 1 ? s : -1;
    return std::sscanf(clock.c_str(), "%d:%d", &m, &s) == 2 ? m * 60 + s : -1;
}

// "1ST" .. "4TH" as 0 .. 3, "OT" as 4 (-1: not known).
int Quarter(const std::string& quarter)
{
    if (quarter.empty()) return -1;
    if (quarter[0] >= '1' && quarter[0] <= '4') return quarter[0] - '1';
    if (quarter.find("OT") != std::string::npos) return 4;
    return -1;
}

// The sun's height at a time of day (seconds after midnight): a stadium at 41 degrees north in early December
// (declination -21 degrees), the sun at its highest at 11:50 by the clock.
float SunElevation(float seconds)
{
    const float latitude = 41.0f * kDegrees, declination = -21.0f * kDegrees;
    const float hourAngle = (seconds / 3600.0f - (11.0f + 50.0f / 60.0f)) * 15.0f * kDegrees;
    const float s = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    return std::asin(std::clamp(s, -1.0f, 1.0f));
}

}  // namespace

void Sunset::BeginFrame(uint64_t frame)
{
    if (frame == frame_) return;
    frame_ = frame;
    active_ = false;
    static const bool off = [] { const char* e = Hc09Env("HC09_SUNSET"); return e && !std::strcmp(e, "0"); }();
    static const int forced = [] {
        const char* e = Hc09Env("HC09_SUNSET_TIME");
        int h = 0, m = 0;
        return e && std::sscanf(e, "%d:%d", &h, &m) >= 1 ? (h * 60 + m) * 60 : -1;
    }();
    if (off || !known_) return;
    // By day under an open sky (rain and snow have no sun; the floodlights light the night).
    if (hc09::g_nightGame.load() || hc09::g_domed.load() || hc09::g_weather.load() != int(hc09::Weather::Clear)) return;

    float seconds = float(forced);
    if (forced < 0)
    {
        const int kickoff = hc09::g_kickoffMinutes.load();
        if (kickoff <= 0) return;
        const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
        const int quarter = Quarter(board.quarter), clock = ClockSeconds(board.clock);
        static uint64_t shown = 0;
        if (Hc09Env("HC09_SUNSET_LOG") && frame >= shown + 600)
        {
            shown = frame;
            std::fprintf(stderr, "[sunset] score bug: quarter \"%s\" (%d), clock \"%s\" (%d)\n", board.quarter.c_str(), quarter, board.clock.c_str(), clock);
        }
        if (quarter < 0 || clock < 0) return;
        // A new game: its quarters' length is learned again (the longest clock it shows).
        const uint32_t game = hc09::g_gamesStarted.load();
        if (game != game_id_) { game_id_ = game; quarterLength_ = 0; }
        quarterLength_ = std::max(quarterLength_, clock);
        const float length = float(std::max(quarterLength_, 60));
        // A broadcast's three hours over the four quarters, and twenty minutes of halftime.
        const float played = std::min(4.0f, float(quarter) + std::clamp(1.0f - float(clock) / length, 0.0f, 1.0f));
        seconds = float(kickoff) * 60.0f + played / 4.0f * 3.0f * 3600.0f + (quarter >= 2 ? 20.0f * 60.0f : 0.0f);
    }
    // The game's own sun stands for the early afternoon; ours only takes over once it is lower (a kickoff at 1 pm
    // starts with the game's own light).
    // No lower than 5 degrees: the field stays lit enough to play on.
    elevation_ = std::max(SunElevation(seconds), 5.0f * kDegrees);
    const float gameElevation = std::asin(std::clamp(std::fabs(game_[0][1]) / std::max(1e-6f, std::sqrt(game_[0][0] * game_[0][0] +
                                                     game_[0][1] * game_[0][1] + game_[0][2] * game_[0][2])), 0.0f, 1.0f));
    elevation_ = std::min(elevation_, gameElevation);
    // Warmer and dimmer near the horizon: from 22 degrees down to 2, toward a deep amber at three quarters of the light.
    const float low = 1.0f - std::clamp((elevation_ / kDegrees - 2.0f) / 20.0f, 0.0f, 1.0f);
    const float w = low * low * (3.0f - 2.0f * low);
    const float amber[3] = { 1.0f, 0.66f, 0.40f };
    for (int i = 0; i < 3; ++i) tint_[i] = (1.0f + (amber[i] - 1.0f) * w) * (1.0f - 0.25f * w);
    // The rest of the light too: the sky's light on everything warmer and a little dimmer (golden hour).
    const float golden[3] = { 1.06f, 0.93f, 0.76f };
    for (int i = 0; i < 3; ++i) sceneTint_[i] = (1.0f + (golden[i] - 1.0f) * w) * (1.0f - 0.08f * w);
    active_ = true;
    static uint64_t logged = 0;
    if (Hc09Env("HC09_SUNSET_LOG") && frame >= logged + 600)
    {
        logged = frame;
        std::fprintf(stderr, "[sunset] %02d:%02d: sun %.1f degrees up (the game's %.1f), tint %.2f %.2f %.2f\n", int(seconds) / 3600,
                     int(seconds) / 60 % 60, elevation_ / kDegrees, gameElevation / kDegrees, tint_[0], tint_[1], tint_[2]);
    }
}

void Sunset::NoteGameSun(const float* direction, const float* colour)
{
    std::memcpy(game_[0], direction, sizeof(game_[0]));
    std::memcpy(game_[1], colour, sizeof(game_[1]));
    known_ = std::max({ colour[0], colour[1], colour[2] }) > 0.05f;
}

bool Sunset::Apply(float* direction, float* colour) const
{
    if (!active_) return false;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(direction[i] - game_[0][i]) > 1e-4f || std::fabs(colour[i] - game_[1][i]) > 1e-4f) return false;
    // The same bearing, lower: the horizontal part kept, the height from the elevation (the vector's length and which
    // way it points, to the sun or from it, as the game's).
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    const float flat = std::sqrt(direction[0] * direction[0] + direction[2] * direction[2]);
    if (length < 1e-6f || flat < 1e-6f) return false;
    const float up = direction[1] < 0.0f ? -1.0f : 1.0f;
    const float c = std::cos(elevation_), s = std::sin(elevation_);
    direction[0] = direction[0] / flat * c * length;
    direction[2] = direction[2] / flat * c * length;
    direction[1] = up * s * length;
    for (int i = 0; i < 3; ++i) colour[i] *= tint_[i];
    return true;
}

}  // namespace hc09vk
