#pragma once
// Guest addresses that depend on the game version (docs/HANDOFF_M10_5.md step 4). The runtime is written against
// the 1.0 executable: function names (sub_8XXXXXXX) and GameAddr(0x8XXXXXXX) literals are 1.0's. The 1.2 build
// (CMake HC09_GAME_VERSION=1.2) force-includes runtime/game_addr_v12.h (tools/gen_addr.py), which #defines each
// 1.0 function name the runtime uses to its 1.2 function and gives GameAddr its 1.2 values; an address it does not
// list is a compile error, so nothing runs against a 1.0 address by mistake.
#include <cstdint>

#ifndef HC09_GAME_V12
#define HC09_GAME_V12 0
#endif

#if HC09_GAME_V12
#include "game_addr_v12.h"
consteval uint32_t GameAddr(uint32_t a)
{
    for (const auto& p : hc09_v12::kAddr)
        if (p.v10 == a) return p.v12;
    throw "GameAddr: no 1.2 address for this 1.0 address (run tools/gen_addr.py)";
}
#else
consteval uint32_t GameAddr(uint32_t a) { return a; }
#endif

// The image this build runs (its PE header's TimeDateStamp), checked when it is loaded (runtime.cpp).
constexpr uint32_t kImageTimestamp = HC09_GAME_V12 ? 0x48BF0CB0u : 0x485C057Fu;   // 2008-09-03 / 2008-06-20
constexpr const char* kGameVersion = HC09_GAME_V12 ? "1.2" : "1.0";
