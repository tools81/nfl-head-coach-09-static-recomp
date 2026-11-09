// hc09: the menus' text at the screen's resolution; see font_hd.h.
//
// The game's glyph cache (sub_82C570E0: r3 the cache, r4 the entry, r6 the glyph, r8 and r9 the style, f1 the size in
// pixels, the font at the callee's 340(r1)) asks the font (vtable +20: r3 the font, r4 the glyph, r7 the style, r8 a
// status out, f1 a scale, f2 the size) for the glyph's bitmap and queues it for its page (sub_82C55B30: op 1, r4 the
// bitmap, r5 the page, r6 x, r7 y; op 0 clears the page r5). A bitmap: +0 its description (+32 its pixels), +4 its
// width w, +8 its height h, +12 and +16 its left and top bearings; its pixels are (w + 1) x (h + 1) bytes of coverage,
// copied (sub_82CE63E8, page r3; sub_82CE5CF0 converts, r4 the page's pixels) to the page as white with the coverage
// as alpha, the next glyph after it.
//
// Here, once the game has placed a glyph, the font is asked again at Factor() times the size, and that bitmap is put
// in the page's copy: on the game's glyph's area (its bitmap's, Factor() times larger), centred alike (the coverage's
// centroids matched, so the two sit the same on the screen). A glyph the font does not give again at that size gets
// the game's own, enlarged.

#include "common/font_hd.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ppc_recomp_shared.h>

#include "vk/dev_env.h"

namespace hc09::font_hd {
namespace {

uint32_t U32(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }

struct Page
{
    uint32_t pa = 0;                 // 0: not known yet (the game has not copied a glyph to it)
    uint32_t width = 0, height = 0;  // the game's
    std::vector<uint8_t> alpha;      // Factor() times as wide and high
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Page> g_pages;   // by the page object's guest address

// The pages' versions by physical address, read without the lock.
constexpr int kSlots = 128;
std::atomic<uint32_t> g_slotPa[kSlots];
std::atomic<uint64_t> g_slotVersion[kSlots];
std::atomic<uint64_t> g_nextVersion{ 1 };

void Touch(uint32_t pa)   // under g_mutex
{
    if (!pa) return;
    const uint64_t v = g_nextVersion.fetch_add(1);
    for (int i = 0; i < kSlots; ++i)
        if (g_slotPa[i].load(std::memory_order_relaxed) == pa)
        {
            g_slotVersion[i].store(v, std::memory_order_release);
            return;
        }
    for (int i = 0; i < kSlots; ++i)
        if (!g_slotPa[i].load(std::memory_order_relaxed))
        {
            g_slotVersion[i].store(v, std::memory_order_release);
            g_slotPa[i].store(pa, std::memory_order_release);
            return;
        }
}

Page& PageOf(uint8_t* base, uint32_t object)   // under g_mutex
{
    Page& p = g_pages[object];
    if (p.alpha.empty())
    {
        p.width = std::clamp(U32(base, object + 8), 1u, 2048u);
        p.height = std::clamp(U32(base, object + 12), 1u, 2048u);
        p.alpha.assign(size_t(p.width) * p.height * Factor() * Factor(), 0);
    }
    return p;
}

// A bitmap's coverage, (w + 1) x (h + 1).
struct Bitmap
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    bool Read(uint8_t* base, uint32_t bitmap)
    {
        w = U32(base, bitmap + 4) + 1;
        h = U32(base, bitmap + 8) + 1;
        if (w > 1024 || h > 1024) return false;
        const uint32_t data = U32(base, U32(base, bitmap) + 32);
        px.assign(base + data, base + data + size_t(w) * h);
        return true;
    }
    bool Centroid(double& cx, double& cy) const   // of the pixels' centres, weighted by coverage
    {
        double sum = 0, sx = 0, sy = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (const uint8_t a = px[size_t(y) * w + x])
                {
                    sum += a;
                    sx += a * (x + 0.5);
                    sy += a * (y + 0.5);
                }
        if (sum <= 0) return false;
        cx = sx / sum;
        cy = sy / sum;
        return true;
    }
};

// The glyph being placed (sub_82C570E0 running on this thread) and what it queued.
struct Placing
{
    bool active = false, queued = false;
    uint32_t page = 0, x = 0, y = 0;
    Bitmap low;
};
thread_local Placing t_placing;
thread_local uint32_t t_copyPage = 0;   // sub_82CE63E8's page, for sub_82CE5CF0

void Put(Page& p, const Placing& g, const Bitmap* high)
{
    const uint32_t k = Factor(), pw = p.width * k, ph = p.height * k;
    // The glyph's area: its bitmap's, k times larger.
    const uint32_t x0 = g.x * k, y0 = g.y * k, x1 = std::min(pw, (g.x + g.low.w) * k), y1 = std::min(ph, (g.y + g.low.h) * k);
    for (uint32_t y = y0; y < y1; ++y)
        std::fill(p.alpha.begin() + size_t(y) * pw + x0, p.alpha.begin() + size_t(y) * pw + x1, uint8_t(0));
    double lx, ly, hx, hy;
    if (high && g.low.Centroid(lx, ly) && high->Centroid(hx, hy))
    {
        const int ox = int(std::lround((g.x + lx) * k - hx)), oy = int(std::lround((g.y + ly) * k - hy));
        for (uint32_t y = 0; y < high->h; ++y)
        {
            const int py = oy + int(y);
            if (py < int(y0) || py >= int(y1)) continue;
            for (uint32_t x = 0; x < high->w; ++x)
            {
                const int px = ox + int(x);
                if (px >= int(x0) && px < int(x1)) p.alpha[size_t(py) * pw + px] = high->px[size_t(y) * high->w + x];
            }
        }
        return;
    }
    // The game's own glyph, enlarged (bilinear).
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const float fx = (x + 0.5f) / k - g.x - 0.5f, fy = (y + 0.5f) / k - g.y - 0.5f;
            const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
            const float tx = fx - ix, ty = fy - iy;
            auto at = [&](int sx, int sy) {
                sx = std::clamp(sx, 0, int(g.low.w) - 1);
                sy = std::clamp(sy, 0, int(g.low.h) - 1);
                return float(g.low.px[size_t(sy) * g.low.w + sx]);
            };
            const float a = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
            p.alpha[size_t(y) * pw + x] = uint8_t(std::clamp(a + 0.5f, 0.0f, 255.0f));
        }
}

}  // namespace

