// hc09: the score bar's game state; see broadcast_state.h.
//
// The game's UI is Apt (Flash). A display object's +8 points at its instance name (an Apt string: the characters at
// +8) and +0x1C at its parent. The score bug is the clip "mcGameDayTicker" (in "mcBillboard"); its fields txtAwayScore,
// txtHomeScore, txtQuarter, txtClock, txtDown and txtPlayClock are ShadowedTextFields, whose ActionScript is given the
// text as their member mText.
//
// ActionScript's member writes (sub_82B04600: r4 the object, r6 the member's name, a value whose word 0 points at an
// Apt string, r7 the value) and reads (sub_82B04AD0: r4 the object, r6 the name, the value returned in r3) go through
// here. A value: word 0 its type (kString: word 2 points at an Apt string; kInteger: word 2 the number; kNumber: the
// float at +8; a Boolean: the byte at +8). The fields' mText is kept, and the clip's _visible and its parent's, as
// the game sets them, show and hide the bar. Three clips' _visible are taken over (see the hooks at the end): the
// score bug's, the billboard's (shown only with the clipboard's score box in place, which the renderer covers with the
// Coaching Advantage) and the Coaching Advantage panel's (mcPrep, whose rows are kept). The game's two Booleans are
// fixed objects, true 12 bytes after false.
//
// The teams are the game info's (GINF.GTAG and GTHG, the away and home team: in Coach Now, copies of the teams, whose TOID
// is the team), their abbreviations and colours TEAM's.

#include "common/broadcast_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"
#include "vk/dev_env.h"
#include "vk/vk_select.h"

namespace hc09::gpu { void RequestScreenshot(int tag); void RequestTrace(); }   // vk/gpu_live.cpp (dev)
namespace hc09 { extern uint8_t* g_base; }
extern "C" PPC_FUNC(__imp__sub_826C4F58);  // the user's team (its TGID)

