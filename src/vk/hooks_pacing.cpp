// Frame pacing: how D3D paces the game to the vblank, and who reads the clock.
// Wrappers forward to the original guest functions unchanged unless a knob is set.
//
// D3D's present path (sub_82332ED8) queues a GPU-side callback to the flip
// scheduler sub_82332D40(arg): arg & 0xFFFFF000 = scanout address (0: the GPU
// waits for the flip), (arg >> 8) & 0xF = presentation interval (from
// device+13596: 0/1 -> 1, 2 -> 2, 4 -> 3, 0x80000000 -> 0 = immediately),
// arg & 0xFF = late threshold in percent of a vblank. The scheduler sets the
// target vblank to max(previous target + interval, now) and queues the flip;
// D3D's vblank handler (sub_82332C40, vblank count at device+16548) flips when
// the count reaches the target. The interval is a render state (setter
// sub_82325EE0, reached through D3D's state table).
//
// HC09_TRACE_PACING=1   every 5 s: flips, vblanks, swaps, the intervals asked for,
//                       vblanks between flips, and calls to the game's clock
//                       readers (QueryPerformanceCounter sub_828A91A8, raw mftb
//                       getters sub_82BF4F50 / sub_82EF8078, GetTickCount
//                       sub_828A91C8 = KeTimeStampBundle ms) by caller, and the
//                       game's idle step sub_82B35A60 (yield / sleep) by caller.
// HC09_PRESENT_INTERVAL=n   experiment: replace the interval the game asks for
//                       (0 = flip at once, 1 = every vblank, 2 = every second).
//
// The game's frame rate is its own: GameKernel::Loop (sub_82994848) runs four
// loop threads, each ticking when the time accumulated since its last tick
// reaches its period (1e9 / rate ns, set by sub_82994188; the clock is
// GetTickCount in ns, 1 ms steps; idle = sub_82B35A60, a yield). The step a
// tick gets (sub_829944B0 -> loop+132, seconds) depends on the mode (+56):
// 0 = fixed (the period), 1 = the time measured, 2 = measured, at most 3 periods.
//   LoopCore 60 Hz mode 0, LoopRender 35 Hz mode 1, LoopSimulation 30 Hz mode 0,
//   LoopSound 60 Hz mode 1.
// HC09_RENDER_HZ=n / HC09_SIM_HZ=n   experiment: replace LoopRender's 35 Hz /
//                       LoopSimulation's 30 Hz (the loops are told apart by rate).
//
// HC09_FRAMERATE=n (hc09.ini [display] framerate; unset or 35 = the game's own):
// the setting built on the above (docs/REPORT_60FPS.md). LoopRender runs at n Hz
// and the loops' clock (sub_829940C8, GetTickCount * 1e6) gets nanoseconds from
// the same time base instead of 1 ms steps. Above 35 D3D flips at once
// (interval 0; the host present waits for the display when vsync is on); at a
// rate that divides 60 (30, 20, 15) it flips every 60/n vblanks, an even cadence.
// HC09_RENDER_HZ / HC09_PRESENT_INTERVAL still win.
//
// HC09_IDLE_SLEEP_US=n (experiment; default 0 = the game's own yield): when a GameKernel loop is
// not due (sub_82994848 calling the idle step at 0x82994924) and the step would only yield,
// sleep n microseconds instead (high-resolution timer). On the console each loop owns a
// hardware thread and a yield costs nothing; here the four loops polled ~1.5 M times/s each
// (lock, clock, unlock, yield), keeping cores busy beside the render thread
// (docs/REPORT_GAME_CPU.md). A loop's tick is then late by at most ~n us plus timer slack.
// Not the default: with any sleep here (200 us, or until due) the game stalls now and then for
// good while it keeps drawing and playing sound: a third of the boots never left the splash screen
// (its loading stops), and in games the play stopped after a play call. With the yield, none did.
#include "dev_env.h"
#include "kernel.h"
#include "game_addr.h"
#include "gpu.h"
#include "interp.h"
#include "vk_select.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

