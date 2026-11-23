// hc09: breath in the cold (roadmap 16); see vk_breath.h.
#include "vk_breath.h"

#include "../dev_env.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc09vk {

namespace {

// Before the translated vertex shader's motion pass (its clip position `pos` is final there): the vertex's clip
// position (as the ray tracer's capture takes it, before widescreen's squeeze) added to the draw's slot.
constexpr const char* kAt = "    xeMcur = vec3(0.0); xeMprev = vec3(0.0);\n";
constexpr const char* kDeclaration = "layout(std430, set = 0, binding = 10) buffer XeHeads { uint xeHeads[]; };\n";
constexpr const char* kSum = R"GLSL(    if (D.clip.w != 0u)   // hc09: the helmets' sums (vk_breath.h)
    {
        bool hxy = (D.vte & 1u) != 0u, hw0 = (D.vte & 4u) != 0u;
        float hiw = hw0 ? (pos.w != 0.0 ? 1.0 / pos.w : 1.0) : pos.w;
        float hcw = hiw != 0.0 ? 1.0 / hiw : 1.0;
        vec2 hq = (hxy ? pos.xy : pos.xy * hiw) * hcw;
        uint hs = (D.clip.w - 1u) * 8u;
        atomicAdd(xeHeads[hs], 1u);
        atomicAdd(xeHeads[hs + 1u], uint(int(round(hq.x * 64.0))));
        atomicAdd(xeHeads[hs + 2u], uint(int(round(hq.y * 64.0))));
        atomicAdd(xeHeads[hs + 3u], uint(int(round(hcw * 64.0))));
    }
)GLSL";

double Now()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

bool HeadPatch(uint64_t vsHash, std::string& glsl)
{
    if (vsHash != kHelmetVs) return false;
    const size_t main = glsl.find("void main()"), at = glsl.find(kAt);
    if (main == std::string::npos || at == std::string::npos || at < main) return false;
    glsl.insert(at, kSum);
    glsl.insert(main, kDeclaration);
    return true;
}

float Breath::Random()
{
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return float(random_ >> 8) / float(1u << 24);
}

void Breath::Begin(uint64_t frame, uint32_t* heads)
{
    RingFrame& r = ring_[frame % kHeadRing];
    if (r.frame == frame) return;
    r.frame = frame;
    r.slots = 0;
    r.camera = false;
    // The GPU finished with this part of the buffer kHeadRing frames ago.
    std::memset(heads + (frame % kHeadRing) * kHeadSlots * kHeadWords, 0, kHeadSlots * kHeadWords * 4);
}

uint32_t Breath::Helmet(uint64_t frame, const float* model, uint32_t* heads)
{
    Begin(frame, heads);
    RingFrame& r = ring_[frame % kHeadRing];
    // The model's place: its translation (c183) tells one player from the next; its z axis (c182) is where he faces.
    const float key[3] = { model[12], model[13], model[14] };
    uint32_t slot = 0;
    for (; slot < r.slots; ++slot)
        if (std::fabs(r.keys[slot][0] - key[0]) + std::fabs(r.keys[slot][1] - key[1]) + std::fabs(r.keys[slot][2] - key[2]) < 1e-3f) break;
    if (slot == r.slots)
    {
        if (r.slots == kHeadSlots) return 0;
        ++r.slots;
        std::memcpy(r.keys[slot], key, sizeof(key));
        float f[3] = { model[8], model[9], model[10] };
        const float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (int i = 0; i < 3; ++i) r.forward[slot][i] = n > 1e-6f ? f[i] / n : (i == 2 ? 1.0f : 0.0f);
    }
    return uint32_t(frame % kHeadRing) * kHeadSlots + slot + 1;
}

void Breath::Camera(uint64_t frame, const float* worldView, const float* projection, uint32_t* heads)
{
    Begin(frame, heads);
    RingFrame& r = ring_[frame % kHeadRing];
    std::memcpy(r.worldView, worldView, sizeof(r.worldView));
    std::memcpy(r.projection, projection, sizeof(r.projection));
    r.camera = true;
}