namespace hc09::broadcast
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The bar goes when the score bug and its parent have not been touched for this long (their screen has gone;
        // the parent's script reads its members each frame).
        constexpr auto kAliveFor = std::chrono::seconds(2);

        // ActionScript values' types (their word 0: static objects of the game's).
        constexpr uint32_t kString = 0x8218F400, kInteger = 0x8218F20C, kNumber = 0x8218F284;

        constexpr const char* kBug = "mcGameDayTicker";


        enum Field
        {
            kNone = -1,
            kAwayScore,
            kHomeScore,
            kQuarter,
            kClock,
            kDown,
            kPlayClock,
        };

        std::mutex g_mutex;
        Scoreboard g_board;             // .shown and .opacity: see Snapshot
        Clock::time_point g_touched{};  // the score bug last read or written
        bool g_everTouched = false;
        bool g_wanted = false;          // the clip's _visible as the game set it
        Clock::time_point g_lastWanted{};   // when the game last hid it
        Clock::time_point g_lastField{};    // when the game last gave a score bug field its text
        Clock::time_point g_bugUsed{};      // when the game last read or wrote the score bug itself (not its parent)

        // Whether a game is being played (mutex held): the game shows its score bug, or did, or filled in its fields,
        // within the last minute (it hides it for cutscenes, and before the first snap the clipboard may open first).
        bool InGame()
        {
            const auto now = Clock::now();
            auto recent = [&](Clock::time_point t) { return t != Clock::time_point{} && now - t < std::chrono::seconds(60); };
            // The score bug shown counts while the game still uses it (after a game it may be left shown, unused).
            return (g_wanted && recent(g_bugUsed)) || recent(g_lastWanted) || recent(g_lastField);
        }
        bool g_parentVisible = true;    // its parent's
        // A billboard's _x: kStripX shows the score bug, 0 the clipboard's score box. The renderer's box is drawn
        // within kInPlace of 0 (as the box slides in), the game's only once it is at 0 (within kAtRest), so the game's
        // never shows beside the renderer's.
        constexpr float kStripX = -910.0f, kInPlace = 20.0f, kAtRest = 0.5f;
        // Its _y with the score box at the clipboard's top (the only place the renderer covers it).
        constexpr float kBoxY = -355.0f;
        float g_box = 0;   // how far in the score box is of the billboard shown (0 .. 1; see UpdateBillboards)
        // The Coaching Advantage panel (mcPrep) as the game sets it: shown (_visible), and on screen (_y 565; it slides
        // to 730 to go). Its rows (its sliders' mValue) are g_board's.
        bool g_prepVisible = false;
        float g_prepY = 730.0f;
        constexpr float kPrepShownBelow = 700.0f;
        // The game thread's:
        uint32_t g_true = 0;            // the game's true Boolean, once seen

        uint32_t Word(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }

        bool Guest(uint32_t a) { return a >= 0x40000000u && a < 0xC0000000u && !(a & 3); }

        // An Apt string's characters ("" for none).
        const char* StringAt(uint8_t* base, uint32_t string) { return Guest(string) ? reinterpret_cast<const char*>(base + string + 8) : ""; }

        // A display object's instance name ("" if it has none).
        const char* NameOf(uint8_t* base, uint32_t object) { return Guest(object) ? StringAt(base, Word(base, object + 8)) : ""; }

        // A member's name, from the value naming it.
        const char* MemberOf(uint8_t* base, uint32_t value) { return Guest(value) ? StringAt(base, Word(base, value)) : ""; }

        Field FieldOf(const char* name)
        {
            if (std::strncmp(name, "txt", 3) != 0)
                return kNone;
            name += 3;
            if (!std::strcmp(name, "AwayScore")) return kAwayScore;
            if (!std::strcmp(name, "HomeScore")) return kHomeScore;
            if (!std::strcmp(name, "Quarter")) return kQuarter;
            if (!std::strcmp(name, "Clock")) return kClock;
            if (!std::strcmp(name, "Down")) return kDown;
            if (!std::strcmp(name, "PlayClock")) return kPlayClock;
            return kNone;
        }

        // A number's value (an Integer's or a Number's); false for anything else.
        bool NumberOf(uint8_t* base, uint32_t value, float& out)
        {
            if (!Guest(value))
                return false;
            const uint32_t type = Word(base, value), payload = Word(base, value + 8);
            if (type == kInteger)
                out = float(int32_t(payload));
            else if (type == kNumber)
                std::memcpy(&out, &payload, 4);
            else
                return false;
            return std::isfinite(out);
        }

        // A value as text: a string's characters, a number's digits.
        std::string Text(uint8_t* base, uint32_t value)
        {
            if (!Guest(value))
                return {};
            const uint32_t type = Word(base, value), payload = Word(base, value + 8);
            if (type == kString)
            {
                const char* s = StringAt(base, payload);
                std::string out;
                for (int i = 0; i < 64 && s[i]; ++i)
                    out.push_back(s[i] >= 32 && s[i] < 127 ? s[i] : '?');
                return out;
            }
            if (type == kInteger)
                return std::to_string(int32_t(payload));
            if (type == kNumber)
            {
                float f;
                std::memcpy(&f, &payload, 4);
                return std::isfinite(f) ? std::to_string(std::lround(f)) : std::string();
            }
            return {};
        }

        // The teams of the game being played (the game info's), with their abbreviations and colours; false while
        // the game info has none (before a game).
        bool ReadTeams(PPCContext& ctx, uint8_t* base)
        {
            hc09::guest::Database db(ctx, base);
            if (!db.Ready())
                return false;
            if (!db.Query("select 'GTAG' into \x85 and 'GTHG' into \x85 from 'FNIG'", { db.Out(0), db.Out(1) }))
                return false;
            const uint32_t ids[2] = { db.Get(0), db.Get(1) };
            if (ids[0] == 0 || ids[0] >= hc09::guest::Database::kNone || ids[1] == 0 || ids[1] >= hc09::guest::Database::kNone)
            {
                if (Hc09Env("HC09_SCORE_BAR_LOG"))
                    std::fprintf(stderr, "[scorebar] no teams in the game info: %u, %u\n", ids[0], ids[1]);
                return false;
            }
            Team teams[2];
            for (int i = 0; i < 2; ++i)
            {
                // The team it is (TOID): Coach Now plays copies of the teams, under TGIDs past 1000.
                teams[i].id = ids[i];
                if (db.Query("select 'DIOT' into \x85 from 'MAET' where ('DIGT' = \x85)", { db.Out(0), ids[i] }) && db.Get(0) >= 1 &&
                    db.Get(0) <= 32)
                    teams[i].id = db.Get(0);
                if (db.Query("select 'RCBT' into \x85 and 'GCBT' into \x85 and 'BCBT' into \x85 from 'MAET' where ('DIGT' = \x85)",
                             { db.Out(0), db.Out(1), db.Out(2), ids[i] }))
                    teams[i].rgb = ((db.Get(0) & 0xFF) << 16) | ((db.Get(1) & 0xFF) << 8) | (db.Get(2) & 0xFF);
                const uint32_t text = db.Text(0, std::string(15, '\0'));
                if (db.Query("select 'ANST' into \x88 from 'MAET' where ('DIGT' = \x85)", { text, ids[i] }))
                {
                    const char* s = reinterpret_cast<const char*>(base + text);
                    for (int k = 0; k < 15 && s[k] >= 32 && s[k] < 127; ++k)
                        teams[i].abbreviation.push_back(s[k]);
                }
            }
            // The user's team (in Coach Now, the copy of the team the user chose).
            const uint32_t user = db.Call(__imp__sub_826C4F58, std::initializer_list<uint32_t>{});
            const int userSide = user == ids[0] || (user >= 1 && user <= 32 && user == teams[0].id) ? 0
                                 : user == ids[1] || (user >= 1 && user <= 32 && user == teams[1].id) ? 1 : -1;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_board.away = teams[0];
            g_board.home = teams[1];
            g_board.user = userSide;
            if (Hc09Env("HC09_SCORE_BAR_LOG"))
                std::fprintf(stderr, "[scorebar] teams %u %s #%06X at %u %s #%06X, the user's %u (side %d)\n", ids[0],
                             teams[0].abbreviation.c_str(), teams[0].rgb, ids[1], teams[1].abbreviation.c_str(), teams[1].rgb, user, userSide);
            return true;
        }

        // The game thread's: whether the teams are read (again each time the game shows the score bug).
        bool g_teamsKnown = false;

        // The score bug (or its parent) touched: alive, and its teams read until the game info has them.
        void Touched(PPCContext& ctx, uint8_t* base)
        {
            static Clock::time_point lastTry{};
            const auto now = Clock::now();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_touched = now;
                g_everTouched = true;
            }
            if (!g_teamsKnown && now - lastTry > std::chrono::seconds(1))
            {
                lastTry = now;
                g_teamsKnown = ReadTeams(ctx, base);
            }
        }

        void Log(const char* format, const char* a, const char* b)
        {
            static const bool on = Hc09Env("HC09_SCORE_BAR_LOG") != nullptr;
            if (on)
                std::fprintf(stderr, format, a, b);
        }
    }

    bool Enabled()
    {
        static const bool on = [] {
            const char* e = Hc09Env("HC09_SCORE_BAR");
            if ((e && (!std::strcmp(e, "0") || !std::strcmp(e, "off"))) || !hc09::vk::Selected())
                return false;
            std::error_code ec;
            return std::filesystem::exists(kFontFile, ec);
        }();
        return on;
    }

    Scoreboard Snapshot()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Scoreboard board = g_board;
        const bool alive = g_everTouched && Clock::now() - g_touched < kAliveFor;
        board.shown = alive && g_wanted && g_parentVisible;
        // The box is covered in games: while the score bug is shown, and through the cutscenes that hide it a while.
        const bool inGame = InGame();
        // Exactly while the game shows its box (at rest at the clipboard's top, in a game): see UpdateBillboards.
        board.box = inGame ? g_box : 0.0f;
        board.coaching = g_prepVisible && g_prepY < kPrepShownBelow;
        board.inGame = inGame;
        return board;
    }

    std::atomic<bool> g_replayScreen{ false };   // the screen manager's latest event is the instant replay's
    std::atomic<bool> g_replayShown{ false };    // the instant replay is on the screen

    bool ReplayShown() { return g_replayShown.load(); }

    bool FieldLines(float& scrimmage, float& gain)
    {
        constexpr uint32_t kLine = 0x830A8A74;   // the line of scrimmage's z (common/extra_point.cpp)
        if (!hc09::g_base)
            return false;
        const Scoreboard board = Snapshot();
        if (!board.inGame)
            return false;
        const uint32_t bits = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(hc09::g_base + kLine));
        float z;
        std::memcpy(&z, &bits, sizeof(z));
        if (!std::isfinite(z) || std::abs(z) > 50.0f)
            return false;
        scrimmage = z;
        gain = 1e9f;
        // "3rd & 2 on CHI 28": the yards to go, and the spot, whose team's half tells which way the offense goes.
        const std::string& down = board.down;
        const size_t on = down.find(" on "), amp = down.find('&');
        if (on == std::string::npos || amp == std::string::npos || amp > on)
            return false;  // a kickoff, a try: no lines
        const std::string toGo = down.substr(amp + 1, on - amp - 1);
        float yards = 0.0f;
        if (toGo.find_first_of("0123456789") != std::string::npos)
            yards = float(std::atof(toGo.c_str()));
        else if (toGo.find("In") != std::string::npos || toGo.find("IN") != std::string::npos)
            yards = 0.3f;   // inches
        else
            return true;    // goal to go: the goal line is the line
        static float ahead = 1.0f;   // the sign of z the offense goes towards (kept from the play before at midfield)
        const std::string spot = down.substr(on + 4);
        const size_t space = spot.find(' ');
        // The score bug's spot and the line must agree (one can be a play behind the other as they change): no
        // lines until they do.
        const int yard = std::atoi(spot.c_str() + (space == std::string::npos ? 0 : space + 1));
        if (yard < 1 || yard > 50 || std::abs(float(50 - yard) - std::abs(z)) >= 1.5f)
            return false;
        if (space != std::string::npos && board.possession >= 0 && std::abs(z) > 0.5f)
        {
            const std::string team = spot.substr(0, space);
            const int side = !_stricmp(team.c_str(), board.home.abbreviation.c_str()) ? 1
                           : !_stricmp(team.c_str(), board.away.abbreviation.c_str()) ? 0 : -1;
            if (side < 0)
                return false;
            const float half = z < 0 ? -1.0f : 1.0f;
            ahead = side == board.possession ? -half : half;   // out of its own half, into the other's
        }
        float line = z + ahead * yards;
        // The yards to go are rounded: the line a first down set (10 yards on, exactly) holds through its series.
        static float series = 1e9f;
        if (down.compare(0, 3, "1st") == 0 || down.compare(0, 3, "1ST") == 0 || std::abs(line - series) >= 1.0f)
            series = line;
        else
            line = series;
        if (std::abs(line) < 50.0f)
            gain = line;
        // HC09_FIELD_LINES_LOG=1 (dev): the lines when they change.
        static const bool log = Hc09Env("HC09_FIELD_LINES_LOG") != nullptr;
        static float lastZ = 1e9f, lastGain = 1e9f;
        if (log && (z != lastZ || gain != lastGain))
        {
            lastZ = z, lastGain = gain;
            std::fprintf(stderr, "[field-lines] scrimmage %.3f gain %.3f (%s, possession %d)\n", z, gain, down.c_str(), board.possession);
        }
        return true;
    }

    std::atomic<int64_t> g_replayTestAt{ 0 };   // dev: when the Gameday page was filled and the replay is opened (ms)
    std::atomic<int> g_replayTestSlot{ 1 };      // dev: the Gameday list's item that is Instant Replay (1, 2)

    int ReplayTestStep(bool& hold)
    {
        hold = false;
        const int64_t at = g_replayTestAt.load();
        if (at <= 0)
            return 0;
        const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() - at;
        if (t > 40000)   // the replay has had its time: the script goes on
            return 0;
        hold = true;
        // RB to the Gameday page (its list's first item chosen), down to Instant Replay, A; a second apart from two
        // seconds in. Then a click of the right stick five and ten seconds into the replay (its next angles).
        // Screenshots (build/screenshot_<9000 + k>): before each press, and every 2.5 s of the replay.
        static int64_t lastAt = 0;
        static std::vector<std::pair<int64_t, int>> steps;   // when, button (1 RB, 2 down, 3 A, 4 the right stick)
        static std::vector<int64_t> shots;
        static size_t shot = 0;
        if (lastAt != at)
        {
            lastAt = at;
            steps.clear();
            shots.clear();
            shot = 0;
            int64_t when = 2000;
            steps.push_back({ when, 1 });
            for (int k = 1; k < g_replayTestSlot.load(); ++k)
                steps.push_back({ when += 1000, 2 });
            steps.push_back({ when += 1000, 3 });
            const int64_t opened = when;
            steps.push_back({ opened + 5000, 4 });
            steps.push_back({ opened + 10000, 4 });
            for (const auto& step : steps)
                if (step.first < opened)
                    shots.push_back(step.first - 300);
            for (int64_t k = 1; k <= 6; ++k)
                shots.push_back(opened + 2500 * k - 1000);
        }
        if (shot < shots.size() && t >= shots[shot])
        {
            hc09::gpu::RequestScreenshot(9000 + int(shot));
            if (shot == 1 && Hc09Env("HC09_REPLAY_TEST_TRACE"))   // the Gameday page: its draws
                hc09::gpu::RequestTrace();
            ++shot;
        }
        for (const auto& step : steps)
            if (t >= step.first && t < step.first + 120)
                return step.second;
        return 0;
    }
}




