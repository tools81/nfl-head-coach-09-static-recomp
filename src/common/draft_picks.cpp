// HC27: the 2028 draft pick trades (see draft_picks.h).
//
// The career database is an EA TDB in guest memory, laid out as in a save (tools/roster_import/tdb.py):
// "DB" 00 08, the database size and table count, a directory of (reversed name, offset) pairs, then
// each table: a 36-byte header (record size at +4, capacity and count at +16/+18, field count at +24),
// 16-byte field definitions (type, bit offset, reversed name, bit count) and the bit-packed records,
// most significant bit first.

#include "common/draft_picks.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/edition.h"
#include "memory.h"

namespace hc09::edition
{
#ifdef HC09_EDITION_HC27
    namespace
    {
        struct Trade
        {
            int draft, round, from, to, original;  // original: 0 = from's own pick, or one it acquired
        };

        const std::vector<Trade>& Trades()
        {
            static const std::vector<Trade> trades = [] {
                std::vector<Trade> list;
                std::ifstream file("hc27\\draft_picks.txt");
                std::string line;
                while (std::getline(file, line))
                {
                    if (line.empty() || line[0] == '#')
                        continue;
                    std::istringstream fields(line);
                    Trade trade{};
                    if (fields >> trade.draft >> trade.round >> trade.from >> trade.to)
                    {
                        fields >> trade.original;
                        list.push_back(trade);
                    }
                }
                return list;
            }();
            return trades;
        }

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

        struct Field
        {
            uint32_t offset = 0, bits = 0;
        };

        // A DRPK table in guest memory.
        struct Picks
        {
            uint8_t* records = nullptr;
            uint32_t recordBytes = 0, count = 0;
            Field owner, original, number, year;  // DPID, DPOD, DPNM, DPYO

            uint32_t Get(uint32_t record, const Field& field) const
            {
                const uint8_t* base = records + size_t(record) * recordBytes;
                uint32_t value = 0;
                for (uint32_t i = 0; i < field.bits; i++)
                {
                    const uint32_t bit = field.offset + i;
                    value = value << 1 | ((base[bit / 8] >> (7 - bit % 8)) & 1);
                }
                return value;
            }

            void Set(uint32_t record, const Field& field, uint32_t value) const
            {
                uint8_t* base = records + size_t(record) * recordBytes;
                for (uint32_t i = 0; i < field.bits; i++)
                {
                    const uint32_t bit = field.offset + i;
                    const uint8_t mask = uint8_t(1 << (7 - bit % 8));
                    if ((value >> (field.bits - 1 - i)) & 1)
                        base[bit / 8] |= mask;
                    else
                        base[bit / 8] &= uint8_t(~mask);
                }
            }
        };

        bool ReadPicks(uint8_t* table, Picks& picks)
        {
            picks.recordBytes = Be32(table + 4);
            picks.count = uint32_t(table[18]) << 8 | table[19];
            const uint32_t fields = table[24];
            if (picks.count != 448 || picks.recordBytes == 0 || picks.recordBytes > 64 || fields > 32)
                return false;
            int found = 0;
            for (uint32_t f = 0; f < fields; f++)
            {
                const uint8_t* def = table + 36 + f * 16;
                Field field{ Be32(def + 4), Be32(def + 12) };
                Field* slot = std::memcmp(def + 8, "DIPD", 4) == 0 ? &picks.owner
                    : std::memcmp(def + 8, "DOPD", 4) == 0       ? &picks.original
                    : std::memcmp(def + 8, "MNPD", 4) == 0       ? &picks.number
                    : std::memcmp(def + 8, "OYPD", 4) == 0       ? &picks.year
                                                                 : nullptr;
                if (slot)
                {
                    *slot = field;
                    found++;
                }
            }
            picks.records = table + 36 + fields * 16;
            return found == 4;
        }

        // The 2028 trades for one pick table, when its next draft is 2027 (Chicago's first-round pick
        // held by Tennessee, from the template) and its 2028 picks are untouched.
        // Returns whether it changed the table.
        bool Apply(const Picks& picks)
        {
            constexpr uint32_t kChicago = 1, kTennessee = 30;
            const auto round = [&](uint32_t r) { return picks.Get(r, picks.number) / 32 + 1; };
            bool nextIs2027 = false;
            for (uint32_t r = 0; r < picks.count; r++)
                if (picks.Get(r, picks.year) == 0 && round(r) == 1 && picks.Get(r, picks.original) == kChicago &&
                    picks.Get(r, picks.owner) == kTennessee)
                    nextIs2027 = true;
            if (!nextIs2027)
                return false;
            for (uint32_t r = 0; r < picks.count; r++)
                if (picks.Get(r, picks.year) == 1 && picks.Get(r, picks.owner) != picks.Get(r, picks.original))
                    return false;  // applied before, or traded in the career since

            int moved = 0;
            for (const Trade& trade : Trades())
            {
                if (trade.draft != 2028)
                    continue;
                const uint32_t own = trade.original ? uint32_t(trade.original) : uint32_t(trade.from);
                int chosen = -1;
                for (uint32_t r = 0; r < picks.count && chosen < 0; r++)
                    if (picks.Get(r, picks.year) == 1 && round(r) == uint32_t(trade.round) && picks.Get(r, picks.original) == own &&
                        picks.Get(r, picks.owner) == uint32_t(trade.from))
                        chosen = int(r);
                for (uint32_t r = 0; r < picks.count && chosen < 0; r++)  // a pick `from` acquired
                    if (picks.Get(r, picks.year) == 1 && round(r) == uint32_t(trade.round) && picks.Get(r, picks.owner) == uint32_t(trade.from))
                        chosen = int(r);
                if (chosen < 0)
                    continue;
                picks.Set(uint32_t(chosen), picks.owner, uint32_t(trade.to));
                moved++;
            }
            std::fprintf(stderr, "[hc27] career draft picks: %d trades of 2028 picks applied\n", moved);
            return moved > 0;
        }