uint32_t Breath::Update(uint64_t frame, const uint32_t* heads, const float* worldView, bool breathe, float* out)
{
    const double now = Now();
    const float dt = last_ < 0.0 ? 0.0f : float(std::min(now - last_, 0.1));
    last_ = now;

    // The heads of kHeadRing - 1 frames back, in the world.
    if (breathe && frame >= kHeadRing - 1)
    {
        const uint64_t then = frame - (kHeadRing - 1);
        const RingFrame& r = ring_[then % kHeadRing];
        if (r.frame == then && r.camera)
        {
            const float* a = r.worldView;   // x, y and z axes and origin in view space
            const float det = a[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (a[1] * a[8] - a[7] * a[2]) + a[6] * (a[1] * a[5] - a[4] * a[2]);
            for (uint32_t s = 0; s < r.slots && std::fabs(det) > 1e-9f; ++s)
            {
                const uint32_t* h = heads + ((then % kHeadRing) * kHeadSlots + s) * kHeadWords;
                if (h[0] < 30) continue;
                const float n = float(h[0]) * 64.0f;
                const float cx = float(int32_t(h[1])) / n, cy = float(int32_t(h[2])) / n, cw = float(int32_t(h[3])) / n;
                if (cw <= 0.05f) continue;
                // Clip to view (the field camera's projection), then view to world.
                const float v[3] = { (cx - r.projection[2] * cw) / r.projection[0] - a[9], (cy - r.projection[3] * cw) / r.projection[1] - a[10],
                                     cw - a[11] };
                const float X = (v[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (v[1] * a[8] - a[7] * v[2]) + a[6] * (v[1] * a[5] - a[4] * v[2])) / det;
                const float Y = (a[0] * (v[1] * a[8] - a[7] * v[2]) - v[0] * (a[1] * a[8] - a[7] * a[2]) + a[6] * (a[1] * v[2] - v[1] * a[2])) / det;
                const float Z = (a[0] * (a[4] * v[2] - v[1] * a[5]) - a[3] * (a[1] * v[2] - v[1] * a[2]) + v[0] * (a[1] * a[5] - a[4] * a[2])) / det;
                static const bool log = Hc09Env("HC09_BREATH_LOG") != nullptr;   // dev: the heads, every 300 frames
                if (log && frame % 300 == 0)
                    fprintf(stderr, "[breath] frame %llu slot %u/%u: %u vertices, key %.2f %.2f %.2f, head %.2f %.2f %.2f, forward %.2f %.2f %.2f\n",
                            (unsigned long long)frame, s, r.slots, h[0], r.keys[s][0], r.keys[s][1], r.keys[s][2], X, Y, Z, r.forward[s][0],
                            r.forward[s][1], r.forward[s][2]);
                // Now and then (every 2.2 seconds or so) a breath out, from the mouth: in front of the helmet's middle
                // and a little below it, forward and up.
                if (Random() >= dt / 2.2f || puffs_.size() >= kMaxPuffs) continue;
                const float* f = r.forward[s];
                Puff p;
                p.pos[0] = X + f[0] * 0.1f;
                p.pos[1] = Y + f[1] * 0.1f - 0.08f;
                p.pos[2] = Z + f[2] * 0.1f;
                for (int i = 0; i < 3; ++i) p.vel[i] = f[i] * (0.45f + 0.25f * Random()) + (Random() - 0.5f) * 0.1f;
                p.vel[1] += 0.05f;
                p.age = 0.0f;
                p.life = 0.9f + 0.5f * Random();
                p.seed = Random();
                puffs_.push_back(p);
            }
        }
    }

    // The puffs drift, slow down and rise a little as they cool.
    for (Puff& p : puffs_)
    {
        p.age += dt;
        const float drag = std::exp(-2.8f * dt);
        for (int i = 0; i < 3; ++i) p.pos[i] += p.vel[i] * dt, p.vel[i] *= drag;
        p.vel[1] += 0.04f * dt;
    }
    puffs_.erase(std::remove_if(puffs_.begin(), puffs_.end(), [](const Puff& p) { return p.age >= p.life; }), puffs_.end());

    // For this frame's camera.
    const float* a = worldView;
    const float scale = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    uint32_t count = 0;
    for (const Puff& p : puffs_)
    {
        float* o = out + count * 8;
        for (int i = 0; i < 3; ++i) o[i] = a[i] * p.pos[0] + a[3 + i] * p.pos[1] + a[6 + i] * p.pos[2] + a[9 + i];
        const float t = p.age / p.life;
        // A breath swells to about a foot and a half across as it goes, and thins out within a second or so.
        o[3] = scale * (0.05f + 0.19f * (1.0f - std::exp(-p.age * 3.0f)));
        o[4] = 0.75f * std::min(1.0f, p.age / 0.08f) * std::pow(1.0f - t, 1.3f);
        o[5] = p.age;
        o[6] = p.seed;
        o[7] = 0.0f;
        if (++count == kMaxPuffs) break;
    }
    return count;
}

}  // namespace hc09vk