uint32_t Factor()
{
    static const uint32_t k = [] {
        const char* on = Hc09Env("HC09_SHARP_TEXT");
        if (on && *on && (!std::strcmp(on, "0") || !std::strcmp(on, "off"))) return 1u;
        const char* s = Hc09Env("HC09_SCALE");
        return s && *s ? uint32_t(std::clamp(std::atoi(s), 1, 4)) : 1u;
    }();
    return k;
}

uint64_t Version(uint32_t pa)
{
    if (!pa) return 0;
    for (int i = 0; i < kSlots; ++i)
        if (g_slotPa[i].load(std::memory_order_acquire) == pa) return g_slotVersion[i].load(std::memory_order_acquire);
    return 0;
}

bool Image(uint32_t pa, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba, uint32_t& outWidth, uint32_t& outHeight, uint64_t& version)
{
    std::lock_guard lock(g_mutex);
    for (auto& [object, p] : g_pages)
    {
        if (p.pa != pa || p.width != width || p.height != height) continue;
        const uint32_t k = Factor();
        outWidth = width * k;
        outHeight = height * k;
        rgba.resize(p.alpha.size());
        for (size_t i = 0; i < p.alpha.size(); ++i) rgba[i] = (uint32_t(p.alpha[i]) << 24) | 0x00FFFFFFu;
        version = Version(pa);
        return true;
    }
    return false;
}

}  // namespace hc09::font_hd

using namespace hc09::font_hd;

