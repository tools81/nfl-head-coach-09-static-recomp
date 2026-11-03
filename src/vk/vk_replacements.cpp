// The replacement images (gpu/texture_replacements.h: HC27's portraits, logos, ...) in the Vulkan renderer,
// through its texture override (gpu_vk/vk_native.h). `set` names the replacement file of the swappable image at a
// texture's address, so the texture cache keeps one texture per image; `replace` then swaps the decoded base level
// for that file, or, for any other 2D texture, for a file named by its content hash. The decoder has already
// applied the fetch swizzle, so a file's colours (as drawn) go in unchanged. The texture keeps the game's size
// (vk_native.cpp sets it after a replacement): the shaders' texel units.

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vk_native.h"
#include "gpu/swappable_images.h"
#include "gpu/texture_replacements.h"

namespace
{
    std::mutex g_mutex;
    std::vector<std::wstring> g_files;                     // set value - 1 -> file
    std::unordered_map<std::wstring, uint32_t> g_fileSets;  // file -> set value
    std::map<std::wstring, xenos::Image> g_images;         // decoded files (empty: unreadable)

    bool IsSingle2D(const xenos::TextureFetch& tf)
    {
        const uint32_t dimension = (tf.dword[5] >> 9) & 3;
        const bool stacked = (tf.dword[1] >> 10) & 1;
        return tf.valid() && dimension == 1 && !stacked;
    }

    uint32_t Set(const xenos::TextureFetch& tf)
    {
        if (!hc09::gpu::ReplacementsEnabled() || !IsSingle2D(tf))
            return 0;
        const auto image = hc09::gpu::FindSwappableImage(tf.baseAddress() & 0x1FFFFFFF);
        if (!image)
            return 0;
        std::wstring path = hc09::gpu::SwappableReplacementPath(*image);
        if (path.empty())
            return 0;
        std::lock_guard lock(g_mutex);
        auto [it, inserted] = g_fileSets.try_emplace(path, uint32_t(g_files.size() + 1));
        if (inserted)
            g_files.push_back(std::move(path));
        return it->second;
    }

    // The file replacing the texture; empty if there is none (`hash`: its content hash, when it was needed).
    std::wstring FileFor(const xenos::TextureFetch& tf, uint32_t set, uint64_t& hash)
    {
        if (!hc09::gpu::ReplacementsEnabled() || !IsSingle2D(tf))
            return {};
        if (set)
        {
            std::lock_guard lock(g_mutex);
            return set <= g_files.size() ? g_files[set - 1] : std::wstring();
        }
        const std::wstring* file = hc09::gpu::TextureContentHash(tf.dword, hash) ? hc09::gpu::HashedReplacementPath(hash) : nullptr;
        return file ? *file : std::wstring();
    }

    bool Has(const xenos::TextureFetch& tf, uint32_t set)
    {
        uint64_t hash = 0;
        return !FileFor(tf, set, hash).empty();
    }

    bool Replace(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img)
    {
        uint64_t hash = 0;
        const std::wstring path = FileFor(tf, set, hash);
        if (path.empty())
            return false;

        std::lock_guard lock(g_mutex);
        auto [it, inserted] = g_images.try_emplace(path);
        if (inserted)
        {
            std::vector<uint8_t> pixels;
            uint32_t width = 0, height = 0;
            if (hc09::gpu::LoadReplacementPixels(path, img.width, img.height, pixels, width, height))
            {
                it->second.width = width;
                it->second.height = height;
                it->second.rgba.resize(size_t(width) * height);
                std::memcpy(it->second.rgba.data(), pixels.data(), pixels.size());
                if (set)
                    std::fprintf(stderr, "[texture] %ls replaces a swappable image (%ux%u, the game's is %ux%u)\n", path.c_str(), width,
                        height, img.width, img.height);
                else
                    std::fprintf(stderr, "[texture] %ls replaces texture %016llX (%ux%u, the game's is %ux%u)\n", path.c_str(),
                        (unsigned long long)hash, width, height, img.width, img.height);
            }
        }
        if (it->second.rgba.empty())
            return false;
        img = it->second;
        return true;
    }
}

namespace hc09::vk
{
    // gpu_live.cpp, before the renderer starts.
    void InstallReplacements()
    {
        hc09vk::TextureOverride override;
        override.set = Set;
        override.replace = Replace;
        override.has = Has;
        hc09vk::SetTextureOverride(override);
    }
}
