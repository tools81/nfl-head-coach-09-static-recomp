// HC27: the fifth-year option of first-round picks.
//
// A first-round pick's four-year rookie contract carries a team option on a fifth year, which the
// team takes up or declines in the off-season before his fourth year. The game has no such option;
// HC27 offers it on the game's restricted free agent tender screen, which already asks the same
// kind of question at that time of year. The player selection is RFRF (player, team, round, tender
// level RFLE, state RFNS), which the game fills with its restricted free agents as the tender
// period begins (sub_82D01C90); HC27 adds each first-round pick going into the last year of his
// rookie deal (three years in the league, the fourth of four contract years still to play), and
// for those players the screen offers two choices instead of the tenders:
//
//   Decline Option            he plays out his rookie contract; nothing changes
//   Exercise 5th-Year Option  a fifth contract year at the option salary
//
// The option salary is the 2020 CBA's basic one: the average of the 3rd to the 25th highest
// salaries (this season's cap numbers) at his position across the league. (Its higher tiers for
// Pro Bowl selections are not applied: careers keep no Pro Bowl history for the imported players.)
// A choice is applied at once, as the game applies tenders, and the player leaves the tender
// selection. The computer's teams exercise an option when the player rates at least as well as the
// players paid that salary, less two; a player whose option nobody decides keeps his contract.
// The imported players' draft rounds come from tools/roster_import/draft_rounds.py.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"

#ifdef HC09_EDITION_HC27
extern "C" PPC_FUNC(__imp__sub_82D01C90);  // fills RFRF as the tender period begins
extern "C" PPC_FUNC(__imp__sub_827A8910);  // the tender screen's choices for a player
extern "C" PPC_FUNC(__imp__sub_827A8D38);  // applies the user's choice
extern "C" PPC_FUNC(__imp__sub_825BA870);  // a tender decided by the staff (a delegated task)
extern "C" PPC_FUNC(__imp__sub_82604958);  // the computer's tender decisions for a team
extern "C" PPC_FUNC(__imp__sub_826C4F58);  // the user's team

namespace
{
    using hc09::guest::Database;

    constexpr uint32_t kUndecided = 7;  // RFLE of a player not yet decided on
    constexpr uint32_t kLastTeam = 999;

    // A first-round pick in the last year of his rookie deal: his team, or Database::kNone.
    uint32_t OptionTeam(Database& db, uint32_t player)
    {
        db.Query("select 'DIGT' into \x85 from 'YALP' where ('DIGP' = \x85) and ('PRYP' = 3) and ('LYCP' = 1) and "
                 "('NOCP' = 4) and ('ORDP' = 1)",
                 { db.Out(0), player });
        const uint32_t team = db.Get(0);
        return team <= kLastTeam ? team : Database::kNone;
    }

    // An option player still in the tender selection: his tender level (RFLE), or Database::kNone.
    uint32_t PendingLevel(Database& db, uint32_t player)
    {
        if (OptionTeam(db, player) == Database::kNone)
            return Database::kNone;
        db.Query("select 'ELFR' into \x85 from 'FRFR' where ('DIGP' = \x85)", { db.Out(0), player });
        return db.Get(0);
    }

    // Position groups as the league's tenders count them: QB, RB, WR, TE, OL, DE, DT, LB, CB, S, K/P.
    int Group(uint32_t position)
    {
        static const int groups[] = { 0, 1, 1, 2, 3, 4, 4, 4, 4, 4, 5, 5, 6, 7, 7, 7, 8, 9, 9, 10, 10 };
        return position < std::size(groups) ? groups[position] : -1;
    }

    struct Price
    {
        uint32_t salary = 0;  // $10,000s
        uint32_t rating = 0;  // the overall rating of the players paid it
    };

