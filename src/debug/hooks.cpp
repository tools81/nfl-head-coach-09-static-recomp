// Debug hooks on recompiled functions.
//
// XenonRecomp emits every function as __imp__sub_<address> with a weak alias sub_<address>;
// defining sub_<address> here overrides the alias for every caller (direct and indirect)
// while the original stays reachable through __imp__sub_<address>.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "gpu/command_processor.h"
#include "kernel/format.h"
#include "kernel/guest.h"
#include "kernel/threads.h"

extern "C" PPC_FUNC(__imp__sub_82D357D0);
extern "C" PPC_FUNC(__imp__sub_823288C8);
extern "C" PPC_FUNC(__imp__sub_82323A30);

using namespace hc09;

// Video decode thread procedure: loops while frame counter (+0x3C) < limit (+0x40).
PPC_FUNC(sub_82D357D0)
{
    const uint32_t self = ctx.r3.u32;
    std::fprintf(stderr, "[hook] video decode thread object 0x%08X: +0x3C=%d +0x40=%d\n", self,
        Load<int32_t>(self + 0x3C), Load<int32_t>(self + 0x40));
    __imp__sub_82D357D0(ctx, base);
    std::fprintf(stderr, "[hook] video decode thread done: +0x3C=%d +0x40=%d, returned 0x%X\n",
        Load<int32_t>(self + 0x3C), Load<int32_t>(self + 0x40), ctx.r3.u32);
}

// D3D fence wait (device, target, flags): loops on sub_82323A30 while the GPU-written fence
// value at *(device + 10896) has not passed `target` relative to the latest issued fence
// (device + 10908). Reports a wait every second while it lasts.
namespace
{
    // HC09_TRACE_D3D=1 enables the D3D fence-wait and kickoff diagnostics below.
    const bool g_traceD3d = std::getenv("HC09_TRACE_D3D") != nullptr;
    thread_local uint32_t t_waitDevice = 0, t_waitTarget = 0;
    thread_local std::chrono::steady_clock::time_point t_waitStart, t_waitReported;
}

PPC_FUNC(sub_823288C8)
{
    t_waitDevice = ctx.r3.u32;
    t_waitTarget = ctx.r4.u32;
    t_waitStart = t_waitReported = std::chrono::steady_clock::now();
    __imp__sub_823288C8(ctx, base);
    t_waitDevice = 0;
}

PPC_FUNC(sub_82323A30)
{
    __imp__sub_82323A30(ctx, base);
    if (!g_traceD3d || t_waitDevice == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - t_waitReported < std::chrono::seconds(1))
        return;
    t_waitReported = now;
    const uint32_t device = t_waitDevice, fencePtr = Load<uint32_t>(device + 10896);
    std::fprintf(stderr, "[hook] fence wait %.0f s (thread 0x%X): device 0x%08X target 0x%08X issued 0x%08X completed@0x%08X = 0x%08X\n",
        std::chrono::duration<double>(now - t_waitStart).count(), CurrentThreadId(), device, t_waitTarget, Load<uint32_t>(device + 10908),
        fencePtr, Load<uint32_t>(fencePtr));
    // The two D3D worker contexts (threads 0x130 and 0x134): queue indices, entries, event.
    for (uint32_t worker : { 0xAA7FD4DCu, 0xAA7FD52Cu })
        std::fprintf(stderr, "    worker 0x%08X idx %u queue consumed %u produced %u [%08X %08X %08X %08X] event state %d%c", worker,
            Load<uint32_t>(worker + 4), Load<uint32_t>(worker + 60), Load<uint32_t>(worker + 56), Load<uint32_t>(worker + 64),
            Load<uint32_t>(worker + 68), Load<uint32_t>(worker + 72), Load<uint32_t>(worker + 76), Load<int32_t>(worker + 32 + 4), 10);
}

// D3D KickOff (device): publishes the device's ring write position to CP_RB_WPTR. Logs any
// call that moves the write pointer backwards.
extern "C" PPC_FUNC(__imp__sub_82327F80);

