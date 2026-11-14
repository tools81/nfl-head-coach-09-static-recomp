#pragma once

// Portrait pictures: the game draws textures\swappables\<library>\<portrait id>.png in place of a portrait
// (src/gpu/swappable_images.h).

#include <windows.h>

#include <filesystem>

namespace hc09::editor
{
    // The picture as a `size` x `size` bitmap over `background`, or null if it cannot be read.
    HBITMAP LoadPreview(const std::filesystem::path& path, int size, COLORREF background);
    // Any picture Windows can read (PNG, JPEG, BMP, ...) as a square PNG of `size`: its largest centred
    // square from the top, transparency kept.
    bool ImportPicture(const std::filesystem::path& source, const std::filesystem::path& destination, int size);
}
