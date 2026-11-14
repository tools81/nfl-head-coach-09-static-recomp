#include "roster.h"

#include <algorithm>
#include <map>
#include <set>

namespace hc09::editor
{
    const char* const kPositions[21] = { "QB", "HB", "FB", "WR", "TE", "LT", "LG", "C", "RG", "RT", "LE", "RE", "DT",
                                         "LOLB", "MLB", "ROLB", "CB", "FS", "SS", "K", "P" };
    const char* const kCoachRoles[11] = { "Head Coach", "Off. Coordinator", "Def. Coordinator", "S.T. Coordinator",
                                          "Quarterbacks", "Running Backs", "Wide Receivers", "Offensive Line",
                                          "Defensive Line", "Linebackers", "Defensive Backs" };

    namespace
    {
        // The depth the game allows at each depth chart position (DCHT.PPOS; 91 in all): 0-20 the positions,
        // 21 kick returner, 22 punt returner, 23 kickoff, 24 long snapper, 25 third-down back. A deeper chart
        // makes the game hang at the kickoff (tools/roster_import/import_roster.py POSITION_DEPTH).
        constexpr int kDepth[26] = { 3, 4, 3, 6, 3, 3, 3, 3, 3, 3, 3, 3, 5, 3, 4, 3, 5, 3, 3, 3, 3, 5, 5, 3, 3, 3 };
        constexpr int kNoPlayer = 32767;

        const char* const kContract[] = { "PCSA", "PTSA", "PVTS", "PVSB", "PSBO", "PCON", "PCYL", "PSA0", "PSB0", "PSA1",
                                          "PSB1", "PSA2", "PSB2", "PSA3", "PSB3", "PSA4", "PSB4", "PSA5", "PSB5", "PSA6", "PSB6" };
    }

    bool Roster::Load(const std::filesystem::path& file, std::wstring& error)
    {
        if (!save.Load(file, error))
            return false;
        play = save.db.Find("PLAY");
        coch = save.db.Find("COCH");
        team = save.db.Find("TEAM");
        dcht = save.db.Find("DCHT");
        cskl = save.db.Find("CSKL");
        if (!play || !coch || !team || !dcht || save.db.Find("SEAI"))
        {
            error = file.filename().wstring() + L" is not a roster save (careers cannot be edited).";
            return false;
        }
        path = file;
        dirty = false;
        std::wstring lower = file.wstring();
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        hc27 = lower.find(L"hc27") != std::wstring::npos;
        teams.clear();
        for (uint32_t i = 0; i < team->count; i++)
        {
            const int id = int(team->Get(i, "TGID"));
            if (id >= 1 && id <= 32)
                teams.push_back({ id, team->Text(i, "TLNA") + " " + team->Text(i, "TDNA") });
        }
        std::sort(teams.begin(), teams.end(), [](const TeamEntry& a, const TeamEntry& b) { return a.name < b.name; });
        teams.push_back({ kFreeAgents, "Free Agents" });
        return true;
    }

    bool Roster::Save(std::wstring& error)
    {
        std::filesystem::path backup = path;
        backup += L".bak";
        std::error_code ec;
        if (!std::filesystem::exists(backup))
            std::filesystem::copy_file(path, backup, ec);
        if (!save.Save(path, error))
            return false;
        dirty = false;
        return true;
    }

    std::string Roster::TeamName(int id) const
    {
        for (const TeamEntry& t : teams)
            if (t.id == id)
                return t.name;
        return "Team " + std::to_string(id);
    }

    std::vector<uint32_t> Roster::Players(int teamId) const
    {
        std::vector<uint32_t> records;
        for (uint32_t i = 0; i < play->count; i++)
            if (play->Get(i, "TGID") == teamId)
                records.push_back(i);
        std::stable_sort(records.begin(), records.end(), [&](uint32_t a, uint32_t b) {
            const int64_t pa = play->Get(a, "PPOS"), pb = play->Get(b, "PPOS");
            return pa != pb ? pa < pb : play->Get(a, "POVR") > play->Get(b, "POVR");
        });
        return records;
    }

