// Native renderer (docs/NATIVE_RENDERER.md): the game's D3D calls, for
//  - the live native renderer (gpu_live.cpp NativeLive; the default, HC09_RENDERER=emulated
//    turns it off) through
//    d3dn::Frontend: every call below is handed to the front end (device before /
//    after, the data its pointers point at); the original D3D keeps running (its
//    ring still carries the fences, interrupts and swaps the game waits on).
//  - D3D-level frame capture (below), the offline test bench of the same front end.
//
// HC09_D3D_CAPTURE=<n>[,<n>...]: record the frame between the game's n-th and
// (n+1)-th call of D3D Swap (= GPU swap n: every Swap writes one XE_SWAP, so the
// emulated capture HC09_CAPTURE_FRAME=<n> of the same run is the same frame)
// into <HC09_D3D_CAPTURE_DIR, default captures>/d3d_<n>.bin
// (format: runtime/xenos/d3d_stream.h). The original D3D keeps running: every
// wrapper forwards unchanged. Recorded: the calls a native renderer replaces
// (draws, ring-written constants, clears, resolves, tiling, predication, swap,
// callbacks, command buffers) with their arguments, the device structure before
// and after each, the vertex shader microcode as D3D patches it for the vertex
// declaration, and every 4 KiB page a draw reads (surfaces, shaders, vertex
// declaration, index and vertex buffers, textures), again whenever it changed —
// checked at the draw and again when D3D submits the commands to the GPU
// (sub_82328440): the game may still write what a draw reads until then (Apt
// fills its glyph cache after issuing the text draws).
#include "dev_env.h"
#include "kernel.h"
#include "game_addr.h"
#include "gpu.h"
#include "xenos/d3d_stream.h"
#include "xenos/xenos_texture.h"
#include "native_live.h"
#include "interp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xenos { const uint8_t* HostPhys(uint32_t physicalAddress); }
void D3DTraceCount(uint32_t addr, PPCContext& ctx);   // hooks_d3d.cpp (HC09_TRACE_D3D census)
int D3DTraceEnter(uint32_t addr, PPCContext& ctx);     // ... and HC09_TRACE_D3D=time
void D3DTraceLeave(int token);
namespace {
struct D3DTimed
{
    int token;
    D3DTimed(uint32_t addr, PPCContext& ctx) : token(D3DTraceEnter(addr, ctx)) {}
    ~D3DTimed() { D3DTraceLeave(token); }
};
}  // namespace

extern "C" PPC_FUNC(__imp__sub_8233D760);
extern "C" PPC_FUNC(__imp__sub_8233D378);
extern "C" PPC_FUNC(__imp__sub_8233CE80);
extern "C" PPC_FUNC(__imp__sub_8233D320);
extern "C" PPC_FUNC(__imp__sub_8233D330);
extern "C" PPC_FUNC(__imp__sub_8233CD60);
extern "C" PPC_FUNC(__imp__sub_8233CE00);
extern "C" PPC_FUNC(__imp__sub_8233CC18);
extern "C" PPC_FUNC(__imp__sub_82322A98);
extern "C" PPC_FUNC(__imp__sub_8233B0A0);
extern "C" PPC_FUNC(__imp__sub_8233B540);
extern "C" PPC_FUNC(__imp__sub_8233AF30);
extern "C" PPC_FUNC(__imp__sub_82333410);
extern "C" PPC_FUNC(__imp__sub_82328D28);
extern "C" PPC_FUNC(__imp__sub_8232A400);
extern "C" PPC_FUNC(__imp__sub_8232A8A8);
extern "C" PPC_FUNC(__imp__sub_8232ABA8);
extern "C" PPC_FUNC(__imp__sub_8232BD98);
extern "C" PPC_FUNC(__imp__sub_82328440);
extern "C" PPC_FUNC(__imp__sub_82323A30);
extern "C" PPC_FUNC(__imp__sub_82330460);
extern "C" PPC_FUNC(__imp__sub_82328C88);
extern "C" PPC_FUNC(__imp__sub_82329400);

namespace {

using namespace d3dcap;

inline uint32_t Pa(uint32_t va) { return va & 0x1FFFFFFFu; }
inline uint32_t Ld(uint32_t va) { return GLoad32(va); }

struct Recorder
{
    std::mutex mu;
    FILE* f = nullptr;
    long target = -1;              // >= 0: armed (HC09_D3D_CAPTURE)
    std::vector<uint64_t> frames;  // swaps after which to record
    // HC09_D3D_CAPTURE_HEAVY=<draws>[,<n>[,<from swap>]]: also record the frame after one with at least <draws> draws
    // by the game (up to n frames, default 1, at least 600 swaps apart; from that swap on, e.g. 90000 for play: the
    // menus have frames of 3,000 UI draws): crowd close-ups come at different swaps every run.
    uint32_t heavyDraws = 0, heavyLeft = 0;
    uint64_t heavyLast = 0, heavyFrom = 0;
    std::atomic<uint32_t> frameDraws{ 0 };
    std::string dir;
    uint64_t swaps = 0;            // Swap calls so far (all of them, any depth)
    uint32_t device = 0;           // guest address of the device being recorded
    std::vector<uint8_t> devCopy;  // the device as last written ('M')
    std::unordered_map<uint32_t, std::vector<uint8_t>> pages;   // page -> contents last written
    std::unordered_map<uint32_t, bool> pagesThisCall;           // pages already checked for the current record
    uint64_t records = 0, pageRecords = 0;
    // BeginVertices / GpuBeginShaderConstantF4 hand out a ring pointer the caller
    // fills after the call returns: the data is recorded at the next record.
    uint32_t vtxPtr = 0, vtxBytes = 0;
    bool constPending = false;
    uint32_t constArgs[8] = {}, constRet = 0;
    uint32_t constDepth = 0, constLr = 0;
    // What draws read since the last submit: checked again at the submit (sub_82328440),
    // when the GPU may start reading it.
    std::vector<std::pair<uint32_t, uint32_t>> kickRanges;
    void Kick()
    {
        pagesThisCall.clear();
        std::sort(kickRanges.begin(), kickRanges.end());
        kickRanges.erase(std::unique(kickRanges.begin(), kickRanges.end()), kickRanges.end());
        for (auto [pa, bytes] : kickRanges)
            for (uint32_t p = pa & ~0xFFFu; p < pa + bytes; p += 0x1000) Page(p);
        kickRanges.clear();
        fputc('K', f);
    }