using namespace hc09::broadcast;

extern "C" PPC_FUNC(__imp__sub_82B04AD0);
extern "C" PPC_FUNC(__imp__sub_82B04600);

// ---- what the bar takes over of the game's clips ----------------------------------------------------------------------
//
// Three clips' _visible: the game's writes of it are kept and written as the bar wants, and its reads give back what
// the game wrote (the clips' scripts go on as they would):
//   - the score bug (mcGameDayTicker): never shown;
//   - its parent, the billboard along the top (which slides between the score bug and the clipboard's score box): only
//     while it shows the score box (the clipboard is open), which the renderer covers with the Coaching Advantage;
//   - the Coaching Advantage panel along the foot of the screen (mcPrep): never shown (the renderer draws it in the
//     score box instead).
// Only the billboard's is also written without the game writing it, when the score bug or the slide changes: with the
// game's own Booleans.
namespace
{
    enum Role
    {
        kNoRole,
        kRoleBug,
        kRoleBillboard,
        kRolePrep,
    };

    // The game thread's.
    std::vector<uint32_t> g_billboards;   // the score bugs' parents (one per game screen)
    uint32_t g_visibleName = 0;           // the member name "_visible" (an interned value, from the game's writes)
    struct Visible
    {
        bool game = true;      // as the game wrote it
        int written = -1;      // as last written (-1: not yet)
        float x = kStripX;     // a billboard's _x, as the game wrote it
        float y = 0;           // and its _y
    };
    std::unordered_map<uint32_t, Visible> g_visible;   // the clips' _visible

