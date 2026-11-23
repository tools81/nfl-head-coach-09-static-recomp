#pragma once
// hc09: breath in the cold (roadmap 16). The game has breath of its own (a particle emitter per player, PclGroupBreath0)
// but it never shows, so players' breath is drawn here: puffs of vapour from each helmet, now and then, that drift
// off, swell and fade.
//
// Where the heads are: each player's helmet (and facemask) draws (vertex shader kHelmetVs) add up their vertices' clip
// positions in a small buffer (draw binding 10, HeadPatch; the draw's slot in DrawConsts::clip[3]), one slot a player
// (told apart by the model's place in the world, c180..c183). A few frames later, when the GPU is done with them, the
// sums are read back and taken to the world (with that frame's field camera): the middle of each helmet. Which way the
// player faces is the model's axis in the world. The puffs live on the CPU and are drawn by the scene effects
// (SceneEffects::SetBreath, a pass of its own).
#include <cstdint>
#include <string>
#include <vector>

namespace hc09vk {

// The helmets' and facemasks' vertex shader.
constexpr uint64_t kHelmetVs = 0x590ce12dae1d6099ull;
constexpr uint32_t kHeadSlots = 64, kHeadRing = 4, kHeadWords = 8;   // per slot: count, x, y, w sums (64ths)
constexpr uint32_t kHeadBufferBytes = kHeadSlots * kHeadRing * kHeadWords * 4;
constexpr uint32_t kMaxPuffs = 64;                                   // two vec4 each (SceneEffects::SetBreath)

// Adds the helmets' sums to the translated vertex shader `glsl` with that hash. Returns whether it did.
bool HeadPatch(uint64_t vsHash, std::string& glsl);

class Breath
{
public:
    // A helmet draw of this frame's scene: its model's place (c180..c183, four vec4) in the world. Returns the value for
    // DrawConsts::clip[3] (slot + 1; 0: none). `heads`: the mapped head buffer (kHeadBufferBytes).
    uint32_t Helmet(uint64_t frame, const float* model, uint32_t* heads);
    // The field's camera this frame (as SceneEffects::SetFootprints's world axes, and the projection's scale and
    // centre: sx, sy, cx, cy), for taking the heads' sums back to the world.
    void Camera(uint64_t frame, const float* worldView, const float* projection, uint32_t* heads);
    // Once a frame, before the scene effects: the heads of a few frames back read, the puffs moved, new ones breathed
    // out (when `breathe`), and written for this frame's camera (`worldView`) to `out` (kMaxPuffs x 8 floats: centre in
    // view space and radius; opacity, age, seed). Returns how many.
    uint32_t Update(uint64_t frame, const uint32_t* heads, const float* worldView, bool breathe, float* out);

private:
    struct RingFrame
    {
        uint64_t frame = ~0ull;
        uint32_t slots = 0;
        float keys[kHeadSlots][3];
        float forward[kHeadSlots][3];
        bool camera = false;
        float worldView[12], projection[4];
    };
    struct Puff
    {
        float pos[3], vel[3], age, life, seed;
    };
    RingFrame ring_[kHeadRing];
    std::vector<Puff> puffs_;
    double last_ = -1.0;
    uint32_t random_ = 0x9E3779B9u;
    float Random();
    // A new frame's part of the head buffer cleared (the GPU finished with it kHeadRing frames ago).
    void Begin(uint64_t frame, uint32_t* heads);
};

}  // namespace hc09vk