    // The option salary at a player's position.
    Price OptionPrice(Database& db, uint32_t player)
    {
        db.Query("select 'SOPP' into \x85 from 'YALP' where ('DIGP' = \x85)", { db.Out(0), player });
        const int group = Group(db.Get(0));
        std::vector<std::pair<uint32_t, uint32_t>> paid;  // (cap number, rating)
        db.ForEach("declare \x8A fastcursor for select 'SOPP' into \x85 and 'ASCP' into \x85 and 'RVOP' into \x85 "
                   "from 'YALP' where ('LYCP' > 0) and ('DIGT' < 1000)",
                   3, [&] {
                       if (Group(db.Get(0)) == group)
                           paid.emplace_back(db.Get(1), db.Get(2));
                   });
        std::sort(paid.begin(), paid.end(), std::greater<>());
        Price price;
        uint64_t salaries = 0, ratings = 0, count = 0;
        for (size_t i = 2; i < paid.size() && i < 25; i++, count++)
        {
            salaries += paid[i].first;
            ratings += paid[i].second;
        }
        if (count)
        {
            price.salary = uint32_t(salaries / count);
            price.rating = uint32_t(ratings / count);
        }
        return price;
    }

    // Takes up or declines the option; either way the player leaves the tender selection.
    void Decide(Database& db, uint32_t player, bool exercise, const char* who)
    {
        const Price price = exercise ? OptionPrice(db, player) : Price{};
        if (exercise && price.salary)
        {
            db.Query("select 'ASTP' into \x85 from 'YALP' where ('DIGP' = \x85)", { db.Out(0), player });
            const uint32_t total = db.Get(0) + price.salary;
            db.Query("update 'YALP' set 'NOCP' = 5 and 'LYCP' = 2 and '4ASP' = \x85 and '4BSP' = 0 and 'ASTP' = \x85 "
                     "where ('DIGP' = \x85)",
                     { price.salary, total, player });
            db.Query("update 'YALP' set 'STVP' = \x85 where ('DIGP' = \x85)", { total, player });
        }
        db.Query("delete from 'FRFR' where ('DIGP' = \x85)", { player });
        std::fprintf(stderr, "[hc27] fifth-year option of player %u %s by %s", player,
                     exercise && price.salary ? "exercised" : "declined", who);
        if (exercise && price.salary)
            std::fprintf(stderr, " ($%.2fM)", price.salary / 100.0);
        std::fprintf(stderr, "\n");
    }

    // The computer's choice: whether the player is worth what the option pays.
    bool ComputerExercises(Database& db, uint32_t player)
    {
        const Price price = OptionPrice(db, player);
        db.Query("select 'RVOP' into \x85 from 'YALP' where ('DIGP' = \x85)", { db.Out(0), player });
        return price.salary && db.Get(0) + 2 >= price.rating;
    }

    std::string Money(uint32_t tenThousands)
    {
        char text[32];
        std::snprintf(text, sizeof text, "$%.2fM", tenThousands / 100.0);
        return text;
    }
}

// The tender period begins: the option players join the restricted free agents.
PPC_FUNC(sub_82D01C90)
{
    __imp__sub_82D01C90(ctx, base);
    Database db(ctx, base);
    if (!db.Ready())
        return;
    std::vector<std::pair<uint32_t, uint32_t>> players;  // (player, team)
    db.ForEach("declare \x8A fastcursor for select 'DIGP' into \x85 and 'DIGT' into \x85 from 'YALP' where "
               "('PRYP' = 3) and ('LYCP' = 1) and ('NOCP' = 4) and ('ORDP' = 1)",
               2, [&] {
                   if (db.Get(1) <= kLastTeam)
                       players.emplace_back(db.Get(0), db.Get(1));
               });
    int added = 0;
    for (auto [player, team] : players)
    {
        db.Query("select 'DIGT' into \x85 from 'FRFR' where ('DIGP' = \x85)", { db.Out(0), player });
        if (db.Get(0) != Database::kNone)
            continue;
        added += db.Query("insert into 'FRFR' set 'DIGT' = \x85 and 'DIGP' = \x85 and 'ORDP' = 1 and 'ELFR' = 7 and 'SNFR' = 0",
                          { team, player });
    }
    std::fprintf(stderr, "[hc27] fifth-year option: %d first-round picks to decide on\n", added);
}

