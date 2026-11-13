// HC09_GPU_LIVE=1: hc09.exe renders every frame with the Vulkan EDRAM renderer
// and presents it in a window (docs/BACKEND_PLAN.md milestone 5). The game's
// 512 MiB of physical memory is imported into Vulkan in place
// (VK_EXT_external_memory_host on the section view at the base of the guest
// space), so vertex fetch reads and resolves write the game's own memory.
//
// Threads: the command processor queues draws/resolves/frame ends to a
// renderer thread that does all Vulkan work (Live::Worker; HC09_GPU_THREAD=0:
// everything on the CP thread). Window: created and pumped by a thread of its
// own (it stays responsive while a frame or a load takes long); the renderer
// decodes the XE_SWAP front buffer on the GPU and presents it (FIFO). The
// window is resizable (the image is scaled, aspect kept); closing it ends the
// game (pipeline cache saved). HC09_GPU_WINDOW=0: offscreen;
// HC09_GPU_WINDOW=hidden: the window exists and is presented to but never shown.
// HC09_GPU_LIVE_DUMP=N writes every N-th presented frame (the XE_SWAP front
// buffer) to HC09_GPU_LIVE_DIR (default captures/live) as frame_<swap>.png and
// checks, when the window shows the image 1:1, that the swapchain image read
// back equals it. HC09_GPU_LIVE_DUMP_AT=<s>,<n>: instead, n consecutive frames from s seconds
// after start (native renderer; e.g. to see motion frame by frame). GpuLiveRequestDump() (the "dump"
// command of HC09_INPUT_FILE, imports_input.cpp): the next presented frame, once.
//
// HC09_RENDERER=native (milestone 9, docs/NATIVE_RENDERER.md): the native
// renderer (runtime/gpu_vk/vk_native) instead of the EDRAM renderer. The game's
// D3D calls reach it through runtime/hooks_native.cpp and d3dn::Frontend; the
// command processor keeps running the ring (fences, interrupts, vblank) but
// renders nothing. NativeLive below: at each D3D submit the game thread snapshots
// small draw data (as the EDRAM path's snapshots) and queues the work for a
// renderer thread, which draws, resolves and presents; at a frame's present the
// game thread waits until the GPU has drawn the frame before it (its snapshot half
// is then free again). The renderer itself keeps two frames in flight (vk_native).
#include "dev_env.h"
#include "widescreen.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <windows.h>

namespace {
// hc09: HDTextures=on (HC09_HD_TEXTURES): when the game closes, HC09HDPack.exe (beside it) upscales what it collected
// (tools/hdpack), in the background, after this process has ended.
void StartHdPack()
{
    const char* e = std::getenv("HC09_HD_TEXTURES");
    if (!e || !*e || !strcmp(e, "0")) return;
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
    std::wstring line = L"\"" + (std::filesystem::path(self).parent_path() / L"HC09HDPack.exe").wstring() + L"\" --wait " +
                        std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fprintf(stderr, "[hdpack] started HC09HDPack.exe for the textures collected\n");
    }
}
}  // namespace
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vk_device.h"
#include "vk_edram.h"
#include "vk_native.h"
#include "vk_platform.h"
#include "vk_swapchain.h"
#include "xe_glsl.h"
#include "xenos.h"
#include "xenos_raster.h"
#include "xenos_texture.h"
#include "common/edition.h"
#include "native_live.h"
#include "interp.h"
#include "overlay.h"
#include "score_bar.h"
#include "profiler.h"
#include "small_vec.h"
void HostSetThreadName(void* thread, const char* name);   // runtime.cpp

// gpu.cpp
void GpuCpIdle(double* ringEmptySeconds, double* memWaitSeconds);
// runtime.cpp: CPU accesses to resolved memory the GPU keeps write it back first.
void GuestWatchAccess(uint32_t pa, uint32_t bytes);
void GuestSetAccessHook(void (*hook)(uint32_t pa), bool log);
void NativeLiveWriteBack(uint32_t pa);
namespace hc09::gpu { int FrameNumber(); }   // gpu/renderer.cpp: the frame counter of HC09_PRESS

// HC09_SCREENSHOT=<frame>[,<frame>...]: the window as presented at those frames (counted as HC09_PRESS
// counts them), saved as build/screenshot_<frame>.bmp, as the D3D12 renderer does
// (tools/bmp_to_png.py converts them).
static std::vector<int> ScreenshotFrames()
{
    std::vector<int> frames;
    if (const char* spec = std::getenv("HC09_SCREENSHOT"))
        for (const char* p = spec; *p;)
        {
            char* end;
            frames.push_back(int(std::strtol(p, &end, 10)));
            if (end == p) break;
            p = *end == ',' ? end + 1 : end;
        }
    std::sort(frames.begin(), frames.end());
    return frames;
}

// R,G,B,A pixels as a 32-bit top-down BMP.
static bool WriteBmp(const char* path, uint32_t width, uint32_t height, const std::vector<uint32_t>& rgba)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t pixelBytes = width * height * 4;
    uint8_t header[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
    put32(2, 54 + pixelBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, uint32_t(-int32_t(height)));   // negative: top-down
    header[26] = 1;
    header[28] = 32;
    put32(34, pixelBytes);
    fwrite(header, 1, sizeof(header), f);
    std::vector<uint32_t> bgra(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i)
        bgra[i] = (rgba[i] & 0xFF00FF00u) | ((rgba[i] >> 16) & 0xFFu) | ((rgba[i] & 0xFFu) << 16);
    fwrite(bgra.data(), 4, bgra.size(), f);
    return fclose(f) == 0;
}

// One-shot dump of the next presented frame (HC09_INPUT_FILE "dump").
static std::atomic<bool> g_dumpRequest{ false };
void GpuLiveRequestDump() { g_dumpRequest.store(true); }
static bool TakeDumpRequest(const std::string& dir)
{
    if (!g_dumpRequest.load(std::memory_order_relaxed) || !g_dumpRequest.exchange(false)) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return true;
}
void GpuLogRegisterWrites(bool on);
void GpuTakeRegisterWrites(std::vector<std::pair<uint32_t, uint32_t>>& out);
void GpuCopyRegisters(uint32_t* out, uint32_t count);
void GpuRedirectFenceWord(uint32_t pa, void (*sink)(uint32_t value));
void GpuStoreFenceWord(uint32_t pa, uint32_t value);

namespace {

struct Live : xenos::DrawObserver
{
    hc09vk::Device dev;
    hc09vk::Buffer guest;
    hc09vk::EdramRenderer renderer;
    hc09vk::Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    hc09vk::Swapchain swapchain;
    bool swapchainOk = false;
    std::atomic<bool> closeRequested{ false }, exiting{ false };
    uint64_t frames = 0, dumpEvery = 0;
    bool dumpScaled = false;   // HC09_GPU_LIVE_DUMP_SCALED=1: also the presented image at resolution scale
    double dumpAt = -1;        // HC09_GPU_LIVE_DUMP_AT=<s>,<n>
    uint64_t dumpAtLeft = 0;
    std::vector<std::pair<uint64_t, xenos::Image>> dumpHeld;
    std::string dir;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double busy = 0;      // seconds spent in the renderer since the last report
    double present = 0;   // seconds spent presenting since the last report
    hc09vk::RendererStats last;   // at the last report
    double lastRingEmpty = 0, lastMemWait = 0;   // GpuCpIdle at the last report

    // ---- renderer thread (HC09_GPU_THREAD, default on) ----------------------
    // The CP thread queues draws, resolves and frame ends; this thread records
    // them (all Vulkan work happens here), so packet processing and recording
    // overlap. It renders from its own copy of the register file: each command
    // carries the registers the CP wrote since the previous one, and the copy
    // is resynchronized whole at every frame end. The CP waits at XE_SWAP until
    // the frame is done (rendered, submitted, presented), as before.
    struct Cmd
    {
        enum Kind { kDraw, kResolve, kFrameEnd } kind = kDraw;
        xenos::DrawPacket d;
        uint64_t vsHash = 0, psHash = 0;
        const xenos::ShaderCode* vs = nullptr;
        const xenos::ShaderCode* ps = nullptr;
        xenos::ResolveInfo r;
        uint32_t front = 0, width = 0, height = 0;
        std::vector<std::pair<uint32_t, uint32_t>> regs;   // register writes since the previous command
        std::vector<uint32_t> fullRegs;   // HC09_GPU_PIPELINE frame end: the whole file, applied after the frame
    };
    // HC09_GPU_PIPELINE=1 (experiment, frame pacing study): at a frame end the CP
    // waits only for the previous frame, so the renderer finishes frame N while
    // the CP runs frame N+1. Fences and interrupts then reach the game before
    // the frame is rendered; the snapshot halves still alternate safely.
    bool pipeline = false;
    static constexpr size_t kQueue = 16384;
    bool threaded = false;
    std::vector<Cmd> queue;
    size_t head = 0, tail = 0;   // consumed / produced (queue[i % kQueue])
    uint64_t framesQueued = 0, framesDone = 0;
    bool workerWaiting = false, producerWaiting = false;
    std::mutex qm;
    std::condition_variable qcv;   // worker: commands queued; CP: space / frame done
    std::vector<uint32_t> shadow;  // the renderer thread's register file
    std::atomic<uint64_t> syncWaitNs{ 0 };   // CP time waiting for the renderer at frame ends

    Cmd& Reserve()
    {
        std::unique_lock<std::mutex> lk(qm);
        if (tail - head >= kQueue)
        {
            producerWaiting = true;
            qcv.wait(lk, [&] { return tail - head < kQueue; });
            producerWaiting = false;
        }
        Cmd& c = queue[tail % kQueue];
        GpuTakeRegisterWrites(c.regs);
        return c;
    }
    void Commit()
    {
        std::lock_guard<std::mutex> lk(qm);
        ++tail;
        if (workerWaiting) qcv.notify_all();
    }
    void Worker()
    {
        ProfilerRegisterThread(0, "emulated renderer");
        HostSetThreadName(nullptr, "hc09 renderer (emulated)");
        xenos::SetThreadRegisterFile(shadow.data());
        for (;;)
        {
            Cmd* c;
            {
                std::unique_lock<std::mutex> lk(qm);
                if (head == tail)
                {
                    workerWaiting = true;
                    qcv.wait(lk, [&] { return head != tail; });
                    workerWaiting = false;
                }
                c = &queue[head % kQueue];
            }
            for (auto [i, v] : c->regs)
                if (i < xenos::kRegisterCount) shadow[i] = v;
            bool frameEnd = c->kind == Cmd::kFrameEnd;
            if (c->kind == Cmd::kDraw) DoDraw(c->d, c->vsHash, c->vs, c->psHash, c->ps);
            else if (c->kind == Cmd::kResolve) DoResolve(c->r);
            else DoFrameEnd(c->front, c->width, c->height);
            if (frameEnd && !c->fullRegs.empty()) memcpy(shadow.data(), c->fullRegs.data(), shadow.size() * 4);
            std::lock_guard<std::mutex> lk(qm);
            ++head;
            if (frameEnd) ++framesDone;
            if (producerWaiting || frameEnd) qcv.notify_all();
        }
    }

    // ---- HC09_GPU_HAZARD=1: guest memory changed under a queued draw --------
    // The command processor runs ahead of the GPU: it queues a draw and goes
    // on to write fences the game may take as "the GPU is done with that
    // memory", while the renderer thread records the draw later (indices,
    // textures) and the GPU reads its vertices later still. At each draw the
    // CP thread hashes the draw's index range and vertex ranges (the vertex
    // shader's fetch slots, as the fetch constants say now; ranges up to
    // 64 KiB, each once per frame); at the frame end, after the GPU finished,
    // it hashes them again. A difference means the game rewrote memory a
    // draw of this frame still needed.
    struct HazardRange { uint32_t pa, bytes; uint64_t hash; uint32_t draw; uint32_t slot; uint32_t primType; };
    bool hazard = false;
    uint8_t* phys = nullptr;
    std::unordered_map<uint64_t, std::vector<uint32_t>> hzSlots;   // vertex fetch slots per VS hash
    std::vector<HazardRange> hz;
    std::unordered_set<uint64_t> hzSeen;
    uint32_t hzDraw = 0, hzSkipped = 0;
    uint64_t hzFrames = 0, hzBadFrames = 0, hzReported = 0;