extern "C" PPC_FUNC(__imp__sub_82332D40);
extern "C" PPC_FUNC(__imp__sub_82325EE0);
extern "C" PPC_FUNC(__imp__sub_828A91A8);
extern "C" PPC_FUNC(__imp__sub_82BF4F50);
extern "C" PPC_FUNC(__imp__sub_82EF8078);
extern "C" PPC_FUNC(__imp__sub_828A91C8);
extern "C" PPC_FUNC(__imp__sub_82B35A60);

// Sleep `us` microseconds on a per-thread high-resolution timer (Sleep() rounds to 1 ms).
static void IdleSleep(uint32_t us)
{
#if defined(_WIN32)
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(us) * 10;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) return;
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}
extern "C" PPC_FUNC(__imp__sub_82994188);
extern "C" PPC_FUNC(__imp__sub_829944B0);
extern "C" PPC_FUNC(__imp__sub_826811E8);

void PacingReportD3DWaits(double dt);
void PacingReportStep35(double dt);

namespace {

bool PacingOn() { static const bool on = Hc09Env("HC09_TRACE_PACING") != nullptr; return on; }

// HC09_FRAMERATE: 0 = the game's own pacing (unset, invalid or 35). Only with the Vulkan renderer here (the D3D12
// renderer keeps the game's own pacing).
int FrameRate()
{
    static const int v = [] {
        const char* e = hc09::vk::Selected() ? Hc09Env("HC09_FRAMERATE") : nullptr;
        int n = e && *e ? atoi(e) : 0;
        if (n == 35) return 0;
        if (n && (n < 10 || n > 240))
        {
            fprintf(stderr, "[pacing] HC09_FRAMERATE=%s: not 10..240, ignored\n", e);
            return 0;
        }
        if (n)
            fprintf(stderr, "[pacing] frame rate %d (game's own: 35): render loop %d Hz, %s, loop clock in ns\n", n, n,
                    n > 35 ? "D3D flips at once" : 60 % n == 0 ? "D3D flips at an even vblank cadence" : "D3D flips at vblanks");
        return n;
    }();
    return v;
}

int IntervalOverride()
{
    static const int v = [] {
        if (const char* e = Hc09Env("HC09_PRESENT_INTERVAL")) return atoi(e);
        int f = FrameRate();
        if (f > 35) return 0;
        if (f && 60 % f == 0) return std::min(60 / f, 15);
        return -1;
    }();
    return v;
}

uint32_t Device()
{
    uint32_t p = GLoad32(GameAddr(0x82000A34));
    return p ? GLoad32(p) : 0;
}

std::mutex g_mu;
std::map<uint32_t, uint64_t> g_asked;     // interval asked -> count
std::map<uint32_t, uint64_t> g_spacing;   // target - previous target, in vblanks
std::map<uint32_t, uint64_t> g_clock;     // (which << 30 | lr) -> calls
uint64_t g_flips = 0, g_immediate = 0;

// Idle calls (sub_82B35A60: yield or sleep, the game's spin-wait step) by call
// site: one counter per instruction of the code range, so the hot path is one
// relaxed atomic add.
constexpr uint32_t kCodeBase = PPC_CODE_BASE, kCodeWords = PPC_CODE_SIZE / 4 + 1;
std::atomic<uint32_t>* g_idle = nullptr;

struct LoopStat { uint64_t ticks = 0, updates = 0; double dt = 0, dtMin = 1e9, dtMax = 0, busy = 0, busyMax = 0; };
std::map<uint32_t, LoopStat> g_loops;     // loop object -> ticks since the last report

void Reporter()
{
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    uint64_t sw0 = GpuSwapCount();
    uint32_t vb0 = 0;
    for (;;)
    {
        std::this_thread::sleep_for(seconds(5));
        auto t1 = steady_clock::now();
        double dt = duration<double>(t1 - t0).count();
        t0 = t1;
        uint32_t dev = Device();
        uint32_t vb = dev ? GLoad32(dev + 16548) : 0;
        uint64_t sw = GpuSwapCount();
        std::map<uint32_t, uint64_t> asked, spacing, clk;
        uint64_t flips, imm;
        {
            std::lock_guard<std::mutex> l(g_mu);
            asked.swap(g_asked); spacing.swap(g_spacing); clk.swap(g_clock);
            flips = g_flips; imm = g_immediate; g_flips = g_immediate = 0;
        }
        fprintf(stderr, "[pacing] sw%llu %.1f s: %.1f flips/s, %.1f vblanks/s, %.1f swaps/s; %llu flipped at once;"
                " interval now %u, vblank cb %08X, swap cb %08X, flip-done word %08X\n",
                (unsigned long long)sw, dt, flips / dt, (vb - vb0) / dt, (sw - sw0) / dt, (unsigned long long)imm,
                dev ? GLoad32(dev + 13596) : 0, dev ? GLoad32(dev + 16544) : 0, dev ? GLoad32(dev + 16540) : 0,
                dev && GLoad32(dev + 10900) ? GLoad32(dev + 10900) + 4 : 0);
        vb0 = vb; sw0 = sw;
        std::string s;
        char b[64];
        for (auto& [k, v] : asked) { snprintf(b, sizeof(b), " %u:%llu", k, (unsigned long long)v); s += b; }
        fprintf(stderr, "[pacing]   interval asked (value:count)%s\n", s.c_str());
        s.clear();
        for (auto& [k, v] : spacing) { snprintf(b, sizeof(b), " %u:%llu", k, (unsigned long long)v); s += b; }
        fprintf(stderr, "[pacing]   vblanks between flips%s\n", s.c_str());
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto& [k, v] : clk) top.push_back({ v, k });
        std::sort(top.rbegin(), top.rend());
        if (g_idle)
        {
            std::vector<std::pair<uint32_t, uint32_t>> idle;
            for (uint32_t i = 0; i < kCodeWords; ++i)
                if (uint32_t c = g_idle[i].exchange(0, std::memory_order_relaxed)) idle.push_back({ c, kCodeBase + i * 4 });
            std::sort(idle.rbegin(), idle.rend());
            for (size_t i = 0; i < idle.size() && i < 12; ++i)
                fprintf(stderr, "[pacing]   idle (sub_82B35A60) lr=%08X %.1f/s\n", idle[i].second, idle[i].first / dt);
        }
        std::map<uint32_t, LoopStat> loops;
        { std::lock_guard<std::mutex> l(g_mu); loops.swap(g_loops); }
        for (auto& [obj, st] : loops)
        {
            float rate; uint32_t r = GLoad32(obj + 64); memcpy(&rate, &r, 4);
            fprintf(stderr, "[pacing]   loop %08X (%.0f Hz mode %u): %.1f ticks/s, step mean %.2f ms (%.2f..%.2f), period %.2f ms; "
                            "update %.2f ms/tick (max %.2f)\n",
                    obj, rate, GLoad32(obj + 56), st.ticks / dt, 1000.0 * st.dt / std::max<uint64_t>(st.ticks, 1),
                    1000.0 * st.dtMin, 1000.0 * st.dtMax, double(GLoad64(obj + 72)) * 1e-6,
                    1000.0 * st.busy / std::max<uint64_t>(st.updates, 1), 1000.0 * st.busyMax);
        }
        PacingReportD3DWaits(dt);
        PacingReportStep35(dt);
        static const char* kName[] = { "QPC", "tb82BF4F50", "tb82EF8078", "ticks" };
        for (size_t i = 0; i < top.size() && i < 40; ++i)
            fprintf(stderr, "[pacing]   clock %-10s lr=%08X %.1f/s\n", kName[top[i].second >> 30],
                    0x80000000u | (top[i].second & 0x3FFFFFFFu), top[i].first / dt);
    }
}

