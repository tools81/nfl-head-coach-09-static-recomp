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
        int tackles = 0, sacks = 0, picks = 0, defended = 0;
        int kicksMade = 0, kicks = 0;
    };

    enum Kind
    {
        kOther,
        kRush,
        kPass,
        kSack,
        kFieldGoal,
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
    };

    std::mutex g_mutex;
    Play g_play;
    std::unordered_map<std::string, Player> g_players;   // by name, lower case
    std::unordered_map<std::string, Line> g_lines;       // by name as the log writes it
    uint32_t g_teams[2] = {};                             // the teams the players were read for
    LowerThird g_third;
    int g_sinceThird = 0;                                 // plays since the last lower-third

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
    void Read(Play& play, const std::string& line)
    {
        size_t at;
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
        return s;
    }

    // The play counted in its players' games, and a lower-third made if it is worth one: scores, turnovers, sacks,
    // field goals and long gains always; an ordinary play when the last few had none.
    void Finish(Play& play)
    {
        if (!play.open)
            return;
        play.open = false;
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
        if (play.fumble)
            big = false, worth = false;   // who has the ball now is not told the same way: no lower-third
        if (Logging())
            std::fprintf(stderr, "[playlog] play: kind %d, %s, %d yards%s%s -> %s | %s | %s%s\n", int(play.kind), play.actor.c_str(), play.yards,
                         play.touchdown ? ", touchdown" : "", play.picked ? ", intercepted" : "", name.c_str(), headline.c_str(), stats.c_str(),
                         worth ? " (shown)" : "");
        if (name.empty() || play.fumble)
            return;
        if (!worth && ++g_sinceThird < 3)
            return;
        g_sinceThird = 0;
        LowerThird third;
        third.id = g_third.id + 1;
        third.at = Clock::now();
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
        g_third = third;
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

namespace hc09::broadcast
{
    LowerThird LatestLowerThird()
    {
        if (!LowerThirds())
            return {};
        std::lock_guard<std::mutex> lock(g_mutex);
        // A play is over half a second after a line that ends one, or four seconds after any.
        if (g_play.open)
        {
            const auto quiet = Clock::now() - g_play.last;
            if (quiet > std::chrono::seconds(4) || (g_play.ended && quiet > std::chrono::milliseconds(500)))
                Finish(g_play);
        }
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
    g_play.open = true;
    g_play.last = Clock::now();
    Read(g_play, line);
}