    bool IsBillboard(uint8_t* base, uint32_t object)
    {
        return std::find(g_billboards.begin(), g_billboards.end(), object) != g_billboards.end() &&
               !std::strcmp(NameOf(base, object), "mcBillboard");
    }

    Role RoleOf(uint8_t* base, uint32_t object)
    {
        const char* name = NameOf(base, object);
        if (!name[0])
            return kNoRole;
        if (!std::strcmp(name, kBug))
            return kRoleBug;
        if (!std::strcmp(name, "mcPrep"))
            return kRolePrep;
        return IsBillboard(base, object) ? kRoleBillboard : kNoRole;
    }

    // What a clip's _visible is written as (`v`: the game's, and a billboard's _x).
    bool Wanted(Role role, const Visible& v)
    {
        if (role != kRoleBillboard)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        // In a game, only with its score box at rest at the clipboard's top, where the renderer covers it.
        return v.game && !(InGame() && !(std::fabs(v.x) <= kAtRest && std::fabs(v.y - kBoxY) < 1.0f));
    }

    // The billboards' _visible written again where what it should be has changed (`call`: the registers of a member
    // write being handled, whose context and stack the write reuses).
    void UpdateBillboards(const PPCContext& call, uint8_t* base)
    {
        if (!g_true || !g_visibleName)
            return;
        for (uint32_t billboard : g_billboards)
        {
            if (!IsBillboard(base, billboard))
                continue;
            Visible& v = g_visible[billboard];
            const bool want = Wanted(kRoleBillboard, v);
            if (v.written == int(want))
                continue;
            PPCContext write = call;
            write.r4.u64 = billboard;
            write.r5.u64 = 0;
            write.r6.u64 = g_visibleName;
            write.r7.u64 = want ? g_true : g_true - 12;
            __imp__sub_82B04600(write, base);
            v.written = want;
        }
        float box = 0;
        for (uint32_t billboard : g_billboards)
            if (const Visible& v = g_visible[billboard]; IsBillboard(base, billboard) && v.game && v.x >= -kInPlace && v.x <= kInPlace &&
                                                                   std::fabs(v.y - kBoxY) < 1.0f)
                box = 1;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_box = box;
    }