void Start()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_idle = new std::atomic<uint32_t>[kCodeWords]();
        std::thread(Reporter).detach();
    });
}

void ClockCall(uint32_t which, PPCContext& ctx)
{
    if (!PacingOn()) return;
    Start();
    std::lock_guard<std::mutex> l(g_mu);
    ++g_clock[(which << 30) | (uint32_t(ctx.lr) & 0x3FFFFFFFu)];
}

}  // namespace

// Flip scheduler (runs on the CP's interrupt thread, from a GPU callback).
PPC_FUNC(sub_82332D40)
{
    int ov = IntervalOverride();
    if (ov >= 0)
        ctx.r3.u64 = (ctx.r3.u32 & ~0xF00u) | (uint32_t(ov & 0xF) << 8);
    if (!PacingOn()) { __imp__sub_82332D40(ctx, base); return; }
    Start();
    uint32_t arg = ctx.r3.u32, dev = Device();
    uint32_t prev = dev ? GLoad32(dev + 16556) : 0;
    __imp__sub_82332D40(ctx, base);
    uint32_t target = dev ? GLoad32(dev + 16556) : 0, now = dev ? GLoad32(dev + 16548) : 0;
    std::lock_guard<std::mutex> l(g_mu);
    ++g_asked[(arg >> 8) & 0xF];
    ++g_spacing[std::min(target - prev, 99u)];
    ++g_flips;
    if (target == now) ++g_immediate;
}