    static uint64_t Fnv(const uint8_t* p, uint32_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
    void HazardRangeNote(uint32_t pa, uint32_t bytes, uint32_t slot, uint32_t primType)
    {
        pa &= 0x1FFFFFFF;
        if (!bytes || pa + uint64_t(bytes) > 0x20000000ull) return;
        if (bytes > 65536) { ++hzSkipped; return; }
        if (!hzSeen.insert((uint64_t(pa) << 32) | bytes).second) return;
        hz.push_back({ pa, bytes, Fnv(phys + pa, bytes), hzDraw, slot, primType });
    }
    // ---- draw-data snapshots (HC09_GPU_SNAPSHOT, default on with the renderer thread)
    // The fix for the race above: on the CP thread each queued draw's index
    // range and vertex ranges up to HC09_GPU_SNAPSHOT_MAX bytes (default 32 KiB:
    // the dynamic UI quads and text the game rewrites within a frame; large
    // static meshes are left alone) are copied into pa 0x01000000-0x01FFFFFF, and
    // the draw reads the copy: its
    // DMA index address is rewritten, and a fetch-constant write pointing at the
    // copy is appended to the draw's register writes, so only the renderer's
    // register file sees it. Two 8 MiB halves alternate per frame (the CP waits
    // for the frame at XE_SWAP before the next frame reuses a half). That range
    // is never allocated: the image lives at pa 0x02000000, the heap grows up
    // from above it and physical allocations down from 512 MiB to meet it (the
    // game takes nearly all of that: no room can be reserved at the top).
    static constexpr uint32_t kSnapBase = 0x01000000, kSnapHalf = 0x00800000;
    bool snapshot = false;
    uint32_t snapMax = 32768, snapOff = 0, snapParity = 0;
    uint64_t snapCopies = 0, snapBytes = 0, snapReused = 0, snapOverflowFrames = 0, snapFrames = 0;
    bool snapOverflowed = false;
    std::unordered_map<uint64_t, uint32_t> snapDone;   // (pa << 32 | bytes) -> this frame's copy

    // Copy [pa, pa+bytes) into this frame's half; returns the copy's address, 0 if full.
    // A range copied earlier in the frame is reused while its bytes are unchanged
    // (static buffers drawn many times cost a compare, not a copy).
    uint32_t SnapCopy(uint32_t pa, uint32_t bytes)
    {
        pa &= 0x1FFFFFFF;
        uint32_t lead = pa & 3, n = bytes + lead, need = (n + 63) & ~63u;
        uint64_t key = (uint64_t(pa) << 32) | bytes;
        auto it = snapDone.find(key);
        if (it != snapDone.end() && !memcmp(phys + it->second - lead, phys + (pa - lead), n)) { ++snapReused; return it->second; }
        if (snapOff + need > kSnapHalf) { snapOverflowed = true; return 0; }
        uint32_t dst = kSnapBase + snapParity * kSnapHalf + snapOff;
        memcpy(phys + dst, phys + (pa - lead), n);
        snapOff += need;
        ++snapCopies;
        snapBytes += n;
        snapDone[key] = dst + lead;
        return dst + lead;
    }

    // CP thread, per draw: snapshot (threaded: `regs` = the draw's register writes,
    // `d` its packet) and/or note ranges for the hazard check.
    void PrepareDraw(xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs,
                     std::vector<std::pair<uint32_t, uint32_t>>* regs)
    {
        ++hzDraw;
        bool snap = snapshot && regs;
        if (d.sourceSelect == 0 && d.dmaWords)
        {
            uint32_t bytes = d.dmaWords * 4, copy = 0;
            if (snap && bytes <= snapMax) copy = SnapCopy(d.dmaBase, bytes);
            if (copy) d.dmaBase = copy;
            else if (hazard) HazardRangeNote(d.dmaBase, bytes, 0xFFFF, d.primType);
        }
        if (!vs || (!snap && !hazard)) return;
        auto it = hzSlots.find(vsHash);
        if (it == hzSlots.end()) it = hzSlots.emplace(vsHash, hc09vk::VertexFetchSlots(*vs)).first;
        if (it->second.empty()) return;
        uint32_t fetch[192];
        xenos::RegRange(xenos::reg::FETCH_CONSTANTS, 192, fetch);
        for (uint32_t s : it->second)
        {
            if (s >= 96) continue;
            uint32_t d0 = fetch[2 * s], bytes = ((fetch[2 * s + 1] >> 2) & 0xFFFFFF) * 4, copy = 0;
            if (snap && bytes && bytes <= snapMax) copy = SnapCopy(d0 & ~3u, bytes);
            // Always state the slot's value for this draw: an earlier draw may have
            // left a copy's address in the renderer's register file.
            if (snap) regs->emplace_back(xenos::reg::FETCH_CONSTANTS + 2 * s, copy ? (copy | (d0 & 3)) : d0);
            if (!copy && hazard) HazardRangeNote(d0 & ~3u, bytes, s, d.primType);
        }
    }
    void SnapFrameEnd()
    {
        if (!snapshot) return;
        ++snapFrames;
        if (snapFrames % 600 == 0)
            fprintf(stderr, "[gpu-live] draw-data snapshots per frame: %.0f copies (%.1f KiB), %.0f reused; %llu frames over the %u KiB budget\n",
                    double(snapCopies) / 600.0, double(snapBytes) / 600.0 / 1024.0, double(snapReused) / 600.0,
                    (unsigned long long)snapOverflowFrames, kSnapHalf / 1024);
        if (snapFrames % 600 == 0) { snapCopies = 0; snapBytes = 0; snapReused = 0; }
        if (snapOverflowed) ++snapOverflowFrames;
        snapOverflowed = false;
        snapDone.clear();
        snapParity ^= 1;
        snapOff = 0;
    }
    void HazardCheck()
    {
        ++hzFrames;
        uint32_t bad = 0;
        for (const HazardRange& r : hz)
            if (Fnv(phys + r.pa, r.bytes) != r.hash)
            {
                if (++bad <= 4 && hzReported < 200)
                {
                    ++hzReported;
                    fprintf(stderr, "[gpu-hazard] swap %llu: draw %u/%u (%s) %s pa 0x%08X+0x%X changed before the frame ended\n",
                            (unsigned long long)xenos::HostSwapCount(), r.draw, hzDraw, xenos::PrimitiveName(r.primType),
                            r.slot == 0xFFFF ? "indices" : ("vertices, fetch slot " + std::to_string(r.slot)).c_str(), r.pa, r.bytes);
                }
            }
        if (bad) ++hzBadFrames;
        if (hzFrames % 600 == 0)
            fprintf(stderr, "[gpu-hazard] %llu frames checked, %llu with changed draw data (last frame: %zu ranges, %u over 64 KiB skipped)\n",
                    (unsigned long long)hzFrames, (unsigned long long)hzBadFrames, hz.size(), hzSkipped);
        hz.clear();
        hzSeen.clear();
        hzDraw = 0;
        hzSkipped = 0;
    }

    void OnDraw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash,
                const xenos::ShaderCode* ps) override
    {
        if (!threaded)
        {
            xenos::DrawPacket dd = d;
            if (hazard) PrepareDraw(dd, vsHash, vs, nullptr);
            DoDraw(d, vsHash, vs, psHash, ps);
            return;
        }
        Cmd& c = Reserve();
        c.kind = Cmd::kDraw;
        c.d = d;
        c.vsHash = vsHash; c.vs = vs; c.psHash = psHash; c.ps = ps;
        if (snapshot || hazard) PrepareDraw(c.d, vsHash, vs, &c.regs);
        Commit();
    }
    void OnResolved() override {}
    bool PerformsResolves() const override { return true; }
    void OnResolve(const xenos::ResolveInfo& r) override
    {
        if (!threaded) { DoResolve(r); return; }
        Cmd& c = Reserve();
        c.kind = Cmd::kResolve;
        c.r = r;
        Commit();
    }
    void OnFrameEnd(uint32_t front, uint32_t width, uint32_t height) override
    {
        if (!threaded)
        {
            DoFrameEnd(front, width, height);
            if (hazard) HazardCheck();
            return;
        }
        Cmd& c = Reserve();
        c.kind = Cmd::kFrameEnd;
        c.front = front; c.width = width; c.height = height;
        c.fullRegs.clear();
        if (pipeline)
        {
            // The renderer resynchronizes from this copy once it has finished the frame.
            c.fullRegs.resize(xenos::kRegisterCount);
            GpuCopyRegisters(c.fullRegs.data(), xenos::kRegisterCount);
        }
        auto t = std::chrono::steady_clock::now();
        uint64_t frame;
        {
            std::lock_guard<std::mutex> lk(qm);
            ++tail;
            frame = ++framesQueued;
            if (workerWaiting) qcv.notify_all();
        }
        std::unique_lock<std::mutex> lk(qm);
        if (pipeline)
        {
            qcv.wait(lk, [&] { return framesDone + 1 >= frame; });
            lk.unlock();
            std::vector<std::pair<uint32_t, uint32_t>> drop;
            GpuTakeRegisterWrites(drop);   // contained in the copy above
            syncWaitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
            SnapFrameEnd();
            return;
        }
        qcv.wait(lk, [&] { return framesDone >= frame; });
        // The renderer thread is idle until the next command: resynchronize its
        // register file whole (also catches registers written outside packets).
        std::vector<std::pair<uint32_t, uint32_t>> drop;
        GpuTakeRegisterWrites(drop);
        GpuCopyRegisters(shadow.data(), xenos::kRegisterCount);
        syncWaitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
        if (hazard) HazardCheck();
        SnapFrameEnd();
    }

    void DoDraw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash,
                const xenos::ShaderCode* ps)
    {
        auto t = std::chrono::steady_clock::now();
        renderer.OnDraw(d, vsHash, vs, psHash, ps);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }
    void DoResolve(const xenos::ResolveInfo& r)
    {
        auto t = std::chrono::steady_clock::now();
        renderer.OnResolve(r);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }

    // The window was closed: finish on this thread (Vulkan is used only here).
    void Exit()
    {
        exiting = true;
        fprintf(stderr, "[gpu-live] window closed: exiting\n");
        renderer.Shutdown();   // waits for the GPU, saves the pipeline cache
        StartHdPack();
        fflush(stderr);
        _exit(0);
    }

