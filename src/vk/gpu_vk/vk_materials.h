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
// The texture slot the field's shaders leave free, where the footprints in the snow are bound for them.
constexpr uint32_t kFootprintSlot = 30;
constexpr uint64_t kHelmetPs = 0xe263d5063e531450ull;   // the helmets' shells
// The game's own shadows of players, coaches and officials: their models drawn dark into a 640x360 target (cleared
// white, then blurred), which the field's shader reads.
constexpr uint64_t kPlayerShadowPs = 0x5c2d30a29ad4dff3ull;

// The line to gain the game draws on the field before a snap: one flat orange quad (its vertex and pixel shaders).
constexpr uint64_t kLineToGainVs = 0x44e93b41316d9d61ull;
constexpr uint64_t kLineToGainPs = 0x438c2af84c78a133ull;
// Where the field's draws get the lines we paint instead (FieldLineValues): the texture size entry of a slot the
// field's shaders leave free (DrawConsts::texSize[kFieldLineSlot]).
constexpr uint32_t kFieldLineSlot = 29;

// The menus' and the clipboard's pixel shader (the game's UI, drawn by Apt): its grey art times a tint (r[1]).
constexpr uint64_t kUiPs = 0x63730412cd8ec1b6ull;

// Adds our shading to the translated pixel shader `glsl` with that hash, if it is one we know. Returns whether it
// did. HC09_SHADER_TINT=<hash>[,<hash>...] (dev): those shaders' draws in solid colours, in that order red, green,
// blue, yellow, magenta, cyan, orange, white (finding which shader draws what); =all: every shader in a colour
// of its own, logged as [tint] <hash> <r> <g> <b>.
bool MaterialPatch(uint64_t hash, std::string& glsl);

// A draw's value for our code (DrawConsts::pad0) when its pixel shader is one of ours: the turf's wear, how wet the
// field is and whether it is night (the day's and the night's field shaders). `fetch`:
// the draw's texture fetch constants, `slots` the ones it samples.
bool MaterialDrawValue(uint64_t psHash, const uint32_t (*fetch)[6], uint32_t slots, float& value);

// The UI's draws' value (kUiPs, DrawConsts::pad0) in a game: the 2026 style's on, and its accent colour (the user's
// team's). False outside games (the menus keep the game's look) or with the style off.
bool UiStyleValue(float& value);

// FieldLines (HC09_FIELD_LINES): on by default; off keeps the game's orange line to gain.
bool FieldLinesOn();
// The lines painted into the field (the 2026 style: a blue line of scrimmage, a yellow line to gain) from the teams'
// line-up for a snap to the end of the play: x the line of scrimmage's z, y the line to gain's (1e9: none), z 1. All zero when none.
void FieldLineValues(float (&out)[4]);

}  // namespace hc09vk