PPC_FUNC(sub_82327F80)
{
    // Detects two threads inside KickOff at once (the write pointer can then go backwards).
    static std::atomic<uint64_t> inside{ 0 };  // thread id << 32 | caller of the current kicker
    const uint32_t device = ctx.r3.u32, caller = uint32_t(ctx.lr);
    const uint64_t self = (uint64_t(CurrentThreadId()) << 32) | caller;
    const uint64_t other = inside.exchange(self);
    if (g_traceD3d && other != 0)
    {
        static std::atomic<int> reports{ 0 };
        if (reports++ < 20)
            std::fprintf(stderr, "[hook] concurrent kickoff: thread 0x%X (caller 0x%08X) while thread 0x%X (caller 0x%08X) is inside, device 0x%08X\n",
                CurrentThreadId(), caller, uint32_t(other >> 32), uint32_t(other), device);
    }
    const uint32_t before = gpu::ReadRegister(0x1C5);
    __imp__sub_82327F80(ctx, base);
    inside.store(0);
    // HC09_TRACE_KICKS=<seconds>: log 300 kicks (thread, caller, write pointer) from then on.
    static const int traceKicksAfter = std::getenv("HC09_TRACE_KICKS") ? std::atoi(std::getenv("HC09_TRACE_KICKS")) : -1;
    static const auto start = std::chrono::steady_clock::now();
    static std::atomic<int> kicksTraced{ 0 };
    if (((traceKicksAfter >= 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(traceKicksAfter)) ||
            gpu::FenceTraceActive()) && kicksTraced++ < 600)
        std::fprintf(stderr, "[kick] thread 0x%X caller 0x%08X wptr %u -> %u%c", CurrentThreadId(), caller, before,
            gpu::ReadRegister(0x1C5), 10);
}

// D3D submit routine (device, list, flags), run by the render thread and, on its 30 ms
// wait timeout, by the D3D worker thread.
extern "C" PPC_FUNC(__imp__sub_82333410);

PPC_FUNC(sub_82333410)
{
    static const int traceKicksAfter = std::getenv("HC09_TRACE_KICKS") ? std::atoi(std::getenv("HC09_TRACE_KICKS")) : -1;
    static const auto start = std::chrono::steady_clock::now();
    static std::atomic<int> traced{ 0 };
    const bool trace = ((traceKicksAfter >= 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(traceKicksAfter)) ||
        gpu::FenceTraceActive()) && traced++ < 300;
    const uint32_t thread = CurrentThreadId(), list = ctx.r4.u32, flags = ctx.r5.u32, caller = uint32_t(ctx.lr);
    if (trace)
        std::fprintf(stderr, "[submit] enter thread 0x%X caller 0x%08X list 0x%08X flags 0x%X%c", thread, caller, list, flags, 10);
    __imp__sub_82333410(ctx, base);
    if (trace)
        std::fprintf(stderr, "[submit] leave thread 0x%X%c", thread, 10);
}


// D3D worker token dispatcher (token_ptr, shared_state): logs the token addresses it runs
// while the fence trace is active.
extern "C" PPC_FUNC(__imp__sub_82339E20);

PPC_FUNC(sub_82339E20)
{
    static std::atomic<int> traced{ 0 };
    const uint32_t token = ctx.r3.u32;
    if (gpu::FenceTraceActive() && traced++ < 300)
        std::fprintf(stderr, "[token] thread 0x%X at 0x%08X: %08X %08X %08X%c", CurrentThreadId(), token,
            Load<uint32_t>(token), Load<uint32_t>(token + 4), Load<uint32_t>(token + 8), 10);
    __imp__sub_82339E20(ctx, base);
}

// D3D worker segment processing (worker): logs its queue and the shared barrier state.
extern "C" PPC_FUNC(__imp__sub_8233A330);

PPC_FUNC(sub_8233A330)
{
    static std::atomic<int> traced{ 0 };
    const uint32_t worker = ctx.r3.u32, shared = Load<uint32_t>(worker);
    const bool trace = gpu::FenceTraceActive() && traced++ < 200;
    auto dump = [&](const char* when) {
        std::fprintf(stderr, "[worker] %s thread 0x%X idx %u queue %u/%u [%08X %08X %08X %08X] shared lock %u mask %08X active %d "
            "+64 %d +72 %08X cur %08X arrivals %d flag96 %d%c", when, CurrentThreadId(), Load<uint32_t>(worker + 4),
            Load<uint32_t>(worker + 60), Load<uint32_t>(worker + 56), Load<uint32_t>(worker + 64), Load<uint32_t>(worker + 68),
            Load<uint32_t>(worker + 72), Load<uint32_t>(worker + 76), Load<uint32_t>(shared), Load<uint32_t>(shared + 56),
            Load<int32_t>(shared + 60), Load<int32_t>(shared + 64), Load<uint32_t>(shared + 72), Load<uint32_t>(shared + 36),
            Load<int32_t>(shared + 100), Load<int32_t>(shared + 96), 10);
    };
    if (trace)
        dump("enter");
    __imp__sub_8233A330(ctx, base);
    if (trace)
        dump("leave");
}

// D3D worker end-of-stream / jump handling (shared, worker, token_ptr) -> next token or 0.
extern "C" PPC_FUNC(__imp__sub_8233A188);

