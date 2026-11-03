// Fixes for bugs in the original game, as overrides of recompiled functions (see debug/hooks.cpp:
// defining sub_<address> replaces the function for every caller; __imp__sub_<address> is the original).

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"

extern "C" PPC_FUNC(__imp__sub_82691378);  // the game mode: select GMOD into ? from GINF
extern "C" PPC_FUNC(__imp__sub_826913C0);  // set the game mode: update GINF set GMOD = ?
extern "C" PPC_FUNC(__imp__sub_82698100);  // practice session: start (switches the game mode to practice)
extern "C" PPC_FUNC(__imp__sub_82689370);  // practice session: reset, when the session ends

// Injuries never heal after Play Creator. Play Creator runs as a practice session, which switches
// the game mode (GINF.GMOD) to practice, 2. When the session ends, its reset leaves the mode as it is,
// so every later game and week of a career runs as practice for the injury system, which then
// records, counts down and heals no injuries: an injured player is never fit again, and no new
// injuries occur. The mode in force before the session is remembered when the session switches it
// and written back when the session ends. A career already stuck in practice mode (the mode was 2
// before the session started) gets 0, a career's mode, so opening and closing Play Creator once
// repairs it.
namespace
{
    constexpr uint32_t kPracticeMode = 2;
    int64_t g_modeBeforePractice = -1;  // -1: no practice session switched the mode

    uint32_t GuestWord(uint8_t* base, uint32_t address)
    {
        return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + address));
    }
}

PPC_FUNC(sub_82698100)
{
    // The session object's state (+20) is 2 when this call switches the game mode to practice.
    if (g_modeBeforePractice < 0 && GuestWord(base, ctx.r3.u32 + 20) == 2)
    {
        PPCContext call = ctx;
        __imp__sub_82691378(call, base);
        g_modeBeforePractice = call.r3.u32 == kPracticeMode ? 0 : call.r3.u32;
    }
    __imp__sub_82698100(ctx, base);
}

PPC_FUNC(sub_82689370)
{
    if (g_modeBeforePractice >= 0)
    {
        std::fprintf(stderr, "[fix] practice session ended: game mode %u restored\n", uint32_t(g_modeBeforePractice));
        PPCContext call = ctx;
        call.r3.u64 = uint64_t(g_modeBeforePractice);
        __imp__sub_826913C0(call, base);
        g_modeBeforePractice = -1;
    }
    __imp__sub_82689370(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82D1BAA8);  // a game starts (r3 1: from the game info, else the schedule's game r4)
extern "C" PPC_FUNC(__imp__sub_82CFCEC0);  // a scheduled game's teams (r3 the game, r4 and r5 where they go)

// A career game hangs at the kickoff when a team's depth chart is deeper than the game allows. As a
// game starts (sub_82D1BAA8) the game copies both teams' depth charts (DCHT) into DCGA, which holds
// 182 entries: two teams of 91, the standard depth of every position (the computer's teams always have
// exactly that). With more, the copy is cut short and the second team's lineup misses players: the
// game's check that both lineups are complete fails (sub_82CFC9A0, a team without its quarterbacks),
// and the game is played without its setup, which assigns each side its coach (0x8300FA50, 255 =
// none). When the user calls the kickoff, the play call locks the missing coach (null + 0x28), which
// is never free: the game thread waits for good while the screen and the sound go on. (The coaches
// last assigned stay, so it hangs only when no game has been set up since the career was loaded.)
// Before a game starts, entries deeper than a position's standard depth (a fourth tight end, say) are
// taken off both teams' depth charts; the players stay on the roster.
namespace
{
    // The standard depth of each depth chart position (DCHT.PPOS), 91 in all.
    constexpr uint32_t kDepth[26] = { 3, 4, 3, 6, 3, 3, 3, 3, 3, 3, 3, 3, 5, 3, 4, 3, 5, 3, 3, 3, 3, 5, 5, 3, 3, 3 };

    void TrimDepthChart(hc09::guest::Database& db, uint32_t team)
    {
        std::vector<std::pair<uint32_t, uint32_t>> extra;  // (position, depth)
        const std::string declare = "declare \x8A fastcursor for select 'SOPP' into \x85 and 'pedd' into \x85 from 'THCD' where ('DIGT' = " +
            std::to_string(team) + ")";
        db.ForEach(declare.c_str(), 2, [&] {
            const uint32_t position = db.Get(0), depth = db.Get(1);
            if (position < std::size(kDepth) && depth >= kDepth[position])
                extra.emplace_back(position, depth);
        });
        for (auto [position, depth] : extra)
        {
            db.Query("delete from 'THCD' where ('DIGT' = \x85) and ('SOPP' = \x85) and ('pedd' = \x85)", { team, position, depth });
            std::fprintf(stderr, "[fix] team %u depth chart: position %u depth %u is deeper than the game allows, taken off\n", team, position,
                depth + 1);
        }
    }
}

PPC_FUNC(sub_82D1BAA8)
{
    hc09::guest::Database db(ctx, base);
    if (db.Ready())
    {
        if (ctx.r3.u8 == 1)
            db.Query("select 'GTHG' into \x85 and 'GTAG' into \x85 from 'FNIG'", { db.Out(0), db.Out(1) });
        else
            db.Call(__imp__sub_82CFCEC0, { ctx.r4.u32, db.Out(0), db.Out(1) });
        const uint32_t first = db.Get(0), second = db.Get(1);
        for (uint32_t team : { first, second })
            if (team >= 1 && team <= 32)
                TrimDepthChart(db, team);
    }
    __imp__sub_82D1BAA8(ctx, base);
}