// The screen's choices (r4 the player, r5 the list they go in: its methods +0x14 set a string,
// +0x0C a number and +0x08 a flag of a row, by row and the field's name).
PPC_FUNC(sub_827A8910)
{
    Database db(ctx, base);
    const uint32_t player = ctx.r4.u32, list = ctx.r5.u32;
    const uint32_t level = db.Ready() ? PendingLevel(db, player) : Database::kNone;
    if (level == Database::kNone)
    {
        __imp__sub_827A8910(ctx, base);
        return;
    }
    // The field names, as the game's own rows use them.
    constexpr uint32_t kTitle = 0x8209C2B4, kDescription = 0x8209C2E0, kKey = 0x8209C2EC, kDefault = 0x8209C2F0;
    const auto method = [&](uint32_t offset) {
        return PPC_LOOKUP_FUNC(base, PPC_LOAD_U32(PPC_LOAD_U32(list) + offset));
    };
    const auto row = [&](uint32_t n, const std::string& title, const std::string& description, bool chosen) {
        db.Call(method(0x14), { list, n, kTitle, db.Text(0, title) });
        db.Call(method(0x14), { list, n, kDescription, db.Text(1, description) });
        db.Call(method(0x0C), { list, n, kKey, n });
        db.Call(method(0x08), { list, n, kDefault, chosen });
    };
    const Price price = OptionPrice(db, player);
    row(0, "Decline Option", "He plays out his rookie contract, then can become a free agent.", level == 0);
    row(1, "Exercise 5th-Year Option", "Adds a fifth year to his contract at " + Money(price.salary) + ".",
        level >= 1 && level <= 4);
}

// The user's choice (r4 the player, r5 the row's key).
PPC_FUNC(sub_827A8D38)
{
    Database db(ctx, base);
    const uint32_t player = ctx.r4.u32, key = ctx.r5.u32;
    if (!db.Ready() || PendingLevel(db, player) == Database::kNone)
    {
        __imp__sub_827A8D38(ctx, base);
        return;
    }
    Decide(db, player, key >= 1, "the user");
}

// A tender the staff decide (the task's +0x1C: team << 16 | player).
PPC_FUNC(sub_825BA870)
{
    Database db(ctx, base);
    const uint32_t task = ctx.r3.u32;
    const uint32_t player = PPC_LOAD_U32(task + 0x1C) & 0xFFFF;
    if (!db.Ready() || PendingLevel(db, player) == Database::kNone)
    {
        __imp__sub_825BA870(ctx, base);
        return;
    }
    Decide(db, player, ComputerExercises(db, player), "the staff");
    db.Call(sub_82C94DF8, task);  // the task is done, as the game's own ending
    ctx.r3.u64 = 0;
}

// The computer's tender decisions for a team (r3 the team, r4 its players: a vector of 24-byte
// entries, the player first). Option players are taken out first: a computer team decides on them
// itself, and the user's (for which the game only works out recommendations) are left to the user.
PPC_FUNC(sub_82604958)
{
    Database db(ctx, base);
    if (db.Ready())
    {
        const uint32_t team = ctx.r3.u32, players = ctx.r4.u32;
        const uint32_t user = db.Call(__imp__sub_826C4F58, std::initializer_list<uint32_t>{});
        const uint32_t begin = PPC_LOAD_U32(players);
        uint32_t end = PPC_LOAD_U32(players + 4);
        for (uint32_t entry = begin; entry < end;)
        {
            const uint32_t player = PPC_LOAD_U32(entry);
            if (PendingLevel(db, player) == Database::kNone)
            {
                entry += 24;
                continue;
            }
            if (team != user)
                Decide(db, player, ComputerExercises(db, player), "the computer");
            std::memmove(base + entry, base + entry + 24, end - entry - 24);
            end -= 24;
        }
        PPC_STORE_U32(players + 4, end);
    }
    __imp__sub_82604958(ctx, base);
}
#endif
