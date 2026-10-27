#include "common/edition.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "kernel/guest.h"
#include "kernel/vfs.h"

namespace hc09::edition
{
#ifdef HC09_EDITION_HC27
    namespace
    {
        // The default roster: a TDB database (16 tables, PLAY players, COCH coaches, ...) stored
        // uncompressed in qkl_boot.ast. A roster save made in the game (MC02 container) holds a
        // database of the same layout, which tools/roster_import edits without changing it.
        constexpr const char* kRosterArchive = "qkl_boot.ast";
        constexpr uint64_t kRosterOffset = 0x72CD90;
        constexpr const wchar_t* kRosterFile = L"hc27\\ROSTER";
        constexpr const wchar_t* kPatchFolder = L"hc27\\disc";

        uint32_t BigEndian32(const uint8_t* bytes)
        {
            return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
        }

        // Header: "DB" 00 08, u32, u32 database size, u32 0, u32 table count, u32 crc; then the
        // table directory (count x name and offset). Returns the header plus directory size, or 0.
        size_t DatabaseLayoutSize(const uint8_t* bytes, size_t available)
        {
            if (available < 24 || std::memcmp(bytes, "DB\x00\x08", 4) != 0)
                return 0;
            const uint32_t tables = BigEndian32(bytes + 16);
            const size_t layout = 24 + size_t(tables) * 8;
            return tables && tables < 256 && layout <= available ? layout : 0;
        }
    }

    bool Apply(std::string& error)
    {
        std::ifstream file(kRosterFile, std::ios::binary);
        std::vector<uint8_t> save(std::istreambuf_iterator<char>(file), {});
        if (save.size() < 28 || std::memcmp(save.data(), "MC02", 4) != 0)
        {
            error = "HC27 needs its roster, hc27\\ROSTER, which tools\\roster_import builds (see tools\\roster_import\\README.md).";
            return false;
        }
        // The database is the save's second section, after the 28-byte header and the first.
        const size_t start = 28 + BigEndian32(save.data() + 8);
        const size_t layout = start < save.size() ? DatabaseLayoutSize(save.data() + start, save.size() - start) : 0;
        const uint32_t size = layout ? BigEndian32(save.data() + start + 8) : 0;
        if (!layout || start + size > save.size())
        {
            error = "hc27\\ROSTER is not a roster save.";
            return false;
        }

        // Only replace a database of exactly the same layout (sizes and table directory).
        std::vector<uint8_t> disc(layout);
        if (!ReadDiscFile(kRosterArchive, kRosterOffset, disc.data(), uint32_t(disc.size())) ||
            std::memcmp(disc.data(), save.data() + start, 4) != 0 || BigEndian32(disc.data() + 8) != size ||
            std::memcmp(disc.data() + 16, save.data() + start + 16, 4) != 0 ||
            std::memcmp(disc.data() + 24, save.data() + start + 24, layout - 24) != 0)
        {
            error = "This disc image's default roster is not where HC27 expects it, or hc27\\ROSTER was not built from "
                    "a roster of this game. Rebuild it with tools\\roster_import.";
            return false;
        }
        OverlayDiscFile(kRosterArchive, kRosterOffset, std::vector<uint8_t>(save.begin() + start, save.begin() + start + size));
        std::fprintf(stderr, "[hc27] default roster replaced by %ls (%u bytes)\n", kRosterFile, size);

        // Other patches of the disc's files (tools/roster_import/league_salaries.py: the league's
        // finances), each named <archive>@<hex offset>.bin.
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(kPatchFolder, ignored))
        {
            const std::string name = entry.path().filename().string();
            const size_t at = name.find('@');
            if (at == std::string::npos || entry.path().extension() != ".bin")
                continue;
            const uint64_t offset = std::strtoull(name.c_str() + at + 1, nullptr, 16);
            std::ifstream patch(entry.path(), std::ios::binary);
            std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(patch), {});
            if (bytes.empty() || !OverlayDiscFile(name.substr(0, at), offset, std::move(bytes)))
                std::fprintf(stderr, "[hc27] %ls\\%s does not fit the disc image; ignored\n", kPatchFolder, name.c_str());
        }
        return true;
    }

    // Teams tools/roster_import renames (TEAM_RENAMES in import_roster.py): name in the roster ->
    // name in the game's files. The game names a team's assets after its name in the roster
    // (redskinscampus_big.ast, CAMPUS_REDSKINS, REDSKINS_LOGOICON_COL, ...: its uniforms, helmets,
    // career campus) and finds them by a hash of that name, so for a renamed team the old name is
    // hashed instead. The old name must not be longer (it is written over the new one for the hash).
    constexpr std::pair<const char*, const char*> kRenamedTeams[] = { { "commanders", "redskins" } };

    std::string RenameTeamAsset(char* name)
    {
        const size_t length = std::strlen(name);
        for (const auto& [renamed, original] : kRenamedTeams)
        {
            const size_t renamedLength = std::strlen(renamed), originalLength = std::strlen(original);
            if (length == renamedLength)
                continue;  // the name itself
            for (size_t at = 0; at + renamedLength <= length; at++)
            {
                if (_strnicmp(name + at, renamed, renamedLength) != 0)
                    continue;
                std::string saved(name);
                for (size_t i = 0; i < originalLength; i++)  // in the case the name is written in
                    name[at + i] = std::isupper(static_cast<unsigned char>(name[at])) ? char(std::toupper(original[i])) : original[i];
                std::memmove(name + at + originalLength, name + at + renamedLength, length - at - renamedLength + 1);
                return saved;
            }
        }
        return {};
    }
