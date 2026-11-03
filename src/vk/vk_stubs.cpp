// Adapter (this project): the development tools of NFL Head Coach 09 for PC that the renderer reaches but this
// project does not include (its tools_stubs.cpp did the same for its release build). Everything here answers "off".
#include "runtime.h"
#include "overlay.h"
#include "profiler.h"

// hooks_d3d.cpp (HC09_TRACE_D3D's D3D call census and timing).
void D3DTraceCount(uint32_t, PPCContext&) {}
int D3DTraceEnter(uint32_t, PPCContext&) { return -1; }
void D3DTraceLeave(int) {}

// hooks_gameplay.cpp (the live tweaker's in-game overlay): nothing to draw.
void OverlayBuild(std::vector<OvVtx>&, float, float, float, float) {}

// profiler.cpp (HC09_PROFILE): no sampler.
void ProfilerRegisterThread(uint64_t, const char*) {}
void ProfilerStart() {}
