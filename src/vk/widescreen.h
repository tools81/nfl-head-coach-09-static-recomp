#pragma once
// hc09: ultrawide screens (hc09.ini [Graphics] AspectRatio, as the switch HC09_ASPECT: the picture's width / height).
//
// The game renders 16:9; the window shows its image stretched out to the aspect (vk_swapchain.cpp). Before that, the
// native renderer squeezes the image's content toward the centre by the same factor (vk_native.cpp), so stretched,
// it keeps its shape:
// - 3D draws (their vertex shader reads the scenes' perspective camera, c156..c159) on targets of the screen's shape
//   have their clip x squeezed: they see wider, the extra width at the sides. The game still culls to its 16:9 view,
//   so something just off the original picture's sides can be missing.
// - 2D draws on screen-sized targets (menus, HUD, the 2D drawn into the 3D scenes) move toward the centre: they keep
//   their shape, in a 16:9 area in the middle. Vertices past the screen's left and right edges stay (what the game
//   keeps off-screen stays there), and so do those on the edges of plain draws (no texture over 32x32: fades, bars).
//   Textured full-screen pictures keep their shape, pillarboxed. The HUD is not moved to the screen's edges.
// - Quads that show 3D content (they sample a texture resolved from a target with 3D draws this frame) are squeezed
//   with their vertices on the screen's edges kept: full-screen ones fill the width, the panels' backdrops cut from
//   the scene move with their panels.
// The game itself is not told: its cameras, and the HUD it lays out from them, stay 16:9.
#include <cstdlib>

#include "dev_env.h"

namespace hc09vk {

// How much wider than 16:9 the picture is (1: 16:9, the game's own).
inline float WidescreenStretch()
{
    static const float stretch = [] {
        const char* e = Hc09Env("HC09_ASPECT");
        float aspect = e && *e ? float(std::atof(e)) : 0.0f;
        float s = aspect / (16.0f / 9.0f);
        return s > 1.01f && s < 4.0f ? s : 1.0f;
    }();
    return stretch;
}

}  // namespace hc09vk
