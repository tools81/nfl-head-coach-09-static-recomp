#pragma once
// hc09: smooth motion (hc09.ini [Graphics] SmoothMotion, as HC09_SMOOTH_MOTION). The game's simulation runs at 30 Hz;
// its render loop draws faster (FrameRate) and moves the camera every frame, but the players, the ball and other
// simulated things keep their state until the next tick, so they move in 30 Hz steps. That state reaches the draws
// as vertex constants (bone palettes, object matrices). Each draw is matched with the same draw in the frame before:
// among the draws with the same shaders and index range (one per player for the same model), the one whose constants
// were closest. Each of its constants (vec4) that changes only with simulation ticks is drawn between the previous
// tick's value and the new one by the fraction of a tick the frame is past the tick (interp.h MotionFrameState): one
// tick late, but in even steps. Constants that change between ticks (the camera's) and big jumps (cuts) are drawn as
// they are.
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hc09vk {

class MotionInterpolator
{
public:
    // The frame being drawn: the last simulation tick it follows and the fraction of a tick past it.
    void BeginFrame(uint64_t frame, uint64_t tick, float fraction);
    // A draw's vertex constants `c` (`count` vec4, the shader's window), blended in place. `key`: the draw's kind
    // (shaders, index range); draws of the same kind are told apart by their constants.
    void Process(uint64_t key, float* c, uint32_t count, uint32_t indices);
    // Every 600 frames, a line of statistics on stderr (HC09_SMOOTH_MOTION_STATS=1).
    void Report();

private:
    struct Entry
    {
        std::vector<float> last, prev, cur;   // as last drawn by the game; per vec4, the tick before and the newest tick
        std::vector<uint8_t> blend;           // per vec4: drawn blended (changed with the newest tick)
        std::vector<uint8_t> between;         // per vec4: frames left during which it counts as changing between ticks
        uint64_t lastTick = 0, curTick = 0, lastFrame = 0;
    };
    std::unordered_map<uint64_t, std::vector<Entry>> entries_;
    std::unordered_map<uint64_t, uint32_t> seen_;   // occurrences of each kind this frame
    uint64_t seenFrame_ = ~0ull;
    uint64_t frame_ = 0, tick_ = 0;
    float fraction_ = 1.0f;
    // Statistics since the last report.
    std::unordered_map<uint32_t, uint64_t> blendedBySize_;   // blended draws by index count (statistics)
    uint64_t draws_ = 0, blendedDraws_ = 0, blendedVecs_ = 0, tickVecs_ = 0, betweenVecs_ = 0, jumps_ = 0, fresh_ = 0;
};

}  // namespace hc09vk
