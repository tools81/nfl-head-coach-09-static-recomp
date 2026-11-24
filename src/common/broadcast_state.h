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
        bool inGame = false;   // a game is being played (its screens, the field, the score bug in use: not the menus)
        Team away, home;
        int user = -1;         // the user's team: 0 the away team, 1 the home team, -1 not known
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

    // A lower-third: after a play worth one, the player who made it, what he did and his game so far, from the
    // game's own play-by-play (common/play_log.cpp).
    struct LowerThird
    {
        uint64_t id = 0;                               // changes with each (0: none yet)
        std::chrono::steady_clock::time_point at;      // when it was made
        int side = -1;                                 // 0 the away team's player, 1 the home team's, -1 not known
        std::string name, tag, headline, stats;        // "JOSH JACOBS", "HB  #8", "18-YARD RUN", "5 CAR   46 YDS"
        bool big = false;                              // a score or a turnover
        uint32_t portrait = 0;                         // his portrait (PLAY.PSXP; 0: not known)
    };

    // The latest lower-third (LowerThirds, HC09_LOWER_THIRDS: on with the score bar; "off" for none).
    LowerThird LatestLowerThird();

    // The TV cameras (common/tv_camera.cpp; TVCamera, HC09_TV_CAMERA). The next one of main, end zone, low sideline
    // and aerial: false when the game's camera is not the Broadcast one (nothing changed).
    bool CycleTvCamera();
    // The camera's name for a few seconds after a change (nullptr otherwise), for the score bar to show.
    const char* TvCameraNote();

    // The clipboard's Instant Replay is being shown (its screen is up).
    bool ReplayShown();

    // The game's play flow (common/extra_point.cpp): 5 the play call, 1 the teams lined up waiting for the snap, 3 the
    // play, 4 after it; 0 not in a game (the flow has not run for half a second).
    int PlayState();

    // The field's lines in a game (the renderer paints them before a snap): the line of scrimmage's and the line to
    // gain's z (yards along the field, 0 at midfield; gain 1e9 when there is none: goal to go).
    // False outside games, and for a kickoff or a try.
    bool FieldLines(float& scrimmage, float& gain);

    // Dev (HC09_REPLAY_TEST): opens the clipboard's Instant Replay after a play, at most once a minute. The button to
    // press now (0 none, 1 RB, 2 down, 3 A); hold: the scripted presses (HC09_PRESS) wait meanwhile.
    int ReplayTestStep(bool& hold);

    // The bar's font (SIL Open Font License, fonts/OFL.txt), from the game's folder.
    inline constexpr const char* kFontFile = "fonts\\BarlowCondensed-SemiBold.ttf";

    // ScoreBar (HC09_SCORE_BAR): on by default with the Vulkan renderer (which draws the bar); "off" keeps the game's
    // own score bug, as do the D3D12 renderer and a missing font.
    bool Enabled();

    // The score bug as the game last set it. (The game's own is kept hidden while the bar is on.)
    Scoreboard Snapshot();
}