    // A Coaching Advantage row from its slider's name (mcOverallSliderHome ...): 0 .. 3, and its side (0 away).
    int RowOf(const char* name, int& side)
    {
        static const char* kRows[4] = { "mcOverallSlider", "mcCoachSlider", "mcKnowledgeSlider", "mcGameplanSlider" };
        for (int r = 0; r < 4; ++r)
        {
            const size_t n = std::strlen(kRows[r]);
            if (!std::strncmp(name, kRows[r], n))
            {
                if (!std::strcmp(name + n, "Away"))
                    return side = 0, r;
                if (!std::strcmp(name + n, "Home"))
                    return side = 1, r;
            }
        }
        return -1;
    }
}

// Member reads: the clips' _visible read back as the game wrote it.
PPC_FUNC(sub_82B04AD0)
{
    const uint32_t object = ctx.r4.u32;
    const Role role = Enabled() && Guest(object) ? RoleOf(base, object) : kNoRole;
    if (role == kNoRole)
    {
        __imp__sub_82B04AD0(ctx, base);
        return;
    }
    if (role != kRolePrep)
        Touched(ctx, base);
    if (role == kRoleBug)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bugUsed = Clock::now();
    }
    const bool visible = g_true && !std::strcmp(MemberOf(base, ctx.r6.u32), "_visible");
    __imp__sub_82B04AD0(ctx, base);
    if (visible && (ctx.r3.u32 == g_true || ctx.r3.u32 == g_true - 12))
        if (auto it = g_visible.find(object); it != g_visible.end() && it->second.written >= 0)
            ctx.r3.u64 = it->second.game ? g_true : g_true - 12;
}

