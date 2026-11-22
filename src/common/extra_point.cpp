// HC27: extra points are kicked from the 15-yard line (a 33-yard kick), as the NFL has had it since 2015. A
// two-point try stays at the 2.
//
// The game spots every try at the 2 (the field object's number at +0x54: 48 yards from midfield) when the touchdown
// is scored, before either team has called its play, and its play calling takes a try at the 2 for an extra point
// (from the 15 the play suggested to the user is no longer the kick). So the spot is left at the 2 until the plays
// are in, and moved as the teams line up:
//
// - The play flow is a state machine (sub_82533ED0 runs it: +24 its states, +32 the current one, +36 the one before,
//   +48 a change is pending): 5 the play call, 1 the line-up and the wait for the snap, 3 the play, 4 after it. The
//   line-up state's first update puts the players in their formation (sub_826A0DF8), in the same call that makes the
//   change, so a hook on the state machine runs just before it.
// - The plays called are at [0x83115000] + team * 0x14140 + 128; each of a play's 11 players has a place in its
//   formation (a word at + i * 80 + 48): 19 is the kicker of a field goal formation.
// - The line of scrimmage (its distance along the field, z) is the float at 0x830A8A74, and the game keeps about
//   eighteen more copies of it in its objects. They are found as the game runs: before each ordinary play the places
//   whose word is the line's, bit for bit (a line is a fraction of a yard: nothing else holds it), kept only while
//   they go on matching from one play to the next. Moving all of them before the line-up moves the play: the teams
//   line up there, the score bug shows it, and the play counts from it.

#include <windows.h>

#include <emmintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/broadcast_state.h"
#include "vk/dev_env.h"

extern "C" PPC_FUNC(__imp__sub_82533ED0);

namespace
{
    constexpr uint32_t kLine = 0x830A8A74;      // the line of scrimmage's z
    constexpr uint32_t kPlays = 0x83115000;     // the teams' plays
    constexpr uint32_t kTry = 0x42400000;       // 48.0f: the 2-yard line
    constexpr uint32_t kKick = 0x420C0000;      // 35.0f: the 15-yard line
    constexpr uint32_t kSign = 0x80000000;

