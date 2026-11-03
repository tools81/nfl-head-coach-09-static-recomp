#pragma once
// Smooth motion above 30 Hz: the 30 Hz simulation seen by the render loop
// (docs/NATIVE_RENDERER.md 7.12). runtime/interp.cpp.
#include <cstddef>
#include <cstdint>
#include <utility>

// HC09_INTERP_TRACE=1 (diagnostic, off by default): measures the stepping. Every 5 s `[interp]`
// lines: frames and simulation ticks per second, render updates that overlap a simulation update;
// the characters' world positions (pose block +80): how many frames repeat the previous frame's
// position (30 Hz steps at 240 fps would be ~7 of 8) and the displacement per frame split by what
// the simulation did in between (after a tick / during an update / nothing: equal = even motion);
// whether all palettes of a frame equal the previous frame's; per model object the instance words
// that change by the same split; the ball entity's words by the same split; draw constants
// (ring-set and pointer-loaded) that change only with simulation activity ("sim-driven"); where the
// ball's and a character's positions appear in draw constants. HC09_INTERP_TRACE_AT=<s>[,<s>...]
// (seconds after start; default 430,530,630,730,830) logs HC09_INTERP_TRACE_FRAMES (40) frames in
// detail there (per frame: present time, the tick it follows, the ball, props);
// HC09_INTERP_TRACE_DUMPS=1 adds word diffs of model objects, instances, animation records,
// palettes and pose blocks and the palette loads' guest call chains. With gpu_live.cpp's
// HC09_GPU_LIVE_DUMP_AT=<s>,<n> (n consecutive presented frames) for frame-by-frame images.
bool InterpTraceOn();
// The simulation loop (LoopSimulation, 30 Hz) and the render loop: their update begins
// (begin = true) or ends. hooks_pacing.cpp, with the loop told apart by the rate it set.
void InterpLoopUpdate(int loop, bool begin);   // loop: 0 simulation, 1 render
bool InterpLoopsWanted();
// Native front end (game thread): a frame was presented.
void InterpFrame();
// Native front end (game thread, at the hand-over): a draw's pointer-loaded float
// constants starting at vertex constant `first`, `dwords` big-endian words as handed over.
// `id` identifies the draw across frames (a recorded DrawOp replays every frame).
void InterpTraceLoads(const void* id, uint32_t first, uint32_t address, const uint32_t* be, uint32_t dwords);
// D3D GpuLoadShaderConstantF4Pointer (hooks_native.cpp): in a detail window the guest call
// chains of the palette loads (vertex c0.., 64+ constants), each distinct chain once.
struct PPCContext;
// Native front end (game thread): a draw with the registers it changes (`regs`, n pairs).
// HC09_INTERP_TRACE: per draw identity (shaders, index buffer, count, occurrence in the frame)
// and vertex constant set through the ring, whether it changes in frames after a simulation
// tick or in the others: constants that change only after ticks are stepped (30 Hz) transforms.
void InterpTraceDraw(uint64_t vsHash, uint64_t psHash, uint32_t indexBase, uint32_t numIndices,
                     const std::pair<uint32_t, uint32_t>* regs, size_t n);
void InterpTraceConstPointer(const PPCContext& ctx);
