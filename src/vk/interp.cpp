// Smooth motion above 30 Hz (docs/NATIVE_RENDERER.md 7.12): how the render loop sees the 30 Hz
// simulation. Diagnostics only (HC09_INTERP_TRACE, off by default): every wrapper forwards unchanged.
// interp.h has the interface and the knobs.
//
// What the render side reads, as measured (7.12):
// - Characters (players, officials, sideline): model render objects drawn by sub_8293DBE8 (r3 = the
//   object, r4 = the pass, 3/2/5 per frame). Their 928-byte instances are at *(obj+240) (*(obj+248)
//   of them); each frame an instance gets a 3120-byte pose block *(inst+688) from a ring
//   (*(obj+256), index *(obj+272), sub_82933020): world position at +80..88 (yards, y up), joints
//   further on (sub_828CEF08 turns them into matrices *(inst+720); palettes of 104-128 vertex
//   constants, 3 per bone, model space; a 128-constant palette's c127 holds the world position).
// - The ball: BallMgr's BallEntity "football" (816 bytes, constructor sub_827467C0): position at
//   +64..72, orientation quaternion at +80..92, both written by the simulation once per tick.
// - The crowd: sub_828DC1D8 samples its clips per channel (r3 = the animation object, r4 = the
//   channel, record at *(r3+156) + 52 * r4: clip at +28, time offset at +36, written per tick).
#include "dev_env.h"
#include "interp.h"
#include "kernel.h"
#include "gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();
double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

bool EnvOnV(const char* e)
{
    return e && *e && strcmp(e, "0") != 0;
}
#define EnvOn(name) EnvOnV(Hc09Env(name))

// HC09_INTERP_TRACE_DUMPS=1: the detail windows also dump model objects, animation records, palettes and pose blocks.
bool DumpsOn() { static const bool on = EnvOn("HC09_INTERP_TRACE_DUMPS"); return on; }

float F(uint32_t be) { uint32_t v = __builtin_bswap32(be); float f; memcpy(&f, &v, 4); return f; }
float GF(uint32_t ea) { uint32_t v = GLoad32(ea); float f; memcpy(&f, &v, 4); return f; }

// The loops (hooks_pacing.cpp tells them apart).
std::atomic<uint64_t> g_simBegun{ 0 }, g_simDone{ 0 };   // simulation updates begun / finished
std::atomic<int> g_simBusy{ 0 };
std::atomic<double> g_simDoneAt{ 0 };                     // Now() when the last one finished
// Render updates: how many saw a simulation update begin or end while they ran.
std::atomic<uint64_t> g_renderUpdates{ 0 }, g_renderOverlap{ 0 }, g_renderBusySim{ 0 };
thread_local uint64_t t_renderSimBegun = 0, t_renderSimDone = 0;

// Detail windows (frames counted by InterpFrame).
std::atomic<bool> g_detail{ false };
std::atomic<uint64_t> g_frame{ 0 };

struct Pal
{
    uint64_t hash = 0, lastFrame = 0;
    uint32_t run = 0;
    float a[3] = {}, b[3] = {};   // bone 0's translation read two ways: c3.xyz and (c0.w, c1.w, c2.w)
};

struct Trace
{
    std::unordered_map<const void*, Pal> pals;
    std::unordered_map<uint32_t, Pal> byAddr;
    std::vector<std::vector<uint32_t>> prevK, curK;
    uint64_t kFrame = 0;
    uint64_t setHash = 0, prevSetHash = 0, setRun = 0, setRepeats = 0, setChanges = 0, setHist[17] = {};
    uint64_t aRepeats = 0, aChanges = 0, aRuns = 0, aRunFrames = 0, aHist[17] = {};
    // since the last report
    uint64_t frames = 0, loads = 0, repeats = 0, changes = 0, runs = 0, runFrames = 0, ticks0 = 0;
    uint64_t hist[17] = {};
    double dA = 0, dB = 0;            // summed |delta| of the two translations at changes
    double reportAt = 5;
    std::vector<double> at;           // detail windows still to come (seconds)
    int detailLeft = 0, detailFrames = 40;
    std::vector<const void*> follow;  // the palettes the detail window follows
    std::string line;                 // this frame's detail
    uint64_t tickAtFrame = 0;
};
Trace* g_trace = nullptr;

Trace& T()
{
    if (!g_trace)
    {
        g_trace = new Trace;
        const char* e = Hc09Env("HC09_INTERP_TRACE_AT");
        std::string s = e && *e ? e : "430,530,630,730,830";
        for (size_t p = 0; p < s.size();)
        {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            g_trace->at.push_back(atof(s.substr(p, q - p).c_str()));
            p = q + 1;
        }
        std::sort(g_trace->at.begin(), g_trace->at.end());
        if (const char* n = Hc09Env("HC09_INTERP_TRACE_FRAMES")) g_trace->detailFrames = std::max(1, atoi(n));
        fprintf(stderr, "[interp] trace on: report every 5 s, detail windows at %s s (%d frames)\n", s.c_str(), g_trace->detailFrames);
    }
    return *g_trace;
}

