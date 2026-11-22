// HC27: a 17-game regular season in 18 weeks, as the NFL has had since 2021.
//
// A career's first season is played to a schedule from the disc, which HC27's disc patches lengthen
// (tools/roster_import/schedule17.py). Every later season's schedule is made by the game (sub_82D2AC48: it empties
// SCHD and fills it with the season's preseason, regular season and playoff games). It lays the regular season out
// over 18 weeks, its playoffs starting in the week after them, but gives each team only its 16 games: the last week
// of the regular season is left empty. Here that week gets a seventeenth game for every team: one against a team of
// the other conference that it does not meet otherwise.
//
// The same check runs as the season's stage changes (sub_82D21D78), so a career whose schedule was made before this
// was added gets its seventeenth games before the regular season starts. A schedule that does not look like that
// (17 games already, or a week in use) is left as it is.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"
#include "gpu/draw.h"
#include "vk/dev_env.h"

#ifdef HC09_EDITION_HC27
extern "C" PPC_FUNC(__imp__sub_82D2AC48);  // makes a season's schedule
extern "C" PPC_FUNC(__imp__sub_82D21D78);  // the season's stage changes (r3: 1 the preseason, 2 the regular season)

namespace
{
    using hc09::guest::Database;

    constexpr uint32_t kTeams = 32;

    struct Game
    {
        uint32_t away, home, week;
    };

    void Lengthen(Database& db, const char* when)
    {
        // The regular season's games (SEST 0: the season; SEWT 1: its regular season).
        std::vector<Game> games;
        db.ForEach("declare \x8A fastcursor for select 'GTAG' into \x85 and 'GTHG' into \x85 and 'NWES' into \x85 from 'DHCS' "
                   "where ('TSES' = 0) and ('TWES' = 1)",
                   3, [&] { games.push_back({ db.Get(0), db.Get(1), db.Get(2) }); });
        if (games.size() != kTeams * 8)
            return;   // not a season of 16 games a team (or one already lengthened)
        // The year, the games' numbers and the weeks of the playoffs.
        uint32_t year = Database::kNone, lastGame = 0, playoffs = Database::kNone;
        db.ForEach("declare \x8A fastcursor for select 'RYES' into \x85 and 'DMGS' into \x85 and 'TWES' into \x85 from 'DHCS' "
                   "where ('TSES' = 0)",
                   3, [&] {
                       if (db.Get(2) == 1)
                           year = db.Get(0);
                       if (db.Get(1) != Database::kNone)
                           lastGame = std::max(lastGame, db.Get(1));
                   });
        db.ForEach("declare \x8A fastcursor for select 'NWES' into \x85 from 'DHCS' where ('TSES' = 0) and ('TWES' = 2)", 1,
                   [&] { playoffs = std::min(playoffs, db.Get(0)); });
        if (year == Database::kNone || playoffs == Database::kNone || playoffs == 0)
            return;
        const uint32_t week = playoffs - 1;
        std::map<uint32_t, int> count;
        std::set<std::pair<uint32_t, uint32_t>> meet;
        for (const Game& g : games)
        {
            if (g.week == week)
                return;   // the week before the playoffs is in use
            ++count[g.away];
            ++count[g.home];
            meet.insert({ g.away, g.home });
            meet.insert({ g.home, g.away });
        }
        if (count.size() != kTeams || std::any_of(count.begin(), count.end(), [](const auto& c) { return c.second != 16; }))
            return;
        // The conferences.
        std::map<uint32_t, uint32_t> conference;
        db.ForEach("declare \x8A fastcursor for select 'DIGT' into \x85 and 'DIGC' into \x85 from 'MAET'", 2, [&] {
            if (count.count(db.Get(0)))
                conference[db.Get(0)] = db.Get(1);
        });
        std::vector<uint32_t> one, other;
        for (const auto& [team, c] : conference)
            (c == conference.begin()->second ? one : other).push_back(team);
        if (one.size() != kTeams / 2 || other.size() != kTeams / 2)
            return;
        // A perfect matching between the conferences among teams that do not meet (augmenting paths), from a
        // different starting point each season so the pairs vary.
        std::map<uint32_t, uint32_t> match;   // other -> one
        std::function<bool(uint32_t, std::set<uint32_t>&)> place = [&](uint32_t a, std::set<uint32_t>& seen) {
            for (size_t k = 0; k < other.size(); ++k)
            {
                const uint32_t b = other[(k + year * 5) % other.size()];
                if (meet.count({ a, b }) || seen.count(b))
                    continue;
                seen.insert(b);
                if (!match.count(b) || place(match[b], seen))
                {
                    match[b] = a;
                    return true;
                }
            }
            return false;
        };
        for (uint32_t a : one)
        {
            std::set<uint32_t> seen;
            if (!place(a, seen))
            {
                std::fprintf(stderr, "[hc27] 17-game season: no seventeenth opponent for team %u\n", a);
                return;
            }
        }
        int added = 0, number = 0;
        for (const auto& [b, a] : match)
        {
            // Home games alternate between the conferences from one season to the next, as the NFL has them.
            const bool firstHome = (year + uint32_t(number)) % 2 == 0;
            const uint32_t home = firstHome ? a : b, away = firstHome ? b : a;
            char query[512];
            std::snprintf(query, sizeof query,
                          "insert into 'DHCS' set 'ATSG' = 1 and 'ATSS' = 2 and 'CSAG' = 0 and 'CSHG' = 0 and 'DIGS' = 127 "
                          "and 'DMGS' = %u and 'DOTG' = %u and 'ETUG' = 0 and 'GTAG' = %u and 'GTFG' = 0 and 'GTHG' = %u "
                          "and 'ISES' = 0 and 'MNGS' = %d and 'NWES' = %u and 'RYES' = %u and 'TOFG' = 0 and 'TSES' = 0 "
                          "and 'TWES' = 1 and 'UHFG' = 0 and 'woDG' = 3",
                          lastGame + 1 + uint32_t(number), number < 12 ? 780u : 965u, away, home, number, week, year);
            added += db.Query(query, {});
            ++number;
        }
        std::fprintf(stderr, "[hc27] 17-game season (%s): %d games added in week %u of season %u\n", when, added, week + 1, year);
    }
}

// A season's schedule is made.
PPC_FUNC(sub_82D2AC48)
{
    __imp__sub_82D2AC48(ctx, base);
    Database db(ctx, base);
    if (db.Ready())
        Lengthen(db, "new schedule");
}

// The season moves to its next stage.
PPC_FUNC(sub_82D21D78)
{
    __imp__sub_82D21D78(ctx, base);
    Database db(ctx, base);
    if (db.Ready())
        Lengthen(db, "stage");
}
// Dev (HC09_SEASON17_TEST=<frame>): the check run once, from the first database query the game makes after that
// frame (before the query: the database is not in the middle of anything; debug/hooks.cpp calls this), to test it
// without playing on to a new stage.
void Season17Test(PPCContext& ctx, uint8_t* base)
{
    static const int at = [] {
        const char* e = Hc09Env("HC09_SEASON17_TEST");
        return e ? std::atoi(e) : 0;
    }();
    static bool done = false;
    if (!at || done || hc09::gpu::FrameNumber() < at)
        return;
    done = true;
    Database db(ctx, base);
    if (db.Ready())
        Lengthen(db, "test");
}
#else
void Season17Test(PPCContext&, uint8_t*) {}
#endif