// Member writes: what the bar shows (the score bug's fields' mText, possession, the timeouts, the Coaching Advantage),
// and the clips' _visible.
PPC_FUNC(sub_82B04600)
{
    const uint32_t object = ctx.r4.u32, value = ctx.r7.u32;
    if (!Enabled() || !Guest(object) || !Guest(value))
    {
        __imp__sub_82B04600(ctx, base);
        return;
    }
    const char* name = NameOf(base, object);
    // Dev (HC09_APT_LOG): every text the script sets, with the clip it is set on (and its parents).
    static const bool aptLog = Hc09Env("HC09_APT_LOG") != nullptr;
    if (aptLog && Word(base, value) == kString)
    {
        std::string path;
        uint32_t up = object;
        for (int depth = 0; depth < 5 && Guest(up); ++depth, up = Word(base, up + 0x1C))
        {
            const char* n = NameOf(base, up);
            bool clean = n[0] != 0;
            for (int k = 0; k < 48 && n[k]; ++k)
                clean = clean && n[k] > 32 && n[k] < 127;
            path = std::string(clean ? n : "?") + (depth ? "/" : "") + path;
        }
        const char* m = MemberOf(base, ctx.r6.u32);
        bool clean = m[0] != 0;
        for (int k = 0; k < 48 && m[k]; ++k)
            clean = clean && m[k] > 32 && m[k] < 127;
        static std::string last;
        const std::string line = path + " ." + (clean ? m : "?") + " = \"" + Text(base, value) + "\"";
        if (line != last)
        {
            last = line;
            std::fprintf(stderr, "[apt] %s\n", line.c_str());
        }
    }
    // The instant replay's screen: its id is written to the screen manager's events ("../headcoach/screen/gameday/
    // Replay"), then the event's Type: ready (shown) and unloaded (gone).
    if (Word(base, value) == kString)
    {
        const char* member = MemberOf(base, ctx.r6.u32);
        if (!std::strcmp(member, "ScreenId"))
            g_replayScreen = Text(base, value).find("/gameday/Replay") != std::string::npos;
        else if (g_replayScreen && !std::strcmp(member, "Type"))
        {
            const std::string type = Text(base, value);
            if (type == "ScreenManagerScreenReady")
                g_replayShown = true;
            else if (type == "ScreenManagerScreenUnloaded" || type == "ScreenStartTransitionOut")
                g_replayShown = false;
        }
    }
    // Dev (HC09_REPLAY_TEST): the clipboard's Gameday page is filled after each play ("Timeout", "Instant Replay").
    static const bool replayTest = Hc09Env("HC09_REPLAY_TEST") != nullptr;
    if (replayTest && Word(base, value) == kString && !std::strcmp(MemberOf(base, ctx.r6.u32), "mLabel") && Text(base, value) == "Instant Replay")
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        if (g_replayTestAt.load() == 0)   // once a game
        {
            const char* slot = std::strstr(NameOf(base, object), "mcOption");
            g_replayTestSlot = slot ? std::max(std::atoi(slot + 8), 1) : 1;
            g_replayTestAt = now;
            std::fprintf(stderr, "[replaytest] opening the instant replay (item %d)\n", g_replayTestSlot.load());
        }
    }
    if (!name[0])
    {
        __imp__sub_82B04600(ctx, base);
        return;
    }
    const char* member = MemberOf(base, ctx.r6.u32);
    const char* upName = NameOf(base, Word(base, object + 0x1C));
    float number;
    // Possession (the clipboard's score box glows on the side with the ball: mcGameDay's mcAwayPosGlow and
    // mcHomePosGlow, _alpha 100 or 0) and the timeouts left (the billboard's mAwayTimeoutsLeft, mHomeTimeoutsLeft).
    const bool awayGlow = !std::strcmp(name, "mcAwayPosGlow"), homeGlow = !std::strcmp(name, "mcHomePosGlow");
    if ((awayGlow || homeGlow) && !std::strcmp(upName, "mcGameDay") && !std::strcmp(member, "_alpha") && NumberOf(base, value, number))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (number > 50)
            g_board.possession = homeGlow ? 1 : 0;
        else if (g_board.possession == (homeGlow ? 1 : 0))
            g_board.possession = -1;
    }
    else if (!std::strcmp(name, "mcBillboard") && NumberOf(base, value, number))
    {
        const bool away = !std::strcmp(member, "mAwayTimeoutsLeft"), home = !std::strcmp(member, "mHomeTimeoutsLeft");
        if (away || home)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            (away ? g_board.awayTimeouts : g_board.homeTimeouts) = int(number);
        }
    }
    else if (!std::strcmp(upName, "mcPrep"))
    {
        // The Coaching Advantage's rows: their sliders' values, and which rows are shown.
        int side = 0;
        if (const int row = RowOf(name, side); row >= 0)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!std::strcmp(member, "mValue") && NumberOf(base, value, number))
                g_board.rows[row][side] = std::clamp(number, 0.0f, 100.0f);
            else if (!std::strcmp(member, "_visible"))
                g_board.rowShown[row] = base[value + 8] != 0;
        }
    }
    if (!std::strcmp(upName, "mcGameDay") && !std::strcmp(member, "mText") &&
        (!std::strcmp(name, "txtTimeLeft") || !std::strcmp(name, "txtHomeScore") || !std::strcmp(name, "txtAwayScore") ||
         !std::strcmp(name, "txtQuarter") || !std::strcmp(name, "txtDownAndYards")))
    {
        // The clipboard's score box's scores and clock (before the first snap the game fills these, not the score bug's).
        // Only the quarter, clock and down: the career's screens show this box too, with the scores 0 and the rest blank.
        const std::string text = Text(base, value);
        Log("[scorebar] mcGameDay.%s \"%s\"\n", name, text.c_str());
        const bool scores = !std::strcmp(name, "txtHomeScore") || !std::strcmp(name, "txtAwayScore");
        if (!scores && text.find_first_not_of(' ') != std::string::npos)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_lastField = Clock::now();
        }
    }
    // A field's text, if the field is the score bug's.
    if (const Field field = FieldOf(name); field != kNone && !std::strcmp(member, "mText") && !std::strcmp(upName, kBug))
    {
        Touched(ctx, base);
        const std::string text = Text(base, value);
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string* slots[] = { &g_board.awayScore, &g_board.homeScore, &g_board.quarter,
                                 &g_board.clock,     &g_board.down,      &g_board.playClock };
        if (*slots[field] != text)
        {
            Log("[scorebar] %s \"%s\"\n", name, text.c_str());
            if (text.find_first_not_of(' ') != std::string::npos)
                g_lastField = Clock::now();
            // A play: the play clock stops at the snap (its last change before a pause; it changes again when the game
            // resets it after the play), and the down and distance changes after the whistle.
            static Clock::time_point playClockChanged{}, snap{};
            const auto now = Clock::now();
            if (field == kPlayClock)
            {
                if (playClockChanged != Clock::time_point{} && now - playClockChanged > std::chrono::milliseconds(1500))
                    snap = playClockChanged;
                playClockChanged = now;
                g_board.playClockChanged = now;
            }
            auto blank = [](const std::string& t) { return t.find_first_not_of(' ') == std::string::npos; };
            if (field == kDown && !blank(*slots[field]) && !blank(text))
            {
                // The snap: the play clock's last change if it has not changed since, else the last pause's start.
                const Clock::time_point start =
                    playClockChanged != Clock::time_point{} && now - playClockChanged > std::chrono::milliseconds(1500) ? playClockChanged : snap;
                if (start != Clock::time_point{})
                {
                    ++g_board.play;
                    g_board.playStart = start;
                    g_board.playEnd = now;
                }
            }
            *slots[field] = text;
        }
    }

    const bool isBug = !std::strcmp(name, kBug);
    if (isBug || (FieldOf(name) != kNone && !std::strcmp(upName, kBug)))
    {
        Touched(ctx, base);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bugUsed = Clock::now();
    }
    if (!std::strcmp(name, "mcBillboard") && std::find(g_billboards.begin(), g_billboards.end(), object) == g_billboards.end())
        g_billboards.push_back(object);
    const Role role = RoleOf(base, object);
    if (role == kNoRole)
    {
        __imp__sub_82B04600(ctx, base);
        return;
    }
    const PPCContext entry = ctx;
    if (role == kRoleBillboard && (!std::strcmp(member, "_x") || !std::strcmp(member, "_y")) && NumberOf(base, value, number))
    {
        (member[1] == 'x' ? g_visible[object].x : g_visible[object].y) = number;
        __imp__sub_82B04600(ctx, base);
        UpdateBillboards(entry, base);
        return;
    }
    if (role == kRolePrep && !std::strcmp(member, "_y") && NumberOf(base, value, number))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_prepY = number;
    }
    if (std::strcmp(member, "_visible") != 0)
    {
        __imp__sub_82B04600(ctx, base);
        return;
    }

    const bool on = base[value + 8] != 0;
    g_visibleName = ctx.r6.u32;
    if (isBug)
    {
        // The game's Booleans, from either: true is 12 bytes after false.
        const uint32_t other = on ? value - 12 : value + 12;
        if (!g_true && Word(base, other) == Word(base, value) && (base[other + 8] != 0) != on)
            g_true = on ? value : other;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (on && !g_wanted)
            g_teamsKnown = false;  // shown again: perhaps another game's
        if (g_wanted && !on)
            g_lastWanted = Clock::now();
        g_wanted = on;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (role == kRoleBillboard)
            g_parentVisible = on;
        if (role == kRolePrep)
            g_prepVisible = on;
    }
    Log("[scorebar] %s._visible = %s\n", name, on ? "true" : "false");
    if (!g_true)
    {
        __imp__sub_82B04600(ctx, base);
        return;
    }
    Visible& v = g_visible[object];
    v.game = on;
    const bool want = Wanted(role, v);
    ctx.r7.u64 = want ? g_true : g_true - 12;
    __imp__sub_82B04600(ctx, base);
    v.written = want;
    if (isBug || role == kRoleBillboard)
        UpdateBillboards(entry, base);
}
