// hc09: the TV cameras (roadmap 12). The game's Director updates each of its cameras every frame (sub_82987E68): the
// position (+28) and the point looked at (+44) come from the camera's script, smoothed, with the field of view
// (+60, degrees); then the view matrix (+76) is built from them (sub_82986348: r3 the matrix, r4 the position, r5
// the point looked at, r6 up). The field: x across it (the sidelines at +-26.7), y up, z along it (the goal lines at
// +-50), in yards.
//
// The game's Broadcast camera stands 13 yards outside the sideline at midfield, 21 yards up (-40, 21, 0), and turns
// to follow the play: the point it looks at is the play. Here it is moved to where a TV broadcast's cameras are,
// keeping the play framed (a narrower field of view for a longer distance):
//   the main camera: higher and further back, and along the sideline with the play;
//   the end zone camera: high behind the end zone at the offense's back;
//   the low sideline camera: at the players' height on the sideline, level with the play;
//   the aerial camera: on its wires above and behind the offense, moving with the play.
// A click of the right stick (H on the keyboard) goes from one to the next while the Broadcast camera is the game's.
//
// With the main camera, a broadcast's cuts before the snap as well, on one play in three: once the camera has settled
// on the ball for the next play, a few seconds from another camera (the teams lining up from the low sideline, the quarterback from the
// end zone he faces, the formation from the aerial camera, the defense from the end zone behind the offense), then
// back to the main camera; at the snap (the point looked at moves) at once.
//
// Field goals and extra points: the game has a camera of its own for them, at (-35, 20, 0), which looks at the ball
// and follows it. It becomes the end zone camera behind the goal posts kicked at: the kick comes toward the camera,
// between the uprights in the foreground.
//
// Instant replays (the clipboard's Gameday page): the game plays the play again through the cameras it was shown
// with. While the replay's screen is up (common/broadcast_state.cpp), every camera looking at the field is moved to a
// replay angle on the point it looks at: low behind the offense, the reverse angle from the other sideline, or the
// skycam. Each replay starts on the next one; the right stick goes to the next. (Dev: HC09_REPLAY_TEST opens a
// replay after a play, clicks through the angles and takes screenshots.)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <ppc_recomp_shared.h>

#include "common/broadcast_state.h"
#include "vk/dev_env.h"