    // Records go to the frame capture (f) and, while a command buffer is being
    // recorded, to that buffer's stream as well (cbBuf).
    void Out(const void* p, size_t n)
    {
        if (f) fwrite(p, 1, n, f);
        if (cbBuf) cbBuf->insert(cbBuf->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }
    void Byte(char c) { Out(&c, 1); }
    void U32(uint32_t v) { Out(&v, 4); }

    bool Active() const { return f != nullptr; }
    bool Recording() const { return f != nullptr || cbBuf != nullptr; }

    // Command buffers (RNA records them on D3D's second device, often long before the
    // frame that runs them): while armed, every recording is kept as a stream of
    // records (a full snapshot of the recording device, then the calls and the pages
    // they read); a frame capture starts with the latest stream of every buffer.
    std::unordered_map<uint32_t, std::vector<uint8_t>> cbStreams;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> cbRanges;   // what each buffer's draws read
    std::unordered_set<uint32_t> cbPages;   // pages already in the active stream
    std::vector<uint8_t>* cbBuf = nullptr;
    uint32_t cbActive = 0;
    bool cbSuppress = false;   // Range: this range's pages stay out of the stream
    void CbBegin(uint32_t cb, uint32_t dev)
    {
        Device(dev);   // the frame capture's copy (and file) catch up first
        cbActive = cb;
        cbBuf = &cbStreams[cb];
        cbBuf->clear();
        cbPages.clear();
        cbRanges[cb].clear();
        // Full snapshot of the recording device into the stream.
        Snapshot(dev, *cbBuf);
    }
    // 'S': the whole device as the recorder's copy holds it (Device() just ran), so a
    // reader does not depend on what came before.
    void Snapshot(uint32_t dev, std::vector<uint8_t>& out)
    {
        const std::vector<uint8_t>& copy = dev == device ? devCopy : otherDevs[dev];
        std::vector<uint32_t> all;
        for (uint32_t o = 0; o < kDeviceBytes && o + 4 <= copy.size(); o += 4)
        {
            uint32_t w;
            memcpy(&w, copy.data() + o, 4);
            if (w) { all.push_back(o); all.push_back(w); }
        }
        out.push_back(uint8_t('S'));
        auto put = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
        put(Pa(dev));
        put(uint32_t(all.size() / 2));
        out.insert(out.end(), reinterpret_cast<uint8_t*>(all.data()), reinterpret_cast<uint8_t*>(all.data() + all.size()));
    }
    void CbEnd()
    {
        cbActive = 0;
        cbBuf = nullptr;
    }

    void Open(uint32_t dev)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string path = dir + "/d3d_" + std::to_string(swaps) + ".bin";
        f = fopen(path.c_str(), "wb");
        if (!f) { fprintf(stderr, "[d3dcap] cannot write %s\n", path.c_str()); return; }
        fwrite("XD3D", 1, 4, f);
        uint32_t v = kVersion;
        fwrite(&v, 4, 1, f);
        uint64_t sw = swaps;
        fwrite(&sw, 8, 1, f);
        v = Pa(dev);
        fwrite(&v, 4, 1, f);
        device = dev;
        pages.clear();
        kickRanges.clear();
        // The command buffers recorded so far (the frame may run them).
        size_t cbBytes = 0;
        for (auto& [cb, stream] : cbStreams) { fwrite(stream.data(), 1, stream.size(), f); cbBytes += stream.size(); }
        // Then every device as it is now: the prologue left the reader's copies at
        // older states, and later 'M'/'N' records are changes from these.
        std::vector<uint32_t> devs{ dev };
        for (auto& [d, copy] : otherDevs) if (d != dev) devs.push_back(d);
        for (uint32_t d : devs)
        {
            std::vector<uint8_t>& copy = d == dev ? devCopy : otherDevs[d];
            const uint8_t* m = xenos::HostPhys(Pa(d));
            copy.assign(m, m + kDeviceBytes);
            std::vector<uint8_t> snap;
            Snapshot(d, snap);
            fwrite(snap.data(), 1, snap.size(), f);
        }
        if (!cbStreams.empty())
            fprintf(stderr, "[d3dcap] prologue: %zu command buffer recordings, %.1f MB\n", cbStreams.size(), cbBytes / 1048576.0);
        fprintf(stderr, "[d3dcap] recording frame after D3D swap %llu (GPU swaps so far %llu) into %s, device 0x%08X\n",
                (unsigned long long)swaps, (unsigned long long)GpuSwapCount(), path.c_str(), dev);
    }
    void Close()
    {
        if (!f) return;
        fputc('E', f);
        long bytes = ftell(f);
        fclose(f);
        f = nullptr;
        fprintf(stderr, "[d3dcap] done: %llu records, %llu pages, %.1f MB\n", (unsigned long long)records,
                (unsigned long long)pageRecords, bytes / 1048576.0);
        records = pageRecords = 0;
    }

