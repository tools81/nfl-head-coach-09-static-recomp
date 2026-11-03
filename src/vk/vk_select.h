#pragma once

// Which renderer draws the game: the native Vulkan renderer (src/vk, from NFL Head Coach 09 for PC; the default) or
// the D3D12 renderer (src/gpu). hc09.ini [Graphics] Renderer=vulkan or d3d12; HC09_RENDERER=vulkan or d3d12
// overrides it for one run. Without a Vulkan 1.3 driver, and in builds made without thirdparty/vulkan, the D3D12
// renderer draws.

namespace hc09::vk
{
#ifdef HC09_HAVE_VULKAN
    bool Selected();
    // At startup, before any game code runs: sets the Vulkan renderer's own switches when it is selected.
    void Configure();
    // Once guest memory exists, before any game code runs: keeps the physical memory the renderer uses for itself
    // out of the game's allocations.
    void ReserveMemory();
    // When the game starts the GPU (its ring buffer): the window, the device and the renderer thread.
    void Start();
#else
    inline bool Selected() { return false; }
    inline void Configure() {}
    inline void ReserveMemory() {}
    inline void Start() {}
#endif
}