// The Director camera's update: r3 the camera, f1 the time step.
extern "C" PPC_FUNC(__imp__sub_82987E68);
// The view matrix from a position and a point looked at.
extern "C" PPC_FUNC(__imp__sub_82986348);

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kStates = 0x83157F0C;   // CameraStateMgr::sInstance; +16 its state
    constexpr int kAngles = 4;
    constexpr const char* kNames[kAngles] = { "MAIN", "END ZONE", "LOW SIDELINE", "AERIAL" };

    uint32_t Word(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }
    float Float(uint8_t* base, uint32_t a)
    {
        const uint32_t w = Word(base, a);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }
    void Put(uint8_t* base, uint32_t a, float f)
    {
        uint32_t w;
        std::memcpy(&w, &f, 4);
        *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(w);
    }

    thread_local uint32_t t_camera = 0;   // the camera being updated

    // TVCamera (HC09_TV_CAMERA): on by default, with the main camera; "endzone", "sideline" or "aerial" start with
    // that one; "off" keeps the game's own Broadcast camera. -1: off.
    int Setting()
    {
        static const int setting = [] {
            const char* e = Hc09Env("HC09_TV_CAMERA");
            if (!e) return 0;
            if (!std::strcmp(e, "0") || !_stricmp(e, "off")) return -1;
            if (!_stricmp(e, "endzone")) return 1;
            if (!_stricmp(e, "sideline")) return 2;
            if (!_stricmp(e, "aerial")) return 3;
            return 0;
        }();
        return setting;
    }
    // "fixed": the main camera without the cuts before the snap.
    bool Cuts()
    {
        static const bool cuts = [] {
            const char* e = Hc09Env("HC09_TV_CAMERA");
            return !(e && !_stricmp(e, "fixed"));
        }();
        return cuts;
    }

    std::atomic<int> g_angle{ -2 };                 // -2: not set yet (the setting's)
    std::atomic<int64_t> g_changed{ 0 }, g_seen{ 0 };   // when the angle last changed; when the Broadcast camera was last the game's

    int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

    int Angle()
    {
        const int angle = g_angle.load();
        return angle == -2 ? std::max(Setting(), 0) : angle;
    }

    // Which way the offense faces along the field: the sign of z at its back. From the spot of the ball as the
    // score bug has it ("CHI 37": 13 yards from midfield in Chicago's half) and where the camera looks before the
    // snap: that gives the sign of each team's half; the offense's own half is at its back. Kept from one snap to the
    // next (a turnover in the middle of a play does not swing the camera round).
    float Behind(float tz)
    {
        static float behind = 1.0f;
        static int steady = 0;
        const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
        const size_t on = board.down.find(" on ");
        if (on == std::string::npos || board.possession < 0)
            return steady = 0, behind;
        const std::string spot = board.down.substr(on + 4);
        const size_t space = spot.find(' ');
        if (space == std::string::npos)
            return steady = 0, behind;
        const std::string team = spot.substr(0, space);
        const int yard = std::atoi(spot.c_str() + space + 1);
        const int side = !_stricmp(team.c_str(), board.home.abbreviation.c_str()) ? 1 : !_stricmp(team.c_str(), board.away.abbreviation.c_str()) ? 0 : -1;
        if (side < 0 || yard < 1 || yard > 47 || std::abs(std::abs(tz) - float(50 - yard)) > 4.0f)
            return steady = 0, behind;
        // The camera has looked at that spot for half a second: before a snap.
        if (++steady >= 30)
        {
            const float half = tz < 0 ? -1.0f : 1.0f;   // the sign of that team's half
            behind = side == board.possession ? half : -half;
        }
        return behind;
    }

    // The cuts before the snap. The point the Broadcast camera looks at comes to rest on the ball when the teams line
    // up: a third of a second at rest at a new spot starts a shot from another camera, for a few seconds or until the
    // point moves (the snap).
    constexpr int kShots = 4;   // 1 the low sideline, 2 the quarterback, 3 the aerial, 4 the defense
    int Shot(float tx, float tz, bool beforeSnap)
    {
        static float lastX = 0, lastZ = 0, restX = 1e9f, restZ = 1e9f;
        static int atRest = 0, shot = 0, plays = 0;
        static int64_t started = 0;
        // Dev (HC09_TV_SHOT=<1..4>): that shot, held for as long as the camera rests.
        static const int forced = [] {
            const char* e = Hc09Env("HC09_TV_SHOT");
            return e ? std::atoi(e) : 0;
        }();
        const int64_t now = Now();
        const bool still = std::abs(tx - lastX) < 0.03f && std::abs(tz - lastZ) < 0.03f;
        lastX = tx, lastZ = tz;
        atRest = still ? atRest + 1 : 0;
        // Only before a snap (not when the camera rests on a runner who is down), and on one play in three.
        if (beforeSnap && atRest == 20 && (std::abs(tx - restX) > 1.0f || std::abs(tz - restZ) > 1.0f))
        {
            restX = tx, restZ = tz;
            started = now;
            ++plays;
            shot = forced ? forced : plays % 3 == 1 ? 1 + (plays / 3) % kShots : 0;
        }
        if (shot && (!beforeSnap || std::abs(tx - restX) > 0.8f || std::abs(tz - restZ) > 0.8f || (!forced && now - started > 3400)))
            shot = 0;
        return shot;
    }

    // A field goal or an extra point, from the game's camera for kicks: the sign of z at the goal posts kicked at (0:
    // not one: a punt, or not known). Decided while the camera rests on the ball before the snap, kept while the
    // camera follows the kick.
    float Kick(float tz)
    {
        static float posts = 0, lastZ = 1e9f;
        static int atRest = 0;
        static int64_t seen = 0;
        const int64_t now = Now();
        if (now - seen > 1500)   // another kick's camera
            posts = 0, atRest = 0;
        seen = now;
        atRest = std::abs(tz - lastZ) < 0.03f ? atRest + 1 : 0;
        lastZ = tz;
        const float behind = Behind(tz);
        if (atRest >= 10 && atRest <= 40)
        {
            const float half = tz < 0 ? -1.0f : 1.0f;
            const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();
            const bool extraPoint = board.down.find("PAT") != std::string::npos;
            // The ball in the other team's half, inside its 38: a kick at its posts. (A punt is from further back.)
            posts = std::abs(tz) >= 12.0f && (extraPoint || half == -behind) ? half : 0.0f;
        }
        return posts;
    }
}

namespace
{
    // The clipboard's Instant Replay plays the play again through the cameras it was shown with (the game's for kicks,
    // the Broadcast camera): while it is up, whichever camera looks at the field is moved to a replay angle. Each
    // replay is shown from the next of these; a click of the right stick goes to the next one.
    constexpr int kReplayAngles = 3;
    constexpr const char* kReplayNames[kReplayAngles] = { "END ZONE REPLAY", "REVERSE ANGLE", "SKYCAM" };
    std::atomic<int> g_replayAngle{ -1 };
    std::atomic<int64_t> g_replaySeen{ 0 }, g_replayChanged{ 0 };
}