    // ---- memory ---------------------------------------------------------------
    void Page(uint32_t page)
    {
        if (pagesThisCall.count(page)) return;
        pagesThisCall[page] = true;
        const uint8_t* m = xenos::HostPhys(page);
        if (cbBuf && !cbSuppress && cbPages.insert(page).second)
        {
            cbBuf->push_back('P');
            cbBuf->insert(cbBuf->end(), reinterpret_cast<uint8_t*>(&page), reinterpret_cast<uint8_t*>(&page) + 4);
            cbBuf->insert(cbBuf->end(), m, m + 4096);
        }
        if (!f) return;
        auto it = pages.find(page);
        if (it != pages.end() && memcmp(it->second.data(), m, 4096) == 0) return;
        std::vector<uint8_t>& copy = pages[page];
        copy.assign(m, m + 4096);
        fputc('P', f);
        fwrite(&page, 4, 1, f);
        fwrite(copy.data(), 1, 4096, f);
        ++pageRecords;
    }
    // Large ranges (textures) are recorded at their first touch only: comparing
    // megabytes per draw would stall the game; the CPU rarely rewrites them mid-frame.
    void Range(uint32_t va, uint32_t bytes)
    {
        if (!bytes) return;
        uint32_t pa = Pa(va);
        bytes = std::min<uint32_t>(bytes, 0x20000000u - pa);
        if (f) kickRanges.emplace_back(pa, bytes);
        if (cbActive) cbRanges[cbActive].emplace_back(pa, bytes);
        bool large = bytes > 256 * 1024;
        // A recording's stream keeps only small ranges (objects, shaders, declarations:
        // what the front end reads while recording); vertex data and textures are read
        // when the buffer runs, and recorded then (cbRanges).
        cbSuppress = bytes > 65536;
        for (uint32_t p = pa & ~0xFFFu; p < pa + bytes; p += 0x1000)
        {
            if (large && pages.count(p) && (!cbBuf || cbSuppress)) continue;
            Page(p);
        }
        cbSuppress = false;
    }
    // The device structure's changes: 'M' for the main device, 'N' for another (D3D's
    // command-buffer device).
    std::unordered_map<uint32_t, std::vector<uint8_t>> otherDevs;
    void Device(uint32_t dev = 0)
    {
        if (!dev) dev = device;
        bool main = dev == device;
        std::vector<uint8_t>& copy = main ? devCopy : otherDevs[dev];
        if (copy.empty()) copy.assign(kDeviceBytes, 0);
        const uint8_t* m = xenos::HostPhys(Pa(dev));
        std::vector<uint32_t> delta;
        for (uint32_t o = 0; o < kDeviceBytes; o += 4)
            if (memcmp(&copy[o], m + o, 4) != 0)
            {
                memcpy(&copy[o], m + o, 4);
                uint32_t w;
                memcpy(&w, m + o, 4);
                delta.push_back(o);
                delta.push_back(w);
            }
        if (delta.empty()) return;
        Byte(main ? 'M' : 'N');
        if (!main) U32(Pa(dev));
        U32(uint32_t(delta.size() / 2));
        Out(delta.data(), 4 * delta.size());
    }

    // What a draw reads, from the device as it is now.
    void DrawInputs(uint32_t dev = 0)
    {
        uint32_t d = dev ? dev : device;
        for (uint32_t i = 0; i < 5; ++i)   // RT0..RT3, depth: surface headers
            if (uint32_t s = Ld(d + dev::kRenderTarget + 4 * i)) Range(s, 64);
        if (uint32_t vs = Ld(d + dev::kVertexShader))
        {
            uint32_t hdr = Ld(vs + 872 + 4);
            Range(vs, 872 + std::min<uint32_t>(hdr, 0x10000));
            for (uint32_t v = 0; v < 2; ++v)
            {
                uint32_t off = Ld(vs + 896 + 8 * v);
                if (!off || off > 0x10000) continue;
                uint32_t rec = vs + 872 + off;
                Range(Ld(vs + 32) + Ld(rec), std::min<uint32_t>(Ld(rec + 4), 0x10000));
            }
        }
        if (uint32_t ps = Ld(d + dev::kPixelShader))
        {
            uint32_t hdr = Ld(ps + 40 + 4);
            Range(ps, 40 + std::min<uint32_t>(hdr, 0x10000));
            uint32_t info = ps + 40 + Ld(ps + 64);
            Range(Ld(ps + 24) + Ld(info), std::min<uint32_t>(Ld(info + 4), 0x10000));
        }
        if (uint32_t decl = Ld(d + dev::kVertexDecl)) Range(decl, 56 + 12 * std::min<uint32_t>(Ld(decl + 24), 64));
        if (uint32_t ib = Ld(d + dev::kIndices)) { Range(ib, 32); Range(Ld(ib + 24), Ld(ib + 28)); }
        // Fetch constants: textures (whole, all layers and mips) and vertex buffers.
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            uint32_t g[6];
            for (int i = 0; i < 6; ++i) g[i] = Ld(d + dev::kFetch + 24 * slot + 4 * i);
            if ((g[0] & 3) == 2)
            {
                xenos::TextureFetch tf;
                memcpy(tf.dword, g, sizeof(g));
                uint32_t bytes = xenos::TextureBytes(tf);
                Range(tf.baseAddress(), bytes);
                if (tf.mipAddress()) Range(tf.mipAddress(), bytes);
            }
            else
                for (int v = 0; v < 3; ++v)
                    if ((g[v * 2] & 3) == 3) Range(g[v * 2] & 0xFFFFFFFC, ((g[v * 2 + 1] >> 2) & 0xFFFFFF) * 4);
        }
    }

    // ---- records --------------------------------------------------------------
    struct Blob { uint32_t tag; uint32_t va; uint32_t bytes; };
    void Call(uint32_t fn, uint32_t depth, uint32_t lr, const uint32_t r[8], double f1, double f2, uint32_t ret,
              const std::vector<Blob>& blobs)
    {
        Byte('C');
        U32(fn); U32(depth); U32(lr);
        Out(r, 32);
        Out(&f1, 8);
        Out(&f2, 8);
        U32(ret);
        U32(uint32_t(blobs.size()));
        for (const Blob& b : blobs)
        {
            U32(b.tag);
            U32(b.bytes);
            if (b.bytes) Out(GuestToHost(b.va), b.bytes);
        }
        ++records;
    }
    void FlushPending()
    {
        if (!constPending) return;
        constPending = false;
        std::vector<Blob> blobs;
        if (constRet) blobs.push_back({ kBlobConstants, constRet, constArgs[3] * 16 });
        if (constRet) Range(constRet, constArgs[3] * 16);   // read when the GPU executes (see d3d_frontend)
        Call(kGpuBeginShaderConstantF4, constDepth, constLr, constArgs, 0, 0, constRet, blobs);
    }
};
Recorder g_rec;
thread_local uint32_t t_depth = 0;   // nesting of the wrapped functions
thread_local uint32_t t_drawDevice = 0;   // the device of the draw being flushed (vertex shader patches)