    uint32_t Word(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }
    void Put(uint8_t* base, uint32_t a, uint32_t w) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(w); }

    std::vector<uint32_t> g_copies;   // where the line is kept
    int g_confirmed = 0;              // plays on which every copy still matched
    int g_missed = 0;                 // plays in a row on which the copies did not hold the line

    // Every place in the game's memory that holds these bits.
    std::vector<uint32_t> Find(uint8_t* base, uint32_t bits)
    {
        std::vector<uint32_t> found;
        const uint32_t raw = __builtin_bswap32(bits);
        MEMORY_BASIC_INFORMATION info;
        for (uint64_t at = 0x40000000ull; at < 0xC0000000ull && found.size() < 4096;)
        {
            if (!VirtualQuery(base + at, &info, sizeof(info)))
                break;
            const uint64_t end =
                std::min<uint64_t>(0xC0000000ull, at + info.RegionSize - uint64_t(base + at - static_cast<uint8_t*>(info.BaseAddress)));
            // (Read-only pages too: the renderer write-protects pages of the game's memory it watches; a write to one
            // goes through its handler as the game's own do.)
            if (info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(info.Protect & PAGE_GUARD))
            {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(base + ((at + 15) & ~15ull));
                const uint32_t* const last = reinterpret_cast<const uint32_t*>(base + (end & ~15ull));
                const __m128i want = _mm_set1_epi32(int(raw));
                for (; p < last; p += 4)
                {
                    const int mask = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(p)), want)));
                    if (mask)
                        for (int k = 0; k < 4; ++k)
                            if (mask & (1 << k))
                                found.push_back(uint32_t(reinterpret_cast<const uint8_t*>(p + k) - base));
                }
            }
            at = end;
        }
        return found;
    }

    // Before an ordinary play: the copies checked against the line as it is now, and found afresh now and then.
    void Learn(uint8_t* base, uint32_t bits)
    {
        float line;
        std::memcpy(&line, &bits, 4);
        // Only a line that nothing else is likely to hold: a fraction of a yard.
        if (!(std::abs(line) > 1.0f && std::abs(line) < 50.0f) || std::abs(line - std::round(line * 4.0f) / 4.0f) < 0.01f)
            return;
        const size_t before = g_copies.size();
        const size_t matching = size_t(std::count_if(g_copies.begin(), g_copies.end(), [&](uint32_t a) { return Word(base, a) == bits; }));
        if (before && matching == before)
            ++g_confirmed;
        else if (matching >= 8)
        {
            // The first plays after a search: what else held the first line's bits drops out. Those left have held
            // the line on two plays.
            g_copies.erase(std::remove_if(g_copies.begin(), g_copies.end(), [&](uint32_t a) { return Word(base, a) != bits; }), g_copies.end());
            g_confirmed = std::max(g_confirmed, 1);
        }
        else if (before >= 8 && ++g_missed < 3)
            return;   // not a play from this line (the game has not set it yet): the copies are kept
        else
        {
            g_copies.clear();
            g_confirmed = 0;
        }
        g_missed = 0;
        // A search of the game's memory takes a few tenths of a second: once a game (as the first play's teams line
        // up, behind the cut from the play call), and again only if the copies were lost.
        if (g_copies.size() < 8)
        {
            const auto start = std::chrono::steady_clock::now();
            std::vector<uint32_t> found = Find(base, bits);
            // The places known are all still among them: they stand (a passing copy elsewhere is not taken up).
            // Otherwise the new ones have to match again on the plays that follow.
            const bool stand = g_copies.size() >= 8 && std::all_of(g_copies.begin(), g_copies.end(), [&](uint32_t a) {
                return std::binary_search(found.begin(), found.end(), a);
            });
            if (!stand)
            {
                g_copies.swap(found);
                g_confirmed = 0;
            }
            static const bool log = Hc09Env("HC09_EXTRA_POINT_LOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[extra point] line %.3f: %zu copies (%.0f ms)\n", line, g_copies.size(),
                             std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }

    // Whether either team's play has a kicker (a field goal formation).
    bool KickCalled(uint8_t* base)
    {
        const uint32_t plays = Word(base, kPlays);
        if (plays < 0x40000000u)
            return false;
        for (uint32_t team = 0; team < 2; ++team)
            for (uint32_t i = 0; i < 11; ++i)
                if (Word(base, plays + team * 0x14140 + 128 + i * 80 + 48) == 19)
                    return true;
        return false;
    }

    void Move(uint8_t* base, uint32_t from, uint32_t to)
    {
        int moved = 0;
        for (uint32_t a : g_copies)
            if (Word(base, a) == from)
            {
                Put(base, a, to);
                ++moved;
            }
        static const bool log = Hc09Env("HC09_EXTRA_POINT_LOG") != nullptr;
        if (log)
        {
            float f, t;
            std::memcpy(&f, &from, 4);
            std::memcpy(&t, &to, 4);
            std::fprintf(stderr, "[extra point] line %.2f -> %.2f in %d of %zu copies\n", f, t, moved, g_copies.size());
        }
    }

    // The teams are about to line up.
    void LineUp(uint8_t* base)
    {
        const uint32_t bits = Word(base, kLine);
        const std::string down = hc09::broadcast::Snapshot().down;
        // The score bug's text, where it is read (with the score bar: common/broadcast_state.h), says whether this is
        // a try; without it, a line exactly on the 2 is taken for one (a play's own line is a fraction of a yard).
        const bool known = down.find_first_not_of(' ') != std::string::npos;
        const bool isTry = (bits & ~kSign) == kTry && (!known || down.find("PAT") != std::string::npos);
#ifdef HC09_EDITION_HC27
        if (isTry)
        {
            // The copies must have held the line on two plays (else the kick stays at the 2, as the game has it).
            const bool kick = KickCalled(base);
            static const bool log = Hc09Env("HC09_EXTRA_POINT_LOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[extra point] try: %s, %zu copies held on %d plays\n", kick ? "a kick" : "not a kick", g_copies.size(), g_confirmed);
            if (g_confirmed >= 1 && g_copies.size() >= 8 && kick)
                Move(base, bits, (bits & kSign) | kKick);
            return;
        }
#endif
        if (isTry || (known && down.find(" on ") == std::string::npos))   // a try, a kickoff: not a play from the line
            return;
        Learn(base, bits);
        // Dev (HC09_GOAL_LINE=1): a play in the other team's half is run from its 1-yard line instead, for touchdowns
        // (and extra points to test) every few plays.
        static const bool goalLine = Hc09Env("HC09_GOAL_LINE") != nullptr;
        if (goalLine && known && g_confirmed >= 1 && (bits & ~kSign) < 0x42440000)
        {
            const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
            const std::string spot = down.substr(down.find(" on ") + 4);
            const std::string& offense = board.possession == 1 ? board.home.abbreviation : board.away.abbreviation;
            if (board.possession >= 0 && !offense.empty() && spot.compare(0, offense.size(), offense) != 0)
                Move(base, bits, (bits & kSign) | 0x42440000);   // 49.0f
        }
    }
}

PPC_FUNC(sub_82533ED0)
{
    const uint32_t machine = ctx.r3.u32;
    if (base[machine + 48] && Word(base, machine + 32) == 1 && Word(base, machine + 36) == 5)
        LineUp(base);
    __imp__sub_82533ED0(ctx, base);
}