uint64_t Hash(const uint32_t* p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void InstReport();
void ConstReport();
void BallReport();
void MatchReport();
void BallFrame();
void Report(Trace& t, double now)
{
    static double last = 0;
    double dt = now - last;
    last = now;
    uint64_t ticks = g_simDone.load();
    std::string h;
    char b[48];
    for (int i = 1; i <= 16; ++i)
        if (t.hist[i]) { snprintf(b, sizeof(b), " %d%s:%llu", i, i == 16 ? "+" : "", (unsigned long long)t.hist[i]); h += b; }
    uint64_t ru = g_renderUpdates.exchange(0), ro = g_renderOverlap.exchange(0), rb = g_renderBusySim.exchange(0);
    // (Palettes are not compared per draw: one recorded draw is replayed for many players.)
    fprintf(stderr, "[interp] sw%llu %.1f s: %.1f frames/s, %.1f sim ticks/s; palettes %.1f/frame; "
                    "render updates %llu, %llu saw a sim update begin/end, %llu began during one\n",
            (unsigned long long)GpuSwapCount(), dt, t.frames / dt, (ticks - t.ticks0) / dt, t.frames ? double(t.loads) / t.frames : 0.0,
            (unsigned long long)ru, (unsigned long long)ro, (unsigned long long)rb);
    (void)h;
    std::string sh;
    uint64_t sr = 0, sf = 0;
    for (int i = 1; i <= 16; ++i)
        if (t.setHist[i]) { snprintf(b, sizeof(b), " %d%s:%llu", i, i == 16 ? "+" : "", (unsigned long long)t.setHist[i]); sh += b; sr += t.setHist[i]; sf += i * t.setHist[i]; }
    fprintf(stderr, "[interp]   all palettes of a frame: %.1f%% of frames draw the previous frame's exactly, run %.2f frames mean (length:count%s)\n",
            t.setRepeats + t.setChanges ? 100.0 * t.setRepeats / double(t.setRepeats + t.setChanges) : 0.0, sr ? double(sf) / sr : 0.0, sh.c_str());
    t.setRepeats = t.setChanges = 0;
    memset(t.setHist, 0, sizeof(t.setHist));
    InstReport();
    ConstReport();
    BallReport();
    MatchReport();
    t.aRepeats = t.aChanges = t.aRuns = t.aRunFrames = 0;
    memset(t.aHist, 0, sizeof(t.aHist));
    if (t.byAddr.size() > 100000) t.byAddr.clear();
    t.frames = t.loads = t.repeats = t.changes = t.runs = t.runFrames = 0;
    memset(t.hist, 0, sizeof(t.hist));
    t.dA = t.dB = 0;
    t.ticks0 = ticks;
}

// Small model objects (8 instances or fewer: the ball and props): pose block addresses, and
// whether their position (pb+80..88) changes inside simulation updates or between render frames.
struct Small { std::atomic<uint32_t> pb{ 0 }; uint32_t atBegin[3] = {}, lastSeen[3] = {}, at60[2][3] = {}; uint64_t inSim = 0, outSim = 0, frames = 0, in60[2] = {}; uint64_t seenTicks = 0; };
int g_smallLogged = 0;
Small g_small[8];
std::atomic<uint32_t> g_smallObj{ 0 };
void Small60Edge(int which, bool begin)
{
    for (Small& sm : g_small)
    {
        uint32_t pb = sm.pb.load(std::memory_order_relaxed);
        if (!pb) continue;
        uint32_t v[3] = { GLoad32(pb + 80), GLoad32(pb + 84), GLoad32(pb + 88) };
        if (begin) memcpy(sm.at60[which], v, sizeof(v));
        else if (memcmp(v, sm.at60[which], sizeof(v))) ++sm.in60[which];
    }
}
void SmallSimEdge(bool begin)
{
    for (Small& sm : g_small)
    {
        uint32_t pb = sm.pb.load(std::memory_order_relaxed);
        if (!pb) continue;
        uint32_t v[3] = { GLoad32(pb + 80), GLoad32(pb + 84), GLoad32(pb + 88) };
        if (begin) memcpy(sm.atBegin, v, sizeof(v));
        else if (memcmp(v, sm.atBegin, sizeof(v))) ++sm.inSim;
    }
}

// Character instances of the model objects (928-byte records at *(obj+240), *(obj+248)):
// every word compared with the previous frame's, the changes counted per offset by what the
// simulation did in between (0: a tick ended, 1: an update ran but none ended, 2: nothing).
struct InstObj { std::vector<uint32_t> w; uint64_t ticks = 0, begun = 0; uint32_t n = 0; };
constexpr uint32_t kPoseWords = 512;
struct PoseObj { uint32_t w[kPoseWords]; uint64_t ticks = 0, begun = 0, frame = 0; uint32_t posRun = 1, recent = 0; };
// Characters' world positions (pose block +80): frames of moving characters, those repeating the previous position,
// runs of equal positions (length:count), displacement per frame (all / non-zero), jumps (> 1 unit: cuts).
struct PosStat { uint64_t frames = 0, repeats = 0, runs = 0, runFrames = 0, moving = 0, jumps = 0, hist[17] = {}, nK[3] = {}; double sum = 0, sum2 = 0, sumMoving = 0, sumK[3] = {}; };
PosStat g_posStat;
std::map<uint64_t, uint32_t> g_steppers;   // (model object, instance) -> runs of 6+ equal positions ended by a move
std::unordered_map<uint64_t, PoseObj> g_pose;   // by (model object, instance)
struct PoseStat { uint64_t frames[3] = {}, changed[kPoseWords][3] = {}, blocks = 0; };
PoseStat g_poseStat;
struct InstStat { uint64_t frames[3] = {}, changed[232][3] = {}, instChanged[3] = {}; uint32_t n = 0; };
std::mutex g_instMu;
std::map<uint32_t, InstObj> g_inst;
std::map<uint32_t, InstStat> g_instStat;
void InstFrame(uint32_t obj)
{
    uint32_t arr = GLoad32(obj + 240), n = GLoad32(obj + 248);
    if (!arr || !n || n > 512) return;
    std::lock_guard<std::mutex> lk(g_instMu);
    InstObj& o = g_inst[obj];
    std::vector<uint32_t> w(size_t(n) * 232);
    for (size_t i = 0; i < w.size(); ++i) w[i] = GLoad32(arr + 4 * uint32_t(i));
    uint64_t ticks = g_simDone.load(), begun = g_simBegun.load();
    if (o.n == n && o.w.size() == w.size())
    {
        int k = ticks != o.ticks ? 0 : (begun != o.begun || g_simBusy.load()) ? 1 : 2;
        InstStat& st = g_instStat[obj];
        st.n = n;
        ++st.frames[k];
        for (uint32_t i = 0; i < n; ++i)
        {
            bool any = false;
            for (uint32_t j = 0; j < 232; ++j)
                if (w[i * 232 + j] != o.w[i * 232 + j]) { ++st.changed[j][k]; any = true; }
            if (any) ++st.instChanged[k];
        }
    }
    if (n <= 8)
    {
        g_smallObj.store(obj);
        bool simNow = g_simBusy.load();
        std::string line;
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t pb = w[i * 232 + 688 / 4];
            Small& sm = g_small[i];
            sm.pb.store(pb >= 0x40000000u && pb < 0xC0000000u ? pb : 0);
            if (!sm.pb.load()) continue;
            uint32_t v[3] = { GLoad32(pb + 80), GLoad32(pb + 84), GLoad32(pb + 88) };
            ++sm.frames;
            if (memcmp(v, sm.lastSeen, sizeof(v)))
            {
                if (sm.seenTicks == ticks && !simNow) ++sm.outSim;
                if (g_smallLogged++ < 60)
                {
                    float d[3];
                    for (int q = 0; q < 3; ++q) { float a, c; uint32_t x = v[q], y = sm.lastSeen[q]; memcpy(&a, &x, 4); memcpy(&c, &y, 4); d[q] = a - c; }
                    fprintf(stderr, "[interp-s] %.4f s f%llu tick %llu%s small #%u moved %.4f %.4f %.4f to %.3f %.3f %.3f\n", Now(), (unsigned long long)g_frame.load(),
                            (unsigned long long)ticks, simNow ? " (sim busy)" : "", i, d[0], d[1], d[2], GF(pb + 80), GF(pb + 84), GF(pb + 88));
                }
            }
            memcpy(sm.lastSeen, v, sizeof(v));
            sm.seenTicks = ticks;
            if (g_detail.load(std::memory_order_relaxed))
            {
                char b[96];
                snprintf(b, sizeof(b), " [%u %.3f %.3f %.3f]", i, GF(pb + 80), GF(pb + 84), GF(pb + 88));
                line += b;
            }
        }
        if (!line.empty())
            fprintf(stderr, "[interp-d] f%llu tick %llu%s small %08X:%s\n", (unsigned long long)g_frame.load(), (unsigned long long)ticks,
                    simNow ? " (sim busy)" : "", obj, line.c_str());
    }
    // Their pose blocks (*(inst+688)): same classification, all instances together.
    uint64_t fr = g_frame.load();
    int k = 2;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t pb = w[i * 232 + 688 / 4];
        if (pb < 0x40000000u || pb >= 0xC0000000u) continue;
        PoseObj& po = g_pose[(uint64_t(obj) << 16) | i];
        uint32_t cur[kPoseWords];
        for (uint32_t j = 0; j < kPoseWords; ++j) cur[j] = GLoad32(pb + 4 * j);
        if (po.frame && po.frame == fr)
        {
            k = ticks != po.ticks ? 0 : (begun != po.begun || g_simBusy.load()) ? 1 : 2;
            ++g_poseStat.frames[k];
            for (uint32_t j = 0; j < kPoseWords; ++j)
                if (cur[j] != po.w[j]) ++g_poseStat.changed[j][k];
        }
        if (g_detail.load(std::memory_order_relaxed) && DumpsOn() && po.frame && i < 64)
        {
            static int shown = 0;
            static uint64_t shownFrame = 0;
            if (shownFrame != fr) { shownFrame = fr; shown = 0; }
            if (shown++ < 4)
            {
                std::string s;
                char b[32];
                auto fl = [&](uint32_t j) { uint32_t v = cur[j]; float f; memcpy(&f, &v, 4); return f; };
                for (uint32_t off : { 80u, 84u, 88u, 128u, 132u, 136u, 140u })
                { snprintf(b, sizeof(b), " %u:%.3f", off, fl(off / 4)); s += b; }
                s += " |";
                for (uint32_t off = 528; off < 664; off += 4) { snprintf(b, sizeof(b), " %u:%.4f", off, fl(off / 4)); s += b; }
                fprintf(stderr, "[interp-d] f%llu tick %llu%s pose %08X/%u at %08X:%s\n", (unsigned long long)fr, (unsigned long long)ticks,
                        g_simBusy.load() ? " (sim busy)" : "", obj, i, pb, s.c_str());
            }
        }
        // The character's world position (pb+80..88): per frame displacement, and runs of frames
        // that repeat the previous frame's position, for characters that move.
        {
            auto fl = [&](const uint32_t* w, uint32_t j) { uint32_t v = w[j]; float f; memcpy(&f, &v, 4); return f; };
            float p[3] = { fl(cur, 20), fl(cur, 21), fl(cur, 22) };
            if (po.frame && po.frame == fr)
            {
                float q[3] = { fl(po.w, 20), fl(po.w, 21), fl(po.w, 22) };
                double d = std::sqrt(double(p[0] - q[0]) * (p[0] - q[0]) + double(p[1] - q[1]) * (p[1] - q[1]) + double(p[2] - q[2]) * (p[2] - q[2]));
                PosStat& ps = g_posStat;
                if (d < 1.0)   // larger: a cut / teleport
                {
                    po.recent = d > 0 ? 8 : po.recent ? po.recent - 1 : 0;
                    if (po.recent)   // moved within the last 8 frames
                    {
                        ++ps.frames;
                        int kk = ticks != po.ticks ? 0 : (begun != po.begun || g_simBusy.load()) ? 1 : 2;
                        ps.sumK[kk] += d;
                        ++ps.nK[kk];
                        ps.sum += d;
                        ps.sum2 += d * d;
                        if (d == 0) { ++ps.repeats; ++po.posRun; }
                        else { if (po.posRun >= 6) ++g_steppers[(uint64_t(obj) << 16) | i]; ++ps.hist[std::min<uint32_t>(po.posRun, 16)]; ps.runFrames += po.posRun; ++ps.runs; po.posRun = 1; ps.sumMoving += d; ++ps.moving; }
                    }
                    else po.posRun = 1;
                }
                else { ++ps.jumps; po.posRun = 1; po.recent = 0; }
            }
        }
        memcpy(po.w, cur, sizeof(cur));
        po.ticks = ticks;
        po.begun = begun;
        po.frame = fr + 1;
        ++g_poseStat.blocks;
    }
    o.w.swap(w);
    o.n = n;
    o.ticks = ticks;
    o.begun = begun;
}
void InstReport()
{
    {
        std::lock_guard<std::mutex> lk(g_instMu);
        std::string s;
        char b[80];
        for (uint32_t j = 0; j < kPoseWords; ++j)
        {
            const uint64_t* c = g_poseStat.changed[j];
            if (!c[0] && !c[1] && !c[2]) continue;
            snprintf(b, sizeof(b), " +%u:%llu/%llu/%llu", 4 * j, (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2]);
            s += b;
        }
        fprintf(stderr, "[interp]   pose blocks (*(inst+688)): %zu known, %llu reads; pairs after a tick / sim running / no sim %llu/%llu/%llu;"
                        " words changed%s\n", g_pose.size(), (unsigned long long)g_poseStat.blocks, (unsigned long long)g_poseStat.frames[0],
                (unsigned long long)g_poseStat.frames[1], (unsigned long long)g_poseStat.frames[2], s.c_str());
        g_poseStat = PoseStat();
        PosStat& ps = g_posStat;
        std::string h;
        for (int i = 1; i <= 16; ++i)
            if (ps.hist[i]) { snprintf(b, sizeof(b), " %d%s:%llu", i, i == 16 ? "+" : "", (unsigned long long)ps.hist[i]); h += b; }
        double mean = ps.frames ? ps.sum / ps.frames : 0, var = ps.frames ? ps.sum2 / ps.frames - mean * mean : 0;
        fprintf(stderr, "[interp]   character positions: %llu frames of moving characters, %.1f%% repeat the previous frame's position, run %.2f frames mean"
                        " (length:count%s); displacement per frame mean %.5f (cv %.2f), per changed frame %.5f; jumps %llu; mean after a tick / sim running / no sim %.5f/%.5f/%.5f (%llu/%llu/%llu frames)\n",
                (unsigned long long)ps.frames, ps.frames ? 100.0 * ps.repeats / ps.frames : 0.0, ps.runs ? double(ps.runFrames) / ps.runs : 0.0, h.c_str(),
                mean, mean > 0 ? std::sqrt(std::max(0.0, var)) / mean : 0.0, ps.moving ? ps.sumMoving / ps.moving : 0.0, (unsigned long long)ps.jumps,
                ps.nK[0] ? ps.sumK[0] / ps.nK[0] : 0.0, ps.nK[1] ? ps.sumK[1] / ps.nK[1] : 0.0, ps.nK[2] ? ps.sumK[2] / ps.nK[2] : 0.0,
                (unsigned long long)ps.nK[0], (unsigned long long)ps.nK[1], (unsigned long long)ps.nK[2]);
        g_posStat = PosStat();
        {
            std::string sl;
            for (int i = 0; i < 8; ++i)
                if (g_small[i].frames)
                {
                    snprintf(b, sizeof(b), " #%d: %llu/%llu (in 60 Hz loops %llu/%llu)", i, (unsigned long long)g_small[i].inSim, (unsigned long long)g_small[i].outSim,
                             (unsigned long long)g_small[i].in60[0], (unsigned long long)g_small[i].in60[1]);
                    sl += b;
                    g_small[i].inSim = g_small[i].outSim = g_small[i].frames = g_small[i].in60[0] = g_small[i].in60[1] = 0;
                }
            g_smallLogged = 0;
            if (!sl.empty())
                fprintf(stderr, "[interp]   small model %08X positions changed inside sim updates / between render frames with no sim update:%s\n",
                        g_smallObj.load(), sl.c_str());
        }
        if (!g_steppers.empty())
        {
            std::string st;
            for (auto& [k, n] : g_steppers)
            {
                uint32_t o = uint32_t(k >> 16), i = uint32_t(k & 0xFFFF);
                uint32_t arr = GLoad32(o + 240), r = arr + 928 * i, pb = GLoad32(r + 688);
                snprintf(b, sizeof(b), " %08X/%u:%u", o, i, n);
                st += b;
                if (pb) { snprintf(b, sizeof(b), "(at %.1f,%.1f,%.1f)", GF(pb + 80), GF(pb + 84), GF(pb + 88)); st += b; }
            }
            fprintf(stderr, "[interp]   characters holding a position 6+ frames while moving (object/instance:runs):%s\n", st.c_str());
            g_steppers.clear();
        }
        if (g_pose.size() > 4096) g_pose.clear();
    }
    std::map<uint32_t, InstStat> stats;
    { std::lock_guard<std::mutex> lk(g_instMu); stats.swap(g_instStat); }
    for (auto& [obj, st] : stats)
    {
        std::string s;
        char b[80];
        for (uint32_t j = 0; j < 232; ++j)
        {
            const uint64_t* c = st.changed[j];
            if (!c[0] && !c[1] && !c[2]) continue;
            snprintf(b, sizeof(b), " +%u:%llu/%llu/%llu", 4 * j, (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2]);
            s += b;
        }
        fprintf(stderr, "[interp]   model %08X: %u instances; frames after a tick / sim running / no sim %llu/%llu/%llu, instances changed %llu/%llu/%llu;"
                        " words changed (offset: same split)%s\n", obj, st.n, (unsigned long long)st.frames[0], (unsigned long long)st.frames[1],
                (unsigned long long)st.frames[2], (unsigned long long)st.instChanged[0], (unsigned long long)st.instChanged[1],
                (unsigned long long)st.instChanged[2], s.c_str());
    }
}

