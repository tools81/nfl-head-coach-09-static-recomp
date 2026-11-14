#pragma once
// hc09: hand-written shading for the turf and the players (roadmap 10). The game's own pixel shaders, translated
// from the player's disc as always, get our shading added at their end, matched by shader hash (vk_materials.cpp).
#include <cstdint>
#include <string>

namespace hc09vk {

// The field's pixel shader (the turf).
constexpr uint64_t kTurfPs = 0x076698e96199da36ull;
// The field as night games draw it during play: the same shader less one texture (the same constants).
constexpr uint64_t kTurfNightPs = 0xcf6a1cd735fb0fdcull;
constexpr uint64_t kHelmetPs = 0xe263d5063e531450ull;   // the helmets' shells
// The game's own shadows of players, coaches and officials: their models drawn dark into a 640x360 target (cleared
// white, then blurred), which the field's shader reads.
constexpr uint64_t kPlayerShadowPs = 0x5c2d30a29ad4dff3ull;

// Adds our shading to the translated pixel shader `glsl` with that hash, if it is one we know. Returns whether it
// did. HC09_SHADER_TINT=<hash>[,<hash>...] (dev): those shaders' draws in solid colours, in that order red, green,
// blue, yellow, magenta, cyan, orange, white (finding which shader draws what); =all: every shader in a colour
// of its own, logged as [tint] <hash> <r> <g> <b>.
bool MaterialPatch(uint64_t hash, std::string& glsl);

// A draw's value for our code (DrawConsts::pad0) when its pixel shader is one of ours: the turf's wear. `fetch`:
// the draw's texture fetch constants, `slots` the ones it samples.
bool MaterialDrawValue(uint64_t psHash, const uint32_t (*fetch)[6], uint32_t slots, float& value);

}  // namespace hc09vk
