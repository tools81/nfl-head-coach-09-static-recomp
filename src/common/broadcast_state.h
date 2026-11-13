#pragma once

// hc09: the score bar (roadmap 12). The game's score bug (the banner along the top of the screen in games, the Apt
// clip "mcGameDayTicker") is read as the game sets it, kept hidden, and drawn by the renderer as a bar along the bottom
// instead (vk/score_bar.h).

#include <chrono>
#include <cstdint>
#include <string>

namespace hc09::broadcast
{
    struct Team
    {
        uint32_t id = 0;           // the team (TEAM.TOID: its TGID, 1 .. 32, also for Coach Now's copies; 0: not known)
        std::string abbreviation;  // TEAM.TSNA, e.g. "CHI"
        uint32_t rgb = 0;          // TEAM.TBC* (the menus' primary colour), 0xRRGGBB
    };

    struct Scoreboard
    {
        bool shown = false;    // the game shows its score bug
        Team away, home;
        std::string awayScore, homeScore, quarter, clock, down, playClock;  // as the score bug shows them
        int possession = -1;                      // 0 the away team has the ball, 1 the home team, -1 not known
        int awayTimeouts = -1, homeTimeouts = -1; // left this half (-1: not known)

        // The clipboard's score box (at the clipboard's top while it is open), which the renderer covers with the
        // Coaching Advantage: whether it is shown, and the panel's rows as the game sets
        // them (the game's own panel along the foot of the screen is kept hidden).
        float box = 0;                            // 1: in place (the clipboard is open); 0: not shown
        bool coaching = false;                    // the game shows its Coaching Advantage panel
        bool rowShown[4] = { true, true, true, false };   // overall, coach, knowledge, game plan
        float rows[4][2] = {};                    // their values, 0 .. 100: [row][0 away, 1 home]

        // The last play: from its snap (the play clock's last change before it) to its end (the down and distance's
        // next change), and a number that changes with each.
        uint64_t play = 0;
        std::chrono::steady_clock::time_point playStart, playEnd;
        std::chrono::steady_clock::time_point playClockChanged;   // the play clock's last change (it stops at a snap)
    };

    // The bar's font (SIL Open Font License, fonts/OFL.txt), from the game's folder.
    inline constexpr const char* kFontFile = "fonts\\BarlowCondensed-SemiBold.ttf";

    // ScoreBar (HC09_SCORE_BAR): on by default with the Vulkan renderer (which draws the bar); "off" keeps the game's
    // own score bug, as do the D3D12 renderer and a missing font.
    bool Enabled();

    // The score bug as the game last set it. (The game's own is kept hidden while the bar is on.)
    Scoreboard Snapshot();
}