namespace hc09::broadcast
{
    bool CycleTvCamera()
    {
        if (Setting() < 0)
            return false;
        // A replay (the Broadcast camera is updated through it too, but not shown).
        if (Now() - g_replaySeen.load() < 500)
        {
            g_replayAngle = (std::max(g_replayAngle.load(), 0) + 1) % kReplayAngles;
            g_replayChanged = Now();
            return true;
        }
        // Only while the Broadcast camera is the one the game shows (a moment ago).
        if (Now() - g_seen.load() > 500)
            return false;
        g_angle = (Angle() + 1) % kAngles;
        g_changed = Now();
        return true;
    }

    const char* TvCameraNote()
    {
        const int64_t now = Now();
        if (now - g_replaySeen.load() < 500)
            return now - g_replayChanged.load() < 2500 && g_replayAngle.load() >= 0 ? kReplayNames[g_replayAngle.load()] : nullptr;
        return now - g_changed.load() < 2500 && now - g_seen.load() < 500 ? kNames[Angle()] : nullptr;
    }
}

PPC_FUNC(sub_82986348)
{
    const uint32_t camera = t_camera;
    if (camera && Setting() >= 0 && ctx.r4.u32 == camera + 28 && ctx.r5.u32 == camera + 44)
    {
        const float ex = Float(base, camera + 28), ey = Float(base, camera + 32), ez = Float(base, camera + 36);
        const bool broadcast = std::abs(std::abs(ex) - 40.0f) < 0.01f && std::abs(ey - 21.0f) < 0.01f && std::abs(ez) < 0.01f;
        const bool kicks = std::abs(std::abs(ex) - 35.0f) < 0.01f && std::abs(ey - 20.0f) < 0.01f && std::abs(ez) < 0.01f;
        float tx = Float(base, camera + 44), ty = Float(base, camera + 48), tz = Float(base, camera + 52);
        const float fov = Float(base, camera + 60);
        const float before = std::sqrt((tx - ex) * (tx - ex) + (ty - ey) * (ty - ey) + (tz - ez) * (tz - ez));
        const float side = ex < 0 ? -1.0f : 1.0f;
        float nx = ex, ny = ey, nz = ez, wider = 1.0f, fixed = 0.0f, widest = 60.0f, cover = 0.0f;
        bool moved = false;
        // The instant replay: a camera looking at the field (inside the sidelines: not the coach's on the sideline,
        // nor a camera at rest at the origin).
        const bool replay = hc09::broadcast::ReplayShown() && std::abs(tx) < 27.0f && std::abs(tz) < 65.0f && ty > -1.0f && ty < 6.0f &&
                            !(tx == 0.0f && ty == 0.0f && tz == 0.0f) && fov > 1.0f && fov < 120.0f;
        static std::atomic<float> mainSide{ -1.0f };   // the side of the field the main camera is on
        if (broadcast)
            mainSide = side;
        if (replay)
        {
            const int64_t now = Now();
            if (now - g_replaySeen.load() > 1500)   // a new replay: the next angle
            {
                g_replayAngle = (g_replayAngle.load() + 1) % kReplayAngles;
                g_replayChanged = now;
            }
            g_replaySeen = now;
            const float behind = Behind(1e9f);   // the offense's back, as before the play (not looked at again)
            switch (std::max(g_replayAngle.load(), 0))
            {
            case 0:
                // Low behind the offense: the play coming at the camera's back, down the field.
                nx = tx * 0.5f, ny = ty + 7.0f, nz = std::clamp(tz + behind * 32.0f, -70.0f, 70.0f);
                cover = 20.0f;
                break;
            case 1:
                // The reverse angle: from the other sideline than the main camera's.
                nx = -mainSide.load() * 42.0f, ny = ty + 9.0f, nz = tz;
                cover = 16.0f;
                break;
            default:
                // The skycam: above the play and a little behind it.
                nx = tx * 0.8f, ny = ty + 12.0f, nz = std::clamp(tz + behind * 18.0f, -66.0f, 66.0f);
                cover = 24.0f;
                break;
            }
            moved = true;
        }
        else if (kicks)
        {
            // The game's camera for kicks.
            static float restX = 0, restY = 0, restZ = 0;
            static float lastPosts = 0;
            const float posts = Kick(tz);
            if (posts != 0.0f)
            {
                if (posts != lastPosts || std::abs(tz) < std::abs(restZ))   // the ball's spot: the nearest the camera looked
                    restX = tx, restY = ty, restZ = tz;
                // Behind the posts (at z = 60, the crossbar 3.3 yards up, the uprights 6.2 apart), a little above the
                // crossbar, looking at the kicker: the posts stand in the picture's foreground.
                nx = 0.0f, ny = restY + 5.5f, nz = posts * 70.0f;
                tx = restX * 0.5f, ty = restY + 2.0f, tz = restZ;
                fixed = 44.0f;
                moved = true;
            }
            lastPosts = posts;
        }
        else if (broadcast)
        {
            g_seen = Now();
            moved = true;
            const int angle = Angle();
            // The game's camera state: 1 before the snap (3 during the play, 4 after it, 5 while plays are called).
            const uint32_t states = Word(base, kStates);
            const bool beforeSnap = states >= 0x40000000u && Word(base, states + 16) == 1;
            const int shot = angle == 0 && Cuts() ? Shot(tx, tz, beforeSnap) : 0;
            const float behind = angle == 1 || angle == 3 || shot >= 2 ? Behind(tz) : 1.0f;
            if (shot == 2)
            {
                // The quarterback, from the end zone he faces: he stands behind the ball.
                nx = tx * 0.3f, ny = ty + 22.0f, nz = -behind * 74.0f;
                tz += behind * 3.0f, ty += 1.3f;
                cover = 9.0f;
            }
            else if (shot == 4)
            {
                // The defense, from the end zone behind the offense.
                nx = tx * 0.3f, ny = ty + 22.0f, nz = behind * 74.0f;
                tz -= behind * 4.0f, ty += 1.0f;
                cover = 22.0f;
            }
            else if (angle == 1)
            {
                // The end zone camera.
                nx = 0.0f, ny = 26.0f, nz = behind * 74.0f;   // above the goal posts' tops
                wider = 1.25f;   // the play is deeper than it is wide from here
            }
            else if (angle == 2 || shot == 1)
            {
                // The low sideline camera.
                // (The point the game looks at is at the field's height, which is not y = 0: a camera at y = 1.6 was
                // under the turf.)
                nx = side * 29.0f, ny = ty + 1.7f, nz = std::clamp(tz, -52.0f, 52.0f);
                ty += 1.0f;
                widest = 38.0f;
            }
            else if (angle == 3 || shot == 3)
            {
                // The aerial camera.
                nx = tx * 0.6f, ny = ty + 12.0f, nz = std::clamp(tz + behind * 24.0f, -62.0f, 62.0f);
                tz -= behind * 5.0f;   // looking a little downfield of the play
                fixed = 46.0f;
            }
            else
            {
                // The main camera.
                nx = side * 58.0f, ny = 27.0f, nz = std::clamp(tz * 0.85f, -38.0f, 38.0f);
            }
        }
        if (moved)
        {
            const float after = std::sqrt((tx - nx) * (tx - nx) + (ty - ny) * (ty - ny) + (tz - nz) * (tz - nz));
            Put(base, camera + 28, nx);
            Put(base, camera + 32, ny);
            Put(base, camera + 36, nz);
            Put(base, camera + 44, tx);
            Put(base, camera + 48, ty);
            Put(base, camera + 52, tz);
            if (cover > 0.0f && after > 1.0f)   // a picture `cover` yards across at the point looked at
                Put(base, camera + 60, std::clamp(2.0f * std::atan(cover * 0.5f / after) * 57.29578f, 3.0f, 60.0f));
            else if (fixed > 0.0f)
                Put(base, camera + 60, fixed);
            else if (fov > 1.0f && fov < 120.0f && after > 1.0f)
            {
                const float half = std::atan(std::tan(fov * 0.5f * 0.0174533f) * before / after * wider);
                Put(base, camera + 60, std::clamp(half * 2.0f * 57.29578f, 6.0f, widest));
            }
        }
    }
    __imp__sub_82986348(ctx, base);
}

PPC_FUNC(sub_82987E68)
{
    const uint32_t camera = ctx.r3.u32;
    t_camera = camera;
    __imp__sub_82987E68(ctx, base);
    t_camera = 0;
    // Dev (HC09_CAM_LOG): each camera as the game leaves it, once a second.
    static const bool log = Hc09Env("HC09_CAM_LOG") != nullptr;
    if (log)
    {
        static std::map<uint32_t, Clock::time_point> times;
        const auto now = Clock::now();
        auto& last = times[camera];
        if (now - last > std::chrono::milliseconds(1000))
        {
            last = now;
            const uint32_t states = Word(base, kStates);
            const uint32_t state = states >= 0x40000000u ? Word(base, states + 16) : 99;
            std::fprintf(stderr, "[cam] %08X state %u eye %.2f %.2f %.2f at %.2f %.2f %.2f | %.3f %.3f %.3f %.3f\n", camera, state,
                         Float(base, camera + 28), Float(base, camera + 32), Float(base, camera + 36), Float(base, camera + 44),
                         Float(base, camera + 48), Float(base, camera + 52), Float(base, camera + 60), Float(base, camera + 64),
                         Float(base, camera + 68), Float(base, camera + 72));
        }
    }
}