// SetRenderState(D3DRS_PRESENTINTERVAL): device+13596 = value.
PPC_FUNC(sub_82325EE0)
{
    if (PacingOn())
    {
        static std::atomic<int> logged{ 0 };
        uint32_t old = GLoad32(ctx.r3.u32 + 13596);
        if (old != ctx.r4.u32 && logged.fetch_add(1) < 200)
            fprintf(stderr, "[pacing] present interval %08X -> %08X (sw%llu t%llu lr=%08X)\n", old, ctx.r4.u32,
                    (unsigned long long)GpuSwapCount(), (unsigned long long)KernelCurrentTid(), uint32_t(ctx.lr));
    }
    __imp__sub_82325EE0(ctx, base);
}

PPC_FUNC(sub_828A91A8) { ClockCall(0, ctx); __imp__sub_828A91A8(ctx, base); }
PPC_FUNC(sub_82BF4F50) { ClockCall(1, ctx); __imp__sub_82BF4F50(ctx, base); }
PPC_FUNC(sub_82EF8078) { ClockCall(2, ctx); __imp__sub_82EF8078(ctx, base); }
PPC_FUNC(sub_828A91C8) { ClockCall(3, ctx); __imp__sub_828A91C8(ctx, base); }
// The GameKernel loop running on this thread: GameKernel::Loop (sub_82994848) takes the loop's lock
// (sub_82B35BA8(loop + 8, ...), returning to 0x829948A8) at the top of every iteration. Its idle step below then
// knows when the loop is next due: loop+72 is the period, loop+104 + loop+112 the time accumulated toward it (ns of
// the loops' clock, sub_829940C8).
static thread_local uint32_t t_loop = 0;
extern "C" PPC_FUNC(__imp__sub_82B35BA8);
PPC_FUNC(sub_82B35BA8)
{
    if (uint32_t(ctx.lr) == GameAddr(0x829948A8)) t_loop = ctx.r3.u32 - 8;
    __imp__sub_82B35BA8(ctx, base);
}

double TimeWarp();