    // Blit the decoded front buffer to the window. `check`: the front buffer as
    // decoded by the CPU, compared with what the swapchain image holds.
    void Present(const hc09vk::Image& img, const xenos::Image* check, uint64_t swap)
    {
        uint32_t cw = 0, ch = 0;
        hc09vk::WindowClientSize(window, &cw, &ch);
        if (!cw || !ch) return;   // minimized
        std::string err;
        if (!swapchainOk || cw != swapchain.extent.width || ch != swapchain.extent.height)
        {
            swapchainOk = swapchain.Resize(cw, ch, &err);
            if (!swapchainOk) { fprintf(stderr, "[gpu-live] swapchain: %s\n", err.c_str()); return; }
        }
        bool oneToOne = check && swapchain.extent.width == img.width && swapchain.extent.height == img.height;
        // The live tweaker's in-game overlay (runtime/hooks_gameplay.cpp), placed on the image as the
        // swapchain fits it into the window. HC09_OVERLAY_SHOT=N: every N-th swap, the window image with
        // the overlay is written to <dir>/overlay_<swap>.png. Not in the release build (HC09_DEV_TOOLS=0).
        swapchain.overlay.clear();
#if HC09_DEV_TOOLS
        {
            const uint32_t ew = swapchain.extent.width, eh = swapchain.extent.height;
            uint32_t w = ew, h = uint32_t(uint64_t(ew) * img.height / img.width);
            if (h > eh) { h = eh; w = uint32_t(uint64_t(eh) * img.width / img.height); }
            static thread_local std::vector<OvVtx> ov;
            ov.clear();
            OverlayBuild(ov, float(ew - w) / 2, float(eh - h) / 2, float(w), float(h));
            static_assert(sizeof(OvVtx) == sizeof(hc09vk::OverlayVtx), "overlay vertex layouts");
            swapchain.overlay.resize(ov.size());
            if (!ov.empty()) memcpy(swapchain.overlay.data(), ov.data(), ov.size() * sizeof(OvVtx));
        }
#endif
        // The score bar (roadmap 12), on the picture as the swapchain fits it into the window (`stretch` times wider).
        {
            const double shown = double(img.width) * swapchain.stretch / double(img.height);
            const double ew = swapchain.extent.width, eh = swapchain.extent.height;
            double w = ew, h = std::floor(ew / shown + 0.5);
            if (h > eh) { h = eh; w = std::min(ew, std::floor(eh * shown + 0.5)); }
            ScoreBarBuild(swapchain, float(std::floor((ew - w) / 2)), float(std::floor((eh - h) / 2)), float(w), float(h));
        }
        static const uint64_t shotEvery = Hc09Env("HC09_OVERLAY_SHOT") ? strtoull(Hc09Env("HC09_OVERLAY_SHOT"), nullptr, 10) : 0;
        const bool shot = shotEvery && swap % shotEvery == 0 && !swapchain.overlay.empty();
        // HC09_SCREENSHOT: the first present at or after each frame asked for (the game's frame counter can
        // move on by more than one between presents).
        static const std::vector<int> screenshotFrames = ScreenshotFrames();
        static size_t nextScreenshot = 0;
        const int frame = hc09::gpu::FrameNumber();
        const bool screenshot = nextScreenshot < screenshotFrames.size() && frame >= screenshotFrames[nextScreenshot];
        std::vector<uint32_t> shown;
        VkResult r = swapchain.Present(img, (oneToOne || shot || screenshot) ? &shown : nullptr);
        if (screenshot)
        {
            char path[64];
            snprintf(path, sizeof(path), "build/screenshot_%d.bmp", screenshotFrames[nextScreenshot]);
            if (shown.size() == size_t(swapchain.extent.width) * swapchain.extent.height &&
                WriteBmp(path, swapchain.extent.width, swapchain.extent.height, shown))
                fprintf(stderr, "[gpu] wrote %s (frame %d, %ux%u)\n", path, frame, swapchain.extent.width, swapchain.extent.height);
            else
                fprintf(stderr, "[gpu] screenshot frame %d: the window could not be read back\n", frame);
            while (nextScreenshot < screenshotFrames.size() && screenshotFrames[nextScreenshot] <= frame) ++nextScreenshot;
        }
        if (shot && shown.size() == size_t(swapchain.extent.width) * swapchain.extent.height)
        {
            const std::string od = dir.empty() ? std::string("captures/overlay") : dir;
            std::filesystem::create_directories(od);
            xenos::Image out{ swapchain.extent.width, swapchain.extent.height, shown };
            xenos::WritePng(od + "/overlay_" + std::to_string(swap) + ".png", out);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainOk = false;   // rebuilt next frame
        else if (r != VK_SUCCESS) fprintf(stderr, "[gpu-live] present: %s\n", hc09vk::ResultName(r));
        if (oneToOne && shown.size() == check->rgba.size())
        {
            size_t diff = 0;
            for (size_t i = 0; i < shown.size(); ++i) diff += shown[i] != check->rgba[i];
            fprintf(stderr, "[gpu-live] swap %llu: presented image %s the front buffer (%zu of %zu pixels differ)\n",
                    (unsigned long long)swap, diff ? "DIFFERS from" : "equals", diff, shown.size());
            if (diff)
            {
                xenos::Image out{ img.width, img.height, std::move(shown) };
                xenos::WritePng(dir + "/presented_" + std::to_string(swap) + ".png", out);
            }
        }
    }

    void DoFrameEnd(uint32_t front, uint32_t width, uint32_t height)
    {
        if (closeRequested) Exit();
        auto t = std::chrono::steady_clock::now();
        const hc09vk::Image* img = window ? renderer.DecodeFrontBuffer(front, width, height) : nullptr;
        renderer.EndFrame();
        auto t1 = std::chrono::steady_clock::now();
        busy += std::chrono::duration<double>(t1 - t).count();
        ++frames;
        uint64_t swap = xenos::HostSwapCount();
        xenos::Image dumped;
        bool asked = TakeDumpRequest(dir);
        bool dump = ((dumpEvery && swap % dumpEvery == 0) || asked) && xenos::DecodeFrontBuffer(front, width, height, dumped);
        if (dump) xenos::WritePng(dir + "/frame_" + std::to_string(swap) + ".png", dumped);
        if (dump && asked) fprintf(stderr, "[gpu-live] dumped %s/frame_%llu.png\n", dir.c_str(), (unsigned long long)swap);
        if (img)
        {
            auto tp = std::chrono::steady_clock::now();
            Present(*img, dump ? &dumped : nullptr, swap);
            present += std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count();
        }
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (wall >= 10.0)
        {
            const hc09vk::RendererStats& s = renderer.Stats();
            double n = double(frames ? frames : 1);
            fprintf(stderr, "[gpu-live] swap %llu: %.1f frames/s, renderer %.1f ms/frame: textures %.1f, GPU waits %.1f (GPU busy %.1f), "
                            "shaders+pipelines %.1f; present %.1f (totals: %u draws, %u resolves, %u syncs, %u textures decoded, "
                            "%u of them again, %u pipelines)\n",
                    (unsigned long long)swap, frames / wall, 1000.0 * busy / n, 1000.0 * (s.textureSeconds - last.textureSeconds) / n,
                    1000.0 * (s.waitSeconds - last.waitSeconds) / n, 1000.0 * (s.gpuSeconds - last.gpuSeconds) / n,
                    1000.0 * (s.compileSeconds + s.pipelineSeconds - last.compileSeconds - last.pipelineSeconds) / n,
                    1000.0 * present / n, s.draws, s.resolves, s.syncs, s.textures, s.texturesRedecoded, s.pipelines);
            if (s.drawSections[0] > 0)   // HC09_GPU_CPUPROF=1
            {
                std::string line;
                char buf[64];
                for (int i = 0; i < hc09vk::RendererStats::kDrawSections; ++i)
                {
                    snprintf(buf, sizeof(buf), "%s%s %.2f", i ? ", " : "", hc09vk::kDrawSectionNames[i],
                             1000.0 * (s.drawSections[i] - last.drawSections[i]) / n);
                    line += buf;
                }
                fprintf(stderr, "[gpu-live]   draw CPU ms/frame (%.0f draws/frame): %s\n", (s.draws - last.draws) / n, line.c_str());
            }
            // Where the rest of the frame goes on the CP thread: idle while the game
            // has queued nothing / waits for CPU memory (the game is the limit), or
            // busy outside the renderer (packet processing, xenos state decoding).
            double ringEmpty = 0, memWait = 0;
            GpuCpIdle(&ringEmpty, &memWait);
            double frameMs = 1000.0 * wall / n;
            double idleMs = 1000.0 * (ringEmpty - lastRingEmpty + memWait - lastMemWait) / n;
            if (threaded)
            {
                double syncMs = 1e-6 * double(syncWaitNs.exchange(0)) / n;
                fprintf(stderr, "[gpu-live]   ms/frame: frame %.1f; renderer thread busy %.1f; CP thread idle %.1f (ring empty %.1f, "
                                "CPU waits %.1f), waiting for the renderer at frame end %.1f, busy %.1f\n",
                        frameMs, 1000.0 * busy / n, idleMs, 1000.0 * (ringEmpty - lastRingEmpty) / n, 1000.0 * (memWait - lastMemWait) / n,
                        syncMs, frameMs - idleMs - syncMs);
            }
            else
                fprintf(stderr, "[gpu-live]   CP thread ms/frame: frame %.1f = renderer %.1f + idle %.1f (ring empty %.1f, CPU waits %.1f) + other %.1f\n",
                        frameMs, 1000.0 * busy / n, idleMs, 1000.0 * (ringEmpty - lastRingEmpty) / n, 1000.0 * (memWait - lastMemWait) / n,
                        frameMs - 1000.0 * busy / n - idleMs);
            lastRingEmpty = ringEmpty;
            lastMemWait = memWait;
            last = s;
            frames = 0;
            busy = 0;
            present = 0;
            t0 = std::chrono::steady_clock::now();
        }
    }
};

Live* g_live = nullptr;

bool EnvOnV(const char* e, bool dflt)
{
    if (!e || !*e) return dflt;
    return strcmp(e, "0") != 0;
}
#define EnvOn(name, dflt) EnvOnV(Hc09Env(name), dflt)

// ---- the native renderer (default; HC09_RENDERER=emulated: off) ------------------
struct NativeLive;
NativeLive* g_nativeFenceSink = nullptr;   // the CP's fence writes go here (NativeLive::FencesStart)
// HC09_NATIVE_HAZARD: a range a draw reads (kind 0 indices, 1 vertices from fetch slot `slot`), its
// hash when the draw was handed over, and a copy once that range changed early before.
struct HazRange
{
    uint32_t pa = 0, bytes = 0, kind = 0, slot = 0;
    uint64_t hash = 0;
    std::shared_ptr<std::vector<uint8_t>> copy;
};
struct InFlight
{
    std::atomic<bool>* dynamic;
    uint32_t pa, bytes, kind;
    uint64_t epoch;
    uint64_t learn;   // the stream it was read through (vertex shader + fetch slot), 0: none
    uint64_t vs;      // diagnostics: that vertex shader and slot (0xFF: indices)
    uint32_t slot;
};
struct NativeLive : d3dn::Backend
{
    Live* live = nullptr;                  // device, window, swapchain, Present()
    hc09vk::NativeRenderer renderer;
    uint8_t* phys = nullptr;
    struct Cmd
    {
        enum Kind { kTargets, kClear, kDraw, kResolve, kPresent, kFence, kMotion } kind = kDraw;
        // Targets, clears, resolves and presents (a few per frame): kept apart, so a draw's
        // Cmd (thousands per frame, built and moved at every kick) stays small.
        struct Other
        {
            d3dn::Surface rt[4], depth;
            uint32_t flags = 0, stencil = 0;
            int32_t rect[4] = {};
            float color[4] = {}, z = 0;
            d3dn::ResolveOp r;
            uint32_t front = 0, width = 0, height = 0;
            uint64_t tick = 0;      // kMotion (hc09: smooth motion): the frame's simulation tick and fraction past it
            float fraction = 1.0f;
        };
        std::unique_ptr<Other> o;
        std::shared_ptr<const d3dn::DrawOp> src;   // the front end's op (not copied: command buffers replay it)
        xenos::DrawPacket d;                        // src->d, the indices possibly moved to a snapshot
        // src->loads' values as guest memory holds them at the submit (big-endian, the loads' dwords in order;
        // applied after src->regs).
        d3dn::SmallVec<uint32_t, 64> loaded;
        d3dn::SmallVec<std::pair<uint32_t, uint32_t>, 4> overrides;   // this draw only (snapshots, inline vertex data)
        uint32_t fence = 0;                                     // kFence: the D3D fence this kick wrote
        std::vector<HazRange> haz;               // HC09_NATIVE_HAZARD: what the draw reads, hashed
        std::vector<InFlight> inflight;          // kFence: ranges read in place (write-tracked) since the last one
    };
    std::mutex qm;
    std::condition_variable qcv;
    // Batches: the game thread fills `pending` and hands it over at its submit (swapped
    // into `queue` under the lock); the renderer swaps `queue` into its own batch. The
    // vectors keep their capacity, so a Cmd costs no allocation of its own.
    std::vector<Cmd> queue;
    std::vector<Cmd> pending;   // game thread: queued at the next Flush (D3D's submit), under one lock
    // Write-backs the fault handler asks for (a game thread touched resolved memory the
    // GPU keeps): pages, served by the renderer thread between commands (under qm).
    std::vector<uint32_t> wbReq;
    uint64_t wbSeq = 0, wbDone = 0;
    std::atomic<bool> wbPending{ false };
    void ServeWriteBacks()
    {
        if (!wbPending.load()) return;
        std::vector<uint32_t> req;
        uint64_t seq;
        {
            std::lock_guard<std::mutex> lk(qm);
            req.swap(wbReq);
            seq = wbSeq;
            wbPending = false;
        }
        for (uint32_t pa : req) renderer.SyncToGuest(pa, 0x1000);
        std::lock_guard<std::mutex> lk(qm);
        wbDone = seq;
        qcv.notify_all();
    }
    uint64_t framesQueued = 0, framesDone = 0;
    uint64_t frameSubmit[8] = {};   // frame n's GPU timeline value (renderer.LastSubmit), at n % 8
    // Game-thread side: the register file as handed over (fetch constants for the
    // snapshots) and the draw-data snapshot halves (pa 0x01000000, as Live's).
    std::vector<uint32_t> mirror = std::vector<uint32_t>(xenos::kRegisterCount, 0);
    std::unordered_map<uint64_t, std::vector<uint32_t>> vfSlots;   // vertex fetch slots per vertex shader
    static constexpr uint32_t kSnapBase = 0x01000000, kSnapHalf = 0x00800000;
    uint32_t snapMax = 32768, snapOff = 0, snapParity = 0;
    uint64_t snapOverflowFrames = 0, snapCopies = 0, snapBytes = 0, snapReused = 0, snapFrames = 0;
    bool snapOverflowed = false;
    // This frame: bytes asked for by kind (0 indices, 1 vertices, 2 inline), bytes that
    // did not fit, the largest range (for the overflow report).
    uint64_t snapWant[3] = {}, snapMissed[3] = {}, snapMaxRange = 0;
    uint32_t snapMaxPa = 0, snapMissedRanges = 0;
    // Ranges that did not fit: a hash of their bytes per (pa, bytes), to tell in the
    // report how many changed since they last missed (static meshes read live are
    // harmless; data the game rewrites is not).
    std::unordered_map<uint64_t, uint64_t> missedHash;
    uint64_t snapMissedChanged = 0, snapMissedSame = 0, snapMissedNew = 0;
    // Ranges that stay the same are not copied: a range whose bytes equal last
    // frame's copy `need` frames in a row is write-tracked and its draws read guest
    // memory; a CPU write puts it back to copying, with a 4x longer streak needed
    // before it is trusted again (HC09_NATIVE_SNAP_STATIC=0: copy everything).
    // Almost all of what the crowd shots asked for is static (44c22e8).
    struct RangeState
    {
        uint32_t lastDst = 0;        // last frame's copy (+ lead)
        uint64_t lastFrame = 0, lastUse = 0;
        uint32_t streak = 0, need = 3;
        uint64_t epoch = 0;
        bool isStatic = false;
    };
    std::unordered_map<uint64_t, RangeState> rangeState;   // (pa << 32 | bytes)
    bool snapStatic = EnvOn("HC09_NATIVE_SNAP_STATIC", true);
    uint64_t snapStaticBytes = 0, snapStaticDemoted = 0;
    std::unordered_map<uint64_t, uint32_t> snapDone;   // (pa << 32 | bytes) -> this frame's copy
    // Index/vertex snapshots stop this far short of the half's end: inline vertex
    // data (text, UI quads) has nowhere else to go and must always fit.
    static constexpr uint32_t kInlineReserve = 2u << 20;
    // Renderer thread.
    std::vector<uint32_t> rfile = std::vector<uint32_t>(xenos::kRegisterCount, 0);
    double busy = 0, present = 0, waitGame = 0, idle = 0;
    uint64_t frames = 0;
    // HC09_NATIVE_PROF=1: the renderer's sections (differences per report), the game
    // thread's time in the front end (hooks_native.cpp), the CP thread's busy time.
    bool prof = EnvOn("HC09_NATIVE_PROF", false);
    hc09vk::NativeProfile lastProf;
    uint32_t lastTexCpu = 0, lastTexGpu = 0;
    std::atomic<uint64_t> feNs[3] = {};
    double lastCpIdle = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    // Time between presented frames (ms) since the last report: its median, 99th percentile and maximum, and how
    // many were over 17.5 ms (a missed 60 Hz frame) and 34 ms (two missed), in the [native] frame line.
    std::vector<float> frameMs;
    std::chrono::steady_clock::time_point lastPresentAt{};