// The ball entity (BallMgr:BallEntity "football", 816 bytes, constructor sub_827467C0): its words
// compared frame to frame at the present, by what the simulation did in between.
std::atomic<uint32_t> g_ballEnt{ 0 };
struct BallWatch { uint32_t prev[204] = {}; bool have = false; uint64_t ticks = 0, begun = 0, frames[3] = {}, changed[204][3] = {}; };
BallWatch g_ballW;
void BallFrame()
{
    uint32_t b = g_ballEnt.load(std::memory_order_relaxed);
    if (!b) return;
    uint32_t cur[204];
    for (int i = 0; i < 204; ++i) cur[i] = GLoad32(b + 4 * i);
    uint64_t ticks = g_simDone.load(), begun = g_simBegun.load();
    BallWatch& w = g_ballW;
    if (w.have)
    {
        int k = ticks != w.ticks ? 0 : (begun != w.begun || g_simBusy.load()) ? 1 : 2;
        ++w.frames[k];
        for (int i = 0; i < 204; ++i)
            if (cur[i] != w.prev[i]) ++w.changed[i][k];
    }
    if (g_detail.load(std::memory_order_relaxed))
    {
        auto fl = [&](int i) { uint32_t v = cur[i]; float f; memcpy(&f, &v, 4); return f; };
        fprintf(stderr, "[interp-d] f%llu tick %llu%s ball %.3f %.3f %.3f%s\n", (unsigned long long)g_frame.load(), (unsigned long long)ticks,
                g_simBusy.load() ? " (sim busy)" : "", fl(16), fl(17), fl(18), memcmp(cur + 16, w.prev + 16, 12) ? " moved" : "");
    }
    memcpy(w.prev, cur, sizeof(cur));
    w.have = true;
    w.ticks = ticks;
    w.begun = begun;
}
void BallReport()
{
    BallWatch& w = g_ballW;
    if (!g_ballEnt.load()) return;
    std::string s;
    char b[64];
    for (int i = 0; i < 204; ++i)
    {
        const uint64_t* c = w.changed[i];
        if (c[0] + c[1] + c[2] < 10) continue;
        snprintf(b, sizeof(b), " +%d:%llu/%llu/%llu", 4 * i, (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2]);
        s += b;
    }
    auto fl = [&](int i) { uint32_t v = w.prev[i]; float f; memcpy(&f, &v, 4); return f; };
    std::string v;
    for (int i = 0; i < 204; ++i)
    {
        const uint64_t* c = w.changed[i];
        if (c[0] + c[1] + c[2] < 10) continue;
        snprintf(b, sizeof(b), " +%d=%.3f", 4 * i, fl(i));
        v += b;
    }
    fprintf(stderr, "[interp]   ball entity %08X: frames after a tick / sim running / no sim %llu/%llu/%llu; words changed%s; now%s\n", g_ballEnt.load(),
            (unsigned long long)w.frames[0], (unsigned long long)w.frames[1], (unsigned long long)w.frames[2], s.c_str(), v.c_str());
    memset(w.changed, 0, sizeof(w.changed));
    memset(w.frames, 0, sizeof(w.frames));
}

