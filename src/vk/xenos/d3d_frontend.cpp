#include "../dev_env.h"
#include "d3d_frontend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace d3dn {

using namespace d3dcap;

namespace {
// guest address -> recorded work. A recording gets a new one: a run queued before the buffer is recorded again
// keeps the one it ran.
struct Recording
{
    std::vector<Item> items;
    // Once ended: what replaying the items leaves in the back end's register file (the last value each register gets
    // from the ops, and the registers loaded from memory): a run updates the sender's view by these, not by resending
    // everything.
    bool ended = false;
    std::vector<std::pair<uint32_t, uint32_t>> net;
    std::vector<uint32_t> loaded;
};
std::unordered_map<uint32_t, std::shared_ptr<Recording>> g_commandBuffers;
float BEF(const uint8_t* p) { uint32_t u = BE32(p); float f; memcpy(&f, &u, 4); return f; }
uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
const CallArgs::Span& Blob(const CallArgs& c, uint32_t tag) { static const CallArgs::Span none; return tag < 8 ? c.blob[tag] : none; }
void Floats(const CallArgs::Span& b, float out[4])
{
    for (int i = 0; i < 4; ++i) out[i] = b.p && b.n >= 16 ? BEF(b.p + 4 * i) : 0.0f;
}
bool Rect(const CallArgs::Span& b, int32_t out[4])
{
    if (!b.p || b.n < 16) return false;
    for (int i = 0; i < 4; ++i) out[i] = int32_t(BE32(b.p + 4 * i));
    return true;
}
}  // namespace

void Frontend::Push(Item::Kind kind, std::function<void(Backend&)> fn, std::shared_ptr<DrawOp> op)
{
    if (recording_) recItems_->push_back({ kind, std::move(fn), std::move(op) });
    else if (primary_) queue_.push_back({ kind, std::move(fn), std::move(op) });
}

void Frontend::InvalidateSent(bool constants)
{
    ++counters.invalidates;
    const uint32_t* r = rs_.Regs();
    for (uint32_t i = 0; i < xenos::kRegisterCount; ++i) sent_[i] = ~r[i];
    if (constants) rs_.MarkAllChanged();
    else rs_.MarkBlocksChanged();   // every shadowed register goes out with the next draw
}

void Frontend::FlushState()
{
    auto op = std::make_shared<DrawOp>();
    op->stateOnly = true;
    Delta(op->regs, op->loads);
    if (op->regs.empty() && op->loads.empty()) return;
    Push(Item::kWork, [op](Backend& be) { be.Draw(*op); }, op);
}

Surface Frontend::Bound(const uint8_t* device, const GuestMemory& mem, uint32_t index) const
{
    uint32_t obj = BE32(device + (index < 4 ? dev::kRenderTarget + 4 * index : dev::kDepth));
    Surface s = ReadSurface(obj, index >= 4, mem);
    // Inside a tiling bracket the surfaces are tile-sized EDRAM views of the extent.
    if (tiling_ && s.valid()) { s.width = std::max(s.width, tileW_); s.height = std::max(s.height, tileH_); }
    return s;
}

void Frontend::QueueTargets(const uint8_t* device, const GuestMemory& mem)
{
    std::array<Surface, 5> t;
    for (uint32_t i = 0; i < 5; ++i) t[i] = Bound(device, mem, i);
    if (!recording_)
    {
        if (!primary_ || (targetsQueued_ && t == lastTargets_)) return;   // others' bindings go nowhere outside a recording
        lastTargets_ = t;
        targetsQueued_ = true;
    }
    Push(Item::kTargets, [t](Backend& be) { be.SetTargets(t.data(), t[4]); });
}

const xenos::ShaderCode* Frontend::ShaderFor(xenos::ShaderType type, const uint8_t* code, uint32_t bytes, uint64_t* hash)
{
    uint64_t h = Fnv1a(code, bytes) ^ (type == xenos::ShaderType::kPixel ? 0x9E3779B97F4A7C15ull : 0);
    *hash = Fnv1a(code, bytes);
    auto it = shaders_.find(h);
    if (it != shaders_.end()) return it->second.get();
    auto sc = std::make_unique<xenos::ShaderCode>(xenos::ShaderFromBigEndian(type, code, bytes / 4));
    const xenos::ShaderCode* p = sc.get();
    shaders_.emplace(h, std::move(sc));
    return p;
}