    // ---- Fences from the native GPU (HC09_NATIVE_FENCES, default on; NATIVE_RENDERER.md 7.4 step 2) ----
    // D3D's completed-fence word (*(dev+10896) + 0, which its fence waits, resource locks,
    // queries and Swap's queued-frame throttle read) is the older of: the fence the CP
    // last wrote (it still runs the ring: the ring's space, interrupts and swaps follow
    // it) and the fence whose work the native GPU has finished. Each D3D kick that writes
    // a fence leaves a marker in the renderer's queue (Fence); the renderer thread maps it
    // to the timeline value of the submit that will contain the work before it; the fence
    // thread publishes it when the GPU passes that value. So the game rewrites nothing a
    // draw reads before the native GPU has drawn it, and the draw-data snapshots are off.
    // A D3D wait for a fence whose work is still being recorded asks the renderer to
    // submit at once (RequestFlush); otherwise only frame ends submit.
    // HC09_NATIVE_FENCES=0: the CP's fences as before (and the snapshots); =cp: markers and
    // HC09_NATIVE_HAZARD tracked, but the CP's fence published (to compare).
    int fenceMode = [] { const char* e = Hc09Env("HC09_NATIVE_FENCES"); return !e || !*e ? 1 : !strcmp(e, "cp") ? 2 : atoi(e) ? 1 : 0; }();
    bool fences = fenceMode != 0, cpOnly = fenceMode == 2;
    bool snapshots = EnvOn("HC09_NATIVE_SNAPSHOT", fenceMode != 1);
    bool hazard = EnvOn("HC09_NATIVE_HAZARD", false);
    std::atomic<bool> fencesUp{ false };
    uint32_t fencePa = 0;
    std::mutex fm;
    std::condition_variable fcv;
    uint32_t cpFence = 0, gpuFence = 0, shownFence = 0;   // under fm
    struct FenceMark { uint32_t fence; uint64_t value; std::vector<HazRange> haz; std::vector<InFlight> inflight; };
    std::deque<FenceMark> fenceQ;                          // under fm, oldest first
    std::atomic<uint32_t> flushReq{ 0 };                    // fence a waiter needs submitted (0 = none)
    std::atomic<bool> flushWake{ false };
    uint32_t lastMark = 0;                                 // renderer thread: the last marker processed
    bool unsubmittedMark = false;                          // ... and work before it not yet submitted
    std::vector<HazRange> hazSince;         // renderer thread: hazard ranges since the last marker
    // Native fences: draw data the game rewrites while the GPU may still read it, without
    // waiting for a fence (the skinning palette sub_828CE2C8 writes a player vertex buffer
    // for the next frame while this one is drawn: on the console the GPU has finished by
    // then). Every index/vertex range up to HC09_GPU_SNAPSHOT_MAX is read in place with its
    // pages write-tracked; the fence thread checks, before it publishes the draw's fence,
    // whether the game wrote it meanwhile; such a range is snapshotted from then on (as all
    // were with the CP's fences). HC09_NATIVE_DYNAMIC_SNAP=0: never.
    bool dynTrack = EnvOn("HC09_NATIVE_DYNAMIC_SNAP", true);
    struct Track
    {
        std::atomic<bool> dynamic{ false };
        uint64_t epoch = 0, frame = 0;   // write tracking since `epoch` (taken in snapshot frame `frame`)
        uint64_t mark = 0;               // the marker its in-flight check last went with
        uint64_t copyKick = 0;           // dynamic: the kick its copy was made in, and the copy
        uint32_t copy = 0;
        uint32_t learnGen = 0;           // `learned` generation it was last checked against
    };
    // Vertex ranges of a shape (vertex shader + fetch slot + size) one of which was caught rewritten in flight: the
    // others are snapshotted from their first draw on, not caught one by one (each catch is a frame drawn from data
    // the game was rewriting: the players' bone palettes are new buffers of a few fixed sizes all the time, and wiggled
    // in their first frames). Not indices (their streams also read static buffers, and copies miss the index cache).
    // HC09_NATIVE_LEARN_DYNAMIC=0: per range only.
    bool learnDyn = EnvOn("HC09_NATIVE_LEARN_DYNAMIC", true);
    std::mutex learnMu;
    std::vector<uint64_t> learnQ;                 // fence thread -> game thread
    std::atomic<bool> learnQueued{ false };
    std::unordered_set<uint64_t> learned;         // game thread
    uint32_t learnGen = 1;
    std::atomic<uint64_t> stLearnedRanges{ 0 }, stLearnedStreams{ 0 };
    static uint64_t ShapeKey(uint64_t vsHash, uint32_t slot, uint32_t bytes)
    {
        return vsHash ? (vsHash ^ (uint64_t(slot + 1) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(bytes) * 0xC2B2AE3D27D4EB4Full)) | 1 : 0;
    }
    void DrainLearned()
    {
        std::lock_guard<std::mutex> lk(learnMu);
        bool grew = false;
        for (uint64_t k : learnQ) grew |= learned.insert(k).second;
        learnQ.clear();
        learnQueued.store(false, std::memory_order_relaxed);
        if (grew) ++learnGen;
        stLearnedStreams.store(learned.size(), std::memory_order_relaxed);
    }
    std::unordered_map<uint64_t, Track> tracks;   // game thread; nodes stay put (the fence thread sets `dynamic`)
    // Direct-mapped in front of `tracks`: a crowd close-up's frame reads ~7,000 ranges (indices + vertex streams).
    static constexpr size_t kTrackCache = 16384;
    std::vector<std::pair<uint64_t, Track*>> trackCache = std::vector<std::pair<uint64_t, Track*>>(kTrackCache);
    std::vector<InFlight> inflightPending;        // game thread: ranges read in place since the last marker
    uint64_t kickSeq = 1, markSeq = 1;            // game thread: hand-overs (Flush) and markers (Fence)
    std::atomic<uint64_t> stDynamic{ 0 };
    // A range a draw reads: its snapshot address (0: read in place).
    uint32_t DrawRange(uint32_t pa, uint32_t bytes, int kind, uint64_t vs = 0, uint32_t streamSlot = 0)
    {
        uint64_t learn = kind == 1 ? ShapeKey(vs, streamSlot, bytes) : 0;
        if (!bytes || bytes > snapMax) return 0;
        if (snapshots) return Snap(pa, bytes, kind);
        if (!dynTrack || !fencesUp || cpOnly) return 0;
        pa &= 0x1FFFFFFFu;
        uint64_t key = (uint64_t(pa) << 32) | bytes;
        auto& slot = trackCache[((key * 0x9E3779B97F4A7C15ull) >> 40) & (kTrackCache - 1)];
        if (slot.first != key || !slot.second) slot = { key, &tracks[key] };   // map nodes stay put
        Track& t = *slot.second;
        if (learnDyn && learn)
        {
            if (learnQueued.load(std::memory_order_relaxed)) DrainLearned();
            if (t.learnGen != learnGen)
            {
                t.learnGen = learnGen;
                if (!t.dynamic.load(std::memory_order_relaxed) && learned.count(learn))
                {
                    t.dynamic.store(true, std::memory_order_relaxed);
                    ++stLearnedRanges;
                }
            }
        }
        if (t.dynamic.load(std::memory_order_relaxed))
        {
            // One copy per hand-over: its draws are queued here one after the other, with
            // no game code in between (no compare needed).
            if (t.copyKick != kickSeq || !t.copy) { t.copy = SnapCopy(pa, bytes, kind); t.copyKick = kickSeq; }
            return t.copy;
        }
        uint64_t e = t.epoch;
        if (t.frame != snapFrames + 1 || xenos::HostWrittenSince(pa, bytes, e))
        {
            t.epoch = xenos::HostTrackWrites(pa, bytes);
            t.frame = snapFrames + 1;
            t.mark = 0;
        }
        if (t.mark != markSeq)   // once per marker (and per epoch)
        {
            t.mark = markSeq;
            inflightPending.push_back({ &t.dynamic, pa, bytes, uint32_t(kind), t.epoch, learn, vs, streamSlot });
        }
        return 0;
    }
    // A plain copy into the snapshot area (0: no room).
    uint32_t SnapCopy(uint32_t pa, uint32_t bytes, int kind)
    {
        uint32_t lead = pa & 3, n = bytes + lead, need = (n + 63) & ~63u;
        snapWant[kind] += n;
        if (snapOff + need > kSnapHalf - kInlineReserve) { snapOverflowed = true; snapMissed[kind] += n; ++snapMissedRanges; return 0; }
        uint32_t dst = kSnapBase + snapParity * kSnapHalf + snapOff;
        memcpy(phys + dst, phys + (pa - lead), n);
        snapOff += need;
        ++snapCopies;
        snapBytes += n;
        return dst + lead;
    }
    // Statistics (reported with [native] frame lines): fence-wait flushes, idle flushes,
    // host-side waits and their time, by what held the fence (CP or GPU).
    std::atomic<uint64_t> stFlushes{ 0 }, stIdleFlushes{ 0 }, stWaits{ 0 }, stWaitNs{ 0 }, stWaitCp{ 0 }, stWaitGpu{ 0 };
    std::atomic<uint64_t> stHazRanges{ 0 }, stHazChanged{ 0 }, stHazBytes{ 0 };
    static bool After(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
    // Under fm: write the older of the two fences into guest memory if it moved on.
    void PublishLocked()
    {
        uint32_t v = cpOnly || After(gpuFence, cpFence) ? cpFence : gpuFence;
        if (!After(v, shownFence)) return;
        uint32_t mem = __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(phys + fencePa));
        shownFence = v;
        if (After(v, mem)) GpuStoreFenceWord(fencePa, v);
        fcv.notify_all();
    }
    void OnCpFence(uint32_t v)   // CP thread (its EVENT_WRITE_SHD of the fence word)
    {
        std::lock_guard<std::mutex> lk(fm);
        if (After(v, cpFence)) cpFence = v;
        PublishLocked();
    }
    // Game thread, under the feed lock: the first kick with a fence starts it all.
    void FencesStart(uint32_t pa)
    {
        fencePa = pa & 0x1FFFFFFCu;
        GpuRedirectFenceWord(fencePa, [](uint32_t v) { g_nativeFenceSink->OnCpFence(v); });
        {
            std::lock_guard<std::mutex> lk(fm);
            cpFence = gpuFence = shownFence = __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(phys + fencePa));
        }
        std::thread([this] { FenceThread(); }).detach();
        fencesUp = true;
        fprintf(stderr, "[native] fences from the native GPU%s: D3D's fence word at pa 0x%08X (fence %u); draw-data snapshots %s%s\n",
                HC09_DEV_TOOLS && cpOnly ? " tracked, the CP's published (HC09_NATIVE_FENCES=cp)" : "", fencePa, shownFence,
                snapshots ? "on" : "off", HC09_DEV_TOOLS && hazard ? "; HC09_NATIVE_HAZARD: checking draw data" : "");
    }
    // Game thread, under the feed lock: a D3D kick wrote `fence` after the work handed over.
    void Fence(uint32_t fence)
    {
        Cmd c;
        c.kind = Cmd::kFence;
        c.fence = fence;
        c.inflight.swap(inflightPending);
        ++markSeq;
        pending.push_back(std::move(c));
        Flush();
    }
    // Any thread: a D3D wait needs `fence`; the renderer submits once it has recorded the work before it.
    void RequestFlush(uint32_t fence)
    {
        uint32_t r = flushReq.load();
        while ((r == 0 || After(fence, r)) && !flushReq.compare_exchange_weak(r, fence)) {}
        if (!flushWake.exchange(true))
        {
            std::lock_guard<std::mutex> lk(qm);
            qcv.notify_all();
        }
    }
    bool FencePending(uint32_t cur, uint32_t fence) const { return (cur - fence) < (cur - shownFence); }   // D3D's own test
    // Game thread (D3D's BlockOnFence internal, sub_823288C8): wait host-side until the
    // fence word passes `fence` instead of spinning (the fence must have been kicked).
    // HC09_NATIVE_FENCE_TRACE=1: the waits by D3D caller and block type (with the frame report).
    bool fenceTrace = EnvOn("HC09_NATIVE_FENCE_TRACE", false);
    struct WaitStat { uint64_t n = 0, ns = 0, dist = 0, unsubmitted = 0; };
    std::mutex wsm;
    std::map<uint64_t, WaitStat> waitStats;   // (caller << 8 | type)
    void WaitFence(uint32_t cur, uint32_t fence, uint32_t caller, uint32_t type)
    {
        std::unique_lock<std::mutex> lk(fm);
        if (!FencePending(cur, fence)) return;
        bool gpuHeld = After(fence, gpuFence);
        bool queued = false;
        for (const FenceMark& m : fenceQ) if (m.fence == fence) { queued = m.value > renderer.LastSubmit(); break; }
        lk.unlock();
        auto tw = std::chrono::steady_clock::now();
        RequestFlush(fence);
        auto t = std::chrono::steady_clock::now();
        lk.lock();
        while (FencePending(cur, fence)) fcv.wait_for(lk, std::chrono::milliseconds(5));
        lk.unlock();
        ++stWaits;
        stWaitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
        ++(gpuHeld ? stWaitGpu : stWaitCp);
        if (fenceTrace)
        {
            std::lock_guard<std::mutex> lk2(wsm);
            WaitStat& w = waitStats[(uint64_t(caller) << 8) | (type & 0xFF)];
            ++w.n;
            w.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw).count());
            w.dist += cur - fence;
            w.unsubmitted += queued || fence + 2 == cur;
        }
    }
    // Fence thread: publishes the GPU's progress through the markers.
    void FenceThread()
    {
        ProfilerRegisterThread(0, "native fences");
        HostSetThreadName(nullptr, "hc09 native fences");
        for (;;)
        {
            uint64_t value;
            {
                std::unique_lock<std::mutex> lk(fm);
                fcv.wait(lk, [&] { return !fenceQ.empty(); });
                value = fenceQ.front().value;
            }
            renderer.WaitSubmit(value);
            std::vector<FenceMark> done;
            {
                std::lock_guard<std::mutex> lk(fm);
                while (!fenceQ.empty() && fenceQ.front().value <= value)
                {
                    done.push_back(std::move(fenceQ.front()));
                    fenceQ.pop_front();
                }
            }
            // Ranges read in place that the game wrote while the GPU might read them: copied from now on.
            for (const FenceMark& m : done)
                for (const InFlight& f : m.inflight)
                {
                    if (f.dynamic->load(std::memory_order_relaxed)) continue;
                    uint64_t e = f.epoch;
                    if (!xenos::HostWrittenSince(f.pa, f.bytes, e)) continue;
                    f.dynamic->store(true, std::memory_order_relaxed);
                    if (f.learn && learnDyn)
                    {
                        std::lock_guard<std::mutex> lk(learnMu);
                        learnQ.push_back(f.learn);
                        learnQueued.store(true, std::memory_order_relaxed);
                        static int shown = 0;
                        if (++shown <= 20)
                            fprintf(stderr, "[native] learn: %s of vs %016llx slot %u (pa 0x%08X, %u bytes) rewritten in flight\n",
                                    f.kind ? "vertices" : "indices", (unsigned long long)f.vs, f.slot, f.pa, f.bytes);
                    }
                    uint64_t k = ++stDynamic;
                    if (k <= 20 || k % 100 == 0)
                        fprintf(stderr, "[native] %s at pa 0x%08X (%u bytes) written while in flight (before fence %u): snapshotted from now on (%llu such ranges)\n",
                                f.kind ? "vertices" : "indices", f.pa, f.bytes, m.fence, (unsigned long long)k);
                }
            // HC09_NATIVE_HAZARD: what the finished draws read must be unchanged until
            // their fence is published (the game may rewrite it only after that).
            for (const FenceMark& m : done)
                for (const HazRange& h : m.haz)
                {
                    ++stHazRanges;
                    if (HashRange(h.pa, h.bytes) == h.hash) continue;
                    uint64_t k = ++stHazChanged;
                    stHazBytes += h.bytes;
                    {
                        std::lock_guard<std::mutex> lk(hazMu);
                        hazWatch.insert(h.pa);
                    }
                    if (k > 40 && !h.copy) continue;
                    // With a copy (the range changed early before): which bytes changed.
                    uint32_t lo = 0, hi = 0, n = 0;
                    if (h.copy)
                    {
                        const uint8_t* p = phys + h.pa;
                        for (uint32_t i = 0; i < h.bytes; ++i)
                            if (p[i] != (*h.copy)[i]) { if (!n++) lo = i; hi = i; }
                        static std::atomic<int> detailed{ 0 };
                        if (detailed.fetch_add(1) >= 40) continue;
                    }
                    if (h.copy)
                        fprintf(stderr, "[native] hazard: %s at pa 0x%08X (%u bytes%s) changed before its fence %u was published (swap %llu):"
                                        " %u bytes differ in [%u, %u]\n",
                                h.kind ? "vertices" : "indices", h.pa, h.bytes, h.kind ? (", fetch slot " + std::to_string(h.slot)).c_str() : "",
                                m.fence, (unsigned long long)xenos::HostSwapCount(), n, lo, hi);
                    else
                        fprintf(stderr, "[native] hazard: %s at pa 0x%08X (%u bytes) changed before its fence %u was published (swap %llu)\n",
                                h.kind ? "vertices" : "indices", h.pa, h.bytes, m.fence, (unsigned long long)xenos::HostSwapCount());
                }
            std::lock_guard<std::mutex> lk(fm);
            if (After(done.back().fence, gpuFence)) gpuFence = done.back().fence;
            PublishLocked();
        }
    }
    uint64_t HashRange(uint32_t pa, uint32_t bytes) const
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
        const uint8_t* p = phys + (pa & 0x1FFFFFFFu);
        uint32_t i = 0;
        for (; i + 8 <= bytes; i += 8)
        {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        }
        for (; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
        return h ^ (h >> 29);
    }
    std::mutex hazMu;
    std::unordered_set<uint32_t> hazWatch;   // ranges that changed early: copied from then on (to say which bytes)
    void HazardNote(Cmd& c, uint32_t pa, uint32_t bytes, uint32_t kind, uint32_t slot = 0)
    {
        if (!bytes || bytes > (1u << 20)) return;
        pa &= 0x1FFFFFFFu;
        HazRange h{ pa, bytes, kind, slot, HashRange(pa, bytes), nullptr };
        bool watch;
        {
            std::lock_guard<std::mutex> lk(hazMu);
            watch = hazWatch.count(pa) != 0;
        }
        if (watch) h.copy = std::make_shared<std::vector<uint8_t>>(phys + pa, phys + pa + bytes);
        c.haz.push_back(std::move(h));
    }
    // Renderer thread: submit (without waiting) the work recorded up to the last marker
    // when a waiter needs it, when idle, or every HC09_NATIVE_SUBMIT_DRAWS draws (default
    // 200; 0 = only when asked): then the GPU trails the game by a few kicks as on the
    // console, and the fence a lock asks for is usually done already (the game locks
    // vertex buffers it drew a few kicks earlier in the same frame).
    uint32_t submitDraws = [] { const char* e = Hc09Env("HC09_NATIVE_SUBMIT_DRAWS"); return e ? uint32_t(strtoul(e, nullptr, 10)) : 200u; }();
    uint32_t drawsSinceSubmit = 0;
    std::atomic<uint64_t> stPartials{ 0 };
    void MaybeFlush(bool idle)
    {
        if (!unsubmittedMark) return;
        uint32_t r = flushReq.load();
        bool asked = r && !After(r, lastMark);
        bool due = submitDraws && drawsSinceSubmit >= submitDraws;
        if (!asked && !idle && !due) return;
        if (!renderer.Unsubmitted()) { unsubmittedMark = false; return; }
        if (asked) flushReq.compare_exchange_strong(r, 0);
        renderer.SubmitNow();
        unsubmittedMark = false;
        drawsSinceSubmit = 0;
        ++(asked ? stFlushes : idle ? stIdleFlushes : stPartials);
    }

    // The vertex fetch slots a vertex shader reads (the last one's cached: draws in a row share it).
    uint64_t lastSlotsHash = 0;
    const std::vector<uint32_t>* lastSlots = nullptr;
    const std::vector<uint32_t>& SlotsOf(const d3dn::DrawOp& op)
    {
        if (lastSlots && lastSlotsHash == op.vsHash) return *lastSlots;
        auto it = vfSlots.find(op.vsHash);
        if (it == vfSlots.end()) it = vfSlots.emplace(op.vsHash, hc09vk::VertexFetchSlots(*op.vs)).first;
        lastSlotsHash = op.vsHash;
        lastSlots = &it->second;   // unordered_map nodes stay put
        return *lastSlots;
    }
    // hc09: smooth motion: the first command of each frame is preceded by its state (interp.h MotionFrameState), taken
    // when the render loop's update began (on this thread, before its D3D calls).
    bool motionFrameStart = true;
    void Push(Cmd&& c)
    {
        if (motionFrameStart && MotionOn())
        {
            motionFrameStart = false;
            Cmd m;
            m.kind = Cmd::kMotion;
            m.o = std::make_unique<Cmd::Other>();
            MotionFrameState(&m.o->tick, &m.o->fraction);
            pending.push_back(std::move(m));
        }
        pending.push_back(std::move(c));
    }
    void Flush() override
    {
        ++kickSeq;
        if (pending.empty()) return;
        {
            std::lock_guard<std::mutex> lk(qm);
            if (queue.empty()) queue.swap(pending);
            else for (Cmd& c : pending) queue.push_back(std::move(c));
        }
        pending.clear();
        qcv.notify_all();
    }
    // A range copied earlier in the frame is reused while its bytes are unchanged
    // (static buffers drawn many times cost a compare, not a copy).
    uint32_t Snap(uint32_t pa, uint32_t bytes, int kind, bool mayStatic = true)
    {
        pa &= 0x1FFFFFFF;
        uint32_t lead = pa & 3, n = bytes + lead, need = (n + 63) & ~63u;
        uint64_t key = (uint64_t(pa) << 32) | bytes;
        auto it = snapDone.find(key);
        if (it != snapDone.end() && !memcmp(phys + it->second - lead, phys + (pa - lead), n)) { ++snapReused; return it->second; }
        RangeState* rs = nullptr;
        if (snapStatic && mayStatic)   // ranges written in flight (DrawRange) are always copied
        {
            rs = &rangeState[key];
            rs->lastUse = snapFrames;
            if (rs->isStatic)
            {
                uint64_t e = rs->epoch;
                if (!xenos::HostWrittenSince(pa, bytes, e)) { snapStaticBytes += n; return 0; }   // unchanged: read in place
                rs->isStatic = false;   // written: copy again, and wait longer before trusting it
                rs->streak = 0;
                rs->need = std::min(rs->need * 4, 1024u);
                ++snapStaticDemoted;
            }
            else if (rs->lastDst && rs->lastFrame + 1 == snapFrames && !memcmp(phys + rs->lastDst - lead, phys + (pa - lead), n))
            {
                if (++rs->streak >= rs->need)
                {
                    rs->isStatic = true;
                    rs->epoch = xenos::HostTrackWrites(pa, bytes);
                    snapStaticBytes += n;
                    return 0;
                }
            }
            else
                rs->streak = 0;
        }
        snapWant[kind] += n;
        if (n > snapMaxRange) { snapMaxRange = n; snapMaxPa = pa; }
        if (snapOff + need > kSnapHalf - kInlineReserve)
        {
            snapOverflowed = true;
            snapMissed[kind] += n;
            ++snapMissedRanges;
            if (!prof) return 0;   // HC09_NATIVE_PROF: whether what did not fit changes between frames
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (uint32_t i = 0; i + 8 <= n; i += 8)
            {
                uint64_t w;
                memcpy(&w, phys + (pa - lead) + i, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
            }
            auto [it2, fresh] = missedHash.try_emplace(key, h);
            if (fresh) snapMissedNew += n;
            else if (it2->second == h) snapMissedSame += n;
            else { snapMissedChanged += n; it2->second = h; }
            return 0;
        }
        uint32_t dst = kSnapBase + snapParity * kSnapHalf + snapOff;
        memcpy(phys + dst, phys + (pa - lead), n);
        snapOff += need;
        ++snapCopies;
        snapBytes += n;
        snapDone[key] = dst + lead;
        if (rs) { rs->lastDst = dst + lead; rs->lastFrame = snapFrames; }
        return dst + lead;
    }

    // d3dn::Backend (game thread, at D3D's submits)
    void SetTargets(const d3dn::Surface rt[4], const d3dn::Surface& depth) override
    {
        Cmd c;
        c.kind = Cmd::kTargets;
        c.o = std::make_unique<Cmd::Other>();
        for (int i = 0; i < 4; ++i) c.o->rt[i] = rt[i];
        c.o->depth = depth;
        Push(std::move(c));
    }
    void Clear(uint32_t flags, const int32_t rect[4], const float color[4], float z, uint32_t stencil) override
    {
        Cmd c;
        c.kind = Cmd::kClear;
        c.o = std::make_unique<Cmd::Other>();
        c.o->flags = flags;
        memcpy(c.o->rect, rect, sizeof(c.o->rect));
        memcpy(c.o->color, color, sizeof(c.o->color));
        c.o->z = z;
        c.o->stencil = stencil;
        Push(std::move(c));
    }
    // HC09_NATIVE_TRACE_SHADERS=<frame>: that frame's draws by (vertex, pixel) shader.
    std::map<std::pair<uint64_t, uint64_t>, std::pair<uint32_t, uint64_t>> shaderHist;
    uint64_t traceFrame = [] { const char* e = Hc09Env("HC09_NATIVE_TRACE_SHADERS"); return e ? strtoull(e, nullptr, 10) : 0ull; }();
    void Draw(d3dn::DrawOp& op) override
    {
        if (traceFrame && framesQueued + 1 == traceFrame)
        {
            auto& e = shaderHist[{ op.vsHash, op.psHash }];
            ++e.first;
            e.second += op.d.numIndices;
        }
        Cmd c;
        c.kind = Cmd::kDraw;
        c.src = op.weak_from_this().lock();
        if (!c.src) c.src = std::make_shared<d3dn::DrawOp>(op);   // not owned by a shared_ptr: a copy
        c.d = op.d;
        HandLoads(c, op);
        if (interpTrace) InterpTraceDraw(op.vsHash, op.psHash, op.d.dmaBase, op.d.numIndices, op.regs.data(), op.regs.size());
        HandRanges(c, op);
        if (prof)
        {
            Bump(hDraws, 1);
            Bump(hRegs, op.regs.size());
            Bump(hLoaded, c.loaded.size());
            Bump(hOverrides, c.overrides.size());
        }
        if (hazard)   // what the draw reads in place (not a snapshot)
        {
            if (c.d.sourceSelect == 0 && c.d.dmaWords && c.d.dmaBase == op.d.dmaBase)
                HazardNote(c, c.d.dmaBase, c.d.dmaWords * (c.d.indexSize32 ? 4 : 2), 0);
            if (op.vs)
            {
                for (uint32_t sl : SlotsOf(op))
                {
                    if (sl >= 96) continue;
                    uint32_t reg = xenos::reg::FETCH_CONSTANTS + 2 * sl;
                    if (std::any_of(c.overrides.begin(), c.overrides.end(), [&](const auto& o) { return o.first == reg; })) continue;
                    uint32_t d0 = mirror[reg], d1 = mirror[reg + 1];
                    if ((d0 & 3) == 3) HazardNote(c, d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, 1, sl);
                }
            }
        }
        if (!op.inlineData.empty())
        {
            // BeginVertices / DrawVerticesUP data: vertex fetch constant 95 (endian 8in32, as D3D sets it).
            uint32_t bytes = uint32_t(op.inlineData.size()), need = (bytes + 63) & ~63u;
            snapWant[2] += bytes;
            if (snapOff + need <= kSnapHalf)
            {
                uint32_t dst = kSnapBase + snapParity * kSnapHalf + snapOff;
                memcpy(phys + dst, op.inlineData.data(), bytes);
                snapOff += need;
                c.overrides.emplace_back(xenos::reg::FETCH_CONSTANTS + 190, dst | 3);
                c.overrides.emplace_back(xenos::reg::FETCH_CONSTANTS + 191, (((bytes + 3) / 4) << 2) | 2);
            }
            else
            {
                snapOverflowed = true;
                fprintf(stderr, "[native] inline vertex data dropped: snapshot half full\n");
            }
        }
        Push(std::move(c));
    }
    // HC09_NATIVE_PROF: what the hand-over did (game thread writes, the report reads).
    std::atomic<uint64_t> hDraws{ 0 }, hRegs{ 0 }, hLoaded{ 0 }, hOverrides{ 0 }, hRanges{ 0 };
    static void Bump(std::atomic<uint64_t>& a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
    // Constants loaded through a pointer: read now (the submit point; the GPU
    // reads them when it executes the draw), one copy per load. The register mirror
    // (fetch constants for the ranges) follows the registers; loads are float constants.
    const bool interpTrace = InterpTraceOn();   // HC09_INTERP_TRACE (interp.h)
    HC09_NOINLINE void HandLoads(Cmd& c, const d3dn::DrawOp& op)
    {
        if (!op.loads.empty())
        {
            size_t n = 0;
            for (const d3dn::DrawOp::Load& l : op.loads) n += l.dwords;
            c.loaded.reserve(n);
            for (const d3dn::DrawOp::Load& l : op.loads)
            {
                uint32_t pa = l.address & 0x1FFFFFFF;
                uint32_t* dst = c.loaded.append(l.dwords);
                if (pa + 4 * uint64_t(l.dwords) <= 0x20000000u) memcpy(dst, phys + pa, 4 * size_t(l.dwords));
                else
                    for (uint32_t k = 0; k < l.dwords; ++k) memcpy(dst + k, phys + ((l.address + 4 * k) & 0x1FFFFFFF), 4);
                if (interpTrace) InterpTraceLoads(&op, l.reg - xenos::reg::ALU_CONSTANTS, l.address, dst, l.dwords);
            }
        }
        for (auto [i, v] : op.regs) mirror[i] = v;
    }
    // Snapshots (without native fences): what the draw reads may be rewritten once
    // the CP (which runs ahead of this renderer) has signalled the draw's fences.
    // With native fences: only ranges the game rewrites in flight (DrawRange).
    HC09_NOINLINE void HandRanges(Cmd& c, const d3dn::DrawOp& op)
    {
        if (c.d.sourceSelect == 0 && c.d.dmaWords)
        {
            if (prof) Bump(hRanges, 1);
            if (uint32_t copy = DrawRange(c.d.dmaBase, c.d.dmaWords * (c.d.indexSize32 ? 4 : 2), 0, op.vsHash, 0xFF)) c.d.dmaBase = copy;
        }
        if (op.vs && (snapshots || (dynTrack && fencesUp && !cpOnly)))
        {
            for (uint32_t sl : SlotsOf(op))
            {
                if (sl >= 96) continue;
                uint32_t d0 = mirror[xenos::reg::FETCH_CONSTANTS + 2 * sl], d1 = mirror[xenos::reg::FETCH_CONSTANTS + 2 * sl + 1];
                uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
                if ((d0 & 3) != 3 || !bytes || bytes > snapMax) continue;
                if (prof) Bump(hRanges, 1);
                if (uint32_t copy = DrawRange(d0 & ~3u, bytes, 1, op.vsHash, sl)) c.overrides.emplace_back(xenos::reg::FETCH_CONSTANTS + 2 * sl, copy | (d0 & 3));
            }
        }
    }
    void Resolve(const d3dn::ResolveOp& r, const uint32_t*) override
    {
        Cmd c;
        c.kind = Cmd::kResolve;
        c.o = std::make_unique<Cmd::Other>();
        c.o->r = r;
        Push(std::move(c));
    }
    void Present(uint32_t front, uint32_t width, uint32_t height) override
    {
        if (interpTrace) InterpFrame();
        if (traceFrame && framesQueued + 1 == traceFrame)
        {
            std::vector<std::pair<std::pair<uint32_t, uint64_t>, std::pair<uint64_t, uint64_t>>> v;
            for (auto& [k, e] : shaderHist) v.push_back({ e, k });
            std::sort(v.rbegin(), v.rend());
            fprintf(stderr, "[native] frame %llu draws by shaders:\n", (unsigned long long)traceFrame);
            for (auto& [e, k] : v)
                fprintf(stderr, "[native]   vs %016llx ps %016llx: %u draws, %llu indices\n", (unsigned long long)k.first,
                        (unsigned long long)k.second, e.first, (unsigned long long)e.second);
        }
        Cmd c;
        c.kind = Cmd::kPresent;
        c.o = std::make_unique<Cmd::Other>();
        c.o->front = front;
        c.o->width = width;
        c.o->height = height;
        pending.push_back(std::move(c));
        motionFrameStart = true;
        Flush();
        uint64_t frame = ++framesQueued;
        // The next frame's snapshots reuse the half the frame before this one used:
        // wait until the GPU has drawn that frame.
        auto t = std::chrono::steady_clock::now();
        uint64_t gpuValue;
        {
            std::unique_lock<std::mutex> lk(qm);
            qcv.wait(lk, [&] { return framesDone + 1 >= frame; });
            gpuValue = frame > 1 ? frameSubmit[(frame - 1) % 8] : 0;
        }
        renderer.WaitSubmit(gpuValue);
        waitGame += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        if (snapOverflowed)
        {
            // Which frames overflow and with what: the first ten, then every 50th.
            if (++snapOverflowFrames <= 10 || snapOverflowFrames % 50 == 0)
                fprintf(stderr, "[native] swap %llu over the snapshot budget (#%llu): asked %.0f KiB (indices %.0f, vertices %.0f, inline %.0f),"
                                " %u ranges (%.0f KiB) not copied, largest range %.0f KiB at 0x%08X; ranges not copied so far:"
                                " %.0f KiB unchanged since they last missed, %.0f KiB changed, %.0f KiB first seen\n",
                        (unsigned long long)xenos::HostSwapCount(), (unsigned long long)snapOverflowFrames,
                        double(snapWant[0] + snapWant[1] + snapWant[2]) / 1024.0, double(snapWant[0]) / 1024.0, double(snapWant[1]) / 1024.0,
                        double(snapWant[2]) / 1024.0, snapMissedRanges, double(snapMissed[0] + snapMissed[1] + snapMissed[2]) / 1024.0,
                        double(snapMaxRange) / 1024.0, snapMaxPa, double(snapMissedSame) / 1024.0, double(snapMissedChanged) / 1024.0,
                        double(snapMissedNew) / 1024.0);
        }
        for (int k = 0; k < 3; ++k) snapWant[k] = snapMissed[k] = 0;
        snapMaxRange = 0;
        snapMaxPa = snapMissedRanges = 0;
        if (++snapFrames % 600 == 0)
        {
            fprintf(stderr, "[native] draw-data snapshots per frame: %.0f copies (%.1f KiB), %.0f reused, %.1f KiB static read in place"
                            " (%llu ranges static, %llu written since); %llu frames over the budget\n",
                    double(snapCopies) / 600.0, double(snapBytes) / 600.0 / 1024.0, double(snapReused) / 600.0,
                    double(snapStaticBytes) / 600.0 / 1024.0,
                    (unsigned long long)std::count_if(rangeState.begin(), rangeState.end(), [](const auto& e) { return e.second.isStatic; }),
                    (unsigned long long)snapStaticDemoted, (unsigned long long)snapOverflowFrames);
            snapCopies = snapBytes = snapReused = snapStaticBytes = 0;
            // Ranges not drawn for a while are forgotten.
            for (auto it = rangeState.begin(); it != rangeState.end();)
                if (it->second.lastUse + 600 < snapFrames) it = rangeState.erase(it);
                else ++it;
        }
        snapOverflowed = false;
        snapDone.clear();
        snapParity ^= 1;
        snapOff = 0;
    }

    // Renderer thread.
    void Worker()
    {
        ProfilerRegisterThread(0, "native renderer");
        HostSetThreadName(nullptr, "hc09 renderer");
        xenos::SetThreadRegisterFile(rfile.data());
        std::vector<Cmd> batch;
        std::vector<std::pair<uint32_t, uint32_t>> saved;
        size_t next = 0;
        for (;;)
        {
            if (next == batch.size())
            {
                batch.clear();   // the last batch's Cmds (and their op references) end here
                next = 0;
                auto ti = std::chrono::steady_clock::now();
                MaybeFlush(false);
                std::unique_lock<std::mutex> lk(qm);
                auto ready = [&] { return !queue.empty() || wbPending.load() || flushWake.load(); };
                // Recorded work behind a fence and nothing to do: submit it after a while even
                // unasked (a wait this does not know about must not wait for the frame's end).
                if (unsubmittedMark && !qcv.wait_for(lk, std::chrono::milliseconds(10), ready))
                {
                    lk.unlock();
                    MaybeFlush(true);
                    lk.lock();
                }
                qcv.wait(lk, ready);
                flushWake = false;
                batch.swap(queue);
                idle += std::chrono::duration<double>(std::chrono::steady_clock::now() - ti).count();
            }
            ServeWriteBacks();
            if (next == batch.size()) continue;
            Cmd& c = batch[next++];
            auto t = std::chrono::steady_clock::now();
            switch (c.kind)
            {
            case Cmd::kTargets: renderer.SetTargets(c.o->rt, c.o->depth); break;
            case Cmd::kClear: renderer.Clear(c.o->flags, c.o->rect, c.o->color, c.o->z, c.o->stencil); break;
            case Cmd::kResolve: renderer.Resolve(c.o->r); break;
            case Cmd::kDraw:
            {
                for (auto [i, v] : c.src->regs) rfile[i] = v;
                const uint32_t* lv = c.loaded.data();
                for (const d3dn::DrawOp::Load& l : c.src->loads)
                {
                    for (uint32_t k = 0; k < l.dwords; ++k) rfile[l.reg + k] = __builtin_bswap32(lv[k]);
                    lv += l.dwords;
                }
                saved.clear();
                for (auto [i, v] : c.overrides) { saved.emplace_back(i, rfile[i]); rfile[i] = v; }
                if (!c.src->stateOnly) renderer.Draw(c.d, c.src->vsHash, c.src->vs, c.src->psHash, c.src->ps);
                for (auto [i, v] : saved) rfile[i] = v;
                break;
            }
            case Cmd::kPresent: FrameEnd(c.o->front, c.o->width, c.o->height); break;
            case Cmd::kMotion: renderer.SetMotionFrame(c.o->tick, c.o->fraction); break;
            case Cmd::kFence:
            {
                bool more = renderer.Unsubmitted();
                FenceMark m{ c.fence, renderer.LastSubmit() + (more ? 1 : 0), std::move(hazSince), std::move(c.inflight) };
                hazSince.clear();
                {
                    std::lock_guard<std::mutex> lk(fm);
                    fenceQ.push_back(std::move(m));
                }
                fcv.notify_all();
                lastMark = c.fence;
                unsubmittedMark = more;
                MaybeFlush(false);
                break;
            }
            }
            if (c.kind == Cmd::kDraw && !c.haz.empty()) hazSince.insert(hazSince.end(), c.haz.begin(), c.haz.end());
            if (c.kind == Cmd::kDraw) ++drawsSinceSubmit;
            if (c.kind == Cmd::kPresent) { unsubmittedMark = false; drawsSinceSubmit = 0; }
            busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
            if (c.kind == Cmd::kPresent)
            {
                std::lock_guard<std::mutex> lk(qm);
                ++framesDone;
                frameSubmit[framesDone % 8] = renderer.LastSubmit();
                qcv.notify_all();
            }
        }
    }
    void FrameEnd(uint32_t front, uint32_t width, uint32_t height)
    {
        if (live->closeRequested)
        {
            live->exiting = true;
            fprintf(stderr, "[native] window closed: exiting\n");
            renderer.Shutdown();
            StartHdPack();
            fflush(stderr);
            _exit(0);
        }
        const hc09vk::Image* img = live->window ? renderer.DecodeFrontBuffer(front, width, height) : nullptr;
        renderer.EndFrame();
        ++frames;
        uint64_t swap = framesDone + 1;
        xenos::Image dumped;
        bool asked = TakeDumpRequest(live->dir);
        bool dumpNow = (live->dumpEvery && swap % live->dumpEvery == 0) || asked;
        if (live->dumpAt >= 0)
        {
            // HC09_GPU_LIVE_DUMP_AT: the presented image read back and kept in memory, written when the
            // window ends (decoding and writing each PNG at once would slow the frame rate the frames
            // are meant to show)
            dumpNow = false;
            if (img && live->dumpAtLeft && std::chrono::duration<double>(std::chrono::steady_clock::now() - live->t0).count() >= live->dumpAt)
            {
                std::vector<uint32_t> px;
                uint32_t dw = 0, dh = 0;
                if (renderer.ReadFront(px, dw, dh)) live->dumpHeld.emplace_back(swap, xenos::Image{ dw, dh, std::move(px) });
                --live->dumpAtLeft;
            }
            if (!live->dumpAtLeft && !live->dumpHeld.empty())
            {
                for (auto& [sw, im] : live->dumpHeld) xenos::WritePng(live->dir + "/native_" + std::to_string(sw) + ".png", im);
                fprintf(stderr, "[gpu-live] dumped %zu frames to %s\n", live->dumpHeld.size(), live->dir.c_str());
                live->dumpHeld.clear();
            }
        }
        if (dumpNow) renderer.SyncToGuest(front, ((width + 31) & ~31u) * ((height + 31) & ~31u) * 4);   // the CPU reads it
        bool dump = dumpNow && xenos::DecodeFrontBuffer(front, width, height, dumped);
        if (dump) xenos::WritePng(live->dir + "/native_" + std::to_string(swap) + ".png", dumped);
        if (dump && asked) fprintf(stderr, "[gpu-live] dumped %s/native_%llu.png\n", live->dir.c_str(), (unsigned long long)swap);
        if (dump && img && live->dumpScaled && renderer.Scale() > 1)
        {
            std::vector<uint32_t> px;
            uint32_t sw = 0, sh = 0;
            if (renderer.ReadFront(px, sw, sh))
                xenos::WritePng(live->dir + "/scaled_" + std::to_string(swap) + ".png", xenos::Image{ sw, sh, std::move(px) });
        }
        if (img)
        {
            auto tp = std::chrono::steady_clock::now();
            live->swapchain.gain = renderer.SceneHdrGain();   // hc09: HDR output's highlights (vk_hdr.h)
            live->Present(*img, dump ? &dumped : nullptr, swap);
            const auto shown = std::chrono::steady_clock::now();
            present += std::chrono::duration<double>(shown - tp).count();
            if (lastPresentAt.time_since_epoch().count())
                frameMs.push_back(float(std::chrono::duration<double, std::milli>(shown - lastPresentAt).count()));
            lastPresentAt = shown;
        }
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (wall >= 10.0)
        {
            const hc09vk::RendererStats& s = renderer.Stats();
            double n = double(frames ? frames : 1);
            fprintf(stderr, "[native] frame %llu: %.1f frames/s; renderer thread %.1f ms/frame (present %.1f), game thread waited %.1f ms/frame; "
                            "totals: %u draws (%u skipped), %u resolves, %u textures, %u pipelines; %llu frames over the snapshot budget;"
                            " resolved data to the GPU %u times, back %u\n",
                    (unsigned long long)swap, frames / wall, 1000.0 * busy / n, 1000.0 * present / n, 1000.0 * waitGame / n, s.draws, s.skipped,
                    s.resolves, s.textures + s.texturesOnGpu, s.pipelines, (unsigned long long)snapOverflowFrames, s.resolveAcquires,
                    s.resolveWritebacks);
            if (!frameMs.empty())
            {
                std::sort(frameMs.begin(), frameMs.end());
                const size_t k = frameMs.size();
                const size_t over1 = size_t(frameMs.end() - std::upper_bound(frameMs.begin(), frameMs.end(), 17.5f));
                const size_t over2 = size_t(frameMs.end() - std::upper_bound(frameMs.begin(), frameMs.end(), 34.0f));
                // and over 1.5 periods of the frame-rate setting (a frame shown a refresh late at any rate)
                static const double period = [] { const char* e = Hc09Env("HC09_FRAMERATE"); int n = e ? atoi(e) : 0; return n >= 10 ? 1000.0 / n : 1000.0 / 35; }();
                const size_t late = size_t(frameMs.end() - std::upper_bound(frameMs.begin(), frameMs.end(), float(1.5 * period)));
                fprintf(stderr, "[native] frame times: median %.2f ms, 99%% %.2f ms, max %.2f ms; %zu of %zu over 17.5 ms, %zu over 34 ms, %zu over 1.5 periods\n",
                        frameMs[k / 2], frameMs[std::min(k - 1, k * 99 / 100)], frameMs[k - 1], over1, k, over2, late);
                frameMs.clear();
            }
            for (auto& [why, k] : s.skipReasons) fprintf(stderr, "[native]   skipped %u: %s\n", k, why.c_str());
            if (fencesUp)
            {
                uint64_t w = stWaits.exchange(0), ns = stWaitNs.exchange(0), wc = stWaitCp.exchange(0), wg = stWaitGpu.exchange(0);
                uint64_t fl = stFlushes.exchange(0), ifl = stIdleFlushes.exchange(0), pfl = stPartials.exchange(0);
                uint32_t cp, gpu, shown;
                {
                    std::lock_guard<std::mutex> lk(fm);
                    cp = cpFence; gpu = gpuFence; shown = shownFence;
                }
                fprintf(stderr, "[native] fences: %.2f waits/frame (%.3f ms/frame; behind the GPU %llu, the CP %llu); mid-frame submits/frame:"
                                " %.2f asked, %.2f every %u draws, %.2f idle; now CP %u GPU %u shown %u",
                        double(w) / n, 1e-6 * double(ns) / n, (unsigned long long)wg, (unsigned long long)wc, double(fl) / n, double(pfl) / n,
                        submitDraws, double(ifl) / n, cp, gpu, shown);
                if (dynTrack)
                    fprintf(stderr, "; rewritten in flight: %llu ranges caught, %llu snapshotted from the start (%llu shapes)",
                            (unsigned long long)stDynamic.load(), (unsigned long long)stLearnedRanges.load(), (unsigned long long)stLearnedStreams.load());
                if (hazard)
                    fprintf(stderr, "; hazard check: %llu ranges, %llu changed early (%.1f KiB)", (unsigned long long)stHazRanges.exchange(0),
                            (unsigned long long)stHazChanged.load(), double(stHazBytes.load()) / 1024.0);
                fputc('\n', stderr);
                if (fenceTrace)
                {
                    std::lock_guard<std::mutex> lk2(wsm);
                    for (auto& [k, w] : waitStats)
                        fprintf(stderr, "[native]   fence waits from 0x%08llX type %llu: %.2f/frame, %.3f ms each, %.1f fences back, %.0f%% unsubmitted\n",
                                (unsigned long long)(k >> 8), (unsigned long long)(k & 0xFF), double(w.n) / n, 1e-6 * double(w.ns) / double(w.n),
                                double(w.dist) / double(w.n) / 2.0, 100.0 * double(w.unsubmitted) / double(w.n));
                    waitStats.clear();
                }
            }
            if (const hc09vk::NativeProfile* p = prof ? renderer.Profile() : nullptr)
            {
                auto ms = [&](int i) { return 1000.0 * (p->sec[i] - lastProf.sec[i]) / n; };
                using P = hc09vk::NativeProfile;
                double draw = 0;
                for (int i = P::kDrawState; i <= P::kDrawRecord; ++i) draw += ms(i);
                double ringEmpty = 0, memWait = 0;
                GpuCpIdle(&ringEmpty, &memWait);
                double cpIdle = ringEmpty + memWait;
                uint64_t fe[3];
                for (int k = 0; k < 3; ++k) fe[k] = feNs[k].exchange(0);
                std::string line;
                char buf[160];
                for (int i = 0; i < P::kSections; ++i)
                {
                    uint64_t w = p->waits[i] - lastProf.waits[i];
                    if (w) snprintf(buf, sizeof(buf), " %s %.2f (%.1f/frame)", hc09vk::kNativeSectionNames[i], ms(i), double(w) / n);
                    else snprintf(buf, sizeof(buf), " %s %.2f", hc09vk::kNativeSectionNames[i], ms(i));
                    line += buf;
                }
                std::string gk;
                for (int i = 0; i < P::kGpuKinds; ++i)
                {
                    snprintf(buf, sizeof(buf), " %s %.2f", hc09vk::kNativeGpuKindNames[i], 1000.0 * (p->gpuKind[i] - lastProf.gpuKind[i]) / n);
                    gk += buf;
                }
                line += "\n[native-prof]  GPU by kind:" + gk;
                fprintf(stderr, "[native-prof] ms/frame over %.0f frames: renderer thread busy %.2f (draws %.2f; present %.2f), idle %.2f;"
                                " GPU %.2f in %.1f submits/frame; game thread in the front end %.2f (calls %.2f, draws %.2f, submits %.2f),"
                                " waited for the renderer %.2f;"
                                " CP thread busy %.2f; textures decoded per frame: %.1f CPU, %.1f GPU\n[native-prof]  sections:%s\n",
                        n, 1000.0 * busy / n, draw, 1000.0 * present / n, 1000.0 * idle / n,
                        1000.0 * (p->gpuSeconds - lastProf.gpuSeconds) / n, double(p->submits - lastProf.submits) / n,
                        1e-6 * double(fe[0] + fe[1] + fe[2]) / n, 1e-6 * double(fe[0]) / n, 1e-6 * double(fe[1]) / n, 1e-6 * double(fe[2]) / n,
                        1000.0 * waitGame / n, (1000.0 * wall - 1000.0 * (cpIdle - lastCpIdle)) / n,
                        double(s.textures - lastTexCpu) / n, double(s.texturesOnGpu - lastTexGpu) / n, line.c_str());
                if (uint64_t hd = hDraws.exchange(0))
                    fprintf(stderr, "[native-prof]  hand-over per frame: %.0f draws, %.1f registers, %.1f loaded dwords, %.2f overrides, %.2f ranges each\n",
                            double(hd) / n, double(hRegs.exchange(0)) / hd, double(hLoaded.exchange(0)) / hd, double(hOverrides.exchange(0)) / hd,
                            double(hRanges.exchange(0)) / hd);
                lastTexCpu = s.textures;
                lastTexGpu = s.texturesOnGpu;
                lastProf = *p;
                lastCpIdle = cpIdle;
            }
            frames = 0;
            busy = present = waitGame = idle = 0;
            t0 = std::chrono::steady_clock::now();
        }
    }
};
NativeLive* g_native = nullptr;


// The window's thread: create it, hand it over, handle its events until the
// user closes it, then ask the command-processor thread to end the game. If
// that thread does not get there (the game stopped presenting), exit anyway.
void WindowThread(Live* l, bool show, std::promise<hc09vk::Window*> created)
{
    HostSetThreadName(nullptr, "hc09 window");
    // HC09_WINDOW_SIZE=<w>x<h>: the window's first client size (dev; e.g. a hidden window larger than the image).
    uint32_t ww = 1280, wh = 720;
    if (const char* e = Hc09Env("HC09_WINDOW_SIZE")) sscanf(e, "%ux%u", &ww, &wh);
    hc09vk::Window* w = hc09vk::WindowCreate(hc09::edition::kTitle, ww, wh, show);
    if (w) hc09vk::WindowSetEscapeCloses(w, false);
    if (w && EnvOn("HC09_FULLSCREEN", false)) hc09vk::WindowSetFullscreen(w, true);   // Alt+Enter / F11 toggle
    created.set_value(w);
    if (!w) return;
    while (hc09vk::WindowWaitEvents(w)) {}
    l->closeRequested = true;
    for (int i = 0; i < 30 && !l->exiting; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (l->exiting) return;   // the CP thread exits the process
    fprintf(stderr, "[gpu-live] window closed and no frame for 3 s: exiting\n");
    StartHdPack();
    fflush(stderr);
    _exit(0);
}

}  // namespace

// Called once when the command processor starts. `physicalMemory`: host
// address of guest physical address 0 (512 MiB, page aligned).
#if !HC09_DEV_TOOLS
// docs/HANDOFF_M11.md step 6: what the player needs, said plainly, then a clean exit (code 4, not a crash report).
// Under Wine/Proton a failed import of the game's memory is the AMD Linux driver refusing a file-backed mapping (the
// Steam Deck), not an old driver: the fix is libhc09_userptr.so through Steam's launch options (README "Steam Deck",
// tools/steamdeck/userptr_shim.c). Empty unless that is the case.
static std::string WineImportAdvice(const std::string& what)
{
#ifdef _WIN32
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt || !GetProcAddress(nt, "wine_get_version") || what.find("(import)") == std::string::npos) return "";
    return "On a Steam Deck (or Linux with AMD graphics) the game needs a small fix: start hc09-launcher.exe from Steam, "
           "and it shows the line to paste into this program's Steam launch options (Properties > General > Launch "
           "options). The README's \"Steam Deck\" section has the details.";
#else
    (void)what;
    return "";
#endif
}