// ---- the live native renderer: the live front end ----------------------------------------
struct LiveMem : d3dn::GuestMemory
{
    uint32_t Load32(uint32_t va) const override { return GLoad32(va); }
    const uint8_t* Ptr(uint32_t va, uint32_t) const override { return static_cast<const uint8_t*>(GuestToHost(va)); }
};
struct LiveFeed
{
    std::mutex mu;
    std::unique_ptr<d3dn::Frontend> fe;                 // the main device
    std::unordered_map<uint32_t, std::unique_ptr<d3dn::Frontend>> others;   // devices that record command buffers
    LiveMem mem;
    uint32_t device = 0;
    // The front end of device `dev`, once the live native renderer is up: the main
    // device's presents; others (D3D's command-buffer device) only record.
    d3dn::Frontend* Get(uint32_t dev)
    {
        if (!fe)
        {
            d3dn::Backend* be = NativeLiveBackend();
            if (!be) return nullptr;
            fe = std::make_unique<d3dn::Frontend>(*be);
        }
        if (!device)
        {
            // D3D's global device: *(*0x82000A34) (as BlockOnFence reads it).
            uint32_t slot = GLoad32(GameAddr(0x82000A34));
            device = slot ? GLoad32(slot) : 0;
            if (!device) device = dev;
            fprintf(stderr, "[native] feeding the D3D calls on device 0x%08X\n", device);
        }
        if (dev == device) return fe.get();
        if (!dev) return nullptr;
        auto& o = others[dev];
        if (!o)
        {
            o = std::make_unique<d3dn::Frontend>(*NativeLiveBackend(), false);
            fprintf(stderr, "[native] device 0x%08X records command buffers\n", dev);
        }
        return o.get();
    }
    void FlushPending();
    // HC09_NATIVE_PROF: the front ends' counters per frame, every 1200 swaps (the main device; recording devices).
    d3dn::Frontend::Counters last, lastOthers;
    uint64_t swaps = 0;
    void Report();
};
LiveFeed g_feed;
void LiveFeed::Report()
{
    if (!fe || ++swaps % 1200) return;
    d3dn::Frontend::Counters o;
    for (auto& [d, f] : others)
    {
        for (uint32_t i = 0; i < kCallCount; ++i) o.calls[i] += f->counters.calls[i];
        o.runItems += f->counters.runItems; o.invalidates += f->counters.invalidates;
        o.deltas += f->counters.deltas; o.deltaRanges += f->counters.deltaRanges;
        o.deltaRegs += f->counters.deltaRegs; o.deltaLoads += f->counters.deltaLoads;
    }
    {
        const d3dn::RegisterState& s = fe->State();
        static uint64_t d0 = 0, f0 = 0, b0 = 0;
        uint64_t d = s.statDraws - d0;
        fprintf(stderr, "[native-fe] swap %llu, dirty masks: %.1f%% of draws compared every block, %.1f blocks read per draw; %llu check mismatches so far\n",
                (unsigned long long)GpuSwapCount(), d ? 100.0 * double(s.statFull - f0) / d : 0.0, d ? double(s.statBlocks - b0) / d : 0.0,
                (unsigned long long)s.statMismatch);
        d0 = s.statDraws; f0 = s.statFull; b0 = s.statBlocks;
    }
    auto line = [](const char* who, const d3dn::Frontend::Counters& c, d3dn::Frontend::Counters& prev) {
        const double n = 1200.0;
        char calls[1024];
        int k = 0;
        calls[0] = 0;
        for (uint32_t i = 1; i < kCallCount; ++i)
            if (uint64_t v = c.calls[i] - prev.calls[i]; v && k < 960) k += snprintf(calls + k, sizeof(calls) - k, " %s %.1f", CallName(i), v / n);
        uint64_t deltas = c.deltas - prev.deltas;
        fprintf(stderr, "[native-fe] swap %llu, %s per frame:%s; replayed items %.1f, invalidates %.2f; deltas %.1f (%.1f ranges, %.1f registers, %.2f loads each)\n",
                (unsigned long long)GpuSwapCount(), who, calls, (c.runItems - prev.runItems) / n, (c.invalidates - prev.invalidates) / n, deltas / n,
                deltas ? double(c.deltaRanges - prev.deltaRanges) / deltas : 0.0, deltas ? double(c.deltaRegs - prev.deltaRegs) / deltas : 0.0,
                deltas ? double(c.deltaLoads - prev.deltaLoads) / deltas : 0.0);
        prev = c;
    };
    line("main device", fe->counters, last);
    if (!others.empty()) line("recording devices", o, lastOthers);
}
// HC09_NATIVE_PROF=1: the game thread's time in the front end (and the back end's
// queueing and snapshots behind it), reported by gpu_live.cpp.
// Kinds: 0 calls other than draws, 1 draws, 2 submits (the queued work to the back end).
struct FeTimer
{
    int kind;
    bool on = NativeLiveProfiling();
    std::chrono::steady_clock::time_point t0 = on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    explicit FeTimer(int k) : kind(k) {}
    ~FeTimer()
    {
        if (on) NativeLiveAddFrontendNs(kind, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    }
};
// Per thread (a command buffer may be recorded on another thread): BeginVertices'
// data pointer for EndVertices, and a GpuBeginShaderConstantF4 whose data the
// caller writes after the call (handed over at the thread's next call).
thread_local uint32_t t_vtxPtr = 0, t_vtxBytes = 0;
thread_local bool t_constPending = false;
thread_local d3dn::CallArgs t_constArgs;
thread_local uint32_t t_constPtr = 0, t_constBytes = 0, t_constDev = 0;
void LiveFeed::FlushPending()
{
    if (!t_constPending) return;
    t_constPending = false;
    d3dn::Frontend* f = Get(t_constDev);
    if (!f) return;
    if (t_constPtr) t_constArgs.blob[kBlobConstants] = { static_cast<const uint8_t*>(GuestToHost(t_constPtr)), t_constBytes };
    f->Call(t_constArgs, static_cast<const uint8_t*>(GuestToHost(t_constDev)), mem);
}
bool LiveOn()
{
    return NativeRendererLive();
}

void Args(PPCContext& ctx, uint32_t r[8])
{
    r[0] = ctx.r3.u32; r[1] = ctx.r4.u32; r[2] = ctx.r5.u32; r[3] = ctx.r6.u32;
    r[4] = ctx.r7.u32; r[5] = ctx.r8.u32; r[6] = ctx.r9.u32; r[7] = ctx.r10.u32;
}

// One wrapped call: record (if a capture is running) the device, the inputs and
// the call, forward to the original, then record the device after it.
// `blobs`: the data the call's pointer arguments point at (a capture records them; no allocation otherwise).
template <class F>
void Wrap(uint32_t fn, PPCContext& ctx, uint8_t* base, F original, bool draw, const Recorder::Blob* blobs = nullptr, size_t nBlobs = 0)
{
    uint32_t depth = t_depth++;
    // Live front end: calls by the game (depth 0); a draw goes to it after D3D flushed
    // its state (device after the call), anything else before the call.
    bool isDraw = fn == kDrawIndexedVertices || fn == kDrawVertices || fn == kBeginVertices || fn == kDrawVerticesUP;
    d3dn::Frontend* fe = nullptr;
    d3dn::CallArgs args;
    uint8_t before[48];
    uint32_t dev = ctx.r3.u32;
    if (LiveOn() && (fn == kBeginCommandBuffer || fn == kEndCommandBuffer || fn == kRunCommandBuffer))
    {
        static std::atomic<int> n{ 0 };
        if (n++ < 40) fprintf(stderr, "[native] %s on device 0x%08X (depth %u, thread %llu)\n", CallName(fn), dev, depth,
                              (unsigned long long)KernelCurrentTid());
    }
    if (LiveOn() && depth == 0)
    {
        FeTimer ft(isDraw ? 1 : 0);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        if ((fe = g_feed.Get(dev)))
        {
            g_feed.FlushPending();
            args.fn = fn;
            Args(ctx, args.r);
            args.f1 = ctx.f1.f64;
            for (size_t i = 0; i < nBlobs; ++i)
                if (const Recorder::Blob& b = blobs[i]; b.tag < 8 && b.va) args.blob[b.tag] = { static_cast<const uint8_t*>(GuestToHost(b.va)), b.bytes };
            if (fn == kEndVertices && t_vtxPtr)
                args.blob[kBlobVertices] = { static_cast<const uint8_t*>(GuestToHost(t_vtxPtr)), t_vtxBytes };
            if (isDraw) memcpy(before, GuestToHost(dev), sizeof(before));
            else if (!d3dn::Frontend::ResolveAfterCall(args)) fe->Call(args, static_cast<const uint8_t*>(GuestToHost(dev)), g_feed.mem);
        }
    }
    uint32_t savedDrawDevice = t_drawDevice;
    if (isDraw) t_drawDevice = dev;
    if (isDraw && depth == 0 && g_rec.heavyLeft) g_rec.frameDraws.fetch_add(1, std::memory_order_relaxed);
    bool rec = false;
    if (g_rec.target >= 0)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        if (fn == kBeginCommandBuffer && depth == 0) { g_rec.pagesThisCall.clear(); g_rec.CbBegin(ctx.r4.u32, dev); }
        if (fn == kRunCommandBuffer && g_rec.Active())   // what the buffer's draws read
        {
            g_rec.pagesThisCall.clear();
            auto it = g_rec.cbRanges.find(ctx.r4.u32);
            if (it != g_rec.cbRanges.end())
                for (auto [pa, bytes] : std::vector<std::pair<uint32_t, uint32_t>>(it->second)) g_rec.Range(pa, bytes);
        }
        if (g_rec.Recording())
        {
            rec = true;
            g_rec.pagesThisCall.clear();
            g_rec.FlushPending();
            g_rec.Device(ctx.r3.u32);
            if (draw) g_rec.DrawInputs(ctx.r3.u32);
            // Constants loaded through a pointer are read when the GPU executes: their
            // pages, checked again at the submit like a draw's inputs.
            if (fn == kGpuLoadShaderConstantF4Pointer) g_rec.Range(ctx.r6.u32, ctx.r7.u32 * 16);
            uint32_t r[8];
            Args(ctx, r);
            std::vector<Recorder::Blob> bv(blobs, blobs + nBlobs);
            if (fn == kEndVertices && g_rec.vtxPtr) bv.push_back({ kBlobVertices, g_rec.vtxPtr, g_rec.vtxBytes });
            g_rec.Call(fn, depth, uint32_t(ctx.lr), r, ctx.f1.f64, ctx.f2.f64, 0, bv);
        }
    }
    original(ctx, base);
    --t_depth;
    t_drawDevice = savedDrawDevice;
    if (fe && isDraw)
    {
        FeTimer ft(1);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        fe->Draw(args, before, static_cast<const uint8_t*>(GuestToHost(dev)), g_feed.mem);
    }
    else if (fe && d3dn::Frontend::ResolveAfterCall(args))   // a resolve into a mip level or slice: D3D's destination
    {
        FeTimer ft(0);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        fe->Call(args, static_cast<const uint8_t*>(GuestToHost(dev)), g_feed.mem);
    }
    if (rec)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        if (g_rec.Recording())
        {
            g_rec.Device(dev);
            g_rec.Byte('A');
            g_rec.U32(fn);
        }
        if (fn == kEndCommandBuffer && depth == 0) g_rec.CbEnd();
    }
}

