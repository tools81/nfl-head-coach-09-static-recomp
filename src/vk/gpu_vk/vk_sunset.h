#pragma once
// hc09: the afternoon wearing on (roadmap 16). The game lights a game from one time of day, fixed for the whole game
// (its lighting set's sun, chosen as the game loads), so a 1 pm kickoff ends under the sun it started with. Here the
// time of day follows the game: the kickoff's time (GINF.GTOD) plus the game's progress as the score bug shows it
// (quarter and clock: about three hours of a broadcast, with halftime), and the sun sinks with it: the same bearing as
// the game's, its height from the sun's path in a stadium at about 41 degrees north late in the season, its light
// warmer and dimmer as it nears the horizon. Only by day, under an open sky, before the floodlights take over.
//
// Applied to the draws' constants: every draw whose sun (c204, its direction, and c205, its colour) is the scene's,
// as the field's draw gives it, gets the sun of the time of day instead (the turf, the players, the ray tracer's
// shadows all follow).
//
// HC09_SUNSET=0 (dev): off; HC09_SUNSET_TIME=HH:MM (dev): that time of day, whatever the clock.
#include <cstdint>

namespace hc09vk {

class Sunset
{
public:
    // Once a frame: whether the sun is moved this frame, from the game's state.
    void BeginFrame(uint64_t frame);
    // The scene's sun as the game set it this frame (the field's c204 and c205).
    void NoteGameSun(const float* direction, const float* colour);
    // A draw's c204 and c205 (four floats each), moved in place if they are the scene's sun. Returns whether they were.
    bool Apply(float* direction, float* colour) const;
    bool Active() const { return active_; }
    // The whole scene's tint this frame (the ambient light warming and dimming with the sun), or nullptr.
    const float* SceneTint() const { return active_ ? sceneTint_ : nullptr; }

private:
    bool active_ = false;
    bool known_ = false;
    float game_[2][3] = {};
    float elevation_ = 0.0f;   // radians
    float tint_[3] = { 1.0f, 1.0f, 1.0f };
    float sceneTint_[3] = { 1.0f, 1.0f, 1.0f };
    uint64_t frame_ = ~0ull;
    // The quarters' length: the longest clock seen this game (seconds).
    uint32_t game_id_ = ~0u;
    int quarterLength_ = 0;
};

}  // namespace hc09vk