void Frontend::Delta(DrawOp::Regs& out, DrawOp::Loads& outLoads)
{
    std::vector<std::pair<uint32_t, uint32_t>>& d = deltaScratch_;
    std::vector<DrawOp::Load>& loads = loadsScratch_;
    d.clear();
    loads.clear();
    rs_.TakeChanged(ranges_);
    ++counters.deltas;
    counters.deltaRanges += ranges_.size();
    for (auto [lo, hi] : ranges_)
    {
        hi = std::min(hi, xenos::kRegisterCount - 1);
        if (lo > hi) continue;
        bool constants = hi >= xenos::reg::ALU_CONSTANTS && lo < xenos::reg::ALU_CONSTANTS + 2048;
        if (constants) DeltaConstants(lo, hi);
        else DeltaRegisters(lo, hi);
    }
    counters.deltaRegs += d.size();
    counters.deltaLoads += loads.size();
    out.assign(d.begin(), d.end());
    outLoads.assign(loads.begin(), loads.end());
}

void Frontend::DeltaRegisters(uint32_t lo, uint32_t hi)
{
    // Outside the float constants nothing is loaded from memory (unknown_ stays 0
    // there): a range equal to what was sent has nothing to send.
    const uint32_t* r = rs_.Regs();
    if (!memcmp(r + lo, sent_.data() + lo, 4 * size_t(hi - lo + 1))) return;
    for (uint32_t i = lo; i <= hi; ++i)
        if (r[i] != sent_[i] || unknown_[i]) { sent_[i] = r[i]; unknown_[i] = 0; deltaScratch_.emplace_back(i, r[i]); }
}

void Frontend::DeltaConstants(uint32_t lo, uint32_t hi)
{
    const uint32_t* r = rs_.Regs();
    std::vector<DrawOp::Load>& loads = loadsScratch_;
    // Constants (whole ones, as TakeChanged gives them) none of which is loaded from
    // memory or unknown: equal to what was sent means nothing to send, as below.
    if (lo >= xenos::reg::ALU_CONSTANTS && hi < xenos::reg::ALU_CONSTANTS + 2048)
    {
        bool plain = true;
        for (uint32_t k = (lo - xenos::reg::ALU_CONSTANTS) / 4; k <= (hi - xenos::reg::ALU_CONSTANTS) / 4 && plain; ++k)
            plain = !rs_.External(k);
        if (plain && !memchr(unknown_.data() + lo, 1, hi - lo + 1) && !memcmp(r + lo, sent_.data() + lo, 4 * size_t(hi - lo + 1)))
            return;
    }
    // Whole constants (as TakeChanged gives them): four registers at a time.
    if (lo >= xenos::reg::ALU_CONSTANTS && hi < xenos::reg::ALU_CONSTANTS + 2048 && !((lo - xenos::reg::ALU_CONSTANTS) & 3) &&
        ((hi - xenos::reg::ALU_CONSTANTS) & 3) == 3)
    {
        for (uint32_t k = (lo - xenos::reg::ALU_CONSTANTS) / 4, i = lo; i <= hi; ++k, i += 4)
        {
            if (uint32_t a = rs_.External(k))
            {
                // Read by the back end at execution; consecutive constants merge into one load.
                if (!loads.empty() && loads.back().reg + loads.back().dwords == i && loads.back().address + 4 * loads.back().dwords == a)
                    loads.back().dwords += 4;
                else
                    loads.push_back({ i, a, 4 });
                memset(&unknown_[i], 1, 4);
                continue;
            }
            for (uint32_t j = i; j < i + 4; ++j)
                if (r[j] != sent_[j] || unknown_[j]) { sent_[j] = r[j]; unknown_[j] = 0; deltaScratch_.emplace_back(j, r[j]); }
        }
        return;
    }
    for (uint32_t i = lo; i <= hi; ++i)
    {
        if (i >= xenos::reg::ALU_CONSTANTS && i < xenos::reg::ALU_CONSTANTS + 2048)
            if (uint32_t a = rs_.External((i - xenos::reg::ALU_CONSTANTS) / 4))
            {
                // Read by the back end at execution; consecutive words merge into one load.
                uint32_t addr = a + 4 * ((i - xenos::reg::ALU_CONSTANTS) % 4);
                if (!loads.empty() && loads.back().reg + loads.back().dwords == i &&
                    loads.back().address + 4 * loads.back().dwords == addr)
                    ++loads.back().dwords;
                else
                    loads.push_back({ i, addr, 1 });
                unknown_[i] = 1;
                continue;
            }
        if (r[i] != sent_[i] || unknown_[i]) { sent_[i] = r[i]; unknown_[i] = 0; deltaScratch_.emplace_back(i, r[i]); }
    }
}