PPC_FUNC(sub_8233A188)
{
    static std::atomic<int> traced{ 0 };
    const uint32_t shared = ctx.r3.u32, worker = ctx.r4.u32, token = ctx.r5.u32;
    const uint32_t value = Load<uint32_t>(token), activeBefore = Load<uint32_t>(shared + 60);
    __imp__sub_8233A188(ctx, base);
    if (gpu::FenceTraceActive() && traced++ < 200)
        std::fprintf(stderr, "[advance] thread 0x%X token@0x%08X = %08X -> 0x%08X, active %d -> %d, mask %08X%c", CurrentThreadId(),
            token, value, ctx.r3.u32, activeBefore, Load<int32_t>(shared + 60), Load<uint32_t>(shared + 56), 10);
    (void)worker;
}

// The engine's debug print (format, ...), compiled out in retail: the linker folded it into an empty
// function shared with other empty functions, so only calls whose first argument is printable text in
// .rdata are treated as prints. HC09_ENGINE_LOG=1 prints them as "[engine] ...", e.g.
// "GameRenderAsset::SyncResolveRnaTexture: Asset missing, using default: <path>" for each texture a
// material references that no loaded package provides (such textures render with Missing.dds, a flat
// grey 32x32 texture).
PPC_FUNC(sub_8279F208)
{
    static const bool enabled = std::getenv("HC09_ENGINE_LOG") != nullptr;
    if (!enabled)
        return;
    constexpr uint32_t kRdataBegin = 0x82000600, kRdataEnd = 0x822D0B8C;
    const uint32_t format = ctx.r3.u32;
    if (format < kRdataBegin || format >= kRdataEnd)
        return;
    uint32_t length = 0;
    for (; length < 512 && format + length < kRdataEnd; length++)
    {
        const uint8_t c = Load<uint8_t>(format + length);
        if (c == 0)
            break;
        if ((c < 0x20 && c != '\n' && c != '\t') || c >= 0x7F)
            return;
    }
    // Messages end in a newline or carry a format specifier; bare names (profiler markers such as
    // "FSGameRenderIG::Process" reach the same folded function every frame) are skipped.
    const char* pattern = GuestCString(format);
    if (length < 4 || length == 512 || (pattern[length - 1] != '\n' && !std::memchr(pattern, '%', length)))
        return;
    const std::string text = FormatGuestString(pattern, GuestArgs::FromRegisters(ctx, 1));
    std::fprintf(stderr, "[engine] %s%s", text.c_str(), !text.empty() && text.back() == '\n' ? "" : "\n");
}

// The database query call (result, database, flags, query, arguments...): the game reads and writes
// its tables with SQL-like text such as "update 'YALP' set 'NIWP' = <arg>", table and field names
// four reversed characters, bytes 0x80-0x8F marking where an argument goes.
// HC09_TRACE_SQL=INJY,PIRE (names as the game spells them) logs each query whose text names one of
// them, decoded, with its first four arguments (and the word each one points to after the call, for
// arguments that return values) and the result code.
extern "C" PPC_FUNC(__imp__sub_82C66928);

PPC_FUNC(sub_82C66928)
{
    static const char* const filter = std::getenv("HC09_TRACE_SQL");
    if (!filter)
    {
        __imp__sub_82C66928(ctx, base);
        return;
    }
    const uint32_t query = ctx.r6.u32;
    const uint32_t args[4] = { ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    std::string text;
    for (uint32_t i = 0; i < 400; i++)
    {
        const uint8_t c = query ? Load<uint8_t>(query + i) : 0;
        if (c == 0)
            break;
        text.push_back(c >= 0x80 && c < 0x90 ? '?' : char(c));
    }
    // 'YJNI' -> INJY
    for (size_t at = 0; (at = text.find('\'', at)) != std::string::npos && at + 5 < text.size();)
    {
        if (text[at + 5] == '\'')
        {
            std::reverse(text.begin() + long(at) + 1, text.begin() + long(at) + 5);
            text.erase(at + 5, 1);
            text.erase(at, 1);
            at += 4;
        }
        else
        {
            at++;
        }
    }
    bool wanted = false;
    for (const char* token = filter; *token && !wanted;)
    {
        const char* end = std::strchr(token, ',');
        const size_t length = end ? size_t(end - token) : std::strlen(token);
        wanted = length && text.find(std::string(token, length)) != std::string::npos;
        token = end ? end + 1 : token + length;
    }
    __imp__sub_82C66928(ctx, base);
    if (!wanted)
        return;
    std::string values;
    for (uint32_t arg : args)
    {
        char item[48];
        // Arguments in guest stacks and heaps may be where a value was returned.
        if (arg >= 0x40000000 && arg < 0x70000000)
            std::snprintf(item, sizeof(item), " %08X(->%d)", arg, Load<int32_t>(arg));
        else
            std::snprintf(item, sizeof(item), " %d", int32_t(arg));
        values += item;
    }
    std::fprintf(stderr, "[sql] %s |%s -> %d (thread 0x%X)\n", text.c_str(), values.c_str(), ctx.r3.s32, CurrentThreadId());
}
