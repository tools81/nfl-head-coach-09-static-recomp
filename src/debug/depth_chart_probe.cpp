// HC09_PROBE_DC=1: logs what the AI depth-chart builder sub_8265ABE0 (one team's chart, reached from
// sub_825C4670 -> sub_825B9B78 -> sub_8265D450) calls while it runs: each callee's first arguments
// and its results (r3 and f1), indented by call depth, as "[dc] ..." lines. Only the first
// HC09_PROBE_DC_CHARTS charts (default 40) are logged, up to HC09_PROBE_DC_CALLS calls per
// function per chart (default 40); a count of every call per function closes each chart.
// For finding what the AI ranks players by; it changes nothing.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "debug/stack_dump.h"
#include "kernel/guest.h"

using namespace hc09;

namespace
{
    const bool g_enabled = std::getenv("HC09_PROBE_DC") != nullptr;
    const int g_maxCharts = std::getenv("HC09_PROBE_DC_CHARTS") ? std::atoi(std::getenv("HC09_PROBE_DC_CHARTS")) : 40;
    const int g_maxCalls = std::getenv("HC09_PROBE_DC_CALLS") ? std::atoi(std::getenv("HC09_PROBE_DC_CALLS")) : 40;

    thread_local int t_depth = 0;          // probed calls in progress, inside a logged chart
    thread_local bool t_logging = false;   // inside a chart that is being logged
    thread_local std::map<std::string, int> t_calls;
    int g_charts = 0;

    // HC09_PROBE_DC_TEAMS=15,31: only those teams' charts (sub_8265ABE0's first argument).
    bool TeamWanted(uint32_t team)
    {
        static const char* const teams = std::getenv("HC09_PROBE_DC_TEAMS");
        if (!teams)
            return true;
        for (const char* p = teams; *p;)
        {
            char* end = nullptr;
            const unsigned long value = std::strtoul(p, &end, 10);
            if (end == p)
                break;
            if (value == team)
                return true;
            p = *end == ',' ? end + 1 : end;
        }
        return false;
    }

    // The candidates' records as they were before the chart was built.
    struct Snapshot
    {
        uint32_t record;
        uint32_t words[52];
    };
    thread_local std::vector<Snapshot> t_snapshots;

    // HC09_PROBE_SLOTS=1: a light league-wide log. Every chart: one "[sl] chart" line per candidate
    // (team, position, pgid, record team, ranking score w4, depth slot w10, record words 43-50
    // afterwards); every evaluation read: "[sl] eval pgid slot" with its eight scores, when they
    // differ from the last ones logged for that player and slot.
    const bool g_slots = std::getenv("HC09_PROBE_SLOTS") != nullptr;
    thread_local bool t_slotChart = false;
    thread_local std::vector<uint32_t> t_slotRecords;

    bool Readable(uint32_t address)
    {
        return (address >= 0x40000000 && address < 0x70000000) || (address >= 0xA0000000 && address < 0xC0000000);
    }

    // "1234" or "A7DB8B64[00001234 00050002]" for pointers into the heaps.
    std::string Value(uint32_t value)
    {
        char text[64];
        if (Readable(value) && (value & 3) == 0)
            std::snprintf(text, sizeof(text), "%08X[%08X %08X]", value, Load<uint32_t>(value), Load<uint32_t>(value + 4));
        else
            std::snprintf(text, sizeof(text), "%d", int32_t(value));
        return text;
    }

    thread_local bool t_eval = false;      // inside a logged sub_825E3A88 (HC09_PROBE_EVAL)

    template <void (*Original)(PPCContext&, uint8_t*)>
    void Probe(const char* name, PPCContext& ctx, uint8_t* base)
    {
        if (!t_logging && !t_eval)
        {
            Original(ctx, base);
            return;
        }
        const int count = ++t_calls[name];
        const bool log = t_eval || count <= g_maxCalls;
        const std::string args = Value(ctx.r3.u32) + " " + Value(ctx.r4.u32) + " " + Value(ctx.r5.u32) + " " + Value(ctx.r6.u32);
        const int depth = t_depth++;
        Original(ctx, base);
        t_depth--;
        if (log)
            std::fprintf(stderr, "[dc] %*s%s(%s) -> %s f1=%g\n", depth * 2, "", name, args.c_str(), Value(ctx.r3.u32).c_str(), ctx.f1.f64);
    }
}

