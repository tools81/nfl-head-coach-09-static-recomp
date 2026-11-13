#include "../dev_env.h"
#include "vk_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace hc09vk {

namespace {
// A change bigger than this share of the largest value among the changed constants is a jump, not motion.
constexpr float kJump = 0.25f;
// Kinds drawn more often than this per frame (particles, crowds) are drawn as they are: matching them costs too much.
constexpr size_t kMaxPerKind = 96;
// A constant seen changing between ticks counts as the render loop's for this many frames.
constexpr uint8_t kBetweenFrames = 60;

bool Usable(float f) { return std::isfinite(f) && (f == 0.0f || std::fabs(f) > 1e-30f); }
}  // namespace

void MotionInterpolator::BeginFrame(uint64_t frame, uint64_t tick, float fraction)
{
    frame_ = frame;
    tick_ = tick;
    fraction_ = fraction;
    if (frame % 120 == 0)   // draws unseen for 120 frames
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Entry& e) { return e.lastFrame + 120 < frame; }), list.end());
            it = list.empty() ? entries_.erase(it) : std::next(it);
        }
}

bool MotionInterpolator::Drawn(Entry& e, const float* c, size_t n, float* prevOut)
{
    if (!prevOut) return false;
    const bool had = e.drawnFrame + 1 == frame_ && e.drawn.size() == n;
    if (had) std::memcpy(prevOut, e.drawn.data(), n * 4);
    e.drawn.assign(c, c + n);
    e.drawnFrame = frame_;
    return had;
}

bool MotionInterpolator::Process(uint64_t key, float* c, uint32_t count, uint32_t indices, float* prevOut)
{
    ++draws_;
    const size_t n = size_t(count) * 4;
    std::vector<Entry>& list = entries_[key];
    if (seenFrame_ != frame_) { seen_.clear(); seenFrame_ = frame_; }
    const uint32_t at = seen_[key]++;
    // The same draw in the frame before. Mostly it is the one at the same place in the frame, drawn with the same
    // constants. Else, of this kind's draws not yet matched this frame, the one drawn with the closest constants,
    // compared at up to 32 values spread over the window.
    Entry* e = nullptr;
    bool same = false;
    if (at < list.size() && list[at].lastFrame != frame_ && list[at].last.size() == n && !std::memcmp(list[at].last.data(), c, n * 4))
    {
        e = &list[at];
        same = true;
    }
    const size_t step = std::max<size_t>(1, n / 32);
    float best = 0.0f;
    for (size_t k = 0; k < list.size() && !same; ++k)
    {
        Entry& x = list[k];
        if (x.lastFrame == frame_ || x.last.size() != n) continue;
        float d = 0.0f;
        for (size_t i = 0; i < n && (!e || d < best); i += step) d += std::fabs(c[i] - x.last[i]);
        if (!e || d < best) { e = &x; best = d; }
    }
    if (!e)
    {
        if (list.size() >= kMaxPerKind) return false;
        list.emplace_back();
        e = &list.back();
        e->last.assign(c, c + n);
        e->prev = e->cur = e->last;
        e->blend.assign(count, 0);
        e->between.assign(count, kBetweenFrames);   // the render loop's until seen holding still between ticks
        e->lastTick = e->curTick = tick_;
        e->lastFrame = frame_;
        ++fresh_;
        return Drawn(*e, c, n, prevOut);
    }
    const bool ticked = tick_ != e->lastTick;
    if (ticked)
    {
        // A new tick: the constants that change with it (and never lately between ticks) are blended from the values
        // drawn last (the previous tick's) to the new ones, unless together they jump.
        float most = 0.0f, change = 0.0f;
        bool usable = true, any = false;
        for (uint32_t v = 0; v < count; ++v)
        {
            const float* a = &e->last[v * 4];
            const float* b = c + v * 4;
            const bool changed = std::memcmp(a, b, 16) != 0;
            e->blend[v] = changed && !e->between[v];
            if (changed) ++(e->blend[v] ? tickVecs_ : betweenVecs_);
            if (!e->blend[v]) continue;
            any = true;
            for (int k = 0; k < 4; ++k)
            {
                most = std::max(most, std::max(std::fabs(a[k]), std::fabs(b[k])));
                change = std::max(change, std::fabs(a[k] - b[k]));
                usable = usable && Usable(a[k]) && Usable(b[k]);
            }
        }
        if (any && (!usable || change > kJump * most))
        {
            std::fill(e->blend.begin(), e->blend.end(), uint8_t(0));
            ++jumps_;
        }
        e->prev = e->last;
        e->cur.assign(c, c + n);
        e->curTick = tick_;
    }
    else if (!same)
    {
        // Changed between ticks: those constants are the render loop's (as drawn, now and for a while).
        for (uint32_t v = 0; v < count; ++v)
            if (std::memcmp(&e->last[v * 4], c + v * 4, 16) != 0)
            {
                e->between[v] = kBetweenFrames;
                e->blend[v] = 0;
                ++betweenVecs_;
            }
    }
    for (uint8_t& b : e->between)
        if (b) --b;
    if (!same) std::memcpy(e->last.data(), c, n * 4);
    e->lastTick = tick_;
    e->lastFrame = frame_;
    if (e->curTick != tick_ || fraction_ >= 1.0f) return Drawn(*e, c, n, prevOut);   // drawn at the newest tick's values
    const float t = fraction_;
    // HC09_SMOOTH_MOTION_LOG=<indices>: per frame, the first blended vec4 of draws with that index count, as the
    // game gave it and as drawn (dev).
    static const uint32_t logIndices = [] { const char* e = Hc09Env("HC09_SMOOTH_MOTION_LOG"); return e ? uint32_t(atoi(e)) : 0u; }();
    if (logIndices && indices == logIndices)
        for (uint32_t v = 0; v < count; ++v)
            if (e->blend[v])
            {
                const float* g = c + v * 4;
                const float* p = &e->prev[v * 4];
                const float* q = &e->cur[v * 4];
                std::fprintf(stderr, "[motion-log] frame %llu tick %llu t %.3f c%u game %.4f %.4f %.4f drawn %.4f %.4f %.4f\n",
                             (unsigned long long)frame_, (unsigned long long)tick_, t, v, g[0], g[1], g[2], p[0] + (q[0] - p[0]) * t,
                             p[1] + (q[1] - p[1]) * t, p[2] + (q[2] - p[2]) * t);
                break;
            }
    bool blended = false;
    for (uint32_t v = 0; v < count; ++v)
    {
        if (!e->blend[v]) continue;
        // Only while the game still gives this tick's value (the render loop did not change it since).
        float* out = c + v * 4;
        const float* cur = &e->cur[v * 4];
        if (std::memcmp(out, cur, 16) != 0) continue;
        const float* prev = &e->prev[v * 4];
        for (int k = 0; k < 4; ++k) out[k] = prev[k] + (cur[k] - prev[k]) * t;
        ++blendedVecs_;
        blended = true;
    }
    blendedDraws_ += blended;
    if (blended) ++blendedBySize_[indices];
    return Drawn(*e, c, n, prevOut);
}