// Where the ball's entity position shows up in draw constants (and a character's pose position, for
// comparison): 3 consecutive floats, or a matrix translation (c3.xyz or c0.w/c1.w/c2.w).
struct Match { uint64_t n = 0; uint64_t vs = 0; uint32_t reg = 0, kind = 0, layout = 0, count = 0; };
std::map<uint64_t, Match> g_matches;   // (what, kind, layout, reg, vs) -> count
float g_target[2][3];                  // 0 ball, 1 a character
bool g_targetOn[2] = {};
void SetTargets()
{
    uint32_t b = g_ballEnt.load(std::memory_order_relaxed);
    g_targetOn[0] = b != 0;
    if (b) for (int i = 0; i < 3; ++i) g_target[0][i] = GF(b + 64 + 4 * i);
    uint32_t pb = g_small[0].pb.load(std::memory_order_relaxed);
    g_targetOn[1] = pb != 0;
    if (pb) for (int i = 0; i < 3; ++i) g_target[1][i] = GF(pb + 80 + 4 * i);
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.02f && std::fabs(b) > 0.5f; }
// vals: host floats of constants starting at constant `reg`, n float4s
void MatchConsts(const float* v, uint32_t n, uint32_t reg, int kind, uint64_t vs)
{
    for (int t = 0; t < 2; ++t)
    {
        if (!g_targetOn[t]) continue;
        const float* x = g_target[t];
        if (std::fabs(x[0]) + std::fabs(x[2]) < 1.0f) continue;   // near the origin: matches anything
        for (uint32_t c = 0; c < n; ++c)
        {
            for (int layout = 0; layout < 3; ++layout)
            {
                float a, b2, d;
                if (layout == 0) { a = v[4 * c]; b2 = v[4 * c + 1]; d = v[4 * c + 2]; }        // xyz of one constant
                else if (layout == 1) { if (c + 2 >= n) continue; a = v[4 * c + 3]; b2 = v[4 * c + 7]; d = v[4 * c + 11]; }   // .w of 3 rows
                else { a = v[4 * c]; b2 = v[4 * c + 2]; d = -v[4 * c + 1]; }                  // z-up copy (x, -z, y) as in the entity's +144
                if (!(Near(a, x[0]) && Near(b2, x[1]) && Near(d, x[2]))) continue;
                uint64_t key = (uint64_t(t) << 60) | (uint64_t(kind) << 58) | (uint64_t(layout) << 56) | (uint64_t(reg + c) << 40) | (vs & 0xFFFFFFFFFFull);
                Match& m = g_matches[key];
                ++m.n; m.vs = vs; m.reg = reg + c; m.kind = kind; m.layout = layout; m.count = n;
            }
        }
    }
}
void MatchReport()
{
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (auto& [k, m] : g_matches) v.push_back({ m.n, k });
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 10; ++i)
    {
        const Match& m = g_matches[v[i].second];
        fprintf(stderr, "[interp]   %s position found %llu times: %s constant %u (layout %s) of %u, vs/op %016llx\n", (v[i].second >> 60) ? "character" : "ball",
                (unsigned long long)m.n, m.kind ? "pointer-loaded" : "ring", m.reg, m.layout == 0 ? "xyz" : m.layout == 1 ? "rows .w" : "x,-z,y", m.count,
                (unsigned long long)m.vs);
    }
    g_matches.clear();
}

