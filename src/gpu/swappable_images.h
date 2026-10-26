#pragma once

// The front end's "swappable" images (player and coach portraits, team logos, helmets, ...)
// are loaded by EA's Ion::SwappableImageManager from libraries such as
// headcoach/swappables/PlayerPortraits, each image chosen by a numeric id (a player's portrait
// id is PLAY.PSXP in the roster). Hooks on the manager record which image each texture holds, so
// the renderer can draw a replacement from textures/swappables/<library>/<id>.png instead.

#include <cstdint>
#include <optional>
#include <string>

namespace hc09::gpu
{
    struct SwappableImage
    {
        std::string library;  // e.g. "PlayerPortraits"
        uint32_t id = 0;
    };

    // The swappable image whose texture data is at `physical` (a fetch constant's base address),
    // if that memory still holds the data it was created with.
    std::optional<SwappableImage> FindSwappableImage(uint32_t physical);
}