#else
    bool Apply(std::string&)
    {
        return true;
    }
#endif
}

#ifdef HC09_EDITION_HC27
// The game's name hash (r3 a NUL-terminated name, r4 the seed; 64-bit FNV-1a of the name in upper
// case), which finds files among those of the loaded archives.
extern "C" PPC_FUNC(__imp__sub_82998B08);
PPC_FUNC(sub_82998B08)
{
    char* name = reinterpret_cast<char*>(hc09::GuestToHost(ctx.r3.u32));
    const std::string original = hc09::edition::RenameTeamAsset(name);
    __imp__sub_82998B08(ctx, base);
    if (!original.empty())
        std::memcpy(name, original.c_str(), original.size() + 1);
}

// The years the game shows. Its calendar counts days from 1 January 2008 and seasons from 2008
// (season 0); HC27 starts in 2026, so the years it shows are 18 later. The calendar itself, with
// its weekdays and leap days, is left as it is (it decides when games are played): only the year
// the functions below give out changes.
namespace
{
    constexpr int kYearShift = 2026 - 2008;
}

// The first season's year (2008).
extern "C" PPC_FUNC(__imp__sub_82C93A70);
PPC_FUNC(sub_82C93A70)
{
    __imp__sub_82C93A70(ctx, base);
    ctx.r3.s64 += kYearShift;
}

// The current season's year (2008 + the season number).
extern "C" PPC_FUNC(__imp__sub_82C93A88);
PPC_FUNC(sub_82C93A88)
{
    __imp__sub_82C93A88(ctx, base);
    ctx.r3.s64 += kYearShift;
}

// A time (r4, seconds) into a date (r3): month, day, weekday, then the year at +12 (and the years
// since 2008 at +14, which the game counts with) and the time of day.
extern "C" PPC_FUNC(__imp__sub_82C92CE0);
PPC_FUNC(sub_82C92CE0)
{
    const uint32_t date = ctx.r3.u32;
    __imp__sub_82C92CE0(ctx, base);
    PPC_STORE_U16(date + 12, uint16_t(PPC_LOAD_U16(date + 12) + kYearShift));
}

// Whether a date's year (+12) is a leap year, which the game asks on 1 March: judged on the
// calendar's own year, so the days stay those of the calendar.
PPC_FUNC(sub_82C93150)
{
    const int year = PPC_LOAD_U16(ctx.r3.u32 + 12) - kYearShift;
    ctx.r3.u64 = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}
#endif