// Ring-set vertex constants per draw identity (InterpTraceDraw), classified at the frame's present.
struct ConstState { uint32_t val = 0; uint64_t lastFrame = 0; uint32_t changes[3] = {}, frames[3] = {}; uint64_t vs = 0; uint32_t count = 0, reg = 0; uint32_t raw[4] = {}; };
struct ConstTrace
{
    std::unordered_map<uint64_t, ConstState> st;
    std::unordered_map<uint64_t, uint32_t> occ;               // this frame's draws per base identity
    std::vector<std::pair<ConstState*, bool>> touched;        // this frame: (state, changed)
    uint64_t ticksAtPresent = 0, begunAtPresent = 0;
};
ConstTrace g_ct;

void ConstFrame(uint64_t frame)
{
    ConstTrace& c = g_ct;
    uint64_t ticks = g_simDone.load(), begun = g_simBegun.load();
    int k = ticks != c.ticksAtPresent ? 0 : (begun != c.begunAtPresent || g_simBusy.load()) ? 1 : 2;
    c.ticksAtPresent = ticks;
    c.begunAtPresent = begun;
    for (auto& [s, ch] : c.touched) { ++s->frames[k]; if (ch) ++s->changes[k]; }
    c.touched.clear();
    c.occ.clear();
    (void)frame;
}

