#pragma once
// Native renderer front end (docs/NATIVE_RENDERER.md): the game's D3D calls ->
// the work of a native back end (render-target bindings, clears, draws with the
// register file the GPU would see, resolves, the frame's present). Shared by the
// live game (runtime/hooks_native.cpp, HC09_RENDERER=native) and the offline
// replay of D3D-level captures (tools/d3d_replay.cpp --render). Portable C++17.
//
// Timing: work is queued and handed to the back end when D3D submits the
// command buffer to the GPU (Kick, sub_82328440) — the console's GPU reads a
// draw's inputs only after that, and the game may write them until then (Apt
// fills its glyph cache after issuing the text draws).
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../dev_env.h"
#include "d3d_state.h"
#include "d3d_stream.h"
#include "small_vec.h"
#include "xenos.h"
#include "xenos_shader.h"

namespace d3dn {

// A draw for the back end. Registers: the changes since the previous draw handed
// to the back end (the back end keeps the whole file; it starts from zero).
// Immutable once handed to the back end (Push): the live back end keeps a reference
// (weak_from_this) instead of a copy; command buffers replay the same op. Its lists keep
// their usual sizes inline (one allocation per draw: the op).
struct DrawOp : std::enable_shared_from_this<DrawOp>
{
    xenos::DrawPacket d;
    using Regs = SmallVec<std::pair<uint32_t, uint32_t>, 24>;
    Regs regs;
    const xenos::ShaderCode* vs = nullptr;    // owned by the front end, never freed
    const xenos::ShaderCode* ps = nullptr;
    uint64_t vsHash = 0, psHash = 0;
    // BeginVertices / DrawVerticesUP: the vertex data (stream 0, big-endian as the
    // game wrote it); the back end places it and points vertex fetch constant 95 at it.
    SmallVec<uint8_t, 256> inlineData;
    // Registers the back end reads from guest memory when it executes the draw
    // (constants loaded through a pointer): `dwords` big-endian words at `address`
    // into registers reg.. (never also in `regs`).
    struct Load { uint32_t reg, address, dwords; };
    using Loads = SmallVec<Load, 2>;
    Loads loads;
    // Registers only (regs, loads): the state the GPU has at this point, nothing drawn.
    bool stateOnly = false;
};

struct Backend
{
    virtual ~Backend() = default;
    virtual void SetTargets(const Surface rt[4], const Surface& depth) = 0;
    // flags: bits 0-3 colour target n, 0x10 depth, 0x20 stencil; rect (x0, y0, x1, y1).
    virtual void Clear(uint32_t flags, const int32_t rect[4], const float color[4], float z, uint32_t stencil) = 0;
    virtual void Draw(DrawOp& op) = 0;
    // `destFetch`: the destination texture's fetch constant (diagnostics).
    virtual void Resolve(const ResolveOp& r, const uint32_t destFetch[6]) = 0;
    // Frame end (Swap): the front buffer texture's base address and size.
    virtual void Present(uint32_t frontBase, uint32_t width, uint32_t height) = 0;
    // The end of a batch (Kick): what was handed over since may go to the GPU side.
    virtual void Flush() {}
};

// One call's arguments: r3..r10, f1, and the data its pointer arguments point at
// (big-endian, as in guest memory), keyed by d3dcap::BlobTag.
struct CallArgs
{
    uint32_t fn = 0;
    uint32_t r[8] = {};
    double f1 = 0;
    uint32_t ret = 0;   // the call's return value (GpuBeginShaderConstantF4: the data pointer)
    struct Span { const uint8_t* p = nullptr; uint32_t n = 0; };
    Span blob[8];
};

// Work queued for the back end. Command buffers (Begin/End/RunCommandBuffer) record
// items into a registry shared by all front ends (RNA records them on D3D's second,
// command-buffer device and runs them on the main one); bindings recorded with them
// are not replayed (the buffers draw into what is bound when they run).
struct Item
{
    enum Kind { kTargets, kWork } kind = kWork;
    std::function<void(Backend&)> fn;
    std::shared_ptr<DrawOp> op;   // draws (diagnostics)
};

class Frontend
{
public:
    // `primary`: the device that presents; other devices only record command buffers.
    explicit Frontend(Backend& backend, bool primary = true) : be_(backend), primary_(primary) {}
    // A call by the game other than a draw. `device`: the device before the call.
    void Call(const CallArgs& c, const uint8_t* device, const GuestMemory& mem);
    // A draw call (DrawIndexedVertices, DrawVertices, BeginVertices, DrawVerticesUP):
    // `before`: the device before the call (at least its first 48 bytes, the dirty
    // masks); `after`: the device after it (D3D flushed its state).
    void Draw(const CallArgs& c, const uint8_t* before, const uint8_t* after, const GuestMemory& mem);
    // sub_8232BD98 patched vertex shader `vs` (program `variant`) into `dst`: the
    // shader's own microcode (memory already holds it) or a copy in the ring that
    // the draw being flushed loads.
    void VertexShaderPatched(uint32_t vs, uint32_t variant, uint32_t dst, const uint8_t* code, uint32_t bytes, const GuestMemory& mem);
    // D3D submitted the commands so far: hand the queued work to the back end.
    void Kick();
    // Statistics.
    uint32_t draws = 0, resolves = 0, clears = 0, commandBuffers = 0;
    // Counters for HC09_NATIVE_PROF (running totals; the reader takes differences).
    struct Counters
    {
        uint64_t calls[d3dcap::kCallCount] = {};   // by d3dcap::CallId (Call and Draw)
        uint64_t runItems = 0;             // items replayed by RunCommandBuffer
        uint64_t invalidates = 0;          // InvalidateSent
        uint64_t deltas = 0, deltaRanges = 0, deltaRegs = 0, deltaLoads = 0;   // Delta: calls, ranges visited, registers and loads out
    } counters;
    const RegisterState& State() const { return rs_; }

private:
    void Push(Item::Kind kind, std::function<void(Backend&)> fn, std::shared_ptr<DrawOp> op = nullptr);
    // The back end's register file is no longer what sent_ says (a command buffer ran,
    // or recording starts): the next draw sends every register it may read.
    // The back end's registers are unknown: everything is sent again with the next
    // draw (with `constants` false, the float constants only once something sets them).
    void InvalidateSent(bool constants = true);
    // Changes not yet sent (constants loaded or written without a draw since) go to
    // the back end now, as a state-only item.
    void FlushState();
    Surface Bound(const uint8_t* device, const GuestMemory& mem, uint32_t index) const;   // 0-3 colour, 4 depth (tiling extent applied)
    void QueueTargets(const uint8_t* device, const GuestMemory& mem);
    // `copyDestFromDevice`: the call targets a mip level or slice (DestLevel/DestSliceOrFace non-zero) and `device`
    // is the device after D3D ran: the destination is RB_COPY_DEST_BASE/PITCH as D3D computed them there.
    // Resolves into a mip level or slice (r8/r9 non-zero) go to Call with the device *after* D3D ran, like draws:
    // the caller (hooks_native.cpp, the replay tools) asks this.
public:
    static bool ResolveAfterCall(const CallArgs& c) { return c.fn == d3dcap::kResolve && (c.r[5] || c.r[6]); }
private:
    void QueueResolve(uint32_t flags, const int32_t* rect, uint32_t destTex, const int32_t* point, const float* color, float z,
                      uint32_t stencil, const uint8_t* device, const GuestMemory& mem, bool copyDestFromDevice = false);
    const xenos::ShaderCode* ShaderFor(xenos::ShaderType type, const uint8_t* code, uint32_t bytes, uint64_t* hash);
    // The registers to send with the next op (into `out`, exactly sized: built in a scratch
    // vector that keeps its capacity, so a draw costs one allocation, not a growing vector's).
    void Delta(DrawOp::Regs& out, DrawOp::Loads& loads);
    // Delta's two kinds of range: float constants [lo, hi] (whole ones) and other registers.
    HC09_NOINLINE void DeltaConstants(uint32_t lo, uint32_t hi);
    HC09_NOINLINE void DeltaRegisters(uint32_t lo, uint32_t hi);
    std::vector<std::pair<uint32_t, uint32_t>> deltaScratch_;
    std::vector<DrawOp::Load> loadsScratch_;