const xenos::ShaderCode* Frontend::LoadShader(xenos::ShaderType type, uint32_t va, uint32_t bytes, const GuestMemory& mem,
                                              std::vector<uint8_t>& out, uint64_t* hash)
{
    // The shader's words as big-endian bytes (a size that is not a multiple of 4 keeps
    // zero tail bytes, as before).
    bytes = std::min(bytes, 0x10000u);
    uint32_t words = bytes & ~3u;
    const uint8_t* p = mem.Ptr(va, words);
    uint64_t key = (uint64_t(va) << 32) | bytes | (type == xenos::ShaderType::kPixel ? 0x80000000ull : 0);
    Loaded* e = p ? &loadedAt_[key] : nullptr;
    if (e && e->code && !memcmp(e->bytes.data(), p, words))
    {
        *hash = e->hash;
        return e->code;   // `out` is only the buffer the shader was translated from
    }
    out.assign(bytes, 0);
    if (p) memcpy(out.data(), p, words);
    else
        for (uint32_t i = 0; i < words; i += 4)
        {
            uint32_t w = mem.Load32(va + i);
            out[i] = uint8_t(w >> 24); out[i + 1] = uint8_t(w >> 16); out[i + 2] = uint8_t(w >> 8); out[i + 3] = uint8_t(w);
        }
    const xenos::ShaderCode* code = ShaderFor(type, out.data(), uint32_t(out.size()), hash);
    if (e) { e->bytes = out; e->code = code; e->hash = *hash; }
    return code;
}

void Frontend::QueueResolve(uint32_t flags, const int32_t* rect, uint32_t destTex, const int32_t* point, const float* color, float z,
                            uint32_t stencil, const uint8_t* device, const GuestMemory& mem, bool copyDestFromDevice)
{
    if (!destTex) return;
    std::array<uint32_t, 6> f;
    for (int i = 0; i < 6; ++i) f[i] = mem.Load32(destTex + 28 + 4 * i);
    Surface src = Bound(device, mem, (flags & 7) < 4 ? (flags & 7) : 4);
    ResolveOp r;
    if (!DecodeResolve(flags, rect, f.data(), point, color, z, stencil, src.msaa, r)) return;
    if (copyDestFromDevice)
    {
        // A mip level or slice: D3D's address helper (sub_82320E20) placed it; RB_COPY_DEST_BASE (dev+10780) and
        // RB_COPY_DEST_PITCH (dev+10784: pitch, height << 16) hold the result (Resolve resets only CONTROL and INFO).
        // Without this every level landed on level 0: the game's mipTexture resolves of the crowd card atlas
        // (1024x128, levels 1-3 every frame) overwrote its top rows, and the sideline crowd lost its upper halves.
        uint32_t pitchReg = BE32(device + 10784);
        r.destBase = BE32(device + 10780) & 0x1FFFFFFFu;
        r.destPitch = pitchReg & 0x3FFF;
        r.destHeight = (pitchReg >> 16) & 0x3FFF;
        // Not a whole texture at its base: no scaled copy (fetches decode level 0 and box-filter its mips).
        r.texBase = r.destBase;
        r.texWidth = r.destPitch;
        r.texHeight = 0;
        r.destX = r.destY = 0;
        if (!r.destPitch) return;
    }
    QueueTargets(device, mem);
    ++resolves;
    Push(Item::kWork, [r, f](Backend& be) { be.Resolve(r, f.data()); });
}