        const std::vector<uint32_t>& CrcTable()
        {
            static const std::vector<uint32_t> values = [] {
                std::vector<uint32_t> table(256);
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i << 24;
                    for (int k = 0; k < 8; k++)
                        c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : c << 1;
                    table[i] = c;
                }
                return table;
            }();
            return values;
        }

        void PutBe32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8), p[3] = uint8_t(value);
        }

        // The CRC that ends each table, over everything after its header (tdb.py table_crc): MSB-first
        // CRC-32, initial value 0xFFFFFFFF, no final xor. The game writes it to a save as it is in memory.
        void FixTableCrc(uint8_t* table, uint8_t* end)
        {
            uint32_t crc = 0xFFFFFFFF;
            for (const uint8_t* p = table + 36; p < end - 4; p++)
                crc = CrcTable()[(crc >> 24) ^ *p] ^ (crc << 8);
            PutBe32(end - 4, crc);
        }

        // The save container's checksum (tdb.py ea_crc, the game's sub_82F0D220).
        uint32_t ContainerCrc(const uint8_t* data, size_t size)
        {
            if (size < 4)
                return 0;
            uint32_t crc = ~Be32(data);
            for (size_t i = 4; i < size; i++)
                crc = CrcTable()[crc >> 24] ^ ((crc << 8) | data[i]);
            return ~crc;
        }

        // Applies the 2028 trades to the database at `db` (at most `limit` bytes) if it holds a career's
        // picks that want them. Returns whether it changed anything.
        bool ApplyToDatabase(uint8_t* db, uint64_t limit)
        {
            if (limit < 64 || db[0] != 'D' || db[1] != 'B' || db[2] != 0 || db[3] != 8)
                return false;
            const uint32_t size = Be32(db + 8), tables = Be32(db + 16);
            if (tables == 0 || tables > 256 || size < 64 || size > limit)
                return false;
            const uint8_t* directory = db + 24;
            uint8_t* data = db + 24 + tables * 8 + 4;
            bool changed = false;
            for (uint32_t t = 0; t < tables; t++)
            {
                if (std::memcmp(directory + t * 8, "KPRD", 4) != 0)
                    continue;
                Picks picks;
                uint8_t* table = data + Be32(directory + t * 8 + 4);
                if (!ReadPicks(table, picks) || !Apply(picks))
                    continue;
                // The table runs up to the next one in the database (or the database's end).
                uint8_t* end = db + size;
                for (uint32_t other = 0; other < tables; other++)
                {
                    uint8_t* start = data + Be32(directory + other * 8 + 4);
                    if (start > table && start < end)
                        end = start;
                }
                FixTableCrc(table, end);
                changed = true;
            }
            return changed;
        }
    }

    void ApplyDraftPickTrades()
    {
        if (Trades().empty())
            return;
        // The databases are allocated from the game's heaps: pool and virtual (0x40000000-0x70000000) or
        // physical (0xA0000000-0xC0000000).
        const std::pair<uint32_t, uint32_t> heaps[] = { { 0x40000000, 0x70000000 }, { 0xA0000000, 0xC0000000 } };
        for (auto [low, high] : heaps)
            for (uint32_t address = low; address < high - 64; address += 4)
            {
                uint8_t* db = GuestToHost(address);
                if (db[0] == 'D' && db[1] == 'B' && db[2] == 0 && db[3] == 8)
                    ApplyToDatabase(db, high - address);
            }
    }

    void ApplyDraftPickTradesToSave(const std::filesystem::path& file)
    {
        if (Trades().empty())
            return;
        std::vector<uint8_t> save;
        {
            std::ifstream in(file, std::ios::binary);
            save.assign(std::istreambuf_iterator<char>(in), {});
        }
        // MC02 container: "MC02", file size, section 1 and 2 sizes, their checksums, the checksum of the
        // 24 bytes before it; then section 1 and section 2 (the database).
        if (save.size() < 28 || std::memcmp(save.data(), "MC02", 4) != 0)
            return;
        const size_t size1 = Be32(save.data() + 8), size2 = Be32(save.data() + 12);
        if (28 + size1 + size2 > save.size())
            return;
        // Section 2 holds the career database after other data: try each database header in it.
        bool changed = false;
        const size_t start = 28 + size1, end = start + size2;
        for (size_t at = start; at + 64 < end; at++)
            if (save[at] == 'D' && save[at + 1] == 'B' && save[at + 2] == 0 && save[at + 3] == 8)
                changed |= ApplyToDatabase(save.data() + at, end - at);
        if (!changed)
            return;
        PutBe32(save.data() + 16, ContainerCrc(save.data() + 28, size1));
        PutBe32(save.data() + 20, ContainerCrc(save.data() + 28 + size1, size2));
        PutBe32(save.data() + 24, ContainerCrc(save.data(), 24));
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(save.data()), std::streamsize(save.size()));
        std::fprintf(stderr, "[hc27] career draft picks: %s updated\n", file.filename().string().c_str());
    }
#else
    void ApplyDraftPickTrades() {}
    void ApplyDraftPickTradesToSave(const std::filesystem::path&) {}
#endif
}