[[noreturn]] void RequirementMissing(const std::string& what)
{
    std::string advice = WineImportAdvice(what);
    std::string msg = what + "\n\n" + (!advice.empty() ? advice :
                      "NFL Head Coach 09 for PC needs a graphics card and driver with Vulkan 1.3 (NVIDIA GTX 900 "
                      "series, AMD RX 400 series, Intel UHD 600 or newer, with a current driver). Please update your "
                      "graphics driver from NVIDIA, AMD or Intel and try again.");
    fprintf(stderr, "[gpu-live] *** requirement missing: %s\n", what.c_str());
    fflush(stderr);
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "NFL Head Coach 09: graphics driver", MB_OK | MB_ICONERROR);
#endif
    _exit(4);
}
#endif

void GpuLiveInit(uint8_t* physicalMemory)
{
    if (!EnvOn("HC09_GPU_LIVE", false) || g_live) return;
    Live* l = new Live;
    std::string err;
    // Default: present in a window. HC09_GPU_WINDOW=0: offscreen; =hidden: a
    // window that is never shown (the whole present path, readback checks
    // included, without the desktop — unattended runs while the PC is in use).
    const char* we = Hc09Env("HC09_GPU_WINDOW");
    bool hidden = we && !strcmp(we, "hidden");
    bool windowed = EnvOn("HC09_GPU_WINDOW", true);
    if (windowed)
    {
        std::promise<hc09vk::Window*> created;
        std::future<hc09vk::Window*> f = created.get_future();
        std::thread(WindowThread, l, !hidden, std::move(created)).detach();
        l->window = f.get();
        if (!l->window) fprintf(stderr, "[gpu-live] cannot create a window: rendering offscreen\n");
    }
    // HC09_GPU_EXACT=1: render exactly as the software reference (slower);
    // default: the GPU's fp32 math and hardware samplers.
    bool exact = EnvOn("HC09_GPU_EXACT", false);
#if defined(_WIN32) && !HC09_DEV_TOOLS
    // The release build delay-loads vulkan-1.dll (CMakeLists.txt): no Vulkan driver at all is said here, not by the
    // Windows loader.
    if (!LoadLibraryW(L"vulkan-1.dll"))
        RequirementMissing("This PC has no Vulkan graphics driver (vulkan-1.dll was not found).");
#endif
    bool ok = l->dev.CreateInstance(l->window ? hc09vk::WindowInstanceExtensions() : std::vector<const char*>{}, &err);
    if (ok && l->window && hc09vk::WindowCreateSurface(l->window, l->dev.instance, &l->surface) != VK_SUCCESS)
    {
        err = "vkCreateWin32SurfaceKHR failed";
        ok = false;
    }
    bool native = NativeRendererSelected();
    NativeLive* nl = native ? new NativeLive : nullptr;
    ok = ok && l->dev.CreateDevice(l->surface, &err) &&
         l->dev.ImportHostBuffer(physicalMemory, 0x20000000ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, l->guest, &err) &&
         (native ? nl->renderer.Init(l->dev, l->guest, &err, exact) : l->renderer.Init(l->dev, l->guest, &err, exact));
    // HC09_VSYNC=0: present without waiting for the display (mailbox or
    // immediate). The game still paces itself to its own 60 Hz vblank.
    l->swapchain.vsync = EnvOn("HC09_VSYNC", true);
    // hc09: HC09_UPSCALE=bilinear (or 0): the blit in place of FSR 1; HC09_SHARPEN=0..100 (vk_upscale.h).
    if (const char* e = Hc09Env("HC09_UPSCALE"); e && (!strcmp(e, "0") || !strcmp(e, "bilinear"))) l->swapchain.upscaler.enabled = false;
    if (const char* e = Hc09Env("HC09_SHARPEN"); e && *e) l->swapchain.upscaler.sharpening = std::clamp(float(atof(e)), 0.0f, 100.0f) / 100.0f;
    l->swapchain.stretch = hc09vk::WidescreenStretch();
    // hc09: HC09_HDR=1: HDR10 output when the display offers it; HC09_HDR_PEAK / HC09_HDR_PAPER_WHITE in nits;
    // HC09_HDR_DEBUG=1 (dev): the HDR pass's highlights shown red in SDR (vk_hdr.h).
    if (const char* e = Hc09Env("HC09_HDR"); e && *e && strcmp(e, "0")) l->swapchain.hdrRequested = true;
    if (const char* e = Hc09Env("HC09_HDR_DEBUG")) l->swapchain.hdr.debug = atoi(e);
    if (const char* e = Hc09Env("HC09_HDR_PAPER_WHITE"); e && atof(e) > 0) l->swapchain.hdr.paperWhite = std::max(float(atof(e)), 80.0f);
    if (const char* e = Hc09Env("HC09_HDR_PEAK"); e && atof(e) > 0) l->swapchain.hdr.peak = float(atof(e));
    l->swapchain.hdr.peak = std::max(l->swapchain.hdr.peak, l->swapchain.hdr.paperWhite);
    l->phys = physicalMemory;
    l->hazard = EnvOn("HC09_GPU_HAZARD", false);
    if (ok && l->window) l->swapchainOk = l->swapchain.Create(l->dev, l->surface, 1280, 720, &err);
    if (!ok || (l->window && !l->swapchainOk))
    {
        fprintf(stderr, "[gpu-live] disabled: %s\n", err.c_str());
#if !HC09_DEV_TOOLS
        // The release build cannot go on without a picture (the development build keeps running headless).
        RequirementMissing("The graphics driver could not start the game's renderer: " + err + ".");
#endif
        return;   // leaked on purpose: a half-initialized device is not worth tearing down here
    }
    l->renderer.SetResolveWaits(false);
    if (const char* d = Hc09Env("HC09_GPU_LIVE_DUMP")) l->dumpEvery = strtoull(d, nullptr, 10);
    if (const char* d = Hc09Env("HC09_GPU_LIVE_DUMP_SCALED")) l->dumpScaled = atoi(d) != 0;
    if (const char* d = Hc09Env("HC09_GPU_LIVE_DUMP_AT"))
    {
        l->dumpAt = atof(d);
        const char* c = strchr(d, ',');
        l->dumpAtLeft = c ? strtoull(c + 1, nullptr, 10) : 240;
        if (!l->dumpEvery) l->dumpEvery = ~0ull;   // the directory below; frames picked by time
    }
    const char* dir = Hc09Env("HC09_GPU_LIVE_DIR");
    l->dir = dir && *dir ? dir : "captures/live";
    if (l->dumpEvery)
    {
        std::error_code ec;
        std::filesystem::create_directories(l->dir, ec);
    }
    if (native)
    {
        // The command processor renders nothing (no live observer); D3D's calls feed
        // the native renderer (hooks_native.cpp).
        nl->live = l;
        nl->phys = physicalMemory;
        nl->renderer.SetResolveWaits(false);
        // Resolved data the GPU keeps: the game's CPU accesses write it back first.
        nl->renderer.SetOwnedCallback(GuestWatchAccess);
        GuestSetAccessHook(NativeLiveWriteBack, EnvOn("HC09_NATIVE_WATCH_RESOLVES", false));
        if (const char* m = Hc09Env("HC09_GPU_SNAPSHOT_MAX")) nl->snapMax = uint32_t(strtoul(m, nullptr, 0));
        std::thread([nl] { nl->Worker(); }).detach();
        g_live = l;
        g_native = nl;
        g_nativeFenceSink = nl;
        fprintf(stderr, "[native] native renderer on %s (%s shaders, guest memory imported at %p), %s\n", l->dev.props.deviceName,
                exact ? "exact" : "fast", static_cast<void*>(physicalMemory),
                l->window ? ("presenting in a " + std::to_string(l->swapchain.extent.width) + "x" +
                             std::to_string(l->swapchain.extent.height) + " window").c_str() : "offscreen");
        return;
    }
    // Renderer thread: off with HC09_GPU_THREAD=0 and while frame captures are
    // configured (their page recording is not thread-safe).
    l->threaded = EnvOn("HC09_GPU_THREAD", true) && !Hc09Env("HC09_CAPTURE_FRAME") && !Hc09Env("HC09_RENDER_RANGE");
    // Draw-data snapshots need the renderer thread (they ride on its register writes).
    l->snapshot = l->threaded && EnvOn("HC09_GPU_SNAPSHOT", true);
    if (const char* m = Hc09Env("HC09_GPU_SNAPSHOT_MAX")) l->snapMax = uint32_t(strtoul(m, nullptr, 0));
    l->pipeline = l->threaded && EnvOn("HC09_GPU_PIPELINE", false);
    if (l->pipeline)
    {
        l->hazard = false;   // its check assumes the frame is done at the frame end
        fprintf(stderr, "[gpu-live] HC09_GPU_PIPELINE: the CP runs up to one frame ahead of the renderer\n");
    }
    if (l->threaded)
    {
        l->queue.resize(Live::kQueue);
        l->shadow.resize(xenos::kRegisterCount);
        GpuCopyRegisters(l->shadow.data(), xenos::kRegisterCount);
        GpuLogRegisterWrites(true);
        std::thread([l] { l->Worker(); }).detach();
    }
    g_live = l;
    xenos::SetLiveObserver(l);
    fprintf(stderr, "[gpu-live] rendering every frame on %s (%s shaders, %s, guest memory imported at %p), %s%s\n", l->dev.props.deviceName,
            exact ? "exact" : "fast", l->threaded ? "renderer thread" : "on the CP thread", static_cast<void*>(physicalMemory),
            l->window ? ("presenting in a " + std::to_string(l->swapchain.extent.width) + "x" +
                         std::to_string(l->swapchain.extent.height) + " window").c_str() : "offscreen",
            l->dumpEvery ? (", dumping every " + std::to_string(l->dumpEvery) + " frames to " + l->dir).c_str() : "");
}

