// hc09: lower-thirds (roadmap 12). The game writes a play-by-play of each play as it happens (PlayLog.cpp: "Josh
// Jacobs rush up the middle for a gain of 18 yards", "tackled by Coby Bryant at the CHI 35", ...), which its ticker
// along the foot of the screen then scrolls through. Its lines are read here as the game writes them, each play is
// put together (who ran, threw, caught, tackled; the yards; the score or turnover), every player's game so far is
// kept, and after a play worth it a lower-third is made for the renderer to draw (vk/score_bar.cpp).

#include "common/broadcast_state.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"
#include "vk/dev_env.h"

// Adds a line to the play log: r3 a printf format, then its arguments.
extern "C" PPC_FUNC(__imp__sub_826D4118);

namespace
{
    using Clock = std::chrono::steady_clock;
    using hc09::broadcast::LowerThird;

    // The play log: +4 the lines written for this play, +8224 its lines (128 bytes each, at most 8).
    constexpr uint32_t kLog = 0x83114EF4, kLineSize = 128, kMaxLines = 8;

    const char* const kPositions[21] = { "QB", "HB", "FB", "WR", "TE", "LT", "LG", "C",  "RG", "RT", "LE",
                                         "RE", "DT", "LOLB", "MLB", "ROLB", "CB", "FS", "SS", "K",  "P" };

    struct Player
    {
        int side = -1, number = -1, position = -1;
        uint32_t portrait = 0;
    };

    // A player's game so far.
    struct Line
    {
        int carries = 0, rushYards = 0, rushTds = 0;
        int attempts = 0, completions = 0, passYards = 0, passTds = 0, picked = 0;
        int catches = 0, catchYards = 0, catchTds = 0;
        int tackles = 0, sacks = 0, picks = 0, defended = 0, recoveries = 0;
        int kicksMade = 0, kicks = 0;
        int returns = 0, returnYards = 0, returnTds = 0, punts = 0, puntYards = 0;
    };

    enum Kind
    {
        kOther,
        kRush,
        kPass,
        kSack,
        kFieldGoal,
        kPunt,
        kKickoff,
    };

    // A play, from its lines.
    struct Play
    {
        bool open = false;       // has lines not yet counted
        Clock::time_point last;  // its latest line
        bool ended = false;      // a line that ends a play has come
        Kind kind = kOther;
        std::string actor, receiver, defender, tackler;   // the runner, passer or kicker; ...
        int yards = 0;
        bool complete = false, picked = false, knocked = false, touchdown = false, fumble = false, good = false, missed = false;
        int possession = -1;     // the side with the ball as it began
        // Kicks: the punt's or kickoff's length, an onside kick, the returner and his yards; a fumble's (or an onside
        // kick's) recovery and the team that made it ("CHI"); a kick blocked (1 a punt, 2 a field goal) and by whom.
        int kickYards = 0, returnYards = 0, blocked = 0;
        bool onside = false;
        std::string returner, recoverer, recoverTeam, blocker;
        // A two-point conversion (not counted in the players' games) and whether it was good.
        bool conversion = false, converted = false;
        // The end of a quarter, the half or the game, or the two-minute warning: its title and the score ("CHI 0   GB 16").
        std::string period, score;
        // A challenge of the play ("Play challenged by GB and OVERTURNED."): the team, 1 the ruling stands, 2 overturned.
        std::string challenger;
        int challenge = 0;
    };

    // The drive under way: the team with the ball, its plays and yards, and the game time it began (seconds since
    // kickoff, from the score bug).
    struct Drive
    {
        int side = -1, plays = 0, yards = 0;
        int start = -1;
    };
    Drive g_drive;
    int g_quarterLength = 0;   // the longest clock the score bug has shown (seconds)

    std::mutex g_mutex;
    Play g_play;
    std::unordered_map<std::string, Player> g_players;   // by name, lower case
    std::unordered_map<std::string, Line> g_lines;       // by name as the log writes it
    uint32_t g_teams[2] = {};                             // the teams the players were read for
    LowerThird g_third;
    std::vector<LowerThird> g_waiting;                    // lower-thirds made while another was still shown
    int g_sinceThird = 0;                                 // plays since the last lower-third
    // The players' games and the drive before the last play was counted: a challenge that overturns it takes it back.
    std::unordered_map<std::string, Line> g_undoLines;
    Drive g_undoDrive;
    bool g_undo = false;

    // A lower-third shows for six seconds (vk/score_bar.cpp); one made meanwhile waits its turn (a scoring play's, then
    // its drive's), at most two.
    constexpr auto kShownFor = std::chrono::milliseconds(6500);
    bool Logging();
    void Show(LowerThird third)
    {
        const bool waits = g_third.id && Clock::now() - g_third.at < kShownFor;
        if (Logging())
            std::fprintf(stderr, "[playlog] lower-third%s: %s | %s | %s\n", waits ? " (waits)" : "", third.name.c_str(), third.headline.c_str(),
                         third.stats.c_str());
        if (waits)
        {
            if (g_waiting.size() < 2)
                g_waiting.push_back(std::move(third));
            return;
        }
        third.id = g_third.id + 1;
        third.at = Clock::now();
        g_third = std::move(third);
    }
    void NextShown()
    {
        if (!g_waiting.empty() && Clock::now() - g_third.at >= kShownFor)
        {
            LowerThird next = std::move(g_waiting.front());
            g_waiting.erase(g_waiting.begin());
            Show(std::move(next));
        }
    }