void ConstReport()
{
    ConstTrace& c = g_ct;
    // Per identity (all its constants): stepped = changes after most ticks and almost never otherwise.
    struct Id { uint64_t vs = 0; uint32_t count = 0, stepped = 0, smooth = 0, regs = 0; double tickRate = 0, noneRate = 0; uint32_t reg0 = 0; const ConstState* ex = nullptr; };
    std::unordered_map<uint64_t, Id> ids;
    uint64_t pairs = 0, stepped = 0, smooth = 0;
    for (auto& [key, s] : c.st)
    {
        uint32_t ch = s.changes[0] + s.changes[1] + s.changes[2];
        if (ch < 10 || !s.frames[0] || !s.frames[2]) continue;
        ++pairs;
        double rt = double(s.changes[0]) / s.frames[0], rn = double(s.changes[2]) / s.frames[2];
        Id& id = ids[key >> 12];
        id.vs = s.vs; id.count = s.count; ++id.regs;
        double rs = double(s.changes[0] + s.changes[1]) / std::max(1u, s.frames[0] + s.frames[1]);
        if (rs > 0.2 && rn < 0.02) { ++stepped; ++id.stepped; id.tickRate += rs; id.noneRate += rn; if (!id.reg0) { id.reg0 = s.reg; id.ex = &s; } }
        else if (rn > 0.5) { ++smooth; ++id.smooth; }
    }
    uint64_t idStepped = 0, idSmooth = 0;
    std::vector<std::pair<uint32_t, const Id*>> top;
    for (auto& [k, id] : ids)
    {
        if (id.stepped) { ++idStepped; top.push_back({ id.stepped, &id }); }
        else if (id.smooth) ++idSmooth;
    }
    std::sort(top.rbegin(), top.rend(), [](auto& a, auto& b) { return a.first < b.first; });
    fprintf(stderr, "[interp]   draw constants (ring-set and pointer-loaded): %llu (draw, constant) pairs changing 10+ times: %llu sim-driven (change in >20%% of frames where the sim ticked or ran, <2%% of the others),"
                    " %llu change in most frames; draws with a stepped constant %llu, smooth only %llu\n",
            (unsigned long long)pairs, (unsigned long long)stepped, (unsigned long long)smooth, (unsigned long long)idStepped, (unsigned long long)idSmooth);
    for (size_t i = 0; i < top.size() && i < 12; ++i)
    {
        const Id& id = *top[i].second;
        float v[4];
        for (int q = 0; q < 4; ++q) v[q] = id.ex ? F(id.ex->raw[q]) : 0.0f;
        fprintf(stderr, "[interp]     sim-driven: vs/op %016llx %u indices/dwords, %u of %u constants (first reg %04X, 1xxxx = pointer load at constant xxxx; values %.3f %.3f %.3f %.3f),"
                        " changed in %.0f%% of sim frames, %.1f%% of others\n",
                (unsigned long long)id.vs, id.count, id.stepped, id.regs, id.reg0, v[0], v[1], v[2], v[3], 100.0 * id.tickRate / id.stepped, 100.0 * id.noneRate / id.stepped);
    }
    if (c.st.size() > 1500000) c.st.clear();
    else for (auto& [key, s] : c.st) { memset(s.changes, 0, sizeof(s.changes)); memset(s.frames, 0, sizeof(s.frames)); }
}

// Detail: model render objects and animation records, words compared with their previous dump.
struct Dump { std::vector<uint32_t> w; uint64_t frame = 0, tick = 0; };
std::mutex g_dumpMu;
std::unordered_map<uint64_t, Dump> g_dumps;

void DiffDump(const char* what, uint64_t key, uint32_t ea, uint32_t bytes, const char* extra)
{
    std::vector<uint32_t> w(bytes / 4);
    for (uint32_t i = 0; i < w.size(); ++i) w[i] = GLoad32(ea + 4 * i);
    uint64_t frame = g_frame.load(), tick = g_simDone.load();
    std::lock_guard<std::mutex> lk(g_dumpMu);
    Dump& d = g_dumps[key];
    std::string s;
    if (d.w.size() == w.size())
    {
        char b[48];
        int n = 0;
        for (uint32_t i = 0; i < w.size() && n < 48; ++i)
        {
            if (w[i] == d.w[i]) continue;
            uint32_t j = i;
            while (j + 1 < w.size() && w[j + 1] != d.w[j + 1]) ++j;
            if (j == i) snprintf(b, sizeof(b), " +%u", 4 * i);
            else snprintf(b, sizeof(b), " +%u..%u", 4 * i, 4 * j + 3);
            s += b;
            ++n;
            i = j;
        }
        fprintf(stderr, "[interp-d] f%llu tick %llu%s %s %08X%s: %s since f%llu tick %llu:%s\n", (unsigned long long)frame,
                (unsigned long long)tick, g_simBusy.load() ? " (sim busy)" : "", what, ea, extra, s.empty() ? "unchanged" : "changed",
                (unsigned long long)d.frame, (unsigned long long)d.tick, s.c_str());
    }
    else
        fprintf(stderr, "[interp-d] f%llu tick %llu %s %08X%s: first dump\n", (unsigned long long)frame, (unsigned long long)tick, what, ea, extra);
    d.w.swap(w);
    d.frame = frame;
    d.tick = tick;
}
}  // namespace

bool InterpTraceOn()
{
    static const bool on = EnvOn("HC09_INTERP_TRACE");
    return on;
}

bool InterpLoopsWanted() { return InterpTraceOn(); }

void InterpLoopUpdate(int loop, bool begin)
{
    if (loop >= 2) { if (InterpTraceOn()) Small60Edge(loop - 2, begin); return; }
    if (loop == 0)
    {
        if (begin) { g_simBusy.store(1); g_simBegun.fetch_add(1); if (InterpTraceOn()) SmallSimEdge(true); }
        else { if (InterpTraceOn()) SmallSimEdge(false); g_simDoneAt.store(Now()); g_simDone.fetch_add(1); g_simBusy.store(0); }
        return;
    }
    if (begin)
    {
        if (g_simBusy.load()) g_renderBusySim.fetch_add(1);
        t_renderSimBegun = g_simBegun.load();
        t_renderSimDone = g_simDone.load();
    }
    else
    {
        g_renderUpdates.fetch_add(1);
        if (g_simBegun.load() != t_renderSimBegun || g_simDone.load() != t_renderSimDone) g_renderOverlap.fetch_add(1);
    }
}

void InterpFrame()
{
    if (!InterpTraceOn()) return;
    Trace& t = T();
    double now = Now();
    if (t.detailLeft > 0)
    {
        fprintf(stderr, "[interp-d] f%llu presented %.4f s, tick %llu (%.1f ms after it ended)%s:%s\n", (unsigned long long)g_frame.load(), now,
                (unsigned long long)t.tickAtFrame, 1000.0 * (now - g_simDoneAt.load()), g_simBusy.load() ? " sim busy" : "", t.line.c_str());
        if (--t.detailLeft == 0) { g_detail.store(false); fprintf(stderr, "[interp-d] window ends\n"); }
    }
    t.line.clear();
    if (t.setHash)
    {
        if (t.setHash == t.prevSetHash) { ++t.setRun; ++t.setRepeats; }
        else { if (t.setRun) ++t.setHist[std::min<uint64_t>(t.setRun, 16)]; t.setRun = 1; ++t.setChanges; }
    }
    ConstFrame(g_frame.load());
    BallFrame();
    SetTargets();
    t.prevSetHash = t.setHash;
    t.setHash = 0;
    ++t.frames;
    uint64_t f = g_frame.fetch_add(1) + 1;
    t.tickAtFrame = g_simDone.load();
    if (!t.at.empty() && now >= t.at.front())
    {
        t.at.erase(t.at.begin());
        t.detailLeft = t.detailFrames;
        t.follow.clear();
        for (auto& [id, p] : t.pals)
            if (p.lastFrame + 1 == f && t.follow.size() < 4) t.follow.push_back(id);
        g_detail.store(true);
        fprintf(stderr, "[interp-d] window at %.1f s from frame %llu, following %zu palettes\n", now, (unsigned long long)f, t.follow.size());
    }
    if (now >= t.reportAt) { Report(t, now); t.reportAt = now + 5; }
}

