// The computer's depth charts and its rookies.
//
// The computer orders a depth chart (sub_8265ABE0, one team and position) by an evaluation of
// each player: eight scores per evaluation slot, computed from his ratings. A rookie is the
// exception while the game's rookie flag is set (sub_825D9B98, the byte at 0x830B067F, set
// through his first season): he is ranked by the team's scouting grades for him instead, read
// from RPGR (PSZG ... POVG, by player and team; sub_825D5D78). But RPGR holds the draft class
// being scouted, and the game fills it with the next class right after the draft, so for the
// rookies just drafted there are no grades: the read finds no row, the rookie scores 0 and is
// ranked last at every position whatever his ratings, until the flag clears the next off-season.
// (In a test career, at week 1 of the second season none of the 250 rookies had grades, and
// first-round picks rated as their team's best started no games.)
//
// The depth chart is judged twice: by the ranking pass, and again when it is written to DCHT
// (sub_825FB3D0 -> sub_825F78D8 picks each depth slot's player by sub_825F2B90(player, team,
// slot, scores)), which has the same rule: while the rookie flag is set, a rookie is scored by
// RPGR (sub_825D5D78) at his own position.
//
// So in both places, a player his team has no grades for is judged by his ratings, like every
// other player. Rookies the team did grade keep the game's behaviour. HC09_ROOKIE_GRADES=0 turns
// this off.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"

extern "C" PPC_FUNC(__imp__sub_825D9B98);  // the rookie flag
extern "C" PPC_FUNC(__imp__sub_825F2B90);  // a player's scores for a depth slot, as the chart is written
extern "C" PPC_FUNC(__imp__sub_825EFA08);  // a player's scores for an evaluation slot, from his ratings

namespace
{
    constexpr uint32_t kRookieCheckInChart = 0x8265AE58;  // sub_8265ABE0, after its call of sub_825D9B98
    constexpr uint32_t kRookieFlag = 0x830B067F;

    const bool g_enabled = [] {
        const char* value = std::getenv("HC09_ROOKIE_GRADES");
        return !value || std::strcmp(value, "0") != 0;
    }();
    const bool g_log = std::getenv("HC09_ROOKIE_GRADES_LOG") != nullptr;

    // Whether the team has scouting grades (an RPGR row) for the player.
    bool Graded(PPCContext& ctx, uint8_t* base, uint32_t player, uint32_t team)
    {
        hc09::guest::Database db(ctx, base);
        if (!db.Ready())
            return true;
        db.Query("select 'DIGP' into \x85 from 'RGPR' where ('DIGP' = \x85) and ('DIGT' = \x85)", { db.Out(0), player, team });
        return db.Get(0) != hc09::guest::Database::kNone;
    }
}

// sub_825F2B90(player, team, slot, scores): while the rookie flag is set, a rookie on the team is
// scored by RPGR at his own position; everyone else by sub_825EFA08(player, slot, scores).
PPC_FUNC(sub_825F2B90)
{
    const uint32_t player = ctx.r3.u32, team = ctx.r4.u32, slot = ctx.r5.u32, scores = ctx.r6.u32;
    if (g_enabled && base[kRookieFlag] && !Graded(ctx, base, player, team))
    {
        PPCContext call = ctx;
        call.r3.u64 = player;
        call.r4.u64 = slot;
        call.r5.u64 = scores;
        __imp__sub_825EFA08(call, base);
        return;
    }
    __imp__sub_825F2B90(ctx, base);
}

PPC_FUNC(sub_825D9B98)
{
    // At the chart's check: r30 is the player (record word 2), r15 the team, as sub_8265ABE0
    // passes them to sub_825D5D78 when it reads the grades.
    const bool inChart = uint32_t(ctx.lr) == kRookieCheckInChart;
    const uint32_t player = ctx.r30.u32, team = ctx.r15.u32;
    __imp__sub_825D9B98(ctx, base);
    if (!g_enabled || !inChart || !(ctx.r3.u32 & 0xFF))
        return;

    const uint32_t r3 = ctx.r3.u32;
    const bool graded = Graded(ctx, base, player, team);
    ctx.r3.u64 = graded ? r3 : 0;
    if (g_log)
        std::fprintf(stderr, "[rookie] player %u team %u: %s\n", player, team, graded ? "graded, ranked by grades" : "no grades, ranked by ratings");
}
