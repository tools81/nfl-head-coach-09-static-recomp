#pragma once

// Replacement images for the game's textures, shared by the D3D12 renderer (texture_cache.cpp) and the Vulkan
// renderer (src/vk/vk_replacements.cpp):
//   textures\swappables\<library>\<id>.png  a swappable front-end image (portraits, logos), swappable_images.h
//   textures\hashed\<content hash>.png      any 2D texture by its contents (16 hex digits, as HC09_DUMP_HASHED
//                                           names the game's images)
// An edition's own folder (hc27\textures\...) comes before the shared one. HC09_NO_REPLACEMENTS=1 turns them off.

#include <cstdint>
#include <string>
#include <vector>

namespace hc09::gpu
{
    struct SwappableImage;

    bool ReplacementsEnabled();

    // The file replacing a swappable image; empty if there is none.
    std::wstring SwappableReplacementPath(const SwappableImage& image);

    // The file replacing the 2D texture with this content hash; null if there is none.
    const std::wstring* HashedReplacementPath(uint64_t contentHash);

    // The content hash of a single 2D texture (fetch constant words, as the register file holds them): its base
    // level and its mips as stored in guest memory, hashed as the D3D12 texture cache hashes them. False for cube
    // maps, volumes, stacked textures and formats the D3D12 renderer does not decode.
    bool TextureContentHash(const uint32_t fetchWords[6], uint64_t& hash);

    // A replacement image as straight RGBA8 (R, G, B, A bytes), halved until it is at most twice the game's size
    // (these images are often sampled without mips, so a much larger one would alias). False if it is unreadable.
    bool LoadReplacementPixels(const std::wstring& path, uint32_t originalWidth, uint32_t originalHeight,
        std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height);
}
