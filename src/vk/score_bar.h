#pragma once
// hc09: the score bar (roadmap 12): the game's score bug (common/broadcast_state.h) drawn as a bar along the bottom of
// the picture, in the swapchain's overlay at the window's resolution.

#include <cstdint>

namespace hc09vk
{
    struct Swapchain;
}

namespace xenos
{
    struct Image;
}

// Adds this frame's bar to the swapchain's overlay (and its atlas when the size or the teams' logos change), for a
// game picture shown at (x0, y0) with size (w, h) in the window. Nothing while the game shows no score bug.
void ScoreBarBuild(hc09vk::Swapchain& swapchain, float x0, float y0, float w, float h);

// The teams' logos: whether a texture (by content hash) is one, and its image as drawn (after any replacement:
// HC27's new logos), given by the renderer as it decodes it (vk_replacements.cpp).
bool ScoreBarWantsTexture(uint64_t hash);
void ScoreBarOfferTexture(uint64_t hash, const xenos::Image& image);