#define HC09_PROBE(address)                                       \
    extern "C" PPC_FUNC(__imp__sub_##address);                    \
    PPC_FUNC(sub_##address) { Probe<__imp__sub_##address>("sub_" #address, ctx, base); }

// Called by sub_8265ABE0.
HC09_PROBE(82587F40)
HC09_PROBE(82D01020)
HC09_PROBE(82D00F58)
HC09_PROBE(82D07060)
HC09_PROBE(82CFDD30)
HC09_PROBE(82CFBFD0)
HC09_PROBE(82667E48)
HC09_PROBE(82658998)
HC09_PROBE(826406C8)
HC09_PROBE(8263A748)
HC09_PROBE(82632CB8)
HC09_PROBE(82632110)
HC09_PROBE(82620B48)
HC09_PROBE(82620298)
HC09_PROBE(8260B248)
HC09_PROBE(8260B220)
HC09_PROBE(82606060)
HC09_PROBE(825EFA08)
HC09_PROBE(825EBE20)
HC09_PROBE(825D9B98)
HC09_PROBE(825D5D78)
// Called by sub_825EFA08 (places one player).
HC09_PROBE(826C7158)
HC09_PROBE(825EBC80)
// Called by sub_825E3A88 (computes a player's eight evaluation scores).
HC09_PROBE(825DBC98)
HC09_PROBE(825D58C8)
HC09_PROBE(825D36B8)
HC09_PROBE(82627840)

// HC09_PROBE_EVAL=15633,26669 (player ids): logs sub_825E3A88(evaluator+16, pgid, ?, index, mask r7,
// r8, r9, scores*), which computes the eight scores the evaluator caches per player (the chart's
// record words 43-50), with every lookup it makes through sub_82CE53E8 (the game's named
// multi-dimensional tuning tables) and the scores before and after.
namespace
{
    bool PgidWanted(uint32_t pgid)
    {
        static const char* const pgids = std::getenv("HC09_PROBE_EVAL");
        if (!pgids)
            return false;
        for (const char* p = pgids; *p;)
        {
            char* end = nullptr;
            const unsigned long value = std::strtoul(p, &end, 10);
            if (end == p)
                break;
            if (value == pgid)
                return true;
            p = *end == ',' ? end + 1 : end;
        }
        return false;
    }

    std::string Words(uint32_t address, uint32_t count)
    {
        std::string text;
        for (uint32_t i = 0; i < count; i++)
            text += (i ? " " : "") + std::to_string(int32_t(Load<uint32_t>(address + 4 * i)));
        return text;
    }

    std::string TableName(uint32_t table)
    {
        std::string name;
        for (int i = 4; i < 8; i++)
        {
            const char c = char(Load<uint8_t>(table + i));
            name += c >= 32 && c < 127 ? c : '.';
        }
        return name;
    }
}

extern "C" PPC_FUNC(__imp__sub_82CE53E8);

// sub_82CE53E8(table, keys*, out*): table+20 key count, +16 output count, +32 key kinds,
// +52 strides, +80 value type (0 byte, 1 short, 2 word).
PPC_FUNC(sub_82CE53E8)
{
    if (!t_eval && !t_logging)
    {
        __imp__sub_82CE53E8(ctx, base);
        return;
    }
    if (!t_eval)
    {
        Probe<__imp__sub_82CE53E8>("sub_82CE53E8", ctx, base);
        return;
    }
    const uint32_t table = ctx.r3.u32, keys = ctx.r4.u32, out = ctx.r5.u32;
    const uint32_t keyCount = std::min<uint32_t>(Load<uint32_t>(table + 20), 8);
    const uint32_t outCount = std::min<uint32_t>(Load<uint32_t>(table + 16), 8);
    const std::string keyText = Words(keys, keyCount);
    static std::map<uint32_t, bool> described;
    if (!described[table])
    {
        described[table] = true;
        std::fprintf(stderr, "[ev]   table %08X '%s' (reversed '%c%c%c%c'): header %s; key kinds %s; strides %s; value type %u\n", table,
            TableName(table).c_str(), TableName(table)[3], TableName(table)[2], TableName(table)[1], TableName(table)[0],
            Words(table, 24).c_str(), Readable(Load<uint32_t>(table + 32)) ? Words(Load<uint32_t>(table + 32), keyCount).c_str() : "?",
            Readable(Load<uint32_t>(table + 52)) ? Words(Load<uint32_t>(table + 52), keyCount).c_str() : "?", Load<uint32_t>(table + 80));
    }
    __imp__sub_82CE53E8(ctx, base);
    std::fprintf(stderr, "[ev]   lookup '%s' %08X keys [%s] -> [%s]\n", TableName(table).c_str(), table, keyText.c_str(),
        Words(out, outCount).c_str());
}

extern "C" PPC_FUNC(__imp__sub_82644BE0);

// sub_82644BE0(evaluator, pgid, slot, out): reads a player's eight cached scores for one evaluation
// slot into out+8..+36; r3 is whether he has them.
PPC_FUNC(sub_82644BE0)
{
    if (!g_slots)
    {
        Probe<__imp__sub_82644BE0>("sub_82644BE0", ctx, base);
        return;
    }
    const uint32_t pgid = ctx.r4.u32, slot = ctx.r5.u32, out = ctx.r6.u32;
    Probe<__imp__sub_82644BE0>("sub_82644BE0", ctx, base);
    if (!(ctx.r3.u32 & 0xFF))
        return;
    static std::mutex mutex;
    static std::map<uint64_t, std::string> last;
    const std::string scores = Words(out + 8, 8);
    std::lock_guard<std::mutex> lock(mutex);
    std::string& seen = last[(uint64_t(pgid) << 8) | (slot & 0xFF)];
    if (seen != scores)
    {
        seen = scores;
        std::fprintf(stderr, "[sl] eval pgid %u slot %u scores %s\n", pgid, slot, scores.c_str());
    }
}

extern "C" PPC_FUNC(__imp__sub_825E3A88);

PPC_FUNC(sub_825E3A88)
{
    if (t_eval || !PgidWanted(ctx.r4.u32))
    {
        __imp__sub_825E3A88(ctx, base);
        return;
    }
    static int s_logged = 0;
    const uint32_t self = ctx.r3.u32, pgid = ctx.r4.u32, scores = ctx.r10.u32;
    std::fprintf(stderr, "[ev] sub_825E3A88 pgid %u r5 %d index %d mask r7 %016llX r8 %016llX r9 %d; self+48 %s self+64 %s "
                         "self+448 %d self+1200 %d self+1584 %d; scores before [%s]\n",
        pgid, ctx.r5.s32, ctx.r6.s32, (unsigned long long)ctx.r7.u64, (unsigned long long)ctx.r8.u64, ctx.r9.s32,
        Value(Load<uint32_t>(self + 48)).c_str(), Value(Load<uint32_t>(self + 64)).c_str(), int32_t(Load<uint32_t>(self + 448)),
        int32_t(Load<uint32_t>(self + 1200)), int32_t(Load<uint32_t>(self + 1584)), Words(scores, 8).c_str());
    if (s_logged++ < 3)
        debug::PrintCurrentStack();
    t_eval = true;
    __imp__sub_825E3A88(ctx, base);
    t_eval = false;
    std::fprintf(stderr, "[ev] sub_825E3A88 pgid %u done; scores after [%s]\n", pgid, Words(scores, 8).c_str());
    std::fflush(stderr);
}

// sub_82653590(team, position, vector*) collects, in array order, pointers to the 208-byte player
// records (array: base and count at 0x830C4F90) at that position: the team's own (+0 team, +12
// status, +32 position, +204 flag). Each candidate's record is dumped as 52 words.
extern "C" PPC_FUNC(__imp__sub_82653590);

PPC_FUNC(sub_82653590)
{
    if (t_slotChart)
    {
        const uint32_t vector = ctx.r5.u32;
        __imp__sub_82653590(ctx, base);
        for (uint32_t at = Load<uint32_t>(vector), end = Load<uint32_t>(vector + 4); at < end; at += 4)
            t_slotRecords.push_back(Load<uint32_t>(at));
        return;
    }
    if (!t_logging)
    {
        __imp__sub_82653590(ctx, base);
        return;
    }
    const uint32_t team = ctx.r3.u32, position = ctx.r4.u32, vector = ctx.r5.u32;
    __imp__sub_82653590(ctx, base);
    const uint32_t begin = Load<uint32_t>(vector), end = Load<uint32_t>(vector + 4);
    const uint32_t array = Load<uint32_t>(0x830C4F90), count = Load<uint32_t>(0x830C4F94);
    std::fprintf(stderr, "[dc] candidates team %u position %u: %u (array 0x%08X, %u records)\n", team, position, (end - begin) / 4, array,
        count);
    for (uint32_t at = begin; at < end && at - begin < 4 * 120; at += 4)
    {
        const uint32_t record = Load<uint32_t>(at);
        Snapshot snapshot{ record };
        std::string words;
        for (uint32_t w = 0; w < 52; w++)
        {
            char item[12];
            snapshot.words[w] = Load<uint32_t>(record + 4 * w);
            std::snprintf(item, sizeof(item), " %08X", snapshot.words[w]);
            words += item;
        }
        t_snapshots.push_back(snapshot);
        std::fprintf(stderr, "[dc]   cand %u #%u:%s\n", (at - begin) / 4, (record - array) / 208, words.c_str());
    }
}

extern "C" PPC_FUNC(__imp__sub_8265ABE0);

PPC_FUNC(sub_8265ABE0)
{
    if (g_slots && !t_slotChart && !t_logging)
    {
        const uint32_t team = ctx.r3.u32, position = ctx.r4.u32;
        t_slotChart = true;
        t_slotRecords.clear();
        __imp__sub_8265ABE0(ctx, base);
        t_slotChart = false;
        std::string text;
        for (const uint32_t record : t_slotRecords)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "[sl] chart team %u position %u pgid %u rteam %u w4 %d w10 %d words %s\n", team, position,
                Load<uint32_t>(record + 8), Load<uint32_t>(record), int32_t(Load<uint32_t>(record + 16)),
                int32_t(Load<uint32_t>(record + 40)), Words(record + 172, 8).c_str());
            text += line;
        }
        std::fputs(text.c_str(), stderr);
        return;
    }
    if (!g_enabled || t_logging || g_charts >= g_maxCharts || !TeamWanted(ctx.r3.u32))
    {
        __imp__sub_8265ABE0(ctx, base);
        return;
    }
    const int chart = ++g_charts;
    std::fprintf(stderr, "[dc] chart %d: sub_8265ABE0(%s %s %s %s)\n", chart, Value(ctx.r3.u32).c_str(), Value(ctx.r4.u32).c_str(),
        Value(ctx.r5.u32).c_str(), Value(ctx.r6.u32).c_str());
    t_logging = true;
    t_calls.clear();
    t_snapshots.clear();
    __imp__sub_8265ABE0(ctx, base);
    t_logging = false;
    // What the chart changed in the candidates' records (player id, then word: before -> after).
    for (const Snapshot& snapshot : t_snapshots)
    {
        std::string changes;
        for (uint32_t w = 0; w < 52; w++)
        {
            const uint32_t now = Load<uint32_t>(snapshot.record + 4 * w);
            if (now != snapshot.words[w])
            {
                char item[48];
                std::snprintf(item, sizeof(item), " w%u: %d -> %d", w, int32_t(snapshot.words[w]), int32_t(now));
                changes += item;
            }
        }
        if (!changes.empty())
            std::fprintf(stderr, "[dc]   changed player %u:%s\n", snapshot.words[2], changes.c_str());
    }
    std::string counts;
    for (const auto& [name, count] : t_calls)
        counts += " " + name + "=" + std::to_string(count);
    std::fprintf(stderr, "[dc] chart %d done -> %s; calls:%s\n", chart, Value(ctx.r3.u32).c_str(), counts.c_str());
    std::fflush(stderr);
}