void Frontend::Call(const CallArgs& c, const uint8_t* device, const GuestMemory& mem)
{
    if (!reset_) { rs_.Reset(device); reset_ = true; }
    if (c.fn < kCallCount) ++counters.calls[c.fn];
    // D3D's dirty masks before the call; calls other than these flush state themselves (clearing the masks).
    rs_.NoteDirty(device);
    if (c.fn != kGpuBeginShaderConstantF4 && c.fn != kGpuLoadShaderConstantF4Pointer && c.fn != kEndVertices) rs_.FullNext();
    switch (c.fn)
    {
    case kGpuBeginShaderConstantF4:
        // r4 pixel, r5 start, r6 count; returns where the caller writes the data, in
        // the ring or in the command buffer being recorded. The GPU reads it when it
        // executes, and the game may rewrite it until then: in a recorded command
        // buffer every frame (RNA's bone matrices in static player buffers).
        if (c.ret) rs_.PointerConstants(c.r[1] != 0, c.r[2], c.r[3], c.ret);
        else
        {
            const CallArgs::Span& b = Blob(c, kBlobConstants);
            if (b.p && b.n >= c.r[3] * 16) rs_.RingConstants(c.r[1] != 0, c.r[2], c.r[3], b.p);
        }
        break;
    case kGpuLoadShaderConstantF4Pointer:   // r4 pixel, r5 start, r6 address, r7 count: read when the GPU executes
        rs_.PointerConstants(c.r[1] != 0, c.r[2], c.r[4], c.r[3]);
        break;
    case kClearF:
    {
        QueueTargets(device, mem);
        int32_t rect[4];
        if (!Rect(Blob(c, kBlobRect), rect))
        {
            Surface s = Bound(device, mem, 0);
            if (!s.valid()) s = Bound(device, mem, 4);
            rect[0] = rect[1] = 0;
            rect[2] = int32_t(s.width);
            rect[3] = int32_t(s.height);
        }
        // ClearF clips the rectangle to the viewport (sub_8233C4C8).
        int32_t vx = int32_t(BEF(device + dev::kViewport)), vy = int32_t(BEF(device + dev::kViewport + 4));
        int32_t vw = int32_t(BEF(device + dev::kViewport + 8)), vh = int32_t(BEF(device + dev::kViewport + 12));
        std::array<int32_t, 4> rc = { std::max(rect[0], vx), std::max(rect[1], vy), std::min(rect[2], vx + vw), std::min(rect[3], vy + vh) };
        std::array<float, 4> col;
        Floats(Blob(c, kBlobColor), col.data());
        uint32_t flags = c.r[1], stencil = c.r[5];
        float z = float(c.f1);
        ++clears;
        Push(Item::kWork, [flags, rc, col, z, stencil](Backend& be) { be.Clear(flags, rc.data(), col.data(), z, stencil); });
        break;
    }
    case kBeginTiling:
    {
        tileW_ = tileH_ = 0;
        const CallArgs::Span& b = Blob(c, kBlobRects);
        for (uint32_t i = 0; b.p && i + 16 <= b.n; i += 16)
        {
            tileW_ = std::max<uint32_t>(tileW_, BE32(b.p + i + 8));
            tileH_ = std::max<uint32_t>(tileH_, BE32(b.p + i + 12));
        }
        tiling_ = true;
        QueueTargets(device, mem);
        if (!(c.r[1] & 1))   // flag 1: no clear at the start
        {
            std::array<int32_t, 4> rc = { 0, 0, int32_t(tileW_), int32_t(tileH_) };
            std::array<float, 4> col;
            Floats(Blob(c, kBlobColor), col.data());
            float z = float(c.f1);
            uint32_t stencil = c.r[6];
            ++clears;
            Push(Item::kWork, [rc, col, z, stencil](Backend& be) { be.Clear(0x3F, rc.data(), col.data(), z, stencil); });
        }
        break;
    }
    case kEndTiling:
    {
        // One resolve of the whole extent (the per-tile resolves and the clears
        // between tiles are artefacts of the EDRAM), with the caller's flags and clears.
        float col[4];
        Floats(Blob(c, kBlobColor), col);
        int32_t rect[4] = { 0, 0, int32_t(tileW_), int32_t(tileH_) };
        int32_t point[2] = { 0, 0 };
        QueueResolve(c.r[1], rect, c.r[3], point, col, float(c.f1), c.r[6], device, mem);
        tiling_ = false;
        break;
    }
    case kResolve:
    {
        int32_t rect[4], point[2];
        bool hasRect = Rect(Blob(c, kBlobRect), rect);
        const CallArgs::Span& pb = Blob(c, kBlobPoint);
        bool hasPoint = pb.p && pb.n >= 8;
        if (hasPoint) { point[0] = int32_t(BE32(pb.p)); point[1] = int32_t(BE32(pb.p + 4)); }
        float col[4];
        Floats(Blob(c, kBlobColor), col);
        const CallArgs::Span& st = Blob(c, kBlobStack);
        uint32_t stencil = st.p && st.n >= 96 ? BE32(st.p + 92) : 0;   // the caller's stack: ClearStencil at sp+92
        // r8 DestLevel, r9 DestSliceOrFace: such calls reach the front end after D3D ran (ResolveAfterCall).
        QueueResolve(c.r[1], hasRect ? rect : nullptr, c.r[3], hasPoint ? point : nullptr, col, float(c.f1), stencil, device, mem,
                     ResolveAfterCall(c));
        break;
    }
    case kEndVertices:
    {
        const CallArgs::Span& v = Blob(c, kBlobVertices);
        if (pendingVertices_ && v.p)
        {
            pendingVertices_->inlineData.assign(v.p, v.p + v.n);
            auto op = pendingVertices_;
            Push(Item::kWork, [op](Backend& be) { be.Draw(*op); }, op);
            ++draws;
        }
        pendingVertices_.reset();
        break;
    }
    case kSwap:
    {
        uint32_t tex = c.r[1];
        uint32_t f1 = mem.Load32(tex + 28 + 4), f2 = mem.Load32(tex + 28 + 8);
        uint32_t base = f1 & 0xFFFFF000u, w = (f2 & 0x1FFF) + 1, h = ((f2 >> 13) & 0x1FFF) + 1;
        if (!primary_) break;
        queue_.push_back({ Item::kWork, [base, w, h](Backend& be) { be.Present(base, w, h); } });
        Kick();
        break;
    }
    case kBeginCommandBuffer:   // r4 = the command buffer; inherited state (r6) must be none: everything is recorded
    {
        if (c.r[3]) { static bool warned = false; if (!warned) { warned = true; fprintf(stderr, "[native] BeginCommandBuffer with inherited state: not modelled\n"); } }
        // What this device changed before goes to its own stream. A recording holds
        // only what D3D emits into it: all state from the shadow (Begin marks it
        // dirty), and the float constants whose groups are dirty at its draws. The
        // others are the caller's when it runs: RNA loads its player buffers' bone
        // matrices (c0-c123) by pointer on the main device just before
        // RunCommandBuffer, and the buffer's draws use them.
        FlushState();
        recording_ = c.r[1];
        recItems_ = &(g_commandBuffers[recording_] = std::make_shared<Recording>())->items;
        ++commandBuffers;
        rs_.TakeChanged(ranges_);
        InvalidateSent(false);
        if (commandBuffers <= 40)
            fprintf(stderr, "[native] BeginCommandBuffer on device 0x%08X: buffer 0x%08X (%s device)\n", c.r[0], c.r[1], primary_ ? "main" : "recording");
        break;
    }
    case kEndCommandBuffer:
        if (commandBuffers <= 40 && recording_)
            fprintf(stderr, "[native] EndCommandBuffer: buffer 0x%08X holds %zu items\n", recording_, recItems_->size());
        if (recording_)
            if (auto it = g_commandBuffers.find(recording_); it != g_commandBuffers.end() && &it->second->items == recItems_)
            {
                // The replay's net effect on the register file (ops in order; within one, its loads after its registers).
                Recording& rec = *it->second;
                std::vector<uint8_t>& state = netState_;   // per register: 0 untouched, 1 value, 2 loaded
                if (state.empty()) state.assign(xenos::kRegisterCount, 0);
                std::vector<uint32_t>& val = netValue_;
                if (val.empty()) val.assign(xenos::kRegisterCount, 0);
                std::vector<uint32_t> touched;
                for (const Item& item : rec.items)
                {
                    if (!item.op || item.kind == Item::kTargets) continue;
                    for (auto [i, v] : item.op->regs)
                    {
                        if (!state[i]) touched.push_back(i);
                        state[i] = 1;
                        val[i] = v;
                    }
                    for (const DrawOp::Load& l : item.op->loads)
                        for (uint32_t k = 0; k < l.dwords; ++k)
                        {
                            if (!state[l.reg + k]) touched.push_back(l.reg + k);
                            state[l.reg + k] = 2;
                        }
                }
                for (uint32_t i : touched)
                {
                    if (state[i] == 1) rec.net.emplace_back(i, val[i]);
                    else rec.loaded.push_back(i);
                    state[i] = 0;
                }
                rec.ended = true;
            }
        recording_ = 0;
        recItems_ = nullptr;
        InvalidateSent();
        break;
    case kRunCommandBuffer:   // r4 = the command buffer
    {
        auto it = g_commandBuffers.find(c.r[1]);
        if (it == g_commandBuffers.end())
        {
            static bool warned = false;
            if (!warned) { warned = true; fprintf(stderr, "[native] RunCommandBuffer of a buffer recorded before the native renderer saw it\n"); }
            break;
        }
        static uint32_t runs = 0;
        const std::shared_ptr<Recording> rec = it->second;
        const std::vector<Item>* items = &rec->items;
        if (++runs <= 30) fprintf(stderr, "[native] RunCommandBuffer 0x%08X: %zu items\n", c.r[1], items->size());
        // HC09_NATIVE_TRACE_CB=n: the state of the first draws replayed from buffers
        // with more than 50 items (accumulated register file, key registers).
        static const long traceCb = [] { const char* e = Hc09Env("HC09_NATIVE_TRACE_CB"); return e ? atol(e) : 0L; }();
        static long traced = 0;
        if (traceCb && traced < traceCb && items->size() > 50)
        {
            std::vector<uint32_t> f(xenos::kRegisterCount, 0);
            for (const Item& item : *items)
            {
                if (!item.op) continue;
                for (auto [i, v] : item.op->regs) f[i] = v;
                if (traced++ >= traceCb) break;
                auto F = [&](uint32_t r) { float x; memcpy(&x, &f[r], 4); return x; };
                fprintf(stderr, "[native-cb] buffer 0x%08X draw prim %u count %u%s vs %016llx ps %016llx loads %zu: surf %08X col %08X depth %08X "
                                "mode %08X dc %08X cmask %08X vte %08X vport (%g %g %g | %g %g %g) scissor %08X-%08X winofs %08X "
                                "clip %08X su %08X progcntl %08X vsconst %08X psconst %08X fetch0 %08X %08X\n",
                        c.r[1], item.op->d.primType, item.op->d.numIndices, item.op->d.sourceSelect == 0 ? " indexed" : "",
                        (unsigned long long)item.op->vsHash, (unsigned long long)item.op->psHash, item.op->loads.size(), f[0x2000], f[0x2001], f[0x2002],
                        f[0x2208], f[0x2200], f[0x2104], f[0x2206], F(0x210F), F(0x2111), F(0x2113), F(0x2110), F(0x2112), F(0x2114),
                        f[0x2081], f[0x2082], f[0x2080], f[0x2204], f[0x2205], f[0x2180], f[0x2307], f[0x2308], f[0x48BE], f[0x48BF]);
            }
        }
        counters.runItems += items->size();
        FlushState();   // the caller's constants, which the buffer inherits
        // One item replays the recording (no copy of its items' closures per run).
        Push(Item::kWork, [rec](Backend& be) {
            for (const Item& item : rec->items)
                if (item.kind != Item::kTargets) item.fn(be);
        });
        // The back end now holds the buffer's last state. D3D marks the device dirty
        // (~persist masks), so its next draw sends state and dirty constant groups
        // again; the others (GPU-owned) keep what the buffer left.
        if (rec->ended && netRuns_)
        {
            // What the replay leaves is known: the next draw sends what differs from it.
            for (auto [i, v] : rec->net) { sent_[i] = v; unknown_[i] = 0; }
            for (uint32_t i : rec->loaded) unknown_[i] = 1;
            rs_.MarkBlocksChanged();
        }
        else
            InvalidateSent(false);
        break;
    }
    default: break;
    }
}

