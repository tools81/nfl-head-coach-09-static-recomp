#pragma once
// hc09: the score bar's replay (score_bar.h): the 3D scene's last seconds, recorded small (the shape of the clipboard's
// score box, which plays them) as the layers of one array image, so the last play can loop in the box.
//
// The native renderer records (Capture, at the scene's first resolve of a frame, before the HUD is drawn); the score
// bar says when to record and marks each play's time; the swapchain's overlay samples the layers (all on the
// renderer's thread).

#include <chrono>
#include <cstdint>
#include <vector>

#include "vk_device.h"

namespace hc09vk
{
    using ReplayClock = std::chrono::steady_clock;

    class ReplayRecorder
    {
    public:
        // A layer's size (about the box's shape, 2.1 : 1), how many layers, and how often a frame is kept.
        static constexpr uint32_t kWidth = 384, kHeight = 182, kLayers = 256;
        static constexpr double kFrameSeconds = 1.0 / 25;

        // When due (and wanted, SetReplayWanted), keeps the scene: `src` (`samples` per pixel, GENERAL), the scene's
        // rectangle in its pixels. `cb` is outside a render pass. Images replaced go to `retired`.
        void Capture(Device& dev, VkCommandBuffer cb, const Image& src, VkSampleCountFlagBits samples, float x, float y, float w, float h,
                     std::vector<Image>& retired);
        void Destroy(Device& dev);

    private:
        Image frames_;      // the layers (R8G8B8A8, GENERAL)
        Image resolved_;    // a multisampled scene, resolved
        Image pyramid_;     // the scene's rectangle halved (mips) toward the layers' size
        uint32_t next_ = 0;
        ReplayClock::time_point last_{};
    };

    // The layers, for the overlay: their view (2D array, GENERAL) and a version that changes with it.
    struct ReplayFrames
    {
        VkImageView view = VK_NULL_HANDLE;
        uint64_t version = 0;
    };
    ReplayFrames ReplayView();

    // The score bar: whether to record (a game's scenes are being drawn), and each play as it ends (its first and
    // last moments): the frames between are kept as the replay until the next play.
    void SetReplayWanted(bool wanted);
    void ReplayMarkPlay(ReplayClock::time_point start, ReplayClock::time_point end);

    // The replay's frame at `seconds` from its start, looping: its layer; false while there is none.
    bool ReplayFrameAt(double seconds, uint32_t& layer);
}
