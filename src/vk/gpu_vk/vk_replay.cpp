// hc09: the score bar's replay; see vk_replay.h.
//
// Each kept frame: the scene's rectangle (its middle, cut to the layers' shape) resolved if multisampled, halved by
// blits into a mip pyramid until it is less than twice a layer's size (so the last blit does not alias), then blitted
// into the next free layer. A frame's time and layer are noted; a play's frames (ReplayMarkPlay) are the replay, and
// their layers are not recorded over until the next play's replace them.

#include "vk_replay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace hc09vk
{
    namespace
    {
        struct Shared
        {
            std::mutex mutex;
            bool wanted = false;
            uint64_t version = 0;
            VkImageView view = VK_NULL_HANDLE;
            struct Frame
            {
                ReplayClock::time_point time;
                uint32_t layer;
            };
            std::vector<Frame> frames;           // kept frames, oldest first (each layer at most once)
            std::vector<Frame> replay;           // the replay's frames, in order
            std::vector<bool> kept = std::vector<bool>(ReplayRecorder::kLayers);   // layers of the replay
        } g;

        void Barrier(VkCommandBuffer cb, VkPipelineStageFlags from, VkAccessFlags fromAccess, VkPipelineStageFlags to, VkAccessFlags toAccess)
        {
            VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            b.srcAccessMask = fromAccess;
            b.dstAccessMask = toAccess;
            vkCmdPipelineBarrier(cb, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
        }

        void ToGeneral(VkCommandBuffer cb, const Image& img)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        void Blit(VkCommandBuffer cb, VkImage src, uint32_t srcLevel, const int32_t s[4], VkImage dst, uint32_t dstLevel, uint32_t dstLayer,
                  int32_t dw, int32_t dh)
        {
            VkImageBlit b{};
            b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, srcLevel, 0, 1 };
            b.srcOffsets[0] = { s[0], s[1], 0 };
            b.srcOffsets[1] = { s[2], s[3], 1 };
            b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, dstLevel, dstLayer, 1 };
            b.dstOffsets[1] = { dw, dh, 1 };
            vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        }
    }

    void ReplayRecorder::Capture(Device& dev, VkCommandBuffer cb, const Image& src, VkSampleCountFlagBits samples, float x, float y,
                                 float w, float h, std::vector<Image>& retired)
    {
        const auto now = ReplayClock::now();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (!g.wanted)
                return;
        }
        if (std::chrono::duration<double>(now - last_).count() < kFrameSeconds * 0.9 || w < 64 || h < 32)
            return;
        last_ = now;

        // The rectangle: the scene's middle, cut to the layers' shape.
        const float aspect = float(kWidth) / kHeight;
        float cw = w, ch = h;
        if (cw / ch > aspect)
            cw = ch * aspect;
        else
            ch = cw / aspect;
        int32_t rect[4] = { int32_t(x + (w - cw) / 2), int32_t(y + (h - ch) / 2), 0, 0 };
        rect[2] = std::min<int32_t>(rect[0] + int32_t(cw), int32_t(src.width));
        rect[3] = std::min<int32_t>(rect[1] + int32_t(ch), int32_t(src.height));
        rect[0] = std::max(rect[0], 0), rect[1] = std::max(rect[1], 0);
        const int32_t rw = rect[2] - rect[0], rh = rect[3] - rect[1];
        if (rw < 32 || rh < 16)
            return;

        bool fresh = false;
        if (!frames_.image)
        {
            if (!dev.CreateImage(VK_IMAGE_VIEW_TYPE_2D_ARRAY, kWidth, kHeight, kLayers, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, frames_))
                return;
            ToGeneral(cb, frames_);
            std::lock_guard<std::mutex> lock(g.mutex);
            g.view = frames_.view;
            ++g.version;
            fresh = true;
        }
        (void)fresh;
        if (samples != VK_SAMPLE_COUNT_1_BIT && (resolved_.width != src.width || resolved_.height != src.height || resolved_.format != src.format))
        {
            if (resolved_.image)
                retired.push_back(resolved_);
            resolved_ = Image{};
            if (!dev.CreateImage2D(src.width, src.height, src.format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, resolved_))
                return;
            ToGeneral(cb, resolved_);
        }
        // The pyramid: the rectangle halved, then halved again while a level is at least twice a layer's width.
        const uint32_t pw = std::max<uint32_t>(1, uint32_t(rw) / 2), ph = std::max<uint32_t>(1, uint32_t(rh) / 2);
        uint32_t levels = 1;
        while ((pw >> levels) >= 2 * kWidth && (ph >> levels) >= 1)
            ++levels;
        if (pyramid_.width != pw || pyramid_.height != ph || pyramid_.mipLevels != levels)
        {
            if (pyramid_.image)
                retired.push_back(pyramid_);
            pyramid_ = Image{};
            if (!dev.CreateImage2D(pw, ph, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, pyramid_,
                                   levels))
                return;
            ToGeneral(cb, pyramid_);
        }

        // The next layer not of the replay.
        uint32_t layer = 0;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            uint32_t tries = 0;
            while (g.kept[next_ % kLayers] && tries++ < kLayers)
                ++next_;
            if (tries >= kLayers)
                return;
            layer = next_++ % kLayers;
        }

        Barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImage from = src.image;
        if (samples != VK_SAMPLE_COUNT_1_BIT)
        {
            VkImageResolve r{};
            r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.srcOffset = r.dstOffset = { rect[0], rect[1], 0 };
            r.extent = { uint32_t(rw), uint32_t(rh), 1 };
            vkCmdResolveImage(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, resolved_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            from = resolved_.image;
        }
        Blit(cb, from, 0, rect, pyramid_.image, 0, 0, int32_t(pw), int32_t(ph));
        for (uint32_t l = 1; l < levels; ++l)
        {
            Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const int32_t s[4] = { 0, 0, int32_t(std::max(1u, pw >> (l - 1))), int32_t(std::max(1u, ph >> (l - 1))) };
            Blit(cb, pyramid_.image, l - 1, s, pyramid_.image, l, 0, int32_t(std::max(1u, pw >> l)), int32_t(std::max(1u, ph >> l)));
        }
        Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const int32_t top[4] = { 0, 0, int32_t(std::max(1u, pw >> (levels - 1))), int32_t(std::max(1u, ph >> (levels - 1))) };
        Blit(cb, pyramid_.image, levels - 1, top, frames_.image, 0, layer, int32_t(kWidth), int32_t(kHeight));
        // The scene's target is written again after this; the layer is sampled by the overlay later.
        Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

        std::lock_guard<std::mutex> lock(g.mutex);
        auto& frames = g.frames;
        frames.erase(std::remove_if(frames.begin(), frames.end(), [&](const Shared::Frame& f) { return f.layer == layer; }), frames.end());
        frames.push_back({ now, layer });
    }

    void ReplayRecorder::Destroy(Device& dev)
    {
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.view = VK_NULL_HANDLE;
            ++g.version;
            g.frames.clear();
            g.replay.clear();
            std::fill(g.kept.begin(), g.kept.end(), false);
        }
        for (Image* img : { &frames_, &resolved_, &pyramid_ })
            if (img->image)
                dev.DestroyImage(*img);
    }

    ReplayFrames ReplayView()
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        return { g.view, g.version };
    }

    void SetReplayWanted(bool wanted)
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.wanted = wanted;
    }

    void ReplayMarkPlay(ReplayClock::time_point start, ReplayClock::time_point end)
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        std::vector<Shared::Frame> replay;
        for (const auto& f : g.frames)
            if (f.time >= start && f.time <= std::min(end, start + std::chrono::seconds(12)))   // a play's first 12 seconds
                replay.push_back(f);
        // At most most of the layers (its start), so recording goes on.
        const size_t most = ReplayRecorder::kLayers - 48;
        if (replay.size() > most)
            replay.erase(replay.begin() + most, replay.end());
        if (std::getenv("HC09_SCORE_BAR_LOG"))
            std::fprintf(stderr, "[replay] play of %.1f s: %zu of %zu frames kept\n", std::chrono::duration<double>(end - start).count(), replay.size(),
                         g.frames.size());
        if (replay.size() < 10)
            return;   // too short to be a play (or not recorded): the last replay stays
        std::fill(g.kept.begin(), g.kept.end(), false);
        for (const auto& f : replay)
            g.kept[f.layer] = true;
        g.replay = std::move(replay);
    }

    bool ReplayFrameAt(double seconds, uint32_t& layer)
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.replay.empty() || !g.view)
            return false;
        // As it was recorded (frames are not evenly spaced: only frames with the 3D scene are kept), looping.
        const ReplayClock::time_point first = g.replay.front().time;
        const double length = std::chrono::duration<double>(g.replay.back().time - first).count() + ReplayRecorder::kFrameSeconds;
        const double t = std::fmod(std::max(0.0, seconds), length);
        const auto at = first + std::chrono::duration_cast<ReplayClock::duration>(std::chrono::duration<double>(t));
        auto it = std::upper_bound(g.replay.begin(), g.replay.end(), at, [](ReplayClock::time_point v, const Shared::Frame& f) { return v < f.time; });
        layer = (it == g.replay.begin() ? it : it - 1)->layer;
        return true;
    }
}