void MotionInterpolator::Report()
{
    static const bool on = Hc09Env("HC09_SMOOTH_MOTION_STATS") != nullptr;
    if (!on || frame_ % 600 != 0 || !draws_) return;
    size_t tracked = 0;
    for (const auto& [k, list] : entries_) tracked += list.size();
    std::fprintf(stderr,
                 "[motion] frame %llu: %llu draws, %llu blended (%llu vec4); changed vec4 with a tick %llu, between ticks %llu; "
                 "%llu jumps; %llu new; %zu tracked in %zu kinds\n",
                 (unsigned long long)frame_, (unsigned long long)draws_, (unsigned long long)blendedDraws_, (unsigned long long)blendedVecs_,
                 (unsigned long long)tickVecs_, (unsigned long long)betweenVecs_, (unsigned long long)jumps_, (unsigned long long)fresh_,
                 tracked, entries_.size());
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto& [size, n] : blendedBySize_) top.push_back({ n, size });
    std::sort(top.rbegin(), top.rend());
    std::string line;
    for (size_t i = 0; i < top.size() && i < 8; ++i) line += " " + std::to_string(top[i].second) + ":" + std::to_string(top[i].first);
    if (!line.empty()) std::fprintf(stderr, "[motion]   blended draws by index count (indices:draws):%s\n", line.c_str());
    blendedBySize_.clear();
    draws_ = blendedDraws_ = blendedVecs_ = tickVecs_ = betweenVecs_ = jumps_ = fresh_ = 0;
}

}  // namespace hc09vk
