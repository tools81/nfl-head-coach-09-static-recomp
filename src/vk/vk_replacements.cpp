// The replacement images (gpu/texture_replacements.h: HC27's portraits, logos, ...) in the Vulkan renderer,
// through its texture override (gpu_vk/vk_native.h). `set` names the replacement file of the swappable image at a
// texture's address, so the texture cache keeps one texture per image; `replace` then swaps the decoded base level
// for that file, or, for any other 2D texture, for a file named by its content hash. The decoder has already
// applied the fetch swizzle, so a file's colours (as drawn) go in unchanged. The texture keeps the game's size
// (vk_native.cpp sets it after a replacement): the shaders' texel units.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "score_bar.h"
#include "vk_native.h"
#include "xenos_texture.h"
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

    // HC09_DUMP_HASHED=1: each 2D texture the game loads, once per content, as build\hashed\<content hash>.png in
    // the colours a replacement of it uses (texture_replacements.h; tools/hdpack makes replacements of them). A
    // texture whose memory the CPU keeps rewriting (videos, drawn-on images) is not a texture to replace: after its
    // third content at one address, that address's files are deleted and it is not dumped again.
    constexpr size_t kDynamic = 3;
    std::mutex g_dumpMutex;
    std::unordered_map<uint64_t, bool> g_dumped;
    std::unordered_map<uint32_t, std::vector<uint64_t>> g_dumpedAt;   // contents seen at an address; kDynamic+: dynamic

    // Whether a texture is still dumped (not at a dynamic address): the renderer decodes it on the CPU for the dump.
    bool Dumping(const xenos::TextureFetch& tf)
    {
        std::lock_guard lock(g_dumpMutex);
        auto it = g_dumpedAt.find(tf.baseAddress() & 0x1FFFFFFFu);
        return it == g_dumpedAt.end() || it->second.size() <= kDynamic - 1;
    }

    void Dump(const xenos::TextureFetch& tf, const xenos::Image& img)
    {
        uint64_t hash = 0;
        if (!IsSingle2D(tf) || img.width < 8 || img.height < 8 || !hc09::gpu::TextureContentHash(tf.dword, hash))
            return;
        std::mutex& mutex = g_dumpMutex;
        auto& done = g_dumped;
        auto& byAddress = g_dumpedAt;
        auto pathOf = [](uint64_t h) {
            char path[64];
            std::snprintf(path, sizeof(path), "build/hashed/%016llX.png", (unsigned long long)h);
            return std::string(path);
        };
        std::vector<uint64_t> drop;
        {
            std::lock_guard lock(mutex);
            std::vector<uint64_t>& seen = byAddress[tf.baseAddress() & 0x1FFFFFFFu];
            if (seen.size() > kDynamic) return;
            if (std::find(seen.begin(), seen.end(), hash) == seen.end()) seen.push_back(hash);
            if (seen.size() == kDynamic)
            {
                drop = seen;
                seen.push_back(0);   // dynamic from now on
            }
            else if (!done.try_emplace(hash, true).second)
                return;
        }
        for (uint64_t h : drop) std::filesystem::remove(pathOf(h));
        if (!drop.empty()) return;
        static const bool folder = [] { std::filesystem::create_directories("build/hashed"); return true; }();
        (void)folder;
        const std::string path = pathOf(hash);
        if (!std::filesystem::exists(path)) xenos::WritePng(path, img);
    }

    // A team's logo the score bar draws (score_bar.h): decoded on the CPU, and given to it as drawn.
    bool LogoFor(const xenos::TextureFetch& tf)
    {
        uint64_t hash = 0;
        return IsSingle2D(tf) && hc09::gpu::TextureContentHash(tf.dword, hash) && ScoreBarWantsTexture(hash);
    }

    void OfferLogo(const xenos::TextureFetch& tf, const xenos::Image& img)
    {
        uint64_t hash = 0;
        if (IsSingle2D(tf) && hc09::gpu::TextureContentHash(tf.dword, hash) && ScoreBarWantsTexture(hash))
            ScoreBarOfferTexture(hash, img);
    }

    bool Has(const xenos::TextureFetch& tf, uint32_t set)
    {
        uint64_t hash = 0;
        return !FileFor(tf, set, hash).empty() || LogoFor(tf);
    }

    bool ReplaceFile(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img);

    bool Replace(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img)
    {
        const bool replaced = ReplaceFile(tf, set, img);
        OfferLogo(tf, img);
        return replaced;
    }

    bool ReplaceFile(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img)
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
        if (std::getenv("HC09_DUMP_HASHED")) { override.dump = Dump; override.dumping = Dumping; }
        hc09vk::SetTextureOverride(override);
    }
}