bool Armed()
{
    static const bool armed = [] {
        const char* e = Hc09Env("HC09_D3D_CAPTURE");
        if (const char* h = Hc09Env("HC09_D3D_CAPTURE_HEAVY"))
        {
            char* end = nullptr;
            g_rec.heavyDraws = uint32_t(strtoul(h, &end, 10));
            g_rec.heavyLeft = g_rec.heavyDraws ? (*end == ',' ? uint32_t(strtoul(end + 1, &end, 10)) : 1u) : 0u;
            if (*end == ',') g_rec.heavyFrom = strtoull(end + 1, nullptr, 10);
        }
        if (!e && !g_rec.heavyLeft) return false;
        for (const char* p = e ? e : ""; *p;)
        {
            char* end = nullptr;
            unsigned long long n = strtoull(p, &end, 10);
            if (end == p) break;
            g_rec.frames.push_back(n);
            p = *end ? end + 1 : end;
        }
        if (g_rec.frames.empty() && !g_rec.heavyLeft) return false;
        g_rec.target = 0;
        const char* d = Hc09Env("HC09_D3D_CAPTURE_DIR");
        g_rec.dir = d ? d : "captures";
        fprintf(stderr, "[d3dcap] armed: %zu frame(s) from D3D swap %llu, %u after frames of %u+ draws -> %s\n", g_rec.frames.size(),
                (unsigned long long)(g_rec.frames.empty() ? 0 : g_rec.frames[0]), g_rec.heavyLeft, g_rec.heavyDraws, g_rec.dir.c_str());
        return true;
    }();
    return armed;
}

}  // namespace