    std::vector<uint32_t> Roster::Coaches(int teamId) const
    {
        std::vector<uint32_t> records;
        const int id = teamId == kFreeAgents ? kCoachFreeAgents : teamId;
        for (uint32_t i = 0; i < coch->count; i++)
            if (coch->Get(i, "TGID") == id)
                records.push_back(i);
        std::stable_sort(records.begin(), records.end(), [&](uint32_t a, uint32_t b) { return coch->Get(a, "COPS") < coch->Get(b, "COPS"); });
        return records;
    }

    std::string Roster::PlayerName(uint32_t record) const { return play->Text(record, "PFNA") + " " + play->Text(record, "PLNA"); }
    std::string Roster::CoachName(uint32_t record) const { return coch->Text(record, "CFNM") + " " + coch->Text(record, "CLNM"); }

    void Roster::MovePlayer(uint32_t record, int teamId)
    {
        const int from = int(play->Get(record, "TGID"));
        if (from == teamId)
            return;
        // The league's lowest cap hit, for a free agent's new deal (in $10,000s).
        int64_t lowest = 0;
        for (uint32_t i = 0; i < play->count; i++)
        {
            const int64_t t = play->Get(i, "TGID"), hit = play->Get(i, "PCSA");
            if (t >= 1 && t <= 32 && hit > 0 && (!lowest || hit < lowest))
                lowest = hit;
        }
        play->Set(record, "TGID", teamId);
        play->Set(record, "PPTI", from);
        play->Set(record, "PYWT", 0);
        if (teamId == kFreeAgents)
        {
            for (const char* field : kContract)
                play->Set(record, field, 0);
        }
        else
        {
            if (play->Get(record, "PCON") == 0 || play->Get(record, "PCYL") == 0)
            {
                for (const char* field : kContract)
                    play->Set(record, field, 0);
                play->Set(record, "PCON", 1);
                play->Set(record, "PCYL", 1);
                play->Set(record, "PSA0", lowest);
                play->Set(record, "PCSA", lowest);
                play->Set(record, "PTSA", lowest);
                play->Set(record, "PVTS", lowest);
            }
            // A number no new teammate wears: his own if free, else the nearest above or below.
            std::set<int64_t> worn;
            for (uint32_t i = 0; i < play->count; i++)
                if (i != record && play->Get(i, "TGID") == teamId)
                    worn.insert(play->Get(i, "PJEN"));
            const int64_t number = play->Get(record, "PJEN");
            for (int step = 0; step < 99 && worn.count(play->Get(record, "PJEN")); step++)
            {
                for (const int64_t candidate : { number + step, number - step })
                    if (candidate >= 1 && candidate <= 99 && !worn.count(candidate))
                    {
                        play->Set(record, "PJEN", candidate);
                        break;
                    }
            }
        }
        RebuildDepthChart(from);
        RebuildDepthChart(teamId);
        RateTeams();
        dirty = true;
    }