bool NativeRendererSelected()
{
    static const bool native = [] {
        const char* e = Hc09Env("HC09_RENDERER");
        if (!e || !*e || !strcmp(e, "auto") || !strcmp(e, "native")) return true;
        if (!strcmp(e, "emulated")) return false;
        fprintf(stderr, "[gpu-live] HC09_RENDERER=%s unknown (native or emulated): native\n", e);
        return true;
    }();
    return native;
}
bool NativeRendererLive()
{
    static const bool on = NativeRendererSelected() && EnvOn("HC09_GPU_LIVE", false);
    return on;
}
d3dn::Backend* NativeLiveBackend() { return g_native; }
bool NativeLiveProfiling() { return g_native && g_native->prof; }
// The fault handler's hook (any game thread): blocks until the renderer thread has
// written back the resolved data the GPU keeps around `pa`.
void NativeLiveWriteBack(uint32_t pa)
{
    NativeLive* nl = g_native;
    if (!nl) return;
    std::unique_lock<std::mutex> lk(nl->qm);
    nl->wbReq.push_back(pa);
    uint64_t my = ++nl->wbSeq;
    nl->wbPending = true;
    nl->qcv.notify_all();
    nl->qcv.wait(lk, [&] { return nl->wbDone >= my; });
}
void NativeLiveAddFrontendNs(int kind, uint64_t ns) { if (g_native) g_native->feNs[kind] += ns; }
void NativeLiveKicked(uint32_t fenceWordPa, uint32_t fence)
{
    NativeLive* nl = g_native;
    if (!nl || !nl->fences) return;
    if (!nl->fencesUp) nl->FencesStart(fenceWordPa);
    nl->Fence(fence);
}
bool NativeLiveFencesOn() { return g_native && g_native->fencesUp; }
void NativeLiveFenceWait(uint32_t cur, uint32_t fence, uint32_t caller, uint32_t type)
{
    if (NativeLive* nl = g_native; nl && nl->fencesUp) nl->WaitFence(cur, fence, caller, type);
}
void NativeLiveFenceNeeded(uint32_t cur, uint32_t fence)
{
    NativeLive* nl = g_native;
    if (!nl || !nl->fencesUp) return;
    bool pending;
    {
        std::lock_guard<std::mutex> lk(nl->fm);
        pending = nl->FencePending(cur, fence);
    }
    if (pending) nl->RequestFlush(fence);
}