void InterpTraceLoads(const void* id, uint32_t first, uint32_t address, const uint32_t* be, uint32_t dwords)
{
    if (first < 256 && dwords >= 4)
    {
        uint32_t n = std::min<uint32_t>(dwords / 4, 256 - first);
        std::vector<float> fv(4 * n);
        for (uint32_t q = 0; q < 4 * n; ++q) fv[q] = F(be[q]);
        MatchConsts(fv.data(), n, first, 1, uint64_t(uintptr_t(id)));
    }
    {
        ConstTrace& c = g_ct;
        uint64_t base = (uint64_t(uintptr_t(id)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(first) << 48);
        uint32_t occ = c.occ[base]++;
        uint64_t key = ((base ^ (uint64_t(occ) * 0x165667B19E3779F9ull)) << 12) | 0x800 | (first & 0x1FF);
        uint64_t f = g_frame.load() + 1;
        ConstState& st = c.st[key];
        uint32_t h = uint32_t(Hash(be, dwords) >> 16);
        if (st.lastFrame != f)
        {
            if (st.lastFrame + 1 == f) c.touched.push_back({ &st, st.val != h });
            st.val = h;
            st.lastFrame = f;
            st.vs = uint64_t(uintptr_t(id));
            st.count = dwords;
            st.reg = 0x10000 | first;
            for (uint32_t q = 0; q < 4 && q < dwords; ++q) st.raw[q] = be[q];
        }
    }
    if (first != 0 || dwords < 64) return;
    Trace& t = T();
    uint64_t f = g_frame.load() + 1;   // the frame being built
    ++t.loads;
    if (t.detailLeft > 0 && DumpsOn())
    {
        // k-th palette of this frame against the previous frame's k-th: which words differ
        if (t.kFrame != f) { t.prevK.swap(t.curK); t.curK.clear(); t.kFrame = f; }
        size_t k = t.curK.size();
        t.curK.emplace_back(be, be + dwords);
        if (k < t.prevK.size() && k < 6 && t.prevK[k].size() == dwords)
        {
            std::string d;
            int nd = 0;
            float mx = 0;
            for (uint32_t i = 0; i < dwords; ++i)
                if (t.prevK[k][i] != be[i])
                {
                    float df = std::fabs(F(be[i]) - F(t.prevK[k][i]));
                    mx = std::max(mx, df);
                    if (++nd <= 12) { char b[40]; snprintf(b, sizeof(b), " %u:%.4f>%.4f", i, F(t.prevK[k][i]), F(be[i])); d += b; }
                }
            fprintf(stderr, "[interp-d] f%llu palette #%zu (%u dwords, op %p): %d words differ from the previous frame's #%zu, max |d| %.4f:%s\n",
                    (unsigned long long)f, k, dwords, id, nd, k, mx, d.c_str());
        }
    }
    // The same metric keyed by the palette's address (a player's buffer, if it keeps one).
    {
        Pal& q = t.byAddr[address];
        uint64_t h = Hash(be, dwords);
        if (q.lastFrame + 1 == f)
        {
            if (h == q.hash) { ++q.run; ++t.aRepeats; }
            else { ++t.aHist[std::min(q.run, 16u)]; ++t.aRuns; t.aRunFrames += q.run; q.run = 1; ++t.aChanges; }
        }
        else if (q.lastFrame != f) q.run = 1;
        q.hash = h;
        q.lastFrame = f;
    }
    t.setHash += Hash(be, dwords) * 0x9E3779B97F4A7C15ull;
    Pal& p = t.pals[id];
    if (p.lastFrame == f) return;      // drawn twice in a frame (other passes): the first counts
    uint64_t h = Hash(be, dwords);
    float a[3] = { F(be[12]), F(be[13]), F(be[14]) }, b[3] = { F(be[3]), F(be[7]), F(be[11]) };
    if (p.lastFrame + 1 == f)
    {
        if (h == p.hash) { ++p.run; ++t.repeats; }
        else
        {
            ++t.hist[std::min(p.run, 16u)];
            ++t.runs;
            t.runFrames += p.run;
            p.run = 1;
            ++t.changes;
            t.dA += std::sqrt((a[0] - p.a[0]) * (a[0] - p.a[0]) + (a[1] - p.a[1]) * (a[1] - p.a[1]) + (a[2] - p.a[2]) * (a[2] - p.a[2]));
            t.dB += std::sqrt((b[0] - p.b[0]) * (b[0] - p.b[0]) + (b[1] - p.b[1]) * (b[1] - p.b[1]) + (b[2] - p.b[2]) * (b[2] - p.b[2]));
        }
    }
    else p.run = 1;
    if (t.detailLeft > 0)
    {
        auto it = std::find(t.follow.begin(), t.follow.end(), id);
        if (it != t.follow.end())
        {
            char s[200];
            snprintf(s, sizeof(s), " [%d %s c3 %.3f %.3f %.3f w %.3f %.3f %.3f]", int(it - t.follow.begin()), h == p.hash ? "same" : "NEW",
                     a[0], a[1], a[2], b[0], b[1], b[2]);
            t.line += s;
            if (t.detailLeft == t.detailFrames)
            {
                std::string r;
                for (int i = 0; i < 16; ++i) { snprintf(s, sizeof(s), " %.4f", F(be[i])); r += s; }
                fprintf(stderr, "[interp-d] palette %d (%u dwords) matrix 0:%s\n", int(it - t.follow.begin()), dwords, r.c_str());
            }
        }
    }
    p.hash = h;
    p.lastFrame = f;
    memcpy(p.a, a, sizeof(a));
    memcpy(p.b, b, sizeof(b));
}

// Detail-window probes of the render side (forward unchanged): HC09_INTERP_TRACE_DUMPS=1.
extern "C" PPC_FUNC(__imp__sub_8293DBE8);
extern "C" PPC_FUNC(__imp__sub_828DC1D8);
PPC_FUNC(sub_8293DBE8)
{
    if (InterpTraceOn() && ctx.r4.u32 == 3) InstFrame(ctx.r3.u32);
    if (g_detail.load(std::memory_order_relaxed) && DumpsOn())
    {
        uint32_t obj = ctx.r3.u32, pass = ctx.r4.u32;
        char extra[160];
        snprintf(extra, sizeof(extra), " pass %u tid %llu", pass, (unsigned long long)KernelCurrentTid());
        DiffDump("model", (1ull << 62) | (uint64_t(obj) << 8) | (pass & 0xFF), obj, 0x600, extra);
        // Its character instances: 928-byte records at *(obj+240), *(obj+248) of them; +704 a world position.
        uint32_t arr = GLoad32(obj + 240), n = GLoad32(obj + 248);
        if (arr && n < 256)
            for (uint32_t i = 0; i < std::min(n, 4u); ++i)
            {
                uint32_t r = arr + 928 * i;
                snprintf(extra, sizeof(extra), " pass %u #%u/%u pos %.3f %.3f %.3f %.3f", pass, i, n, GF(r + 704), GF(r + 708), GF(r + 712), GF(r + 716));
                DiffDump("inst", (4ull << 61) | (uint64_t(r) << 8) | (pass & 0xFF), r, 928, extra);
            }
    }
    __imp__sub_8293DBE8(ctx, base);
}
PPC_FUNC(sub_828DC1D8)
{
    if (g_detail.load(std::memory_order_relaxed) && DumpsOn())
    {
        uint32_t obj = ctx.r3.u32, ch = ctx.r4.u32;
        uint32_t rec = GLoad32(obj + 156) + 52 * ch;
        uint32_t c1 = GLoad32(rec + 28), c2 = GLoad32(rec + 32);
        char extra[200];
        snprintf(extra, sizeof(extra), " anim %08X ch %u r5 %08X r6 %08X r7 %08X tid %llu clip %08X t %.4f clip2 %08X t %.4f +36 %.4f +40 %.4f +44 %.4f",
                 obj, ch, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (unsigned long long)KernelCurrentTid(), c1, c1 ? GF(c1 + 16) : 0.0f, c2,
                 c2 ? GF(c2 + 16) : 0.0f, GF(rec + 36), GF(rec + 40), GF(rec + 44));
        DiffDump("animrec", (2ull << 62) | rec, rec, 52, extra);
        if (ch == 0) DiffDump("anim", (3ull << 62) | obj, obj, 0x100, "");
    }
    __imp__sub_828DC1D8(ctx, base);
}

void InterpTraceConstPointer(const PPCContext& ctx)
{
    // r3 device, r4 pixel, r5 start, r6 address, r7 count
    if (!g_detail.load(std::memory_order_relaxed) || ctx.r4.u32 != 0 || ctx.r5.u32 != 0 || ctx.r7.u32 < 64) return;
    static std::mutex mu;
    static std::unordered_map<uint64_t, uint32_t> seen;
    uint32_t chain[16];
    int n = 0;
    chain[n++] = uint32_t(ctx.lr);
    uint32_t sp = ctx.r1.u32;
    while (n < 16 && sp >= 0x1000 && sp < 0xF0000000u)
    {
        uint32_t up = GLoad32(sp);
        if (up <= sp || up >= 0xF0000000u) break;
        uint32_t ret = GLoad32(up - 8);
        if (ret < 0x82000000u || ret >= 0x83000000u) break;
        chain[n++] = ret;
        sp = up;
    }
    uint64_t h = Hash(chain, n);
    std::lock_guard<std::mutex> lk(mu);
    if (++seen[h] > 1 || seen.size() > 24) return;
    std::string s;
    char b[16];
    for (int i = 0; i < n; ++i) { snprintf(b, sizeof(b), " %08X", chain[i]); s += b; }
    fprintf(stderr, "[interp-d] palette load (count %u, address %08X, tid %llu) chain:%s\n", ctx.r7.u32, ctx.r6.u32,
            (unsigned long long)KernelCurrentTid(), s.c_str());
}

void InterpTraceDraw(uint64_t vsHash, uint64_t psHash, uint32_t indexBase, uint32_t numIndices,
                     const std::pair<uint32_t, uint32_t>* regs, size_t n)
{
    bool any = false;
    for (size_t i = 0; i < n && !any; ++i) any = regs[i].first >= 0x4000 && regs[i].first < 0x4400;
    if (!any) return;
    {
        // runs of consecutive registers -> float4 blocks
        std::vector<float> fv;
        uint32_t start = 0, next = 0;
        auto flush = [&]() {
            if (fv.size() >= 4) MatchConsts(fv.data(), uint32_t(fv.size() / 4), (start - 0x4000) / 4, 0, vsHash);
            fv.clear();
        };
        for (size_t i = 0; i < n; ++i)
        {
            uint32_t r = regs[i].first;
            if (r < 0x4000 || r >= 0x4400) continue;
            if (fv.empty() || r != next) { flush(); if ((r & 3) != 0) { next = 0; continue; } start = r; }
            uint32_t v = regs[i].second; float f; memcpy(&f, &v, 4);
            fv.push_back(f);
            next = r + 1;
        }
        flush();
    }
    ConstTrace& c = g_ct;
    uint64_t base = (vsHash * 0x9E3779B97F4A7C15ull) ^ (psHash * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(indexBase) << 20) ^ numIndices;
    uint32_t occ = c.occ[base]++;
    uint64_t id = (base ^ (uint64_t(occ) * 0x165667B19E3779F9ull)) << 12;
    uint64_t f = g_frame.load() + 1;
    for (size_t i = 0; i < n; ++i)
    {
        uint32_t r = regs[i].first;
        if (r < 0x4000 || r >= 0x4400) continue;
        ConstState& s = c.st[id | (r - 0x4000)];
        if (s.lastFrame == f) continue;
        if (s.lastFrame + 1 == f) c.touched.push_back({ &s, s.val != regs[i].second });
        s.val = regs[i].second;
        s.lastFrame = f;
        s.vs = vsHash;
        s.count = numIndices;
        s.reg = r;
        s.raw[0] = __builtin_bswap32(regs[i].second);
    }
}

extern "C" PPC_FUNC(__imp__sub_827467C0);
PPC_FUNC(sub_827467C0)
{
    if (InterpTraceOn())
    {
        g_ballEnt.store(ctx.r3.u32);
        fprintf(stderr, "[interp] ball entity at %08X (model name at %08X)\n", ctx.r3.u32, ctx.r4.u32);
    }
    __imp__sub_827467C0(ctx, base);
}
