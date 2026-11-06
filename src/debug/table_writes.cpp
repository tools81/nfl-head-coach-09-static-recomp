// HC09_TABLE_WRITES=DCHT,PLRM: finds every database in guest memory that holds those tables,
// write-protects the pages of their records and lets each write through one instruction at a time
// (single-step, then protect again). Each distinct writer stack is printed once as
// "[tw] new writer" with the table, record and field it wrote; "[tw] summary" lines count the
// writes per writer. ArmTableWrites is called from the database query hook (hooks.cpp) when a
// career loads ("delete from PLRM") and when an AI depth-chart pass starts, and re-finds the tables
// each time, so the watch follows them if they move.
//
// Pages are protected in all three views of physical memory. File reads into guest memory go through
// a host buffer (xboxkrnl_io.cpp), so the OS never writes a protected page.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "debug/stack_dump.h"
#include "memory.h"

namespace hc09::debug
{
    namespace
    {
        struct Field
        {
            char name[5];
            uint32_t bitOffset;
            uint32_t bits;
        };

        struct Watched
        {
            std::string table;
            uint32_t database;
            uint32_t records;      // guest address of record 0
            uint32_t recordBytes;
            uint32_t capacity;
            std::vector<Field> fields;
        };

        struct Writer
        {
            uint64_t count = 0;
            std::set<uint32_t> records;
            std::set<std::string> fields;
            std::string table;
        };

        std::mutex g_mutex;
        std::vector<Watched> g_tables;
        std::set<uintptr_t> g_pages;                    // host pages kept read-only
        std::map<std::string, Writer> g_writers;        // by unwound return addresses
        std::map<std::string, std::string> g_stacks;    // key -> symbolized stack
        uint64_t g_lastTotal = 0;
        uint64_t g_otherWrites = 0;                     // to watched pages outside the records
        thread_local std::vector<uintptr_t> t_stepping;  // pages opened for the current instruction

        constexpr uintptr_t kPage = 0x1000;

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

        // Physical memory is mapped three times (memory.cpp): 0xA0000000, 0xC0000000 and 0xE0000000
        // (unless HC09_NO_E_WINDOW) reach the same bytes, and the game may write through any of them.
        std::vector<uintptr_t> Aliases(uintptr_t page)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
            const uint32_t guest = uint32_t(page - base);
            if (guest < 0xA0000000u)
                return { page };
            std::vector<uintptr_t> list;
            for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
                if (window != 0xE0000000u || !std::getenv("HC09_NO_E_WINDOW"))
                    list.push_back(base + window + (guest & 0x1FFFFFFFu));
            return list;
        }

        void Protect(uintptr_t page, DWORD protection)
        {
            DWORD previous;
            VirtualProtect(reinterpret_cast<void*>(page), kPage, protection, &previous);
        }

        // Guest address of a write, folded onto the window the tables were found in.
        uint32_t Canonical(uint32_t guest, uint32_t like)
        {
            if (guest < 0xA0000000u || like < 0xA0000000u)
                return guest;
            return (like & 0xE0000000u) | (guest & 0x1FFFFFFFu);
        }