    bool Logging()
    {
        static const bool on = Hc09Env("HC09_SCORE_BAR_LOG") != nullptr;
        return on;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return s;
    }
    std::string Upper(std::string s)
    {
        for (char& c : s)
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        return s;
    }
    std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(' ');
        return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(' ') - a + 1);
    }
    bool Starts(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }
    bool Ends(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    // "a gain of 18 yards", "a loss of 1 yard", "a gain of inches", "no gain".
    int Yards(const std::string& phrase)
    {
        const bool loss = phrase.find("loss") != std::string::npos;
        const size_t digit = phrase.find_first_of("0123456789");
        const int n = digit == std::string::npos ? 0 : std::atoi(phrase.c_str() + digit);
        return loss ? -n : n;
    }

    // The name before " at the ..." (or the whole of it).
    std::string BeforeAt(const std::string& s)
    {
        const size_t at = s.find(" at the ");
        return Trim(at == std::string::npos ? s : s.substr(0, at));
    }

    // One line of the play log, added to the play.
    // "CHI 0, GB 16" or "CHI 7 GB 16 final" as "CHI 0   GB 16".
    std::string Score(std::string text)
    {
        if (Ends(text, " final"))
            text.resize(text.size() - 6);
        std::string out;
        for (char c : text)
            if (c == ',')
                continue;
            else
                out.push_back(c);
        // Three spaces between the teams: after the first number.
        const size_t digit = out.find_first_of("0123456789");
        const size_t after = digit == std::string::npos ? std::string::npos : out.find(' ', digit);
        if (after != std::string::npos)
            out.replace(after, 1, "   ");
        return Trim(out);
    }

    void Read(Play& play, const std::string& line)
    {
        size_t at;
        if (Starts(line, "END OF THE ") && (at = line.find(" QUARTER - ")) != std::string::npos)
        {
            play.period = "END OF " + Upper(line.substr(11, at - 11)) + " QUARTER";
            play.score = Score(line.substr(at + 11));
            play.ended = true;
            return;
        }
        if (Starts(line, "HALFTIME - ") || Starts(line, "GAME OVER - "))
        {
            play.period = line[0] == 'H' ? "HALFTIME" : "FINAL";
            play.score = Score(line.substr(line.find(" - ") + 3));
            play.ended = true;
            return;
        }
        if (line == "TWO-MINUTE WARNING")
        {
            play.period = line;
            play.ended = true;
            return;
        }
        if (Starts(line, "Play challenged by "))
        {
            const std::string rest = line.substr(19);
            play.challenger = Trim(rest.substr(0, rest.find(' ')));
            play.challenge = Upper(rest).find("OVERTURNED") != std::string::npos ? 2 : 1;
            return;
        }
        if (Starts(line, "two point conversion"))
        {
            play.conversion = true;
            play.converted = line.find("good") != std::string::npos;
            play.ended = true;
            return;
        }
        if (Starts(line, "Punt Blocked by ") || Starts(line, "Field Goal Blocked by "))
        {
            play.blocked = line[0] == 'P' ? 1 : 2;
            play.blocker = Trim(line.substr(line.find(" by ") + 4));
            play.ended = true;
            return;
        }
        if ((at = line.find(" punt ")) != std::string::npos || Ends(line, " punt"))
        {
            // "<name> punt <n> yards", "<name> punt <n> yards from <spot>", "<name> punt from <spot>"
            play.kind = kPunt;
            play.actor = line.substr(0, at == std::string::npos ? line.size() - 5 : at);
            if (at != std::string::npos && line.size() > at + 6 && line[at + 6] >= '0' && line[at + 6] <= '9')
                play.kickYards = std::atoi(line.c_str() + at + 6);
            return;
        }
        if ((at = line.find(" kickoff ")) != std::string::npos)
        {
            play.kind = kKickoff;
            play.actor = line.substr(0, at);
            play.kickYards = std::atoi(line.c_str() + at + 9);
            return;
        }
        if (Ends(line, " onside kick"))
        {
            play.kind = kKickoff;
            play.onside = true;
            play.actor = line.substr(0, line.size() - 12);
            return;
        }
        if (Starts(line, "returned by "))
        {
            // "returned by <name> for <n> yards for a touchdown"
            const size_t result = line.find(" for ", 12);
            play.returner = Trim(line.substr(12, result == std::string::npos ? std::string::npos : result - 12));
            if (result != std::string::npos)
                play.returnYards = std::atoi(line.c_str() + result + 5);
            play.touchdown = line.find("touchdown") != std::string::npos;
            play.ended = play.touchdown;
            return;
        }
        if (Starts(line, "returned for "))
        {
            // A fumble's return: "returned for a gain of 12 yards".
            play.returnYards = Yards(line.substr(13));
            return;
        }
        if (Starts(line, "returned ") && (at = line.find(" yards by ")) != std::string::npos)
        {
            // "returned <n> yards by <name>"
            play.returnYards = std::atoi(line.c_str() + 9);
            play.returner = Trim(line.substr(at + 10));
            return;
        }
        if (Starts(line, "recovered by "))
        {
            // "recovered by <name> (<team>)" after a fumble; "recovered by <name>" after an onside kick.
            const std::string rest = line.substr(13);
            const size_t open = rest.rfind(" (");
            play.recoverer = Trim(open == std::string::npos ? rest : rest.substr(0, open));
            if (open != std::string::npos && rest.back() == ')')
                play.recoverTeam = rest.substr(open + 2, rest.size() - open - 3);
            return;
        }
        if (Starts(line, "fair catch by ") || line.find("touchback") != std::string::npos)
        {
            play.ended = true;
            return;
        }
        if ((at = line.find(" rush ")) != std::string::npos || (at = line.find(" scrambles ")) != std::string::npos)
        {
            play.kind = kRush;
            play.actor = line.substr(0, at);
            if (const size_t result = line.rfind(" for "); result != std::string::npos && result > at)
                play.yards = Yards(line.substr(result + 5));
        }
        else if ((at = line.find(" sacked by ")) != std::string::npos)
        {
            play.kind = kSack;
            play.actor = line.substr(0, at);
            play.defender = BeforeAt(line.substr(at + 11));
            play.ended = true;
        }
        else if (Starts(line, "complete to "))
        {
            const size_t result = line.find(" for ", 12);
            play.receiver = Trim(line.substr(12, result == std::string::npos ? std::string::npos : result - 12));
            play.complete = true;
            if (result != std::string::npos)
                play.yards = Yards(line.substr(result + 5));
        }
        else if (Starts(line, "incomplete to "))
        {
            play.receiver = Trim(line.substr(14));
            play.ended = true;
        }
        else if (Starts(line, "intercepted by "))
        {
            play.defender = Trim(line.substr(15));
            play.picked = true;
        }
        else if (Starts(line, "knocked down by "))
        {
            play.defender = Trim(line.substr(16));
            play.knocked = true;
            play.ended = true;
        }
        else if (Starts(line, "dropped by ") || Ends(line, " throws the ball away") || Ends(line, " spikes"))
            play.ended = true;
        else if (Starts(line, "tackled by "))
        {
            play.tackler = BeforeAt(line.substr(11));
            play.ended = true;
        }
        else if (Starts(line, "out of bounds") || Starts(line, "goes down at") || line.find(" kneels at ") != std::string::npos)
            play.ended = true;
        else if (Starts(line, "touchdown") || Ends(line, "for a touchdown"))
        {
            play.touchdown = true;
            play.ended = true;
        }
        else if (line == "FUMBLE")
            play.fumble = true;
        else if ((at = line.find(" yard field goal attempt")) != std::string::npos)
        {
            // "<name> <yards>"
            const size_t space = line.rfind(' ', at - 1);
            if (space != std::string::npos)
            {
                play.kind = kFieldGoal;
                play.actor = line.substr(0, space);
                play.yards = std::atoi(line.c_str() + space + 1);
            }
        }
        else if (line == "good")
        {
            play.good = true;
            play.ended = true;
        }
        else if (line == "missed" || line == "blocked")
        {
            play.missed = true;
            play.ended = true;
        }
        else if ((at = line.find(" pass ")) != std::string::npos || Ends(line, " pass"))
        {
            // "<name> short pass <where>", "<name> deep pass <where>", "<name> pass <where>"
            if (at == std::string::npos)
                at = line.size() - 5;
            std::string name = line.substr(0, at);
            if (Ends(name, " short"))
                name.resize(name.size() - 6);
            else if (Ends(name, " deep"))
                name.resize(name.size() - 5);
            play.kind = kPass;
            play.actor = name;
            // "... pass across the middle overthrown"
            if (Ends(line, " overthrown") || Ends(line, " underthrown"))
                play.ended = true;
        }
    }

    std::string Plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

    void Add(std::string& to, const std::string& part)
    {
        if (!to.empty())
            to += "   ";
        to += part;
    }

    std::string Rushing(const Line& l)
    {
        std::string s = std::to_string(l.carries) + " CAR";
        Add(s, Plural(l.rushYards, "YD", "YDS"));
        if (l.rushTds)
            Add(s, std::to_string(l.rushTds) + " TD");
        return s;
    }
    std::string Passing(const Line& l)
    {
        std::string s = std::to_string(l.completions) + "/" + std::to_string(l.attempts);
        Add(s, Plural(l.passYards, "YD", "YDS"));
        if (l.passTds)
            Add(s, std::to_string(l.passTds) + " TD");
        if (l.picked)
            Add(s, std::to_string(l.picked) + " INT");
        return s;
    }
    std::string Receiving(const Line& l)
    {
        std::string s = std::to_string(l.catches) + " REC";
        Add(s, Plural(l.catchYards, "YD", "YDS"));
        if (l.catchTds)
            Add(s, std::to_string(l.catchTds) + " TD");
        return s;
    }
    std::string Returning(const Line& l)
    {
        std::string s = std::to_string(l.returns) + " RET";
        Add(s, Plural(l.returnYards, "YD", "YDS"));
        if (l.returnTds)
            Add(s, std::to_string(l.returnTds) + " TD");
        return s;
    }
    std::string Punting(const Line& l)
    {
        char average[16];
        std::snprintf(average, sizeof(average), "%.1f AVG", l.punts ? double(l.puntYards) / l.punts : 0.0);
        std::string s = Plural(l.punts, "PUNT", "PUNTS");
        Add(s, average);
        return s;
    }
    // A player's team (0 the away team, 1 the home team; -1 not known).
    int SideOf(const std::string& name)
    {
        const auto found = g_players.find(Lower(name));
        return found == g_players.end() ? -1 : found->second.side;
    }

    std::string Defending(const Line& l)
    {
        std::string s;
        if (l.tackles)
            Add(s, std::to_string(l.tackles) + " TKL");
        if (l.sacks)
            Add(s, Plural(l.sacks, "SACK", "SACKS"));
        if (l.picks)
            Add(s, std::to_string(l.picks) + " INT");
        if (l.defended)
            Add(s, std::to_string(l.defended) + " PD");
        if (l.recoveries)
            Add(s, std::to_string(l.recoveries) + " FR");
        return s;
    }

    // The play counted in its players' games, and a lower-third made if it is worth one: scores, turnovers, sacks,
    // field goals and long gains always; an ordinary play when the last few had none.
    // Seconds of game time since kickoff, from the score bug (-1: not known).
    int GameTime()
    {
        const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
        int m = 0, sec = 0;
        const int quarter = !board.quarter.empty() && board.quarter[0] >= '1' && board.quarter[0] <= '4' ? board.quarter[0] - '1'
                            : board.quarter.find("OT") != std::string::npos ? 4 : -1;
        int clock = -1;
        if (!board.clock.empty() && board.clock[0] == ':' && std::sscanf(board.clock.c_str() + 1, "%d", &sec) == 1)
            clock = sec;
        else if (std::sscanf(board.clock.c_str(), "%d:%d", &m, &sec) == 2)
            clock = m * 60 + sec;
        if (quarter < 0 || clock < 0)
            return -1;
        g_quarterLength = std::max(g_quarterLength, clock);
        const int length = std::max(g_quarterLength, 60);
        return quarter * length + std::max(length - clock, 0);
    }

    // A panel without a player: the end of a period (grey), or a scoring drive (in the team's colour).
    LowerThird Panel(const std::string& title, const std::string& headline, const std::string& stats, int side, bool big)
    {
        LowerThird third;
        third.side = side;
        third.name = title;
        third.headline = headline;
        third.stats = stats;
        third.big = big;
        return third;
    }

    // A challenge's ruling. Overturned: the play's lower-thirds go (the ruling at once) and, if the play was already
    // counted, it is taken back.
    void Ruling(const std::string& team, bool overturned, bool counted)
    {
        const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
        const int side = team == board.away.abbreviation ? 0 : team == board.home.abbreviation ? 1 : -1;
        if (overturned)
        {
            if (counted && g_undo)
            {
                g_lines = g_undoLines;
                g_drive = g_undoDrive;
            }
            g_waiting.clear();
            g_third.at = Clock::now() - kShownFor;
        }
        g_undo = false;
        Show(Panel("REPLAY REVIEW", overturned ? "THE RULING IS OVERTURNED" : "THE RULING STANDS", team.empty() ? "" : team + " CHALLENGE", side, false));
    }

    void Finish(Play& play)
    {
        if (!play.open)
            return;
        play.open = false;
        // Challenged before it was counted: counted only if the ruling stands.
        if (play.challenge)
        {
            Play ruled = play;
            ruled.challenge = 0;
            ruled.open = true;
            if (play.challenge == 1)
                Finish(ruled);
            Ruling(play.challenger, play.challenge == 2, false);
            return;
        }
        // The end of a period: the score.
        if (!play.period.empty())
        {
            std::string score = play.score;
            if (score.empty())
            {
                const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
                score = board.away.abbreviation + " " + Trim(board.awayScore) + "   " + board.home.abbreviation + " " + Trim(board.homeScore);
            }
            Show(Panel(play.period, score, "", -1, false));
            return;
        }
        // A two-point conversion: not a play of the players' games; when good, the scorer's.
        if (play.conversion)
        {
            if (play.converted)
            {
                const std::string scorer = !play.receiver.empty() ? play.receiver : play.actor;
                if (!scorer.empty())
                {
                    LowerThird third = Panel(Upper(scorer), "TWO-POINT CONVERSION", "", SideOf(scorer) >= 0 ? SideOf(scorer) : play.possession, true);
                    Show(third);
                }
            }
            return;
        }
        // Kept to take the play back if a challenge overturns it.
        g_undoLines = g_lines;
        g_undoDrive = g_drive;
        g_undo = true;
        // A fumble lost is written first ("FUMBLE", "recovered by <name> (<team>)", "returned for ..."), then the play;
        // a return can be written as the recoverer's rush: not a play of the offense, and his team did not carry it.
        const bool fumbleReturn = play.fumble && play.kind == kRush && !play.recoverer.empty() && Lower(play.actor) == Lower(play.recoverer);
        if (fumbleReturn)
        {
            play.kind = kOther;
            g_drive = Drive{};
        }
        // The drive: a new one when the other team has the ball (or after a kick).
        const bool offense = play.kind == kRush || play.kind == kPass || play.kind == kSack || play.kind == kFieldGoal;
        if (play.kind == kPunt || play.kind == kKickoff || play.picked || (offense && play.possession != g_drive.side))
            g_drive = Drive{};
        if (offense && play.possession >= 0)
        {
            if (g_drive.side < 0)
            {
                g_drive.side = play.possession;
                g_drive.start = GameTime();
            }
            ++g_drive.plays;
            if (play.kind != kFieldGoal && !(play.kind == kPass && !play.complete))
                g_drive.yards += play.yards;
        }
        const bool scored = offense && !play.picked && !play.fumble && (play.touchdown || (play.kind == kFieldGoal && play.good));
        std::string name, headline, stats;
        int side = play.possession;   // the offense's, unless the player is known
        bool big = false, worth = false;
        const std::string yards = std::to_string(std::abs(play.yards)) + "-YARD ";
        if (play.kind == kRush && !play.actor.empty())
        {
            Line& l = g_lines[play.actor];
            ++l.carries;
            l.rushYards += play.yards;
            l.rushTds += play.touchdown;
            name = play.actor;
            headline = play.touchdown ? yards + "TOUCHDOWN RUN" : play.yards > 0 ? yards + "RUN" : play.yards < 0 ? yards + "LOSS" : "NO GAIN";
            stats = Rushing(l);
            big = play.touchdown;
            worth = play.touchdown || play.yards >= 10;
        }
        else if (play.kind == kPass && !play.actor.empty())
        {
            Line& passer = g_lines[play.actor];
            ++passer.attempts;
            if (play.complete)
            {
                ++passer.completions;
                passer.passYards += play.yards;
                passer.passTds += play.touchdown;
                Line& receiver = g_lines[play.receiver];
                ++receiver.catches;
                receiver.catchYards += play.yards;
                receiver.catchTds += play.touchdown;
                big = play.touchdown;
                worth = play.touchdown || play.yards >= 15;
                // The receiver after a score or a long gain; else the passer.
                if (worth && !play.receiver.empty())
                {
                    name = play.receiver;
                    headline = yards + (play.touchdown ? "TOUCHDOWN CATCH" : "CATCH");
                    stats = Receiving(receiver);
                }
                else
                {
                    name = play.actor;
                    headline = yards + "PASS TO " + Upper(play.receiver);
                    stats = Passing(passer);
                }
            }
            else if (play.picked && !play.defender.empty())
            {
                ++passer.picked;
                Line& l = g_lines[play.defender];
                ++l.picks;
                name = play.defender;
                headline = play.touchdown ? "INTERCEPTION RETURNED FOR A TOUCHDOWN" : "INTERCEPTION";
                stats = Defending(l);
                side = play.possession < 0 ? -1 : 1 - play.possession;
                big = worth = true;
            }
            else if (play.knocked && !play.defender.empty())
            {
                Line& l = g_lines[play.defender];
                ++l.defended;
                name = play.defender;
                headline = "PASS BROKEN UP";
                stats = Defending(l);
                side = play.possession < 0 ? -1 : 1 - play.possession;
            }
        }
        else if (play.kind == kSack && !play.defender.empty())
        {
            Line& l = g_lines[play.defender];
            ++l.sacks;
            ++l.tackles;
            name = play.defender;
            headline = "SACK";
            stats = Defending(l);
            side = play.possession < 0 ? -1 : 1 - play.possession;
            worth = true;
        }
        else if (play.kind == kFieldGoal && !play.actor.empty() && (play.good || play.missed))
        {
            Line& l = g_lines[play.actor];
            ++l.kicks;
            l.kicksMade += play.good;
            name = play.actor;
            headline = yards + (play.good ? "FIELD GOAL" : "FIELD GOAL MISSED");
            stats = std::to_string(l.kicksMade) + "/" + std::to_string(l.kicks) + " FG";
            big = play.good;
            worth = true;
        }
        // Kicks: a long return (and every return for a score), a long punt, an onside kick the kicking team recovered.
        const int kicking = SideOf(play.actor) >= 0 ? SideOf(play.actor) : play.possession;
        if ((play.kind == kPunt || play.kind == kKickoff) && !play.actor.empty())
        {
            if (play.kind == kPunt && play.kickYards > 0)
            {
                Line& l = g_lines[play.actor];
                ++l.punts;
                l.puntYards += play.kickYards;
                if (play.kickYards >= 55)
                {
                    name = play.actor;
                    headline = std::to_string(play.kickYards) + "-YARD PUNT";
                    stats = Punting(l);
                    side = kicking;
                    worth = true;
                }
            }
            if (!play.returner.empty() && !play.onside)
            {
                Line& l = g_lines[play.returner];
                ++l.returns;
                l.returnYards += play.returnYards;
                l.returnTds += play.touchdown;
                const char* what = play.kind == kPunt ? "PUNT RETURN" : "KICKOFF RETURN";
                const bool longOne = play.returnYards >= (play.kind == kPunt ? 15 : 35);
                if (play.touchdown || longOne)
                {
                    name = play.returner;
                    headline = std::to_string(play.returnYards) + "-YARD " + what + (play.touchdown ? " TOUCHDOWN" : "");
                    stats = Returning(l);
                    side = kicking < 0 ? -1 : 1 - kicking;
                    big = play.touchdown;
                    worth = true;
                }
            }
            if (play.onside && !play.recoverer.empty() && kicking >= 0 && SideOf(play.recoverer) == kicking)
            {
                Line& l = g_lines[play.recoverer];
                ++l.recoveries;
                name = play.recoverer;
                headline = "ONSIDE KICK RECOVERED";
                stats = Defending(l);
                side = kicking;
                big = worth = true;
            }
        }
        // A kick blocked.
        if (play.blocked && !play.blocker.empty())
        {
            name = play.blocker;
            headline = play.blocked == 1 ? "BLOCKED PUNT" : "BLOCKED FIELD GOAL";
            stats = Defending(g_lines[play.blocker]);
            side = SideOf(play.blocker) >= 0 ? SideOf(play.blocker) : play.possession < 0 ? -1 : 1 - play.possession;
            big = worth = true;
        }
        // A fumble lost: recovered by the other team than the one carrying the ball (after a kick, the receiving team).
        bool fumbleShown = false;
        if (play.fumble && !play.recoverer.empty())
        {
            int recovering = SideOf(play.recoverer);
            if (recovering < 0 && !play.recoverTeam.empty())
            {
                const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
                recovering = play.recoverTeam == board.away.abbreviation ? 0 : play.recoverTeam == board.home.abbreviation ? 1 : -1;
            }
            // The team carrying the ball: the receiving team's after a kick; the passer's or runner's (the score bug
            // may already show the other team with the ball); the other team's when the return is written as a rush.
            const int carrying = play.kind == kPunt || play.kind == kKickoff ? (kicking < 0 ? -1 : 1 - kicking)
                                 : fumbleReturn                           ? (recovering < 0 ? -1 : 1 - recovering)
                                 : SideOf(play.actor) >= 0                ? SideOf(play.actor)
                                                                          : play.possession;
            if (carrying >= 0 && recovering >= 0 && recovering != carrying)
            {
                Line& l = g_lines[play.recoverer];
                ++l.recoveries;
                name = play.recoverer;
                headline = play.touchdown ? "FUMBLE RETURNED FOR A TOUCHDOWN" : "FUMBLE RECOVERY";
                stats = Defending(l);
                side = recovering;
                big = worth = fumbleShown = true;
            }
        }
        // A tackle (the sack's is counted above); behind the line, the tackler takes an ordinary play's lower-third.
        if (!play.tackler.empty() && play.kind != kSack)
        {
            Line& l = g_lines[play.tackler];
            ++l.tackles;
            if ((play.kind == kRush || (play.kind == kPass && play.complete)) && play.yards < 0 && !worth)
            {
                name = play.tackler;
                headline = "TACKLE FOR A LOSS";
                stats = Defending(l);
                side = play.possession < 0 ? -1 : 1 - play.possession;
            }
        }
        if (play.fumble && !fumbleShown)
            big = false, worth = false;   // a fumble the same team kept: no lower-third
        if (Logging())
            std::fprintf(stderr, "[playlog] play: kind %d, %s, %d yards%s%s -> %s | %s | %s%s\n", int(play.kind), play.actor.c_str(), play.yards,
                         play.touchdown ? ", touchdown" : "", play.picked ? ", intercepted" : "", name.c_str(), headline.c_str(), stats.c_str(),
                         worth ? " (shown)" : "");
        // A scoring drive of three plays or more: after the play's own lower-third.
        auto driveSummary = [&] {
            if (!scored || g_drive.plays < 3 || g_drive.side < 0)
                return;
            std::string time;
            const int now = GameTime();
            if (g_drive.start >= 0 && now > g_drive.start)
            {
                char t[16];
                std::snprintf(t, sizeof(t), "%d:%02d", (now - g_drive.start) / 60, (now - g_drive.start) % 60);
                time = t;
            }
            const std::string headline = Plural(g_drive.plays, "PLAY", "PLAYS") + ", " + Plural(g_drive.yards, "YARD", "YARDS");
            Show(Panel("SCORING DRIVE", headline, time.empty() ? "" : time + " TIME OF POSSESSION", g_drive.side, false));
            g_drive = Drive{};
        };
        if (name.empty() || (play.fumble && !fumbleShown))
        {
            driveSummary();
            return;
        }
        if (!worth && ++g_sinceThird < 3)
        {
            driveSummary();
            return;
        }
        g_sinceThird = 0;
        LowerThird third;
        third.side = side;
        third.name = Upper(name);
        third.headline = headline;
        third.stats = stats;
        third.big = big;
        if (const auto found = g_players.find(Lower(name)); found != g_players.end())
        {
            const Player& p = found->second;
            third.side = p.side;
            third.portrait = p.portrait;
            if (p.position >= 0 && p.position < 21)
                third.tag = kPositions[p.position];
            if (p.number >= 0)
                third.tag += (third.tag.empty() ? "#" : "  #") + std::to_string(p.number);
        }
        Show(std::move(third));
        driveSummary();
    }

    // The two teams' players (name, number, position), from the game's database, when the teams change.
    void ReadPlayers(PPCContext& ctx, uint8_t* base)
    {
        hc09::guest::Database db(ctx, base);
        if (!db.Ready() || !db.Query("select 'GTAG' into \x85 and 'GTHG' into \x85 from 'FNIG'", { db.Out(0), db.Out(1) }))
            return;
        const uint32_t ids[2] = { db.Get(0), db.Get(1) };
        if (ids[0] == 0 || ids[0] >= hc09::guest::Database::kNone || ids[1] == 0 || ids[1] >= hc09::guest::Database::kNone)
            return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (ids[0] == g_teams[0] && ids[1] == g_teams[1])
                return;
            g_teams[0] = ids[0];
            g_teams[1] = ids[1];
        }
        std::unordered_map<std::string, Player> players;
        for (int side = 0; side < 2; ++side)
        {
            // The team's players are under its own id, or (Coach Now plays copies of the teams) the team's it copies.
            uint32_t real = ids[side];
            if (db.Query("select 'DIOT' into \x85 from 'MAET' where ('DIGT' = \x85)", { db.Out(0), ids[side] }) && db.Get(0) >= 1 && db.Get(0) <= 32)
                real = db.Get(0);
            struct Row
            {
                uint32_t id, number, position;
            };
            std::vector<Row> rows;
            for (const uint32_t team : { ids[side], real })
            {
                const std::string declare = "declare \x8A fastcursor for select 'DIGP' into \x85 and 'NEJP' into \x85 and 'SOPP' into \x85 "
                                            "from 'YALP' where ('DIGT' = " + std::to_string(team) + ")";
                db.ForEach(declare.c_str(), 3, [&] { rows.push_back({ db.Get(0), db.Get(1), db.Get(2) }); });
                if (!rows.empty() || real == ids[side])
                    break;
            }
            for (const Row& row : rows)
            {
                const uint32_t first = db.Text(0, std::string(40, '\0')), last = db.Text(1, std::string(40, '\0'));
                if (!db.Query("select 'ANFP' into \x88 and 'ANLP' into \x88 and 'PXSP' into \x85 from 'YALP' where ('DIGP' = \x85)",
                              { first, last, db.Out(0), row.id }))
                    continue;
                const uint32_t portrait = db.Get(0) < hc09::guest::Database::kNone ? db.Get(0) : 0;
                std::string name;
                for (const uint32_t text : { first, last })
                {
                    const char* s = reinterpret_cast<const char*>(base + text);
                    if (!name.empty())
                        name.push_back(' ');
                    for (int k = 0; k < 40 && s[k] >= 32 && s[k] < 127; ++k)
                        name.push_back(s[k]);
                }
                players[Lower(name)] = Player{ side, int(row.number), int(row.position), portrait };
            }
            if (Logging())
                std::fprintf(stderr, "[playlog] team %u (%u): %zu players\n", ids[side], real, rows.size());
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_players = std::move(players);
        g_lines.clear();
        g_drive = Drive{};
        g_quarterLength = 0;
        g_sinceThird = 0;
    }

    bool LowerThirds()
    {
        static const bool on = [] {
            const char* e = Hc09Env("HC09_LOWER_THIRDS");
            return hc09::broadcast::Enabled() && !(e && (!std::strcmp(e, "0") || !std::strcmp(e, "off")));
        }();
        return on;
    }
}

