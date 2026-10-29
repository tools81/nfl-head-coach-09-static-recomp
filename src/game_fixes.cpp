// Fixes for bugs in the original game, as overrides of recompiled functions (see debug/hooks.cpp:
// defining sub_<address> replaces the function for every caller; __imp__sub_<address> is the original).

#include <cstdint>
#include <cstdio>

#include <ppc_recomp_shared.h>

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