PPC_FUNC(sub_82B35A60)
{
    if (PacingOn())
    {
        Start();
        uint32_t i = (uint32_t(ctx.lr) - kCodeBase) / 4;
        if (i < kCodeWords) g_idle[i].fetch_add(1, std::memory_order_relaxed);
    }
    // sub_82B35A60(obj): *obj == 0 -> yield (sub_82DE3DB0, NtYieldExecution); else wait on it.
    // HC09_IDLE_SLEEP_US (default 0 = the game's yield; see the top of the file): the step of the idle wait.
    // HC09_IDLE_UNTIL_DUE (default on): a loop that is not due sleeps until 2 ms before its next tick (at most 20 ms
    // at a time), then in idle steps: ~10 wake-ups per tick instead of one every 200 us (CPU at 60 fps -45%,
    // docs/REPORT_LOWEND.md). HC09_IDLE_SLACK_US (2000): that margin; 500 woke too late too often (more frames
    // missed a refresh).
    static const uint32_t sleepUs = [] {
        const char* e = Hc09Env("HC09_IDLE_SLEEP_US");
        return e && *e ? uint32_t(strtoul(e, nullptr, 10)) : 0u;
    }();
    static const bool untilDue = [] {
        const char* e = Hc09Env("HC09_IDLE_UNTIL_DUE");
        return !(e && *e == '0');
    }();
    static const double slackUs = [] {   // HC09_IDLE_SLACK_US: how early before the tick the long sleep ends
        const char* e = Hc09Env("HC09_IDLE_SLACK_US");
        return e && *e ? atof(e) : 2000.0;
    }();
    if (sleepUs && uint32_t(ctx.lr) == GameAddr(0x82994928) && GLoad32(ctx.r3.u32) == 0)
    {
        uint32_t us = sleepUs;
        if (untilDue && t_loop)
        {
            const int64_t period = int64_t(GLoad64(t_loop + 72));
            const int64_t acc = int64_t(GLoad64(t_loop + 104) + GLoad64(t_loop + 112));
            const double warp = TimeWarp();
            if (period > 0 && acc < period && warp > 0)
            {
                const double left = double(period - acc) / 1000.0 / warp - slackUs;   // us, less the timer's slack
                if (left > double(sleepUs)) us = uint32_t(left < 20000.0 ? left : 20000.0);
            }
        }
        IdleSleep(us);
        ctx.r3.u64 = 1;   // as the yield's "yielded"; the loop ignores it
        return;
    }
    __imp__sub_82B35A60(ctx, base);
}

// The simulation and render loops (by the rate they ask for), for interp.cpp.
std::atomic<uint32_t> g_simLoop{ 0 }, g_renderLoop{ 0 }, g_loop60a{ 0 }, g_loop60b{ 0 };

// SetRate(loop r3, Hz f1): period = 1e9 / rate + 0.5 ns.
PPC_FUNC(sub_82994188)
{
    static const double renderHz = Hc09Env("HC09_RENDER_HZ") ? atof(Hc09Env("HC09_RENDER_HZ")) : FrameRate();
    static const double simHz = Hc09Env("HC09_SIM_HZ") ? atof(Hc09Env("HC09_SIM_HZ")) : 0;
    double asked = ctx.f1.f64;
    if (renderHz > 0 && float(asked) == 35.0f) ctx.f1.f64 = renderHz;
    if (simHz > 0 && float(asked) == 30.0f) ctx.f1.f64 = simHz;
    if (float(asked) == 30.0f) g_simLoop.store(ctx.r3.u32);
    if (float(asked) == 35.0f) g_renderLoop.store(ctx.r3.u32);
    if (float(asked) == 60.0f)
    {
        uint32_t none = 0;
        if (!g_loop60a.compare_exchange_strong(none, ctx.r3.u32) && g_loop60a.load() != ctx.r3.u32) g_loop60b.store(ctx.r3.u32);
    }
    if (PacingOn() || ctx.f1.f64 != asked)
        fprintf(stderr, "[pacing] loop %08X rate %.1f Hz%s (lr=%08X)\n", ctx.r3.u32, ctx.f1.f64,
                ctx.f1.f64 != asked ? " (frame-rate setting)" : "", uint32_t(ctx.lr));
    __imp__sub_82994188(ctx, base);
}

