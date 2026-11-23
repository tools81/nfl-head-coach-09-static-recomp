#pragma once

// The conditions of the game being played (roadmap 16), for the Vulkan renderer: set as each game is set up
// (common/weather.cpp, from the game-info record by the game's own rules).

#include <atomic>

namespace hc09
{
    enum class Weather : int { Clear = 0, Rain = 1, Snow = 2 };

    // Clear under a roof (a dome has no weather).
    inline std::atomic<int> g_weather{ 0 };
    // Kickoff after 18:00: the stadium's floodlights light the game.
    inline std::atomic<bool> g_nightGame{ false };
    // The record's precipitation (GWPG, 0 when dry) and wind (GWWG, mph).
    inline std::atomic<int> g_precipitation{ 0 };
    inline std::atomic<int> g_wind{ 0 };
    // The record's temperature (GWTG, degrees F) and whether the stadium has a roof.
    inline std::atomic<int> g_temperature{ 55 };
    inline std::atomic<bool> g_domed{ false };
    // The kickoff, minutes after midnight (GINF.GTOD).
    inline std::atomic<int> g_kickoffMinutes{ 0 };

    // Rain or snow by day: an overcast sky, no sun.
    inline bool Overcast() { return g_weather.load() != int(Weather::Clear) && !g_nightGame.load(); }
}
