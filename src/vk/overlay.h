#pragma once
// The live tweaker's in-game overlay (runtime/hooks_gameplay.cpp): triangles in window pixels, drawn by the
// swapchain over the presented image (hc09vk::Swapchain::overlay; same layout as hc09vk::OverlayVtx).
#include <cstdint>
#include <vector>

struct OvVtx
{
    float x, y;       // window pixels, y down
    uint32_t rgba;    // R, G, B, A bytes in memory order
    float u = -1, v = -1;   // no atlas (hc09vk::OverlayVtx)
};

// Fill `out` with this frame's overlay for a game image shown at (x0, y0) with size (w, h) in the window.
// Does nothing unless the tweaker runs (HC09_TWEAK) and the overlay is on.
void OverlayBuild(std::vector<OvVtx>& out, float x0, float y0, float w, float h);