// The loops' clock: (GetTickCount() + 1) * 1e6 ns. With a frame-rate setting it
// returns the same time base in nanoseconds, so loop periods are not rounded to
// whole milliseconds (16.67 ms frames would otherwise alternate 16 and 17 ms).
uint64_t KernelInterruptTimeNs();   // kernel_vars.cpp
extern "C" PPC_FUNC(__imp__sub_829940C8);
// HC09_TIME_WARP=k (diagnostic, for unattended measurement runs): the loops' clock runs k times as fast,
// so every GameKernel loop ticks k times as often. The simulation keeps its fixed 1/30 s step (mode 0), so
// on-field play is the same play, k times faster in real time, as long as the host keeps up.
// HC09_TIME_WARP_AFTER=s: start warping only s seconds after the first clock read (default 0), so a
// scripted HC09_INPUT through the menus keeps its timing.
// SetTimeWarp(k) changes the rate at run time (the live tweak server's slow motion / fast forward); the
// clock stays continuous. HC09_TWEAK also routes the clock through here so the rate can change later.
namespace {
struct Warp {
    std::mutex mu;
    bool init = false, on = false;
    double k = 1;
    uint64_t realAt = 0, warpAt = 0;   // from realAt on: warped = warpAt + (real - realAt) * k
} g_warp;
uint64_t WarpedLocked(uint64_t ns)
{
    if (!g_warp.on) return ns;
    if (ns <= g_warp.realAt) return g_warp.warpAt - (g_warp.realAt - ns);   // before the change: rate 1
    return g_warp.warpAt + uint64_t(double(ns - g_warp.realAt) * g_warp.k);
}
}  // namespace

void SetTimeWarp(double k)
{
    std::lock_guard<std::mutex> lk(g_warp.mu);
    const uint64_t now = KernelInterruptTimeNs();
    const uint64_t w = WarpedLocked(now);
    g_warp.realAt = now; g_warp.warpAt = w; g_warp.k = k < 0 ? 0 : k; g_warp.on = true;
    fprintf(stderr, "[pacing] time warp x%.2f from now\n", g_warp.k);
}
double TimeWarp() { std::lock_guard<std::mutex> lk(g_warp.mu); return g_warp.on ? g_warp.k : 1.0; }

PPC_FUNC(sub_829940C8)
{
    static const double warp = Hc09Env("HC09_TIME_WARP") ? atof(Hc09Env("HC09_TIME_WARP")) : 0;
    static const double after = Hc09Env("HC09_TIME_WARP_AFTER") ? atof(Hc09Env("HC09_TIME_WARP_AFTER")) : 0;
    static const bool tweak = Hc09Env("HC09_TWEAK") != nullptr;
    if (!FrameRate() && warp <= 0 && !tweak) { __imp__sub_829940C8(ctx, base); return; }
    const uint64_t now = KernelInterruptTimeNs();
    uint64_t ns;
    {
        std::lock_guard<std::mutex> lk(g_warp.mu);
        if (!g_warp.init) {
            g_warp.init = true;
            if (warp > 0) {   // starts at first clock read + after
                g_warp.realAt = g_warp.warpAt = now + uint64_t(after * 1e9);
                g_warp.k = warp; g_warp.on = true;
            }
        }
        ns = WarpedLocked(now);
    }
    ctx.r3.u64 = ns + 1000000ull;
}

// A loop's tick: computes the step it hands its update (loop+132, seconds).
PPC_FUNC(sub_829944B0)
{
    uint32_t obj = ctx.r3.u32;
    __imp__sub_829944B0(ctx, base);
    if (!PacingOn()) return;
    Start();
    float step; uint32_t r = GLoad32(obj + 132); memcpy(&step, &r, 4);
    std::lock_guard<std::mutex> l(g_mu);
    LoopStat& st = g_loops[obj];
    ++st.ticks; st.dt += step; st.dtMin = std::min<double>(st.dtMin, step); st.dtMax = std::max<double>(st.dtMax, step);
}