// ---- draws -------------------------------------------------------------------
PPC_FUNC(sub_8233D760) { D3DTimed d3dTimed(0x8233D760, ctx); Wrap(kDrawIndexedVertices, ctx, base, __imp__sub_8233D760, true); }
PPC_FUNC(sub_8233D378) { D3DTimed d3dTimed(0x8233D378, ctx); Wrap(kDrawVertices, ctx, base, __imp__sub_8233D378, true); }
PPC_FUNC(sub_8233CE80)
{
    D3DTimed d3dTimed(0x8233CE80, ctx);
    uint32_t count = ctx.r5.u32, stride = ctx.r6.u32;
    Wrap(kBeginVertices, ctx, base, __imp__sub_8233CE80, true);
    if (LiveOn() && t_depth == 0)
    {
        t_vtxPtr = ctx.r3.u32;
        t_vtxBytes = ctx.r3.u32 ? count * stride : 0;
    }
    if (g_rec.target >= 0)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        g_rec.vtxPtr = ctx.r3.u32;   // the data pointer BeginVertices returned
        g_rec.vtxBytes = ctx.r3.u32 ? count * stride : 0;
        if (g_rec.Recording()) { g_rec.Byte('R'); g_rec.U32(kBeginVertices); g_rec.U32(ctx.r3.u32); }
    }
}
PPC_FUNC(sub_8233D320)
{
    D3DTimed d3dTimed(0x8233D320, ctx);
    Wrap(kEndVertices, ctx, base, __imp__sub_8233D320, false);
    g_rec.vtxPtr = 0;
    if (LiveOn() && t_depth == 0) t_vtxPtr = 0;
}

PPC_FUNC(sub_8233D330)
{
    D3DTimed d3dTimed(0x8233D330, ctx);
    const Recorder::Blob b[] = { { kBlobVertices, ctx.r6.u32, ctx.r5.u32 * ctx.r7.u32 } };
    Wrap(kDrawVerticesUP, ctx, base, __imp__sub_8233D330, true, b, 1);
}

// ---- constants written straight into the ring ----------------------------------
PPC_FUNC(sub_8233CD60)
{
    D3DTimed d3dTimed(0x8233CD60, ctx);
    uint32_t r[8];
    Args(ctx, r);
    uint32_t depth = t_depth, lr = uint32_t(ctx.lr);
    __imp__sub_8233CD60(ctx, base);
    if (LiveOn() && depth == 0)
    {
        FeTimer ft(0);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        if (g_feed.Get(r[0]))
        {
            g_feed.FlushPending();
            t_constArgs = d3dn::CallArgs{};
            t_constArgs.fn = kGpuBeginShaderConstantF4;
            memcpy(t_constArgs.r, r, sizeof(r));
            t_constArgs.ret = ctx.r3.u32;
            t_constPtr = ctx.r3.u32;
            t_constBytes = r[3] * 16;
            t_constDev = r[0];
            t_constPending = true;   // the caller writes the constants after we return
        }
    }
    if (g_rec.target < 0) return;
    std::lock_guard<std::mutex> lk(g_rec.mu);
    if (!g_rec.Recording()) return;
    g_rec.FlushPending();
    g_rec.Device(r[0]);
    memcpy(g_rec.constArgs, r, sizeof(r));
    g_rec.constRet = ctx.r3.u32;
    g_rec.constDepth = depth;
    g_rec.constLr = lr;
    g_rec.constPending = true;   // the caller writes the constants after we return
}
PPC_FUNC(sub_8233CE00)
{
    D3DTimed d3dTimed(0x8233CE00, ctx);
    if (InterpTraceOn()) InterpTraceConstPointer(ctx);
    const Recorder::Blob b[] = { { kBlobConstants, ctx.r6.u32, ctx.r7.u32 * 16 } };
    Wrap(kGpuLoadShaderConstantF4Pointer, ctx, base, __imp__sub_8233CE00, false, b, 1);
}

