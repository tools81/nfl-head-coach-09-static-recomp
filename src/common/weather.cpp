// Weather and time of day (roadmap 16): what the game decides for a game, and dev overrides to test it.
//
// The game keeps a game's conditions in its game-info record (GINF, one row), read through small
// database readers when a game's environment is set up (sub_827AC7D8, the script binding that sets
// the scene's isDomed, isNight and weather):
//   GWTG  temperature (degrees F; 55 when a mode resets it)       read by sub_82CFCE30('GTWG')
//   GWPG  precipitation (above 0.01: rain, or snow at 32 F or less) sub_82CFCE30('GPWG')
//   GWWG  wind; GWTH, GWCL: sky (2, 1 when reset)                  sub_82CFCE30
//   GTOD  kickoff, minutes after midnight (night after 18:00)      sub_82CFCDE8
// weather: 0 clear, 1 rain, 2 snow; none under a roof (STAD.STRY 1). The same is kept for the renderer
// (common/weather.h) as a game is set up.
//
// Dev switches (left out of the release build), given by the readers (the lighting set is chosen from them as the
// game loads) and written into the record as a game is set up (the scene takes the record's values another way;
// overriding only the readers left a game dry). They apply to the hub's scene too (a snowy Chicago in July):
//   HC09_WEATHER=clear|rain|snow|<temperature>,<precipitation>[,<wind>]
//   HC09_KICKOFF=HH:MM
// Every game's conditions are logged as "[weather] ...".
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_recomp_shared.h>

#include "common/guest_db.h"
#include "common/weather.h"
#include "vk/dev_env.h"

namespace
{
    struct Override
    {
        bool parsed = false;
        int temperature = -1000, precipitation = -1, wind = -1, kickoff = -1;
    };

    const Override& Overrides()
    {
        static Override o;
        if (o.parsed)
            return o;
        o.parsed = true;
        if (const char* w = Hc09Env("HC09_WEATHER"))
        {
            if (!std::strcmp(w, "clear"))
                o.temperature = 60, o.precipitation = 0, o.wind = 0;
            else if (!std::strcmp(w, "rain"))
                o.temperature = 50, o.precipitation = 70, o.wind = 10;
            else if (!std::strcmp(w, "snow"))
                o.temperature = 25, o.precipitation = 70, o.wind = 10;
            else
                std::sscanf(w, "%d,%d,%d", &o.temperature, &o.precipitation, &o.wind);
        }
        if (const char* k = Hc09Env("HC09_KICKOFF"))
        {
            int h = 0, m = 0;
            if (std::sscanf(k, "%d:%d", &h, &m) >= 1)
                o.kickoff = h * 60 + m;
        }
        return o;
    }
}

#if HC09_DEV_TOOLS
// The overrides at the readers too: the scene's lighting set (noon_cloudy, dusk_clear, ...) is chosen from them as the
// game loads, before it is set up; with the record alone a snowy game kept Coach Now's random clear late afternoon.
namespace
{
    constexpr uint32_t Key(const char (&name)[5])
    {
        // The readers take the field's name as the game spells it in its queries ('GTWG' for GWTG).
        return uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 |
               uint32_t(uint8_t(name[3]));
    }
}

// A GINF field by name (r3).
extern "C" PPC_FUNC(__imp__sub_82CFCE30);
PPC_FUNC(sub_82CFCE30)
{
    const uint32_t key = ctx.r3.u32;
    __imp__sub_82CFCE30(ctx, base);
    const Override& o = Overrides();
    if (key == Key("GTWG") && o.temperature > -1000)
        ctx.r3.u64 = uint32_t(o.temperature);
    else if (key == Key("GPWG") && o.precipitation >= 0)
        ctx.r3.u64 = uint32_t(o.precipitation);
    else if (key == Key("GWWG") && o.wind >= 0)
        ctx.r3.u64 = uint32_t(o.wind);
}

// GINF.GTOD, the kickoff in minutes.
extern "C" PPC_FUNC(__imp__sub_82CFCDE8);
PPC_FUNC(sub_82CFCDE8)
{
    __imp__sub_82CFCDE8(ctx, base);
    if (Overrides().kickoff >= 0)
        ctx.r3.u64 = uint32_t(Overrides().kickoff);
}
#endif

