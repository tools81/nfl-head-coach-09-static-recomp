// HC27: draft picks signed to the rookie wage scale as the draft ends.
//
// In the game a drafted player joins no team at first: he is held as a draft pick (team 1015) with
// his rights in the DRPP table (player, team, round, pick in the round) until his team signs him,
// the user's picks after a negotiation with their agents. Since 2011 a pick's contract is set by
// the pick instead: four years at the value and signing bonus of its slot. So when a career's
// draft ends, every pick still to be signed is signed to his slot of hc27\rookie_scale.txt
// (tools/roster_import/rookie_scale.py), the pick numbered by round and pick in the round, through
// the game's own signing (which puts him on the team, gives him a jersey number and reports the
// signing). A pick whose team already has 75 players is left to be signed as the game does.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"

#ifdef HC09_EDITION_HC27
// Signs a player: r3 the team, r4 the player, r5 years, r6 the total value and r7 the signing bonus
// (both in $10,000s), r8 the signing's context (8 zero bytes for a plain signing), r9 incentives
// (0 for none).
extern "C" PPC_FUNC(__imp__sub_82D27A28);
// The draft's end: the picks made go to DRPP, and the next drafts' picks move up a year.
extern "C" PPC_FUNC(__imp__sub_82D29270);

namespace
{
    constexpr uint32_t kDraftPick = 1015;  // the team of a drafted player not yet signed
    constexpr uint32_t kNoPlayer = 32767;
    constexpr uint32_t kYears = 4;
    constexpr uint32_t kRounds = 7, kTeams = 32;
    constexpr uint32_t kActiveLimit = 0x8307FCEC;  // u16: the most players off injured reserve a team signs to
    constexpr uint16_t kRosterMost = 75;           // the most players a team signs to in all

    struct Slot
    {
        uint32_t total = 0, bonus = 0, salary[kYears] = {};  // $10,000s
    };

    // The scale, by pick (the first pick first).
    const std::vector<Slot>& Scale()
    {
        static const std::vector<Slot> scale = [] {
            std::vector<Slot> slots;
            std::ifstream file("hc27\\rookie_scale.txt");
            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                uint32_t pick = 0;
                Slot slot;
                if (fields >> pick >> slot.total >> slot.bonus >> slot.salary[0] >> slot.salary[1] >> slot.salary[2] >>
                        slot.salary[3] &&
                    pick == slots.size() + 1)
                    slots.push_back(slot);
            }
            return slots;
        }();
        return scale;
    }

    using hc09::guest::Database;

    uint32_t TeamOf(Database& db, uint32_t player)
    {
        db.Query("select 'DIGT' into \x85 from 'YALP' where ('DIGP' = \x85)", { db.Out(0), player });
        return db.Get(0);
    }

    // The slot's salary and bonus year by year (the game's signing spreads them its own way) and
    // this season's cap number.
    void WriteYears(Database& db, uint32_t player, const Slot& slot)
    {
        uint32_t bonus[kYears];
        for (uint32_t y = 0; y < kYears; y++)
            bonus[y] = slot.bonus / kYears + (y < slot.bonus % kYears ? 1 : 0);
        db.Query("update 'YALP' set '0ASP' = \x85 and '1ASP' = \x85 and '2ASP' = \x85 where ('DIGP' = \x85)",
                 { slot.salary[0], slot.salary[1], slot.salary[2], player });
        db.Query("update 'YALP' set '3ASP' = \x85 and '0BSP' = \x85 and '1BSP' = \x85 where ('DIGP' = \x85)",
                 { slot.salary[3], bonus[0], bonus[1], player });
        db.Query("update 'YALP' set '2BSP' = \x85 and '3BSP' = \x85 and 'ASCP' = \x85 where ('DIGP' = \x85)",
                 { bonus[2], bonus[3], slot.salary[0] + bonus[0], player });
    }

    void SignPicks(PPCContext& ctx, uint8_t* base)
    {
        const std::vector<Slot>& scale = Scale();
        Database db(ctx, base);
        if (scale.empty() || !db.Ready())
            return;
        // The signing needs room on the roster, counting the team's unsigned picks: fewer than 53
        // players off injured reserve (the game's limit all year) and 75 in all. Picks are owed
        // their contracts, so for them only the second applies, as offseason rosters run larger in
        // the NFL too; teams cut down to 53 on the game's cut day before the season.
        const uint16_t activeLimit = PPC_LOAD_U16(kActiveLimit);
        PPC_STORE_U16(kActiveLimit, kRosterMost);
        int signedPicks = 0, leftPicks = 0;
        for (uint32_t round = 1; round <= kRounds; round++)
            for (uint32_t pick = 1; pick <= kTeams; pick++)
            {
                db.Query("select 'DIGP' into \x85 and 'DIGT' into \x85 from 'PPRD' where ('ORDP' = \x85) and ('IPDP' = \x85)",
                         { db.Out(0), db.Out(1), round, pick });
                const uint32_t player = db.Get(0), team = db.Get(1);
                if (player == kNoPlayer || team >= kDraftPick || TeamOf(db, player) != kDraftPick)
                    continue;
                const Slot& slot = scale[std::min<size_t>((round - 1) * kTeams + pick, scale.size()) - 1];
                PPCContext call = ctx;
                call.r3.u64 = team;
                call.r4.u64 = player;
                call.r5.u64 = kYears;
                call.r6.u64 = slot.total;
                call.r7.u64 = slot.bonus;
                call.r8.u64 = db.Zeros();
                call.r9.u64 = 0;
                __imp__sub_82D27A28(call, base);
                if (TeamOf(db, player) == kDraftPick)
                {
                    leftPicks++;  // the team's roster is full
                    continue;
                }
                WriteYears(db, player, slot);
                signedPicks++;
            }
        PPC_STORE_U16(kActiveLimit, activeLimit);
        std::fprintf(stderr, "[hc27] rookie scale: %d draft picks signed to their slots", signedPicks);
        if (leftPicks)
            std::fprintf(stderr, ", %d left to negotiate (rosters of %u)", leftPicks, kRosterMost);
        std::fprintf(stderr, "\n");
    }
}

PPC_FUNC(sub_82D29270)
{
    // The draft's state (byte 3 set for a mock draft, which ends without changing the career).
    const uint32_t draft = PPC_LOAD_U32(0x832111C4);
    const bool mock = draft == 0 || PPC_LOAD_U8(draft + 3) != 0;
    __imp__sub_82D29270(ctx, base);
    if (!mock)
        SignPicks(ctx, base);
}
#endif