// ---- clears, resolves, tiling, predication -------------------------------------
PPC_FUNC(sub_8233CC18)
{
    D3DTimed d3dTimed(0x8233CC18, ctx);
    std::vector<Recorder::Blob> b;
    if (ctx.r5.u32) b.push_back({ kBlobRect, ctx.r5.u32, 16 });
    if (ctx.r6.u32) b.push_back({ kBlobColor, ctx.r6.u32, 16 });
    Wrap(kClearF, ctx, base, __imp__sub_8233CC18, true, b.data(), b.size());
}
PPC_FUNC(sub_82322A98)
{
    D3DTimed d3dTimed(0x82322A98, ctx);
    std::vector<Recorder::Blob> b;
    if (ctx.r5.u32) b.push_back({ kBlobRect, ctx.r5.u32, 16 });
    if (ctx.r7.u32) b.push_back({ kBlobPoint, ctx.r7.u32, 8 });
    if (ctx.r10.u32) b.push_back({ kBlobColor, ctx.r10.u32, 16 });
    b.push_back({ kBlobStack, ctx.r1.u32, 128 });
    if (g_rec.target >= 0 && g_rec.Active() && ctx.r6.u32)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        g_rec.pagesThisCall.clear();
        g_rec.Range(ctx.r6.u32, 64);   // destination texture header
    }
    Wrap(kResolve, ctx, base, __imp__sub_82322A98, true, b.data(), b.size());
}
PPC_FUNC(sub_8233B0A0)
{
    D3DTimed d3dTimed(0x8233B0A0, ctx);
    std::vector<Recorder::Blob> b;
    if (ctx.r6.u32) b.push_back({ kBlobRects, ctx.r6.u32, 16 * std::min<uint32_t>(ctx.r5.u32, 16) });
    if (ctx.r7.u32) b.push_back({ kBlobColor, ctx.r7.u32, 16 });
    Wrap(kBeginTiling, ctx, base, __imp__sub_8233B0A0, true, b.data(), b.size());
}
PPC_FUNC(sub_8233B540)
{
    D3DTimed d3dTimed(0x8233B540, ctx);
    std::vector<Recorder::Blob> b;
    if (ctx.r7.u32) b.push_back({ kBlobColor, ctx.r7.u32, 16 });
    if (g_rec.target >= 0 && g_rec.Active() && ctx.r6.u32)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        g_rec.pagesThisCall.clear();
        g_rec.Range(ctx.r6.u32, 64);
    }
    Wrap(kEndTiling, ctx, base, __imp__sub_8233B540, true, b.data(), b.size());
}
PPC_FUNC(sub_8233AF30) { D3DTimed d3dTimed(0x8233AF30, ctx); Wrap(kSetPredication, ctx, base, __imp__sub_8233AF30, false); }
PPC_FUNC(sub_82328D28) { D3DTimed d3dTimed(0x82328D28, ctx); Wrap(kInsertCallback, ctx, base, __imp__sub_82328D28, false); }
PPC_FUNC(sub_8232A400) { D3DTimed d3dTimed(0x8232A400, ctx); Wrap(kBeginCommandBuffer, ctx, base, __imp__sub_8232A400, false); }
PPC_FUNC(sub_8232A8A8) { D3DTimed d3dTimed(0x8232A8A8, ctx); Wrap(kEndCommandBuffer, ctx, base, __imp__sub_8232A8A8, false); }
PPC_FUNC(sub_8232ABA8) { D3DTimed d3dTimed(0x8232ABA8, ctx); Wrap(kRunCommandBuffer, ctx, base, __imp__sub_8232ABA8, false); }

// ---- swap: frame boundaries ----------------------------------------------------
PPC_FUNC(sub_82333410)
{
    D3DTimed d3dTimed(0x82333410, ctx);
    bool armed = Armed();
    if (armed && g_rec.Active() && ctx.r4.u32)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        g_rec.pagesThisCall.clear();
        g_rec.Range(ctx.r4.u32, 64);   // front buffer texture header
    }
    uint32_t dev = ctx.r3.u32;
    Wrap(kSwap, ctx, base, __imp__sub_82333410, false);
    if (LiveOn() && NativeLiveProfiling())
    {
        std::lock_guard<std::mutex> lk(g_feed.mu);
        g_feed.Report();
    }
    if (!armed) return;
    std::lock_guard<std::mutex> lk(g_rec.mu);
    ++g_rec.swaps;
    bool was = g_rec.Active();
    if (was) g_rec.Close();
    uint32_t drew = g_rec.frameDraws.exchange(0, std::memory_order_relaxed);
    if (std::find(g_rec.frames.begin(), g_rec.frames.end(), g_rec.swaps) != g_rec.frames.end()) g_rec.Open(dev);
    else if (!was && g_rec.heavyLeft && drew >= g_rec.heavyDraws && g_rec.swaps >= g_rec.heavyFrom &&
             (!g_rec.heavyLast || g_rec.swaps >= g_rec.heavyLast + 600))
    {
        fprintf(stderr, "[d3dcap] swap %llu drew %u draws: recording the next frame\n", (unsigned long long)g_rec.swaps, drew);
        --g_rec.heavyLeft;
        g_rec.heavyLast = g_rec.swaps;
        g_rec.Open(dev);
    }
}

// ---- vertex shader patching (sub_8232BD98: vs r3, microcode r4, decl r5, strides r6, variant r7) ----
PPC_FUNC(sub_8232BD98)
{
    uint32_t vs = ctx.r3.u32, dst = ctx.r4.u32, variant = ctx.r7.u32;
    __imp__sub_8232BD98(ctx, base);
    if (LiveOn())
    {
        FeTimer ft(0);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        if (d3dn::Frontend* fe = g_feed.fe && t_drawDevice ? g_feed.Get(t_drawDevice) : nullptr)
        {
            uint32_t off = GLoad32(vs + 896 + 8 * variant);
            uint32_t bytes = std::min<uint32_t>(GLoad32(vs + 872 + off + 4), 0x10000);
            fe->VertexShaderPatched(vs, variant, dst, static_cast<const uint8_t*>(GuestToHost(dst)), bytes, g_feed.mem);
        }
    }
    if (g_rec.target < 0) return;
    std::lock_guard<std::mutex> lk(g_rec.mu);
    if (!g_rec.Recording()) return;
    uint32_t off = GLoad32(vs + 896 + 8 * variant);
    uint32_t bytes = std::min<uint32_t>(GLoad32(vs + 872 + off + 4), 0x10000);
    g_rec.Byte('V');
    g_rec.U32(vs); g_rec.U32(variant); g_rec.U32(dst); g_rec.U32(bytes);
    g_rec.Out(GuestToHost(dst), bytes);
}

