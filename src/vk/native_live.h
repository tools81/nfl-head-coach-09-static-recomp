#pragma once
// The native renderer in the running game (the default; HC09_RENDERER=emulated
// selects the emulated Xenos renderer instead). gpu_live.cpp
// owns the Vulkan device, the window and a renderer thread; runtime/hooks_native.cpp
// turns the game's D3D calls into work for it through d3dn::Frontend.
#include "xenos/d3d_frontend.h"

// HC09_RENDERER (hc09.ini [display] renderer): unset, "auto" or "native" = the
// native renderer (default since 2026-10-07); "emulated" = the emulated Xenos
// renderer (EDRAM, the console's command stream; the correctness reference).
// Unknown values warn once and mean native. Only matters with HC09_GPU_LIVE on.
bool NativeRendererSelected();
// The native renderer runs: selected and the live renderer on (HC09_GPU_LIVE).
bool NativeRendererLive();
// The back end of the live native renderer, or nullptr (the emulated renderer is
// selected, or the live renderer could not start). Valid once GpuLiveInit ran.
d3dn::Backend* NativeLiveBackend();
// HC09_NATIVE_PROF=1: profiling on; the game thread's time in the front end.
bool NativeLiveProfiling();
// kind: 0 calls other than draws, 1 draws, 2 submits (work handed to the back end).
void NativeLiveAddFrontendNs(int kind, uint64_t ns);
// Fences from the native GPU (HC09_NATIVE_FENCES, default on; gpu_live.cpp NativeLive):
// D3D's kick wrote `fence` (its fence word at physical `fenceWordPa`) after the work
// handed over (call under the feed lock). The first call starts the native fences.
void NativeLiveKicked(uint32_t fenceWordPa, uint32_t fence);
bool NativeLiveFencesOn();
// A D3D wait for `fence` (`cur` = D3D's next fence, dev+10908): blocks host-side until
// the fence word passes it, asking the renderer to submit the work if needed.
void NativeLiveFenceWait(uint32_t cur, uint32_t fence, uint32_t caller = 0, uint32_t type = 0);
// The game polls `fence`: if it is pending, the renderer submits the work before it.
void NativeLiveFenceNeeded(uint32_t cur, uint32_t fence);