namespace
{
    // HC09_PLAYLOG_TEST=<file> (dev): plays read from a file through the same reading, each play's lower-third logged,
    // once. Plays are separated by blank lines; "#possession <0|1>" sets the side with the ball for the plays after it,
    // "#player <0|1> <name>" puts a player on a team.
    void SelfTest()
    {
        static bool done = false;
        const char* path = Hc09Env("HC09_PLAYLOG_TEST");
        // In a game, with the score bar up (the last panel stays on the screen).
        if (done || !path || !hc09::broadcast::Snapshot().shown)
            return;
        done = true;
        FILE* f = std::fopen(path, "rb");
        if (!f)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        int possession = 0;
        Play play;
        char text[256];
        auto end = [&] {
            if (!play.open)
                return;
            const uint64_t before = g_third.id;
            g_sinceThird = 0;
            g_third.at = Clock::now() - kShownFor;   // each play's own, not queued
            Finish(play);
            std::fprintf(stderr, "[playtest] %s\n", g_third.id != before ? (g_third.name + " | " + g_third.headline + " | " + g_third.stats +
                                                                           " | side " + std::to_string(g_third.side) + (g_third.big ? " | big" : "")).c_str()
                                                                     : "(none)");
            for (const LowerThird& w : g_waiting)
                std::fprintf(stderr, "[playtest]   then %s | %s | %s | side %d\n", w.name.c_str(), w.headline.c_str(), w.stats.c_str(), w.side);
            g_waiting.clear();
            play = Play{};
        };
        while (std::fgets(text, sizeof(text), f))
        {
            std::string line = Trim(std::string(text).substr(0, std::strcspn(text, "\r\n")));
            if (line.empty())
            {
                end();
                continue;
            }
            if (Starts(line, "#possession "))
            {
                possession = std::atoi(line.c_str() + 12);
                continue;
            }
            if (Starts(line, "#late "))
            {
                // A line for the play before, after it was counted (a challenge's ruling).
                end();
                Play late;
                Read(late, line.substr(6));
                if (late.challenge)
                {
                    g_third.at = Clock::now() - kShownFor;
                    Ruling(late.challenger, late.challenge == 2, true);
                }
                std::fprintf(stderr, "[playtest] late: %s | %s | %s\n", g_third.name.c_str(), g_third.headline.c_str(), g_third.stats.c_str());
                g_waiting.clear();
                continue;
            }
            if (Starts(line, "#player "))
            {
                g_players[Lower(line.substr(10))] = Player{ std::atoi(line.c_str() + 8), 1, 0, 0 };
                continue;
            }
            if (!play.open)
            {
                play = Play{};
                play.possession = possession;
                play.open = true;
            }
            Read(play, line);
        }
        end();
        std::fclose(f);
        g_lines.clear();
        g_third.at = Clock::now();
        g_waiting.clear();
    }
}