// ---- command buffer submit (sub_82328440: closes the segment, the GPU may read it) ----
PPC_FUNC(sub_82328440)
{
    if (LiveOn())
    {
        FeTimer ft(2);
        std::lock_guard<std::mutex> lk(g_feed.mu);
        if (g_feed.fe && ctx.r3.u32 == g_feed.device)
        {
            g_feed.FlushPending();
            g_feed.fe->Kick();
        }
    }
    if (g_rec.target >= 0)
    {
        std::lock_guard<std::mutex> lk(g_rec.mu);
        if (g_rec.Active())
        {
            g_rec.FlushPending();
            g_rec.Kick();
        }
    }
    uint32_t dev = ctx.r3.u32, fence = GLoad32(dev + 10908);
    __imp__sub_82328440(ctx, base);
    // The kick wrote fence `fence` (sub_82328358 steps dev+10908 by 2): the native
    // renderer's marker for it follows the work handed over above.
    if (LiveOn() && GLoad32(dev + 10908) != fence)
    {
        std::lock_guard<std::mutex> lk(g_feed.mu);
        if (g_feed.fe && dev == g_feed.device)
        {
            uint32_t va = GLoad32(dev + 10896);   // the fence word; the GPU address as D3D's phys()
            NativeLiveKicked((va & 0x1FFFFFFFu) + ((((va >> 20) + 0x200) & 0x1000)), fence);
        }
    }
}

// ---- D3D's GPU waits with native fences (NativeLiveFenceNeeded / NativeLiveFenceWait) ----
namespace {
uint32_t D3DDevice()
{
    uint32_t slot = GLoad32(GameAddr(0x82000A34));
    return slot ? GLoad32(slot) : 0;
}
}  // namespace
// sub_82323A30: one step of every D3D spin on the GPU (r3 = the wait's record: +0 device,
// +4 block type). Waits other than ring space (1 primary, 2 secondary: the CP's
// progress) are fence waits: the renderer submits what it has recorded.
PPC_FUNC(sub_82323A30)
{
    if (NativeLiveFencesOn())
    {
        uint32_t rec = ctx.r3.u32, type = GLoad32(rec + 4), dev = GLoad32(rec);
        if (type != 1 && type != 2)
        {
            uint32_t cur = GLoad32(dev + 10908);
            NativeLiveFenceNeeded(cur, cur - 2);
        }
    }
    __imp__sub_82323A30(ctx, base);
}
// Query GetData (r3 = query, its fence at +24): the game spins on it until the result
// is there (RNA 829D77D8).
PPC_FUNC(sub_82330460)
{
    D3DTimed d3dTimed(0x82330460, ctx);
    uint32_t fence = GLoad32(ctx.r3.u32 + 24);
    __imp__sub_82330460(ctx, base);
    if (ctx.r3.u32 == 1 && NativeLiveFencesOn())
        if (uint32_t dev = D3DDevice()) NativeLiveFenceNeeded(GLoad32(dev + 10908), fence);
}
// IsFencePending (r3 = fence).
PPC_FUNC(sub_82328C88)
{
    D3DTimed d3dTimed(0x82328C88, ctx);
    uint32_t fence = ctx.r3.u32;
    __imp__sub_82328C88(ctx, base);
    if (ctx.r3.u32 && NativeLiveFencesOn())
        if (uint32_t dev = D3DDevice()) NativeLiveFenceNeeded(GLoad32(dev + 10908), fence);
}

// HC09_NATIVE_LOCK_TRACE=1 (diagnostic): resource locks (sub_82329400: r3 resource, r4 D3DBLOCKTYPE, r5.. the lock's
// range and flags) that block for the GPU: by resource, every 1200 swaps (count, time, what the resource is).
namespace {
struct LockStat { uint64_t n = 0, ns = 0, maxNs = 0; uint32_t type = 0, lr = 0; uint32_t fetch[6] = {}; };
std::mutex g_lockMu;
std::unordered_map<uint32_t, LockStat> g_lockStats;
uint64_t g_lockLastSwap = 0;
}  // namespace
PPC_FUNC(sub_82329400)
{
    static const bool trace = [] { const char* e = Hc09Env("HC09_NATIVE_LOCK_TRACE"); return e && atoi(e) != 0; }();
    if (!trace) { __imp__sub_82329400(ctx, base); return; }
    uint32_t res = ctx.r3.u32, type = ctx.r4.u32, lr = uint32_t(ctx.lr);
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82329400(ctx, base);
    uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    std::lock_guard<std::mutex> lk(g_lockMu);
    if (ns > 100000)   // blocked (0.1 ms)
    {
        LockStat& s = g_lockStats[res];
        ++s.n;
        s.ns += ns;
        s.maxNs = std::max(s.maxNs, ns);
        s.type = type;
        s.lr = lr;
        for (int i = 0; i < 6; ++i) s.fetch[i] = GLoad32(res + 28 + 4 * i);
    }
    uint64_t sw = GpuSwapCount();
    if (sw >= g_lockLastSwap + 1200 && !g_lockStats.empty())
    {
        std::vector<std::pair<uint64_t, uint32_t>> order;
        for (auto& [r, s] : g_lockStats) order.emplace_back(s.ns, r);
        std::sort(order.rbegin(), order.rend());
        double frames = g_lockLastSwap ? double(sw - g_lockLastSwap) : 1.0;
        for (size_t i = 0; i < order.size() && i < 6; ++i)
        {
            const LockStat& s = g_lockStats[order[i].second];
            xenos::TextureFetch tf;
            memcpy(tf.dword, s.fetch, sizeof(s.fetch));
            fprintf(stderr, "[native] blocking locks by resource (swap %llu): 0x%08X type %u (from 0x%08X): %.2f/frame, %.2f ms each (max %.2f);"
                            " fetch %08X %08X %08X %08X %08X %08X (format %u, %ux%u, base 0x%08X)\n",
                    (unsigned long long)sw, order[i].second, s.type, s.lr, double(s.n) / frames, 1e-6 * double(s.ns) / double(s.n), 1e-6 * double(s.maxNs),
                    s.fetch[0], s.fetch[1], s.fetch[2], s.fetch[3], s.fetch[4], s.fetch[5], tf.format(), (s.fetch[2] & 0x1FFF) + 1,
                    ((s.fetch[2] >> 13) & 0x1FFF) + 1, tf.baseAddress());
        }
        g_lockStats.clear();
        g_lockLastSwap = sw;
    }
}