namespace hc09
{
    // HC09_WEATHER / HC09_KICKOFF written into the game-info record as a game is set up, so every reader agrees.
    void ApplyWeatherOverride(PPCContext& ctx, uint8_t* base)
    {
        const Override& o = Overrides();
        if (o.temperature <= -1000 && o.kickoff < 0)
            return;
        hc09::guest::Database db(ctx, base);
        if (!db.Ready())
            return;
        bool ok = true;
        if (o.temperature > -1000)
            ok &= db.Query("update 'FNIG' set 'GTWG' = \x85", { uint32_t(o.temperature) });
        if (o.precipitation >= 0)
            ok &= db.Query("update 'FNIG' set 'GPWG' = \x85", { uint32_t(o.precipitation) });
        if (o.wind >= 0)
            ok &= db.Query("update 'FNIG' set 'GWWG' = \x85", { uint32_t(o.wind) });
        if (o.kickoff >= 0)
            ok &= db.Query("update 'FNIG' set 'DOTG' = \x85", { uint32_t(o.kickoff) });
        std::fprintf(stderr, "[weather] override written to the game-info record%s\n", ok ? "" : " (a field failed)");
    }

    // Keeps the game-info record's conditions for the renderer and logs them (each field read on its own: one the
    // record lacks fails alone).
    void LogWeather(PPCContext& ctx, uint8_t* base, const char* when)
    {
        hc09::guest::Database db(ctx, base);
        if (!db.Ready())
        {
            std::fprintf(stderr, "[weather] %s: database not ready\n", when);
            return;
        }
        auto field = [&](const char* query) -> long long {
            return db.Query(query, { db.Out(0) }) ? (long long)(int32_t)db.Get(0) : -99999;
        };
        const long long stadium = field("select 'DIGS' into \x85 from 'FNIG'"), tod = field("select 'DOTG' into \x85 from 'FNIG'"),
                        temperature = field("select 'GTWG' into \x85 from 'FNIG'"), precipitation = field("select 'GPWG' into \x85 from 'FNIG'"),
                        wind = field("select 'GWWG' into \x85 from 'FNIG'"), sky = field("select 'HTWG' into \x85 from 'FNIG'"),
                        clouds = field("select 'LCWG' into \x85 from 'FNIG'");
        long long roof = -1;
        if (stadium >= 0 && db.Query("select 'YRTS' into  from 'DATS' where 'DIGS' = ", { db.Out(0), uint32_t(stadium) }))
            roof = (int32_t)db.Get(0);
        // The game's rules (sub_827AC7D8).
        const bool domed = roof == 1, night = tod * 60 > 64800;
        const Weather weather = domed || precipitation <= 0 ? Weather::Clear : temperature <= 32 ? Weather::Snow : Weather::Rain;
        g_weather = int(weather);
        g_nightGame = night;
        g_precipitation = precipitation > 0 ? int(precipitation) : 0;
        g_wind = wind > 0 ? int(wind) : 0;
        g_temperature = temperature > -99999 ? int(temperature) : 55;
        g_domed = domed;
        g_kickoffMinutes = tod > 0 ? int(tod) : 0;
        std::fprintf(stderr, "[weather] %s: %s%s%s; ", when, weather == Weather::Snow ? "snow" : weather == Weather::Rain ? "rain" : "clear",
                     domed ? " (dome)" : "", night ? ", night" : Overcast() ? ", overcast day" : ", day");
        std::fprintf(stderr, "stadium %lld, kickoff %lld (%02lld:%02lld), temperature %lld F, precipitation %lld, wind %lld, GWTH %lld, GWCL %lld%s\n",
                     stadium, tod, tod / 60, tod % 60, temperature, precipitation, wind, sky, clouds,
                     (Overrides().temperature > -1000 || Overrides().kickoff >= 0) ? " (set by HC09_WEATHER/HC09_KICKOFF)" : "");
    }
}

// A scene's environment is set up (isDomed, isNight, weather): log what it is given.
extern "C" PPC_FUNC(__imp__sub_827AC7D8);
PPC_FUNC(sub_827AC7D8)
{
    hc09::LogWeather(ctx, base, "environment");
    __imp__sub_827AC7D8(ctx, base);
}
