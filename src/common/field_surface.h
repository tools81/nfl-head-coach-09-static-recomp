#pragma once

// The field of the game being played, for the Vulkan renderer's turf (src/vk/gpu_vk/vk_materials.cpp): set as each
// game starts (game_fixes.cpp, from the game's stadium, GINF.SGID).

#include <atomic>
#include <cstdint>

namespace hc09
{
    // 1: artificial turf (it never wears), 0: grass, -1: not known yet.
    inline std::atomic<int> g_fieldTurf{ -1 };
    // Counts the games started, so the turf's wear starts over with each.
    inline std::atomic<uint32_t> g_gamesStarted{ 0 };
}