void Frontend::VertexShaderPatched(uint32_t vs, uint32_t variant, uint32_t dst, const uint8_t* code, uint32_t bytes, const GuestMemory& mem)
{
    uint32_t off = mem.Load32(vs + 896 + 8 * variant);
    uint32_t own = mem.Load32(vs + 32) + mem.Load32(vs + 872 + off);
    if ((dst & 0x1FFFFFFF) == (own & 0x1FFFFFFF)) return;   // in place: memory holds it
    vsRingCopy_.assign(code, code + bytes);
    vsRingPending_ = true;
}

void Frontend::Draw(const CallArgs& c, const uint8_t* before, const uint8_t* after, const GuestMemory& mem)
{
    if (!reset_) { rs_.Reset(after); reset_ = true; }
    if (c.fn < kCallCount) ++counters.calls[c.fn];
    auto op = std::make_shared<DrawOp>();
    xenos::DrawPacket& d = op->d;
    d.primType = c.r[1] & 0x3F;
    uint32_t indexOffset = 0;
    switch (c.fn)
    {
    case kDrawIndexedVertices:
    {
        uint32_t ib = BE32(after + dev::kIndices), common = mem.Load32(ib), base = mem.Load32(ib + 24);
        d.sourceSelect = 0;
        d.numIndices = c.r[4];
        d.indexSize32 = common >> 31;
        d.dmaEndian = (common >> 29) & 3;
        d.dmaBase = (base + c.r[3] * (d.indexSize32 ? 4 : 2)) & 0x1FFFFFFF;
        d.dmaWords = d.numIndices;
        indexOffset = c.r[2];
        break;
    }
    case kDrawVertices: d.sourceSelect = 2; d.numIndices = c.r[3]; indexOffset = c.r[2]; break;
    default: d.sourceSelect = 2; d.numIndices = c.r[2]; break;   // BeginVertices, DrawVerticesUP
    }
    rs_.Draw(before, after, indexOffset, mem);

    // The shaders the GPU holds: D3D (sub_8232C9D0) loads the vertex shader only when
    // it is dirty (+16 bit 0x80000: bound, declaration or stride changed), from a copy
    // it patched into the ring or from its own microcode; the pixel shader when bit
    // 0x100000 is set.
    ShaderRefs s = Shaders(after, [&](uint32_t va) { return mem.Load32(va); });
    uint64_t dirty16 = BE64(before + dev::kDirtyState);
    if (s.vs)
    {
        if (vsRingPending_ || (dirty16 & 0x80000) || !vsLoadedCode_ || vsLoadedObj_ != s.vs)
        {
            if (vsRingPending_)
            {
                vsLoaded_ = vsRingCopy_;
                vsLoadedCode_ = ShaderFor(xenos::ShaderType::kVertex, vsLoaded_.data(), uint32_t(vsLoaded_.size()), &vsLoadedHash_);
            }
            else
                vsLoadedCode_ = LoadShader(xenos::ShaderType::kVertex, s.vsCode, s.vsBytes, mem, vsLoaded_, &vsLoadedHash_);
        }
        vsLoadedObj_ = s.vs;
    }
    vsRingPending_ = false;
    if (s.ps)
    {
        if ((dirty16 & 0x100000) || !psLoadedCode_ || psLoadedObj_ != s.ps)
            psLoadedCode_ = LoadShader(xenos::ShaderType::kPixel, s.psCode, s.psBytes, mem, psLoaded_, &psLoadedHash_);
        psLoadedObj_ = s.ps;
    }
    op->vs = s.vs ? vsLoadedCode_ : nullptr;
    op->vsHash = s.vs ? vsLoadedHash_ : 0;
    op->ps = s.ps ? psLoadedCode_ : nullptr;
    op->psHash = s.ps ? psLoadedHash_ : 0;
    Delta(op->regs, op->loads);

    QueueTargets(after, mem);
    if (c.fn == kBeginVertices) { pendingVertices_ = op; return; }   // its data comes with EndVertices
    if (c.fn == kDrawVerticesUP)
    {
        const CallArgs::Span& v = Blob(c, kBlobVertices);
        if (!v.p) return;
        op->inlineData.assign(v.p, v.p + v.n);
    }
    ++draws;
    Push(Item::kWork, [op](Backend& be) { be.Draw(*op); }, op);
}

void Frontend::Kick()
{
    for (auto& item : queue_) item.fn(be_);
    queue_.clear();
    be_.Flush();
}

}  // namespace d3dn