// A loop's update (loop r3, step f1): the game's own work per tick, timed.
PPC_FUNC(sub_826811E8)
{
    uint32_t obj = ctx.r3.u32;
    int il = !InterpLoopsWanted() ? -1 : obj == g_simLoop.load(std::memory_order_relaxed) ? 0
           : obj == g_renderLoop.load(std::memory_order_relaxed) ? 1 : obj == g_loop60a.load(std::memory_order_relaxed) ? 2
           : obj == g_loop60b.load(std::memory_order_relaxed) ? 3 : -1;
    if (il >= 0) InterpLoopUpdate(il, true);
    if (!PacingOn())
    {
        __imp__sub_826811E8(ctx, base);
        if (il >= 0) InterpLoopUpdate(il, false);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_826811E8(ctx, base);
    if (il >= 0) InterpLoopUpdate(il, false);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> l(g_mu);
    LoopStat& st = g_loops[obj];
    ++st.updates; st.busy += sec; st.busyMax = std::max(st.busyMax, sec);
}

// D3D's CPU-side waits for the GPU, timed (HC09_TRACE_PACING): ring space
// (sub_823288C8, wrapped in hooks_diag.cpp, which calls PacingD3DWait) and the
// two spin waits that poll a GPU-written value (sub_823277B0, sub_82327DC8).
namespace {
const char* const kD3DWait[] = { "ring space 823288C8", "spin 823277B0", "spin 82327DC8" };
double g_d3dWait[3];
uint64_t g_d3dCalls[3];
}
void PacingD3DWait(int which, double sec)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_d3dWait[which] += sec;
    ++g_d3dCalls[which];
}
bool PacingTraceOn() { return PacingOn(); }
void PacingReportD3DWaits(double dt)
{
    double w[3]; uint64_t c[3];
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (int i = 0; i < 3; ++i) { w[i] = g_d3dWait[i]; c[i] = g_d3dCalls[i]; g_d3dWait[i] = 0; g_d3dCalls[i] = 0; }
    }
    for (int i = 0; i < 3; ++i)
        if (c[i]) fprintf(stderr, "[pacing]   D3D wait %-20s %.1f ms/s, %.1f calls/s\n", kD3DWait[i], 1000.0 * w[i] / dt, c[i] / dt);
}

extern "C" PPC_FUNC(__imp__sub_823277B0);
extern "C" PPC_FUNC(__imp__sub_82327DC8);
PPC_FUNC(sub_823277B0)
{
    if (!PacingOn()) { __imp__sub_823277B0(ctx, base); return; }
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_823277B0(ctx, base);
    PacingD3DWait(1, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}
PPC_FUNC(sub_82327DC8)
{
    if (!PacingOn()) { __imp__sub_82327DC8(ctx, base); return; }
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82327DC8(ctx, base);
    PacingD3DWait(2, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

// Functions that use a hard-coded 1/35 s (0x3CEA0EA1 at 0x82018644): counted per
// thread (HC09_TRACE_PACING), to see whether they step once per render frame.
namespace {
constexpr uint32_t kStep35[] = { GameAddr(0x828F8248), GameAddr(0x824435A0), GameAddr(0x82455CF8), GameAddr(0x82468970),
                                GameAddr(0x824BB390) };
std::map<std::pair<int, uint64_t>, uint64_t> g_step35;   // (function, tid) -> calls
void Step35(int i)
{
    if (!PacingOn()) return;
    std::lock_guard<std::mutex> l(g_mu);
    ++g_step35[{ i, KernelCurrentTid() }];
}
}
void PacingReportStep35(double dt)
{
    std::map<std::pair<int, uint64_t>, uint64_t> m;
    { std::lock_guard<std::mutex> l(g_mu); m.swap(g_step35); }
    for (auto& [k, v] : m)
        fprintf(stderr, "[pacing]   1/35 user sub_%08X t%llu %.1f calls/s\n", kStep35[k.first], (unsigned long long)k.second, v / dt);
}
extern "C" PPC_FUNC(__imp__sub_828F8248);
extern "C" PPC_FUNC(__imp__sub_824435A0);
extern "C" PPC_FUNC(__imp__sub_82455CF8);
extern "C" PPC_FUNC(__imp__sub_82468970);
extern "C" PPC_FUNC(__imp__sub_824BB390);
PPC_FUNC(sub_828F8248) { Step35(0); __imp__sub_828F8248(ctx, base); }
PPC_FUNC(sub_824435A0) { Step35(1); __imp__sub_824435A0(ctx, base); }
PPC_FUNC(sub_82455CF8) { Step35(2); __imp__sub_82455CF8(ctx, base); }
PPC_FUNC(sub_82468970) { Step35(3); __imp__sub_82468970(ctx, base); }
PPC_FUNC(sub_824BB390) { Step35(4); __imp__sub_824BB390(ctx, base); }