        // Return addresses of the faulting stack (cheap enough to run on every write).
        std::string StackKey(const CONTEXT& faulting)
        {
            CONTEXT context = faulting;
            std::string key;
            for (int depth = 0; depth < 12 && context.Rip; depth++)
            {
                char item[24];
                std::snprintf(item, sizeof(item), "%llX,", (unsigned long long)context.Rip);
                key += item;
                DWORD64 imageBase = 0;
                RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!function)
                    break;
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
            }
            return key;
        }

        std::vector<uint64_t> g_hashes;   // per g_tables entry, when armed
        std::set<size_t> g_changed;       // entries reported as changed

        uint64_t Hash(const Watched& table)
        {
            uint64_t hash = 1469598103934665603ull;
            const uint8_t* p = GuestToHost(table.records);
            for (uint32_t i = 0; i < table.capacity * table.recordBytes; i++)
                hash = (hash ^ p[i]) * 1099511628211ull;
            return hash;
        }

        // Whether the tables changed since they were armed: a copy the game does not use stays put.
        // `all` reports unchanged tables too; otherwise each change is reported once.
        void ReportChanges(bool all)
        {
            for (size_t i = 0; i < g_tables.size() && i < g_hashes.size(); i++)
            {
                const bool changed = Hash(g_tables[i]) != g_hashes[i];
                if (changed ? g_changed.insert(i).second || all : all)
                    std::fprintf(stderr, "[tw] %s at 0x%08X: %s since armed\n", g_tables[i].table.c_str(), g_tables[i].records,
                        changed ? "CHANGED" : "unchanged");
            }
            std::fflush(stderr);
        }

        void Summary(const char* reason)
        {
            uint64_t total = g_otherWrites;
            for (const auto& [key, writer] : g_writers)
                total += writer.count;
            if (total == g_lastTotal)
                return;
            g_lastTotal = total;
            std::fprintf(stderr, "[tw] summary (%s): %llu writes, %llu of them outside the records\n", reason,
                (unsigned long long)total, (unsigned long long)g_otherWrites);
            int index = 0;
            for (const auto& [key, writer] : g_writers)
            {
                std::string fields;
                for (const auto& field : writer.fields)
                    fields += (fields.empty() ? "" : ",") + field;
                // The innermost game function names the writer; the full stack was printed once.
                const std::string& stack = g_stacks[key];
                std::string top;
                for (size_t at = 0; (at = stack.find("sub_", at)) != std::string::npos && top.size() < 120;)
                {
                    const size_t end = stack.find_first_of("+ \n", at);
                    top += stack.substr(at, end - at) + " < ";
                    at = end;
                }
                std::fprintf(stderr, "[tw]   #%d %s: %llu writes, %zu records, fields %s | %s\n", index++, writer.table.c_str(),
                    (unsigned long long)writer.count, writer.records.size(), fields.c_str(), top.c_str());
            }
            std::fflush(stderr);
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP && !t_stepping.empty())
            {
                std::lock_guard lock(g_mutex);
                for (uintptr_t page : t_stepping)
                    if (g_pages.count(page))
                        Protect(page, PAGE_READONLY);
                t_stepping.clear();
                info->ContextRecord->EFlags &= ~0x100u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2 ||
                info->ExceptionRecord->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            const uintptr_t address = info->ExceptionRecord->ExceptionInformation[1];
            const uintptr_t page = address & ~(kPage - 1);
            std::lock_guard lock(g_mutex);
            if (!g_pages.count(page))
                return EXCEPTION_CONTINUE_SEARCH;
            const uint32_t written = uint32_t(address - reinterpret_cast<uintptr_t>(g_base));
            g_otherWrites++;  // taken back below when the write is to a record
            for (const Watched& table : g_tables)
            {
                const uint32_t guest = Canonical(written, table.records);
                if (guest < table.records || guest >= table.records + table.capacity * table.recordBytes)
                    continue;
                const uint32_t record = (guest - table.records) / table.recordBytes;
                const uint32_t byte = (guest - table.records) % table.recordBytes;
                std::string field = "?";
                for (const Field& f : table.fields)
                    if (byte >= f.bitOffset / 8 && byte < (f.bitOffset + f.bits + 7) / 8)
                    {
                        field = f.name;
                        break;
                    }
                g_otherWrites--;
                const std::string key = StackKey(*info->ContextRecord);
                Writer& writer = g_writers[key];
                writer.table = table.table;
                writer.count++;
                writer.records.insert(record);
                writer.fields.insert(field);
                if (!g_stacks.count(key))
                {
                    g_stacks[key] = StackText(info->ContextRecord);
                    std::fprintf(stderr, "[tw] new writer: %s record %u field %s (byte %u), written at 0x%08X, database 0x%08X:\n%s",
                        table.table.c_str(), record, field.c_str(), byte, written, table.database, g_stacks[key].c_str());
                    std::fflush(stderr);
                }
                break;
            }
            // Writes elsewhere on the page go through unlogged.
            Protect(page, PAGE_READWRITE);
            t_stepping.push_back(page);
            info->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void Scan(const std::vector<std::string>& names, std::vector<Watched>& found)
        {
            const std::pair<uint32_t, uint32_t> ranges[] = { { 0x00010000, 0x82000000 }, { 0xA0000000, 0xC0000000 } };
            for (auto [lo, hi] : ranges)
                for (uint32_t a = lo; a < hi - 64; a += 4)
                {
                    const uint8_t* p = GuestToHost(a);
                    if (p[0] != 'D' || p[1] != 'B' || p[2] != 0 || p[3] != 8)
                        continue;
                    const uint32_t size = Be32(p + 8), tables = Be32(p + 16);
                    if (tables == 0 || tables > 256 || size < 64 || size > 0x4000000)
                        continue;
                    const uint8_t* dir = p + 24;
                    const uint8_t* data = dir + tables * 8 + 4;
                    for (uint32_t t = 0; t < tables; t++)
                        for (const std::string& name : names)
                        {
                            const char reversed[4] = { name[3], name[2], name[1], name[0] };
                            if (std::memcmp(dir + t * 8, reversed, 4) != 0)
                                continue;
                            const uint8_t* table = data + Be32(dir + t * 8 + 4);
                            Watched watched{ name, a };
                            watched.recordBytes = Be32(table + 4);
                            watched.capacity = uint32_t(table[16]) << 8 | table[17];
                            const uint32_t fieldCount = table[24];
                            watched.records = uint32_t(table + 36 + 16 * fieldCount - GuestToHost(0));
                            for (uint32_t f = 0; f < fieldCount; f++)
                            {
                                const uint8_t* entry = table + 36 + 16 * f;
                                Field field{};
                                field.bitOffset = Be32(entry + 4);
                                for (int c = 0; c < 4; c++)
                                    field.name[c] = char(entry[11 - c]);
                                field.bits = Be32(entry + 12);
                                watched.fields.push_back(field);
                            }
                            if (watched.recordBytes == 0 || watched.recordBytes > 4096 || watched.capacity == 0)
                                continue;
                            std::fprintf(stderr, "[tw] %s in database 0x%08X: records at 0x%08X, %u of %u, %u bytes each\n", name.c_str(), a,
                                watched.records, uint32_t(table[18]) << 8 | table[19], watched.capacity, watched.recordBytes);
                            found.push_back(std::move(watched));
                        }
                }

            // The databases found this way can be copies (a save's image) while the game works on tables
            // kept elsewhere. Those share the field definitions and, until the game changes them, the
            // records: every other place holding either is watched too, as "<name>?".
            struct Pattern
            {
                size_t source;
                std::vector<uint8_t> bytes;
                uint32_t recordsAfter;  // offset from the match to record 0
                bool definitions;
            };
            std::vector<Pattern> patterns;
            const size_t copies = found.size();
            for (size_t i = 0; i < copies; i++)
            {
                const Watched& copy = found[i];
                const uint32_t definitions = copy.records - 16 * uint32_t(copy.fields.size());
                const uint8_t* d = GuestToHost(definitions);
                patterns.push_back({ i, std::vector<uint8_t>(d, d + 16 * copy.fields.size()), 16 * uint32_t(copy.fields.size()), true });
                const uint8_t* r = GuestToHost(copy.records);
                const uint32_t length = std::max(32u, copy.recordBytes * 4);
                patterns.push_back({ i, std::vector<uint8_t>(r, r + length), 0, false });
            }
            std::set<uint32_t> seen;
            for (const Watched& copy : found)
                seen.insert(copy.records);
            for (auto [lo, hi] : ranges)
                for (uint32_t a = lo; a < hi - 256; a += 4)
                {
                    const uint8_t* p = GuestToHost(a);
                    for (const Pattern& pattern : patterns)
                    {
                        if (p[0] != pattern.bytes[0] || std::memcmp(p, pattern.bytes.data(), pattern.bytes.size()) != 0)
                            continue;
                        const uint32_t records = a + pattern.recordsAfter;
                        if (!seen.insert(records).second)
                            continue;
                        Watched candidate = found[pattern.source];
                        candidate.table += "?";
                        candidate.database = 0;
                        candidate.records = records;
                        std::fprintf(stderr, "[tw] %s: %s match at 0x%08X, records from 0x%08X\n", candidate.table.c_str(),
                            pattern.definitions ? "field definitions" : "first records", a, records);
                        found.push_back(std::move(candidate));
                    }
                }
        }
    }

    void ArmTableWrites(const char* reason)
    {
        static const char* const setting = std::getenv("HC09_TABLE_WRITES");
        if (!setting)
            return;
        static std::vector<std::string> names = [] {
            std::vector<std::string> list;
            for (const char* p = setting; *p;)
            {
                const char* end = std::strchr(p, ',');
                const std::string name(p, end ? size_t(end - p) : std::strlen(p));
                if (name.size() == 4)
                    list.push_back(name);
                p = end ? end + 1 : p + std::strlen(p);
            }
            return list;
        }();
        static std::once_flag installed;
        std::call_once(installed, [] {
            // First in line: ahead of the GPU page tracking and the watch handlers.
            AddVectoredExceptionHandler(1, OnException);
            // Counts every 20 s while they change.
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    std::lock_guard lock(g_mutex);
                    Summary("periodic");
                    ReportChanges(false);
                }
            }).detach();
        });

        std::lock_guard lock(g_mutex);
        Summary(reason);
        ReportChanges(true);
        for (uintptr_t page : g_pages)
            Protect(page, PAGE_READWRITE);
        g_pages.clear();
        g_tables.clear();
        const auto start = std::chrono::steady_clock::now();
        Scan(names, g_tables);
        for (const Watched& table : g_tables)
        {
            const uintptr_t first = reinterpret_cast<uintptr_t>(GuestToHost(table.records)) & ~(kPage - 1);
            const uintptr_t last = reinterpret_cast<uintptr_t>(GuestToHost(table.records + table.capacity * table.recordBytes - 1)) & ~(kPage - 1);
            for (uintptr_t page = first; page <= last; page += kPage)
                for (uintptr_t alias : Aliases(page))
                    g_pages.insert(alias);
        }
        int failed = 0, readOnly = 0;
        for (uintptr_t page : g_pages)
        {
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_READONLY, &previous))
            {
                if (failed++ == 0)
                    std::fprintf(stderr, "[tw] VirtualProtect(guest 0x%08X) failed: error %lu\n",
                        uint32_t(page - reinterpret_cast<uintptr_t>(g_base)), GetLastError());
            }
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) && memory.Protect == PAGE_READONLY)
                readOnly++;
        }
        g_hashes.clear();
        g_changed.clear();
        for (const Watched& table : g_tables)
            g_hashes.push_back(Hash(table));
        std::fprintf(stderr, "[tw] %d of %zu pages read-only, %d VirtualProtect failures\n", readOnly, g_pages.size(), failed);
        std::fprintf(stderr, "[tw] armed (%s): %zu tables, %zu pages, scan %lld ms\n", reason, g_tables.size(), g_pages.size(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        std::fflush(stderr);
    }
}