namespace hc09::broadcast
{
    LowerThird LatestLowerThird()
    {
        if (!LowerThirds())
            return {};
        SelfTest();
        std::lock_guard<std::mutex> lock(g_mutex);
        // A play is over half a second after a line that ends one, or four seconds after any.
        if (g_play.open)
        {
            const auto quiet = Clock::now() - g_play.last;
            if (quiet > std::chrono::seconds(4) || (g_play.ended && quiet > std::chrono::milliseconds(500)))
                Finish(g_play);
        }
        NextShown();
        return g_third;
    }
}

PPC_FUNC(sub_826D4118)
{
    if (!LowerThirds())
    {
        __imp__sub_826D4118(ctx, base);
        return;
    }
    auto word = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    const uint32_t before = word(kLog + 4);
    __imp__sub_826D4118(ctx, base);
    const uint32_t after = word(kLog + 4), lines = word(kLog + 8224);
    if (after != before + 1 || before >= kMaxLines || lines < 0x40000000u)
        return;
    std::string line;
    const char* text = reinterpret_cast<const char*>(base + lines + before * kLineSize);
    for (uint32_t i = 0; i < kLineSize && text[i]; ++i)
        line.push_back(text[i] >= 32 && text[i] < 127 ? text[i] : '?');
    line = Trim(line);
    if (Logging())
        std::fprintf(stderr, "[playlog] %u \"%s\"\n", before, line.c_str());
    if (before == 0)
        ReadPlayers(ctx, base);
    const int possession = hc09::broadcast::Snapshot().possession;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (before == 0)
    {
        Finish(g_play);
        g_play = Play{};
        g_play.possession = possession;
    }
    else if (!g_play.open)
    {
        // A line for a play already counted: a challenge's ruling ("Play challenged by GB and OVERTURNED."); any other
        // is not counted again.
        Play late;
        Read(late, line);
        if (late.challenge)
            Ruling(late.challenger, late.challenge == 2, true);
        return;
    }
    g_play.open = true;
    g_play.last = Clock::now();
    Read(g_play, line);
}