    Backend& be_;
    bool primary_ = true;
    uint32_t recording_ = 0;   // command buffer being recorded (guest address)
    std::vector<Item>* recItems_ = nullptr;   // its items (g_commandBuffers)
    RegisterState rs_;
    bool reset_ = false;
    std::vector<uint32_t> sent_ = std::vector<uint32_t>(xenos::kRegisterCount, 0);   // register file as handed to the back end
    std::vector<uint8_t> unknown_ = std::vector<uint8_t>(xenos::kRegisterCount, 0);  // its value came from memory (a Load)
    std::vector<Item> queue_;
    bool tiling_ = false;
    uint32_t tileW_ = 0, tileH_ = 0;
    // The bindings last queued for the back end (the primary device's queue): the same
    // bindings again are not queued (every draw and clear asks).
    std::array<Surface, 5> lastTargets_;
    bool targetsQueued_ = false;
    // The shaders the GPU holds (sub_8232C9D0 loads only dirty ones).
    std::vector<uint8_t> vsLoaded_, psLoaded_, vsRingCopy_;
    uint32_t vsLoadedObj_ = 0, psLoadedObj_ = 0;
    bool vsRingPending_ = false;
    uint64_t vsLoadedHash_ = 0, psLoadedHash_ = 0;
    const xenos::ShaderCode* vsLoadedCode_ = nullptr;
    const xenos::ShaderCode* psLoadedCode_ = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<xenos::ShaderCode>> shaders_;
    // Shaders D3D (re)loads from their own microcode, by (address, bytes): the bytes
    // last seen there and what they translated to. A reload of unchanged bytes is a
    // compare, not a copy and a hash.
    struct Loaded { std::vector<uint8_t> bytes; const xenos::ShaderCode* code = nullptr; uint64_t hash = 0; };
    std::unordered_map<uint64_t, Loaded> loadedAt_;
    const xenos::ShaderCode* LoadShader(xenos::ShaderType type, uint32_t va, uint32_t bytes, const GuestMemory& mem, std::vector<uint8_t>& out,
                                        uint64_t* hash);
    std::shared_ptr<DrawOp> pendingVertices_;   // BeginVertices waiting for its data (EndVertices)
    // EndCommandBuffer: scratch for a recording's net register effect. RunCommandBuffer applies it to sent_
    // (HC09_NATIVE_CB_NET=0: invalidate everything instead, as before).
    std::vector<uint8_t> netState_;
    std::vector<uint32_t> netValue_;
    bool netRuns_ = [] { const char* e = Hc09Env("HC09_NATIVE_CB_NET"); return !e || atoi(e) != 0; }();
    std::vector<std::pair<uint32_t, uint32_t>> ranges_;
};

}  // namespace d3dn