    // The team's players at each position, best first, as deep as the game allows, then the special-teams
    // slots (tools/roster_import/import_roster.py build_depth_charts). The team keeps the rows it has in the
    // table: with fewer, the deepest slots are left out; with more, the kickoff entry is repeated (a
    // harmless duplicate the game sorts out).
    void Roster::RebuildDepthChart(int teamId)
    {
        if (teamId < 1 || teamId > 32)
            return;
        std::vector<uint32_t> rows;
        for (uint32_t i = 0; i < dcht->count; i++)
            if (dcht->Get(i, "TGID") == teamId)
                rows.push_back(i);
        if (rows.empty())
            return;
        std::map<int, std::vector<uint32_t>> byPosition;
        std::vector<uint32_t> all;
        for (uint32_t i = 0; i < play->count; i++)
            if (play->Get(i, "TGID") == teamId)
            {
                byPosition[int(play->Get(i, "PPOS"))].push_back(i);
                all.push_back(i);
            }
        auto ranked = [&](std::vector<uint32_t> list, auto score) {
            std::stable_sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) { return score(a) > score(b); });
            return list;
        };
        auto overall = [&](uint32_t i) { return play->Get(i, "POVR"); };
        struct Entry
        {
            int position, depth;
            int64_t player;
        };
        std::vector<Entry> entries;
        auto add = [&](int position, const std::vector<uint32_t>& list, size_t most) {
            for (size_t depth = 0; depth < list.size() && depth < most; depth++)
                entries.push_back({ position, int(depth), play->Get(list[depth], "PGID") });
        };
        for (int position = 0; position < 21; position++)
            add(position, ranked(byPosition[position], overall), size_t(kDepth[position]));
        std::vector<uint32_t> returners;
        for (const uint32_t i : all)
        {
            const int64_t p = play->Get(i, "PPOS");
            if (p == 1 || p == 3 || p == 16 || p == 17 || p == 18)
                returners.push_back(i);
        }
        returners = ranked(returners, [&](uint32_t i) { return play->Get(i, "PKRT") * 2 + play->Get(i, "PSPD"); });
        add(21, returners, 2);
        add(22, returners, 1);
        add(23, ranked(byPosition[19], overall), 1);
        add(24, ranked(byPosition[7], [&](uint32_t i) { return -play->Get(i, "POVR"); }), 1);  // the backup snaps
        add(25, ranked(byPosition[1], [&](uint32_t i) { return play->Get(i, "PCTH") + play->Get(i, "PPBK"); }), 1);

        while (entries.size() > rows.size())
        {
            // Never a starter: the deepest slot goes first.
            const auto deepest = std::max_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.depth < b.depth; });
            entries.erase(deepest);
        }
        Entry filler{ 23, 0, kNoPlayer };
        for (const Entry& e : entries)
            if (e.position == 23)
                filler = e;
        if (filler.player == kNoPlayer && !entries.empty())
            filler = entries.back();
        while (entries.size() < rows.size())
            entries.push_back(filler);
        for (size_t k = 0; k < rows.size(); k++)
        {
            dcht->Set(rows[k], "PPOS", entries[k].position);
            dcht->Set(rows[k], "ddep", entries[k].depth);
            dcht->Set(rows[k], "PGID", entries[k].player);
            dcht->Set(rows[k], "DCLK", 7);
        }
        dirty = true;
    }

    // The ratings Team Select shows, from each team's best players at each position, spread over the range
    // the roster's ratings had (tools/roster_import/import_roster.py rate_teams).
    void Roster::RateTeams()
    {
        struct Group
        {
            const char* field;
            std::vector<std::pair<int, int>> spec;  // position, how many
        };
        static const Group kGroups[] = {
            { "TRQB", { { 0, 1 } } }, { "TRRB", { { 1, 1 } } }, { "TWRR", { { 3, 3 }, { 4, 1 } } },
            { "TROL", { { 5, 1 }, { 6, 1 }, { 7, 1 }, { 8, 1 }, { 9, 1 } } }, { "TRDL", { { 10, 1 }, { 11, 1 }, { 12, 2 } } },
            { "TRLB", { { 13, 1 }, { 14, 1 }, { 15, 1 } } }, { "TRDB", { { 16, 2 }, { 17, 1 }, { 18, 1 } } }, { "TRST", { { 19, 1 }, { 20, 1 } } },
        };
        std::map<int, std::map<int, std::vector<int64_t>>> byTeam;
        for (uint32_t i = 0; i < play->count; i++)
            byTeam[int(play->Get(i, "TGID"))][int(play->Get(i, "PPOS"))].push_back(play->Get(i, "POVR"));
        std::vector<uint32_t> rows;
        for (uint32_t i = 0; i < team->count; i++)
            if (team->Get(i, "TGID") >= 1 && team->Get(i, "TGID") <= 32)
                rows.push_back(i);
        std::map<std::string, std::map<uint32_t, double>> raw;
        for (const uint32_t i : rows)
        {
            auto& players = byTeam[int(team->Get(i, "TGID"))];
            std::map<std::string, double> r;
            for (const Group& group : kGroups)
            {
                double sum = 0;
                int n = 0;
                for (const auto& [position, most] : group.spec)
                {
                    std::vector<int64_t>& list = players[position];
                    std::sort(list.begin(), list.end(), std::greater<>());
                    for (int k = 0; k < most && k < int(list.size()); k++, n++)
                        sum += double(list[k]);
                }
                r[group.field] = n ? sum / n : 0;
            }
            r["TROF"] = 0.35 * r["TRQB"] + 0.25 * r["TWRR"] + 0.25 * r["TROL"] + 0.15 * r["TRRB"];
            r["TRDE"] = 0.35 * r["TRDL"] + 0.25 * r["TRLB"] + 0.40 * r["TRDB"];
            r["TROV"] = 0.45 * r["TROF"] + 0.45 * r["TRDE"] + 0.10 * r["TRST"];
            for (const auto& [field, value] : r)
                raw[field][i] = value;
        }
        for (const auto& [field, values] : raw)
        {
            int64_t oldLow = 127, oldHigh = 0;
            double low = 1e9, high = -1e9;
            for (const uint32_t i : rows)
            {
                oldLow = std::min(oldLow, team->Get(i, field));
                oldHigh = std::max(oldHigh, team->Get(i, field));
                low = std::min(low, values.at(i));
                high = std::max(high, values.at(i));
            }
            for (const uint32_t i : rows)
            {
                const double share = high > low ? (values.at(i) - low) / (high - low) : 0.5;
                team->Set(i, field, int64_t(double(oldLow) + share * double(oldHigh - oldLow) + 0.5));
            }
        }
        dirty = true;
    }

    int Roster::Portrait(bool coach, uint32_t record) const
    {
        return int(coach ? coch->Get(record, "PNPt") : play->Get(record, "PSXP"));
    }

    int Roster::OwnPortrait(bool coach, uint32_t record, const std::set<int>& library)
    {
        Table* t = coach ? coch : play;
        const char* field = coach ? "PNPt" : "PSXP";
        std::map<int, int> counts;
        for (uint32_t i = 0; i < t->count; i++)
            counts[int(t->Get(i, field))]++;
        int generic = 0, most = 0;
        for (const auto& [id, n] : counts)
            if (n > most && (coach || id != 0))
            {
                generic = id;
                most = n;
            }
        auto assign = [&](uint32_t who, int id) {
            t->Set(who, field, id);
            if (coach)
                t->Set(who, "CSXP", id);
            dirty = true;
        };
        const int current = int(t->Get(record, field));
        if (counts[current] == 1 && current != generic && (coach || current != 0))
            return current;
        for (const int id : library)
            if (id && id != generic && !counts.count(id))
            {
                assign(record, id);
                return id;
            }
        // No unused id: the one of the least important person without a team who has had his to himself.
        int64_t worst = 1 << 30;
        uint32_t donor = UINT32_MAX;
        for (uint32_t i = 0; i < t->count; i++)
        {
            const int id = int(t->Get(i, field));
            if (i == record || t->Get(i, "TGID") <= 32 || counts[id] != 1 || id == generic || (!coach && id == 0))
                continue;
            const int64_t rating = coach ? t->Get(i, "POVS") : t->Get(i, "POVR");
            if (rating < worst)
            {
                worst = rating;
                donor = i;
            }
        }
        if (donor == UINT32_MAX)
            return -1;
        const int id = int(t->Get(donor, field));
        assign(donor, generic);
        assign(record, id);
        return id;
    }

    std::filesystem::path Roster::PortraitFolder(bool coach, const std::filesystem::path& root) const
    {
        return (hc27 ? root / L"hc27" : root) / L"textures" / L"swappables" / (coach ? L"CoachPortraits" : L"PlayerPortraits");
    }
}