extern "C" PPC_FUNC(__imp__sub_82C570E0);
PPC_FUNC(sub_82C570E0)
{
    if (Factor() <= 1)
    {
        __imp__sub_82C570E0(ctx, base);
        return;
    }
    const PPCContext in = ctx;
    const uint32_t font = U32(base, ctx.r1.u32 + 84);
    t_placing = Placing{};
    t_placing.active = true;
    __imp__sub_82C570E0(ctx, base);
    t_placing.active = false;
    if (!t_placing.queued) return;

    // The glyph again, Factor() times the size.
    Bitmap high;
    bool got = false;
    {
        PPCContext c = in;
        const uint32_t status = in.r1.u32 - 0x100;
        *reinterpret_cast<uint32_t*>(base + status) = 0;
        c.r1.u64 = in.r1.u32 - 0x200;
        c.r3.u64 = font;
        c.r4.u64 = in.r6.u32;
        c.r7.u64 = in.r8.u32;
        c.r8.u64 = status;
        c.f1.f64 = double(in.r9.u32 & 0xFFFF) / 4096.0;
        c.f2.f64 = in.f1.f64 * Factor();
        PPCFunc* rasterize = PPC_LOOKUP_FUNC(base, U32(base, U32(base, font) + 20));
        rasterize(c, base);
        const uint32_t bitmap = c.r3.u32;
        if (U32(base, status) == 1 && bitmap)
        {
            got = high.Read(base, bitmap);
            c.r3.u64 = bitmap;
            sub_82C57718(c, base);   // freed, as the game does
        }
    }
    std::lock_guard lock(g_mutex);
    Page& p = PageOf(base, t_placing.page);
    Put(p, t_placing, got ? &high : nullptr);
    if (Hc09Env("HC09_FONT_LOG"))   // dev
        std::fprintf(stderr, "[font] glyph %04X size %.1f page %08X (pa %08X) at %u,%u: %ux%u, sharp %ux%u%s\n", in.r6.u32, in.f1.f64, t_placing.page, p.pa,
                     t_placing.x, t_placing.y, t_placing.low.w, t_placing.low.h, high.w, high.h, got ? "" : " (not given: enlarged)");
    Touch(p.pa);
}

// Op 0 clears page r5; op 1 queues bitmap r4 for page r5 at (r6, r7).
extern "C" PPC_FUNC(__imp__sub_82C55B30);
PPC_FUNC(sub_82C55B30)
{
    if (Factor() > 1)
    {
        if (ctx.r3.u32 == 1 && t_placing.active)
        {
            t_placing.queued = t_placing.low.Read(base, ctx.r4.u32);
            t_placing.page = ctx.r5.u32;
            t_placing.x = ctx.r6.u32;
            t_placing.y = ctx.r7.u32;
        }
        else if (ctx.r3.u32 == 0 && ctx.r5.u32)
        {
            std::lock_guard lock(g_mutex);
            auto it = g_pages.find(ctx.r5.u32);
            if (it != g_pages.end())
            {
                std::fill(it->second.alpha.begin(), it->second.alpha.end(), uint8_t(0));
                Touch(it->second.pa);
            }
        }
    }
    __imp__sub_82C55B30(ctx, base);
}

// A bitmap's copy to its page (r3): the page's pixels' address, from the conversion it runs.
extern "C" PPC_FUNC(__imp__sub_82CE63E8);
PPC_FUNC(sub_82CE63E8)
{
    t_copyPage = ctx.r3.u32;
    __imp__sub_82CE63E8(ctx, base);
    t_copyPage = 0;
}

extern "C" PPC_FUNC(__imp__sub_82CE5CF0);
PPC_FUNC(sub_82CE5CF0)
{
    if (t_copyPage && Factor() > 1)
    {
        const uint32_t pa = ctx.r4.u32 & 0x1FFFFFFFu;
        std::lock_guard lock(g_mutex);
        auto it = g_pages.find(t_copyPage);
        if (it != g_pages.end() && it->second.pa != pa)
        {
            for (auto& [object, other] : g_pages)   // a page freed and its memory given to another
                if (other.pa == pa) other.pa = 0;
            it->second.pa = pa;
            Touch(pa);
        }
    }
    __imp__sub_82CE5CF0(ctx, base);
}
