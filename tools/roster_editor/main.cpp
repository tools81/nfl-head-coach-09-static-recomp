// HC09RosterEditor.exe: edits the rosters of NFL Head Coach 09 and HC27 (hc27\ROSTER, the roster HC27 starts
// from, and rosters saved from the game's File Management screen).
//
// Pick a team to list its coaches and players; pick one to see his portrait and everything the roster holds
// about him, in groups. Double-click a value (or press Enter) to change it. Players can be moved between
// teams and the free agent pool; a picture file can replace a portrait. For HC27, the draft picks and the
// coaches' special skills a new career starts with can be changed too: they are not in the roster but in the
// game's databases, so they are written to hc27\disc (gamedata.h).

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gamedata.h"
#include "picture.h"
#include "resource.h"
#include "roster.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace hc09::editor;

namespace
{
    constexpr int kPortraitSize = 160;
    constexpr COLORREF kPortraitBack = RGB(38, 44, 54);
    constexpr uint32_t kCoachFlag = 0x80000000u;

    struct Row
    {
        enum Kind { Number, Text, Choice, Skill } kind = Number;
        std::wstring group, label;
        Table* table = nullptr;
        uint32_t record = 0;
        std::string field, ceiling;  // ceiling: a field raised along with this one (a rating's potential)
        int64_t low = 0, high = 0, bias = 0;  // Number: shown = stored + bias
        std::vector<std::pair<int64_t, std::wstring>> choices;
        int skill = -1;
    };

    struct App
    {
        HINSTANCE instance = nullptr;
        HWND window = nullptr, team = nullptr, people = nullptr, grid = nullptr, portrait = nullptr, name = nullptr,
             info = nullptr, status = nullptr, editor = nullptr, look = nullptr, lookLabel = nullptr;
        HFONT font = nullptr, bigFont = nullptr;
        HBITMAP picture = nullptr, lookPicture = nullptr;
        std::filesystem::path root;
        Roster roster;
        GameData game;
        bool loaded = false, skillsDirty = false;
        bool coach = false;
        int64_t selected = -1;  // record of the person shown
        std::vector<Row> rows;
        int editing = -1;
        bool ending = false;
    } g;

    std::wstring Wide(const std::string& text)
    {
        std::wstring out;
        for (const unsigned char c : text)
            out += wchar_t(c);  // the game's text is Latin-1
        return out;
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (const wchar_t c : text)
            out += c < 256 ? char(c) : '?';
        return out;
    }

    void Status(const std::wstring& text) { SendMessageW(g.status, SB_SETTEXTW, 0, LPARAM(text.c_str())); }

    void UpdateTitle()
    {
        std::wstring title = L"HC09 Roster Editor";
        if (g.loaded)
            title += L" - " + g.roster.path.wstring() + (g.roster.dirty || g.skillsDirty ? L" *" : L"");
        SetWindowTextW(g.window, title.c_str());
    }

    int CurrentTeam()
    {
        const LRESULT at = SendMessageW(g.team, CB_GETCURSEL, 0, 0);
        return at < 0 ? 0 : int(SendMessageW(g.team, CB_GETITEMDATA, WPARAM(at), 0));
    }

    // ---- special skills (the static database's PSSS: one row per person and skill; its size is fixed) ----

    bool HasSkill(int64_t person, int skill)
    {
        if (g.game.statics.empty())
            return false;
        Table* t = g.game.statics[0]->db.Find("PSSS");
        for (uint32_t i = 0; t && i < t->count; i++)
            if (t->Get(i, "PNid") == person && t->Get(i, "SPSK") == skill)
                return true;
        return false;
    }

    // A skill is added in a row that repeats another (a spare) and removed by making its row repeat another.
    bool SetSkill(int64_t person, int skill, bool on)
    {
        bool done = true;
        for (auto& d : g.game.statics)
        {
            Table* t = d->db.Find("PSSS");
            if (!t)
                continue;
            auto same = [&](uint32_t a, uint32_t b) { return t->Get(a, "PNid") == t->Get(b, "PNid") && t->Get(a, "SPSK") == t->Get(b, "SPSK"); };
            if (on)
            {
                uint32_t spare = UINT32_MAX;
                for (uint32_t j = 1; j < t->count && spare == UINT32_MAX; j++)
                    for (uint32_t i = 0; i < j; i++)
                        if (same(i, j))
                        {
                            spare = j;
                            break;
                        }
                if (spare == UINT32_MAX)
                {
                    done = false;
                    continue;
                }
                t->Set(spare, "PNid", person);
                t->Set(spare, "SPSK", skill);
            }
            else
            {
                uint32_t other = UINT32_MAX;
                for (uint32_t i = 0; i < t->count && other == UINT32_MAX; i++)
                    if (t->Get(i, "PNid") != person || t->Get(i, "SPSK") != skill)
                        other = i;
                for (uint32_t i = 0; i < t->count && other != UINT32_MAX; i++)
                    if (t->Get(i, "PNid") == person && t->Get(i, "SPSK") == skill)
                    {
                        const int64_t p = t->Get(other, "PNid"), s = t->Get(other, "SPSK");
                        t->Set(i, "PNid", p);
                        t->Set(i, "SPSK", s);
                    }
            }
        }
        return done;
    }

    // ---- the rows of the person shown ----

    struct Spec
    {
        const char* field;
        const wchar_t* label;
        const char* ceiling = nullptr;
    };

    void AddNumbers(const wchar_t* group, Table* t, uint32_t record, std::initializer_list<Spec> specs, int64_t high = -1)
    {
        for (const Spec& s : specs)
        {
            if (!t->Has(s.field))
                continue;
            Row row;
            row.group = group;
            row.label = s.label;
            row.table = t;
            row.record = record;
            row.field = s.field;
            row.high = high >= 0 ? std::min<int64_t>(high, t->Max(s.field)) : t->Max(s.field);
            if (s.ceiling && t->Has(s.ceiling))
                row.ceiling = s.ceiling;
            g.rows.push_back(row);
        }
    }

    Row& AddRow(Row::Kind kind, const wchar_t* group, const wchar_t* label, Table* t, uint32_t record, const char* field)
    {
        Row row;
        row.kind = kind;
        row.group = group;
        row.label = label;
        row.table = t;
        row.record = record;
        row.field = field;
        row.high = kind == Row::Number ? t->Max(field) : 0;
        g.rows.push_back(row);
        return g.rows.back();
    }

    std::vector<std::pair<int64_t, std::wstring>> Choices(const std::vector<Named>& list, bool numbered = false)
    {
        std::vector<std::pair<int64_t, std::wstring>> choices;
        for (const Named& n : list)
            choices.push_back({ n.id, numbered ? std::to_wstring(n.id) + L": " + Wide(n.name) : Wide(n.name) });
        return choices;
    }

    void BuildPlayerRows(uint32_t r)
    {
        Table* t = g.roster.play;
        AddRow(Row::Text, L"Player", L"First name", t, r, "PFNA");
        AddRow(Row::Text, L"Player", L"Last name", t, r, "PLNA");
        Row& position = AddRow(Row::Choice, L"Player", L"Position", t, r, "PPOS");
        for (int i = 0; i < 21; i++)
            position.choices.push_back({ i, Wide(kPositions[i]) });
        AddNumbers(L"Player", t, r, { { "PJEN", L"Jersey number" }, { "PAGE", L"Age" }, { "PYRP", L"Years pro" }, { "PHGT", L"Height (inches)" } }, 99);
        AddRow(Row::Number, L"Player", L"Weight (pounds)", t, r, "PWGT").bias = 160;
        AddRow(Row::Choice, L"Player", L"Throws / kicks", t, r, "PHAN").choices = { { 0, L"Right" }, { 1, L"Left" } };
        if (!g.game.colleges.empty())
            AddRow(Row::Choice, L"Player", L"College", t, r, "PCOL").choices = Choices(g.game.colleges);
        else
            AddNumbers(L"Player", t, r, { { "PCOL", L"College (number)" } });
        AddRow(Row::Text, L"Player", L"Home town", t, r, "PHTN");
        AddNumbers(L"Player", t, r, { { "PDRO", L"Draft round" }, { "PDPI", L"Draft pick in the round" } });

        AddNumbers(L"Ratings", t, r, {
            { "POVR", L"Overall" }, { "PSPD", L"Speed", "PSDX" }, { "PACC", L"Acceleration", "PACX" }, { "PAGI", L"Agility", "PAGX" },
            { "PSTR", L"Strength", "PSTX" }, { "PAWR", L"Awareness", "PAWX" }, { "PJMP", L"Jumping", "PJMX" },
            { "PSTA", L"Stamina", "PSAX" }, { "PINJ", L"Injury", "PINX" }, { "PTGH", L"Toughness", "PTGX" },
            { "PTHP", L"Throw power", "PTPX" }, { "PTHA", L"Throw accuracy", "PTAX" },
            { "PCAR", L"Carrying", "PCAX" }, { "PBCV", L"Ball carrier vision", "PBCX" }, { "PELU", L"Elusiveness", "PELX" },
            { "PLTR", L"Trucking", "PLTX" }, { "PLSA", L"Stiff arm", "PLSX" }, { "PLJM", L"Juke move", "PLJX" }, { "PLSM", L"Spin move", "PSMx" },
            { "PCTH", L"Catching", "PCTX" }, { "PLSC", L"Spectacular catch", "PSCX" }, { "PLCI", L"Catch in traffic", "PLCX" },
            { "PLRR", L"Route running", "PRRX" }, { "PLRL", L"Release", "PRLX" },
            { "PPBK", L"Pass block", "PPBX" }, { "PPBS", L"Pass block strength", "PPSX" }, { "PPBF", L"Pass block footwork", "PPFX" },
            { "PRBK", L"Run block", "PRBX" }, { "PRBS", L"Run block strength", "PRSX" }, { "PRBF", L"Run block footwork", "PRFX" },
            { "PLIB", L"Impact blocking", "PIBX" },
            { "PTAK", L"Tackle", "PTKX" }, { "PLHT", L"Hit power", "PLHX" }, { "PLPU", L"Pursuit", "PPUX" }, { "PLPR", L"Play recognition", "PPRX" },
            { "PBSG", L"Block shedding", "PBSX" }, { "PLPm", L"Power moves", "PPMX" }, { "PFMS", L"Finesse moves", "PFMX" },
            { "PLMC", L"Man coverage", "PLMX" }, { "PLZC", L"Zone coverage", "PLZX" }, { "PLPE", L"Press", "PPEX" },
            { "PKPR", L"Kick power", "PKPX" }, { "PKAC", L"Kick accuracy", "PKAX" }, { "PKRT", L"Kick return", "PKRX" },
            { "PLRN", L"Learning", "PLRX" }, { "PIMP", L"Importance", "PIMX" }, { "PMOR", L"Morale", "PMOX" } }, 99);

        AddNumbers(L"Potential (the most each rating can grow to)", t, r, {
            { "PSDX", L"Speed" }, { "PACX", L"Acceleration" }, { "PAGX", L"Agility" }, { "PSTX", L"Strength" }, { "PAWX", L"Awareness" },
            { "PJMX", L"Jumping" }, { "PSAX", L"Stamina" }, { "PINX", L"Injury" }, { "PTGX", L"Toughness" }, { "PTPX", L"Throw power" },
            { "PTAX", L"Throw accuracy" }, { "PCAX", L"Carrying" }, { "PBCX", L"Ball carrier vision" }, { "PELX", L"Elusiveness" },
            { "PLTX", L"Trucking" }, { "PLSX", L"Stiff arm" }, { "PLJX", L"Juke move" }, { "PSMx", L"Spin move" }, { "PCTX", L"Catching" },
            { "PSCX", L"Spectacular catch" }, { "PLCX", L"Catch in traffic" }, { "PRRX", L"Route running" }, { "PRLX", L"Release" },
            { "PPBX", L"Pass block" }, { "PPSX", L"Pass block strength" }, { "PPFX", L"Pass block footwork" }, { "PRBX", L"Run block" },
            { "PRSX", L"Run block strength" }, { "PRFX", L"Run block footwork" }, { "PIBX", L"Impact blocking" }, { "PTKX", L"Tackle" },
            { "PLHX", L"Hit power" }, { "PPUX", L"Pursuit" }, { "PPRX", L"Play recognition" }, { "PBSX", L"Block shedding" },
            { "PPMX", L"Power moves" }, { "PFMX", L"Finesse moves" }, { "PLMX", L"Man coverage" }, { "PLZX", L"Zone coverage" },
            { "PPEX", L"Press" }, { "PKPX", L"Kick power" }, { "PKAX", L"Kick accuracy" }, { "PKRX", L"Kick return" },
            { "PLRX", L"Learning" }, { "PIMX", L"Importance" }, { "PMOX", L"Morale" } }, 99);

        AddRow(Row::Choice, L"3D appearance", L"Skin tone", t, r, "PSKI").choices = { { 0, L"Light" }, { 2, L"Medium" }, { 1, L"Dark" } };
        if (!g.game.heads.empty())
            AddRow(Row::Choice, L"3D appearance", L"Head", t, r, "PGHE").choices = Choices(g.game.heads, true);
        else
            AddNumbers(L"3D appearance", t, r, { { "PGHE", L"Head (number)" } });
        AddNumbers(L"3D appearance", t, r, {
            { "PUHE", L"Own head of a 2008 player (0: none)" }, { "PHCL", L"Hair colour" }, { "PHLM", L"Helmet" }, { "PFMK", L"Face mask" },
            { "PVIS", L"Visor" }, { "PEYE", L"Eye paint" }, { "PMPC", L"Mouthpiece" }, { "PNEK", L"Neck pad" }, { "PBRE", L"Nose strip" },
            { "PJER", L"Jersey sleeves" }, { "PUND", L"Undershirt" }, { "PGSL", L"Sleeve length on grass" }, { "PTSL", L"Sleeve length on turf" },
            { "PSTM", L"Long sleeves below (degrees F)" }, { "PLEL", L"Left elbow on grass" }, { "PREL", L"Right elbow on grass" },
            { "TLEL", L"Left elbow on turf" }, { "TREL", L"Right elbow on turf" }, { "PLWR", L"Left wrist on grass" },
            { "PRWR", L"Right wrist on grass" }, { "TLWR", L"Left wrist on turf" }, { "TRWR", L"Right wrist on turf" },
            { "PLHA", L"Left hand on grass" }, { "PRHA", L"Right hand on grass" }, { "TLHA", L"Left hand on turf" },
            { "TRHA", L"Right hand on turf" }, { "PLKN", L"Left knee" }, { "PRKN", L"Right knee" }, { "PLTH", L"Left thigh" },
            { "PRTH", L"Right thigh" }, { "PLSH", L"Left shoe" }, { "PRSH", L"Right shoe" }, { "PSPL", L"Left shoe tape" },
            { "PSPR", L"Right shoe tape" }, { "PTAL", L"Left arm tattoo" }, { "PTAR", L"Right arm tattoo" } });
        AddNumbers(L"3D build (shape and how much of it)", t, r, {
            { "BSAT", L"Upper arms: shape" }, { "BSAA", L"Upper arms: amount" }, { "BSPT", L"Shoulders: shape" }, { "BSPA", L"Shoulders: amount" },
            { "BSCT", L"Chest: shape" }, { "BSCA", L"Chest: amount" }, { "BSGT", L"Gut: shape" }, { "BSGA", L"Gut: amount" },
            { "BSBT", L"Seat: shape" }, { "BSBA", L"Seat: amount" }, { "BSTT", L"Thighs: shape" }, { "BSTA", L"Thighs: amount" },
            { "BSST", L"Calves: shape" }, { "BSSA", L"Calves: amount" }, { "BSFT", L"Feet: shape" }, { "BSFA", L"Feet: amount" },
            { "BSWT", L"Wrists: shape" }, { "BSWA", L"Wrists: amount" } });
        AddNumbers(L"Contract (in $10,000s)", t, r, {
            { "PCON", L"Length (seasons)" }, { "PCYL", L"Seasons left" }, { "PCSA", L"This season's cap hit" }, { "PTSA", L"Total value" },
            { "PSBO", L"Signing bonus" }, { "PSA0", L"Season 1 salary" }, { "PSB0", L"Season 1 bonus" }, { "PSA1", L"Season 2 salary" },
            { "PSB1", L"Season 2 bonus" }, { "PSA2", L"Season 3 salary" }, { "PSB2", L"Season 3 bonus" }, { "PSA3", L"Season 4 salary" },
            { "PSB3", L"Season 4 bonus" }, { "PSA4", L"Season 5 salary" }, { "PSB4", L"Season 5 bonus" }, { "PSA5", L"Season 6 salary" },
            { "PSB5", L"Season 6 bonus" }, { "PSA6", L"Season 7 salary" }, { "PSB6", L"Season 7 bonus" } });
    }

    void BuildCoachRows(uint32_t r)
    {
        Table* t = g.roster.coch;
        AddRow(Row::Text, L"Coach", L"First name", t, r, "CFNM");
        AddRow(Row::Text, L"Coach", L"Last name", t, r, "CLNM");
        Row& role = AddRow(Row::Choice, L"Coach", L"Job", t, r, "COPS");
        for (int i = 0; i < 11; i++)
            role.choices.push_back({ i, Wide(kCoachRoles[i]) });
        AddNumbers(L"Coach", t, r, { { "CAGE", L"Age" }, { "CCLN", L"Contract: seasons left" }, { "CSAL", L"Contract: salary a season ($10,000s)" },
                                     { "CCYT", L"Seasons with the team" } });

        if (!g.game.offensePlaybooks.empty())
        {
            AddRow(Row::Choice, L"Playbooks", L"Offensive playbook", t, r, "CPID").choices = Choices(g.game.offensePlaybooks);
            AddRow(Row::Choice, L"Playbooks", L"Defensive playbook", t, r, "CDID").choices = Choices(g.game.defensePlaybooks);
        }
        else
            AddNumbers(L"Playbooks", t, r, { { "CPID", L"Offensive playbook (number)" }, { "CDID", L"Defensive playbook (number)" } });
        AddRow(Row::Choice, L"Playbooks", L"Base defense", t, r, "CDTY").choices = { { 95, L"4-3" }, { 5, L"3-4" } };
        AddNumbers(L"Playbooks", t, r, { { "COTR", L"Offense: run (0) or pass (100)" }, { "COTA", L"Offense: conservative (0) or aggressive (100)" },
                                         { "CDTR", L"Defense: run (0) or pass (100)" }, { "CDTA", L"Defense: conservative (0) or aggressive (100)" },
                                         { "CRBT", L"Running backs: one back (0) or by committee (100)" } }, 100);

        AddNumbers(L"Skills (levels)", t, r, { { "SKPF", L"Performance", "SKPA" }, { "SKPC", L"Play calling", "SKPX" }, { "SKST", L"Strategy", "SKSM" },
                                               { "SKCR", L"Creativity", "SKCM" } }, 5);
        AddNumbers(L"Skills (levels)", t, r, { { "SKPA", L"Performance: highest it can reach" }, { "SKPX", L"Play calling: highest it can reach" },
                                               { "SKSM", L"Strategy: highest it can reach" }, { "SKCM", L"Creativity: highest it can reach" } }, 5);
        AddNumbers(L"Skills (levels)", t, r, { { "SKPT", L"Skill points to spend" } });
        if (Table* k = g.roster.cskl)
        {
            static const wchar_t* const kGroups[] = { L"Quarterbacks", L"Running backs", L"Wide receivers", L"Tight ends", L"Offensive line",
                                                      L"Defensive line", L"Linebackers", L"Defensive backs", L"Kickers", L"Punters" };
            const int64_t person = t->Get(r, "PNid");
            for (int group = 0; group < 10; group++)
                for (uint32_t i = 0; i < k->count; i++)
                    if (k->Get(i, "PNid") == person && k->Get(i, "PPGR") == group)
                    {
                        const std::wstring who = kGroups[group];
                        for (const auto& [field, ceiling, what] : { std::tuple{ "SKPD", "SKPM", L": physical" }, std::tuple{ "SKLD", "SKLM", L": learning" },
                                                                    std::tuple{ "SKID", "SKIM", L": intangibles" } })
                        {
                            Row& row = AddRow(Row::Number, L"Skills at developing players (levels)", (who + what).c_str(), k, i, field);
                            row.high = 5;
                            row.ceiling = ceiling;
                        }
                        break;
                    }
        }
        if (g.roster.hc27 && !g.game.statics.empty() && g.game.CanPatchDisc())
        {
            const int64_t person = t->Get(r, "PNid"), job = t->Get(r, "COPS");
            for (const SpecialSkill& skill : g.game.specialSkills)
            {
                if (!(skill.roles >> job & 1) && !HasSkill(person, skill.id))
                    continue;
                Row row;
                row.kind = Row::Skill;
                row.group = L"Special skills (in careers started from now on)";
                row.label = skill.name.empty() ? L"Special skill " + std::to_wstring(skill.id) : Wide(skill.name);
                row.table = t;
                row.record = r;
                row.skill = skill.id;
                g.rows.push_back(row);
            }
        }

        AddNumbers(L"Ratings", t, r, { { "POVS", L"Overall" }, { "COFF", L"Offense" }, { "CDEF", L"Defense" }, { "CKNW", L"Knowledge" },
                                       { "CMOT", L"Motivation" }, { "CCHM", L"Chemistry" }, { "CETH", L"Work ethic" }, { "HDAp", L"Approval" },
                                       { "CRQB", L"Coaching quarterbacks" }, { "CRRB", L"Coaching running backs" }, { "CRWR", L"Coaching receivers" },
                                       { "CROL", L"Coaching the offensive line" }, { "CRDL", L"Coaching the defensive line" },
                                       { "CRLB", L"Coaching linebackers" }, { "CRDB", L"Coaching defensive backs" }, { "CRSA", L"Coaching safeties" },
                                       { "CRKS", L"Coaching kickers" }, { "CRPS", L"Coaching punters" } }, 99);

        AddRow(Row::Choice, L"3D appearance", L"Skin tone", t, r, "CSKI").choices = { { 0, L"Light" }, { 1, L"Dark" } };
        AddNumbers(L"3D appearance", t, r, { { "CFSH", L"Head (0-36: a 2008 head coach's own)" }, { "CBSZ", L"Build" }, { "PHRd", L"Hair" },
                                             { "CHGT", L"Height (inches)" }, { "PNCw", L"Weight" } });
        AddRow(Row::Choice, L"3D appearance", L"Cap", t, r, "CHTY").choices = { { 0, L"No" }, { 1, L"Yes" } };
        AddRow(Row::Choice, L"3D appearance", L"Team logo on the cap", t, r, "CFHL").choices = { { 0, L"No" }, { 1, L"Yes" } };
        AddRow(Row::Choice, L"3D appearance", L"Glasses", t, r, "CTgw").choices = { { 0, L"No" }, { 1, L"Yes" } };
        AddRow(Row::Choice, L"3D appearance", L"Own body model (2008 head coaches)", t, r, "CThg").choices = { { 0, L"No" }, { 1, L"Yes" } };

        AddNumbers(L"Career record", t, r, { { "CCWI", L"Wins" }, { "CCLO", L"Losses" }, { "CCTI", L"Ties" }, { "CCPW", L"Playoff wins" },
                                             { "CCPL", L"Playoff losses" }, { "CCPM", L"Playoff seasons" }, { "CSBW", L"Super Bowl wins" },
                                             { "CSBL", L"Super Bowl losses" }, { "CCWS", L"Winning seasons" } });
    }

    std::wstring RowText(const Row& row)
    {
        switch (row.kind)
        {
        case Row::Text:
            return Wide(row.table->Text(row.record, row.field));
        case Row::Skill:
            return HasSkill(row.table->Get(row.record, "PNid"), row.skill) ? L"Yes" : L"No";
        case Row::Choice:
        {
            const int64_t value = row.table->Get(row.record, row.field);
            for (const auto& [id, label] : row.choices)
                if (id == value)
                    return label;
            return std::to_wstring(value);
        }
        default:
            return std::to_wstring(row.table->Get(row.record, row.field) + row.bias);
        }
    }

    // ---- the lists ----

    void FillGrid()
    {
        ListView_DeleteAllItems(g.grid);
        ListView_RemoveAllGroups(g.grid);
        std::vector<std::wstring> groups;
        for (int i = 0; i < int(g.rows.size()); i++)
        {
            const Row& row = g.rows[i];
            auto found = std::find(groups.begin(), groups.end(), row.group);
            if (found == groups.end())
            {
                LVGROUP group{ sizeof(group) };
                group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
                group.pszHeader = const_cast<wchar_t*>(row.group.c_str());
                group.iGroupId = int(groups.size());
                group.state = LVGS_COLLAPSIBLE;
                group.stateMask = LVGS_COLLAPSIBLE;
                ListView_InsertGroup(g.grid, -1, &group);
                groups.push_back(row.group);
                found = groups.end() - 1;
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_GROUPID;
            item.iItem = i;
            item.iGroupId = int(found - groups.begin());
            item.pszText = const_cast<wchar_t*>(row.label.c_str());
            ListView_InsertItem(g.grid, &item);
            const std::wstring text = RowText(row);
            ListView_SetItemText(g.grid, i, 1, const_cast<wchar_t*>(text.c_str()));
        }
    }

    std::filesystem::path PicturePath()
    {
        if (g.selected < 0)
            return {};
        return g.roster.PortraitFolder(g.coach, g.root) / (std::to_wstring(g.roster.Portrait(g.coach, uint32_t(g.selected))) + L".png");
    }

    // The name, what he is and his picture, above the grid.
    void ShowHeader(bool rows)
    {
        if (g.picture)
        {
            SendMessageW(g.portrait, STM_SETIMAGE, IMAGE_BITMAP, 0);
            DeleteObject(g.picture);
            g.picture = nullptr;
        }
        if (g.lookPicture)
        {
            SendMessageW(g.look, STM_SETIMAGE, IMAGE_BITMAP, 0);
            DeleteObject(g.lookPicture);
            g.lookPicture = nullptr;
        }
        std::wstring name, info, look;
        if (g.selected >= 0)
        {
            const uint32_t r = uint32_t(g.selected);
            if (g.coach)
            {
                if (rows)
                    BuildCoachRows(r);
                name = Wide(g.roster.CoachName(r));
                const int64_t job = g.roster.coch->Get(r, "COPS");
                info = Wide(job < 11 ? kCoachRoles[job] : "Coach");
            }
            else
            {
                if (rows)
                    BuildPlayerRows(r);
                name = Wide(g.roster.PlayerName(r));
                const int64_t position = g.roster.play->Get(r, "PPOS");
                info = Wide(position < 21 ? kPositions[position] : "?") + L"  #" + std::to_wstring(g.roster.play->Get(r, "PJEN")) + L"  " +
                       std::to_wstring(g.roster.play->Get(r, "POVR")) + L" overall";
            }
            const std::filesystem::path picture = PicturePath();
            const int size = MulDiv(kPortraitSize, int(GetDpiForWindow(g.window)), 96);
            g.picture = LoadPreview(picture, size, kPortraitBack);
            info += g.picture ? L"\r\nPicture: " + picture.filename().wstring() : L"\r\nThe game's own picture (portrait " +
                    std::to_wstring(g.roster.Portrait(g.coach, r)) + L")";
            Pixels pixels;
            if (!g.picture && g.game.images.Portrait(g.coach, g.roster.Portrait(g.coach, r), pixels))
                g.picture = ToBitmap(pixels, size, kPortraitBack);
            if (g.picture)
                SendMessageW(g.portrait, STM_SETIMAGE, IMAGE_BITMAP, LPARAM(g.picture));
            // His 3D head: a player's face texture (the middle of it; the rest wraps around the head), a
            // coach's create-a-coach appearance with that head, as the game pictures it.
            Pixels head;
            if (g.coach)
            {
                const int64_t face = g.roster.coch->Get(r, "CFSH");
                for (const CoachPreset& preset : g.game.coachPresets)
                    if (preset.face == face && g.game.images.Portrait(true, preset.portrait, head))
                    {
                        g.lookPicture = ToBitmap(head, size, kPortraitBack);
                        break;
                    }
                look = g.lookPicture ? L"3D head (as the game pictures it)" : face < 37 ? L"3D head: a 2008 head coach's own" : L"";
            }
            else
            {
                const int64_t id = g.roster.play->Get(r, "PGHE");
                for (const Named& named : g.game.heads)
                    if (named.id == id && g.game.images.Head(named.name, head))
                    {
                        g.lookPicture = ToBitmap(head, size, kPortraitBack, 0.12f, 0.03f, 0.88f, 0.79f);
                        break;
                    }
                look = g.lookPicture ? L"3D head: its face texture" : L"";
                if (g.roster.play->Get(r, "PUHE") != 0)
                    look = L"3D head: a 2008 player's own (the head below is not used)";
            }
            if (g.lookPicture)
                SendMessageW(g.look, STM_SETIMAGE, IMAGE_BITMAP, LPARAM(g.lookPicture));
        }
        SetWindowTextW(g.lookLabel, look.c_str());
        InvalidateRect(g.look, nullptr, TRUE);
        SetWindowTextW(g.name, name.c_str());
        SetWindowTextW(g.info, info.c_str());
        InvalidateRect(g.portrait, nullptr, TRUE);
    }

    void ShowPerson()
    {
        g.rows.clear();
        ShowHeader(true);
        FillGrid();
    }

    void FillPeople(int64_t keep = -1, bool keepCoach = false)
    {
        SendMessageW(g.people, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g.people);
        const int teamId = CurrentTeam();
        int at = 0, select = -1;
        auto add = [&](bool coach, uint32_t record, const std::wstring& position, const std::wstring& number, const std::wstring& name, int64_t overall) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
            item.iItem = at;
            item.iGroupId = coach ? 0 : 1;
            item.lParam = LPARAM(record | (coach ? kCoachFlag : 0));
            item.pszText = const_cast<wchar_t*>(position.c_str());
            ListView_InsertItem(g.people, &item);
            ListView_SetItemText(g.people, at, 1, const_cast<wchar_t*>(number.c_str()));
            ListView_SetItemText(g.people, at, 2, const_cast<wchar_t*>(name.c_str()));
            const std::wstring rating = std::to_wstring(overall);
            ListView_SetItemText(g.people, at, 3, const_cast<wchar_t*>(rating.c_str()));
            if (keep == int64_t(record) && keepCoach == coach)
                select = at;
            at++;
        };
        static const wchar_t* const kShort[] = { L"HC", L"OC", L"DC", L"ST", L"QB", L"RB", L"WR", L"OL", L"DL", L"LB", L"DB" };
        for (const uint32_t r : g.roster.Coaches(teamId))
        {
            const int64_t job = g.roster.coch->Get(r, "COPS");
            add(true, r, job < 11 ? kShort[job] : L"?", L"", Wide(g.roster.CoachName(r)), g.roster.coch->Get(r, "POVS"));
        }
        const std::vector<uint32_t> players = g.roster.Players(teamId);
        for (const uint32_t r : players)
        {
            const int64_t position = g.roster.play->Get(r, "PPOS");
            add(false, r, Wide(position < 21 ? kPositions[position] : "?"), std::to_wstring(g.roster.play->Get(r, "PJEN")),
                Wide(g.roster.PlayerName(r)), g.roster.play->Get(r, "POVR"));
        }
        SendMessageW(g.people, WM_SETREDRAW, TRUE, 0);
        if (select >= 0)
        {
            ListView_SetItemState(g.people, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g.people, select, FALSE);
        }
        else
        {
            g.selected = -1;
            ShowPerson();
        }
        Status(Wide(g.roster.TeamName(teamId)) + L": " + std::to_wstring(players.size()) + L" players" +
               (teamId <= 32 && players.size() > 53 ? L" (more than the 53 a team keeps in the season)" : L""));
    }

    void FillTeams()
    {
        SendMessageW(g.team, CB_RESETCONTENT, 0, 0);
        for (const TeamEntry& t : g.roster.teams)
        {
            const LRESULT at = SendMessageW(g.team, CB_ADDSTRING, 0, LPARAM(Wide(t.name).c_str()));
            SendMessageW(g.team, CB_SETITEMDATA, WPARAM(at), t.id);
        }
        SendMessageW(g.team, CB_SETCURSEL, 0, 0);
        FillPeople();
    }

    // ---- editing a value in place ----

    void EndEdit(bool commit);

    LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE))
        {
            EndEdit(wParam == VK_RETURN);
            return 0;
        }
        if (message == WM_GETDLGCODE)
            return DLGC_WANTALLKEYS;
        if (message == WM_KILLFOCUS && !g.ending)
            EndEdit(true);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void AfterChange(const Row& row)
    {
        g.roster.dirty = true;
        if (!g.coach && row.table == g.roster.play && (row.field == "PPOS" || row.field == "POVR"))
        {
            // The depth chart orders a position's players by overall rating.
            g.roster.RebuildDepthChart(int(row.table->Get(row.record, "TGID")));
            g.roster.RateTeams();
        }
        if (row.field == "PGHE" || row.field == "PUHE" || row.field == "CFSH")
            ShowHeader(false);  // the 3D head pictured
        static const char* const kListed[] = { "PPOS", "POVR", "PJEN", "PFNA", "PLNA", "CFNM", "CLNM", "COPS", "POVS" };
        if (std::find(std::begin(kListed), std::end(kListed), row.field) != std::end(kListed))
        {
            FillPeople(g.selected, g.coach);
            ShowHeader(false);
        }
        UpdateTitle();
    }

    void EndEdit(bool commit)
    {
        if (!g.editor || g.ending)
            return;
        g.ending = true;
        const int item = g.editing;
        Row& row = g.rows[size_t(item)];
        bool changed = false;
        if (commit && row.kind == Row::Choice)
        {
            const LRESULT at = SendMessageW(g.editor, CB_GETCURSEL, 0, 0);
            if (at >= 0)
            {
                const int64_t value = int64_t(SendMessageW(g.editor, CB_GETITEMDATA, WPARAM(at), 0));
                changed = value != row.table->Get(row.record, row.field);
                row.table->Set(row.record, row.field, value);
            }
        }
        else if (commit)
        {
            wchar_t text[256] = L"";
            GetWindowTextW(g.editor, text, int(std::size(text)));
            if (row.kind == Row::Text)
            {
                changed = Narrow(text) != row.table->Text(row.record, row.field);
                row.table->SetText(row.record, row.field, Narrow(text));
            }
            else if (text[0])
            {
                const int64_t value = std::clamp<int64_t>(_wtoi64(text) - row.bias, row.low, row.high);
                changed = value != row.table->Get(row.record, row.field);
                row.table->Set(row.record, row.field, value);
                if (!row.ceiling.empty() && row.table->Get(row.record, row.ceiling) < value)
                    row.table->Set(row.record, row.ceiling, value);  // a rating cannot be above its potential
            }
        }
        DestroyWindow(g.editor);
        g.editor = nullptr;
        g.editing = -1;
        g.ending = false;
        if (changed)
        {
            for (int i = 0; i < int(g.rows.size()); i++)  // the potential may have moved too
            {
                const std::wstring text = RowText(g.rows[size_t(i)]);
                ListView_SetItemText(g.grid, i, 1, const_cast<wchar_t*>(text.c_str()));
            }
            const Row copy = row;  // AfterChange may rebuild the rows
            AfterChange(copy);
        }
        SetFocus(g.grid);
    }

    void BeginEdit(int item)
    {
        if (item < 0 || item >= int(g.rows.size()))
            return;
        EndEdit(true);
        Row& row = g.rows[size_t(item)];
        if (row.kind == Row::Skill)
        {
            const int64_t person = row.table->Get(row.record, "PNid");
            const bool on = !HasSkill(person, row.skill);
            if (!SetSkill(person, row.skill, on))
            {
                MessageBoxW(g.window, L"The game holds a fixed number of special skills over all coaches, and every one is in use. "
                                      L"Take a special skill from a coach first (this one or another).", L"Special skills", MB_ICONINFORMATION);
                return;
            }
            g.skillsDirty = true;
            const std::wstring text = RowText(row);
            ListView_SetItemText(g.grid, item, 1, const_cast<wchar_t*>(text.c_str()));
            UpdateTitle();
            return;
        }
        RECT rect{};
        ListView_GetSubItemRect(g.grid, item, 1, LVIR_LABEL, &rect);
        g.editing = item;
        if (row.kind == Row::Choice)
        {
            g.editor = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, rect.left, rect.top - 2,
                rect.right - rect.left, 400, g.grid, nullptr, g.instance, nullptr);
            SendMessageW(g.editor, WM_SETFONT, WPARAM(g.font), TRUE);
            const int64_t value = row.table->Get(row.record, row.field);
            for (const auto& [id, label] : row.choices)
            {
                const LRESULT at = SendMessageW(g.editor, CB_ADDSTRING, 0, LPARAM(label.c_str()));
                SendMessageW(g.editor, CB_SETITEMDATA, WPARAM(at), LPARAM(id));
                if (id == value)
                    SendMessageW(g.editor, CB_SETCURSEL, WPARAM(at), 0);
            }
            SetFocus(g.editor);
            SendMessageW(g.editor, CB_SHOWDROPDOWN, TRUE, 0);
        }
        else
        {
            g.editor = CreateWindowExW(0, L"EDIT", RowText(row).c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL |
                (row.kind == Row::Number ? ES_NUMBER : 0), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, g.grid, nullptr,
                g.instance, nullptr);
            SendMessageW(g.editor, WM_SETFONT, WPARAM(g.font), TRUE);
            if (row.kind == Row::Text)
                if (const Field* f = row.table->Find(row.field))
                    SendMessageW(g.editor, EM_SETLIMITTEXT, f->bits / 8 - 1, 0);
            SetWindowSubclass(g.editor, EditorProc, 1, 0);
            SendMessageW(g.editor, EM_SETSEL, 0, -1);
            SetFocus(g.editor);
        }
    }

    // The grid passes its in-place combo box's notifications on.
    LRESULT CALLBACK GridProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_COMMAND && HWND(lParam) == g.editor && g.editor)
        {
            if (HIWORD(wParam) == CBN_SELENDOK)
                PostMessageW(g.window, WM_APP, 1, 0);
            else if (HIWORD(wParam) == CBN_SELENDCANCEL || HIWORD(wParam) == CBN_KILLFOCUS)
                PostMessageW(g.window, WM_APP, 0, 0);
            return 0;
        }
        if ((message == WM_VSCROLL || message == WM_MOUSEWHEEL) && g.editor)
            EndEdit(true);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    // ---- files ----

    bool SaveAll()
    {
        std::wstring error;
        if (g.roster.dirty && !g.roster.Save(error))
        {
            MessageBoxW(g.window, error.c_str(), L"Save", MB_ICONERROR);
            return false;
        }
        if (g.skillsDirty)
        {
            if (!g.game.WritePatches(g.game.statics, "PSSS", error))
            {
                MessageBoxW(g.window, error.c_str(), L"Save", MB_ICONERROR);
                return false;
            }
            g.skillsDirty = false;
        }
        UpdateTitle();
        Status(L"Saved " + g.roster.path.wstring() + L" (the file as it was before the first save is kept as " +
               g.roster.path.filename().wstring() + L".bak)");
        return true;
    }

    bool ConfirmDiscard()
    {
        if (!g.loaded || (!g.roster.dirty && !g.skillsDirty))
            return true;
        const int answer = MessageBoxW(g.window, L"Save the changes to this roster?", L"HC09 Roster Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
        return answer == IDNO || (answer == IDYES && SaveAll());
    }

    void OpenRoster(const std::filesystem::path& path)
    {
        EndEdit(false);
        Roster roster;
        std::wstring error;
        if (!roster.Load(path, error))
        {
            MessageBoxW(g.window, error.c_str(), L"Open", MB_ICONERROR);
            return;
        }
        g.roster = std::move(roster);
        // The tables point into the save's data, which moved with it.
        g.roster.save.db.Open(&g.roster.save.data, g.roster.save.db.Offset());
        g.roster.play = g.roster.save.db.Find("PLAY");
        g.roster.coch = g.roster.save.db.Find("COCH");
        g.roster.team = g.roster.save.db.Find("TEAM");
        g.roster.dcht = g.roster.save.db.Find("DCHT");
        g.roster.cskl = g.roster.save.db.Find("CSKL");
        g.loaded = true;
        g.selected = -1;
        FillTeams();
        UpdateTitle();
        if (!g.roster.save.ChecksumsValid())
            Status(L"This roster's checksums were wrong (the game would ignore it); saving it here repairs them.");
    }

    void OpenDialog()
    {
        if (!ConfirmDiscard())
            return;
        wchar_t file[1024] = L"";
        std::filesystem::path start = g.root / L"build" / L"userdata-hc27" / L"content" / L"00000001";
        if (!std::filesystem::exists(start))
            start = g.root / L"build" / L"userdata" / L"content" / L"00000001";
        const std::wstring folder = start.wstring();
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = g.window;
        dialog.lpstrFilter = L"Roster saves (ROSTER-*)\0ROSTER*\0All files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = DWORD(std::size(file));
        dialog.lpstrInitialDir = folder.c_str();
        dialog.lpstrTitle = L"Open a roster (each is in its own ROSTER-<name> folder)";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&dialog))
            OpenRoster(file);
    }

    // ---- the person menu ----

    struct ChooseData
    {
        std::wstring title, text;
        std::vector<std::pair<int, std::wstring>> options;
        int chosen = -1;
        std::function<HBITMAP(int option, int size)> preview;  // a picture of an option, for IDD_PRESET
        HBITMAP shown = nullptr;
    };

    void ChoosePreview(HWND dialog, ChooseData* data)
    {
        if (!data->preview)
            return;
        const LRESULT at = SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_GETCURSEL, 0, 0);
        const HWND picture = GetDlgItem(dialog, IDC_MOVE_PREVIEW);
        RECT rect{};
        GetClientRect(picture, &rect);
        const HBITMAP bitmap = at < 0 ? nullptr : data->preview(int(SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_GETITEMDATA, WPARAM(at), 0)),
            int(std::min(rect.right, rect.bottom)));
        SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, LPARAM(bitmap));
        if (data->shown)
            DeleteObject(data->shown);
        data->shown = bitmap;
    }

    INT_PTR CALLBACK ChooseProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            const ChooseData* data = reinterpret_cast<ChooseData*>(lParam);
            SetWindowTextW(dialog, data->title.c_str());
            SetDlgItemTextW(dialog, IDC_MOVE_TEXT, data->text.c_str());
            for (const auto& [id, label] : data->options)
            {
                const LRESULT at = SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_ADDSTRING, 0, LPARAM(label.c_str()));
                SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_SETITEMDATA, WPARAM(at), id);
            }
            SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_SETCURSEL, 0, 0);
            ChoosePreview(dialog, reinterpret_cast<ChooseData*>(lParam));
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDC_MOVE_TEAM && HIWORD(wParam) == CBN_SELCHANGE)
        {
            ChoosePreview(dialog, reinterpret_cast<ChooseData*>(GetWindowLongPtrW(dialog, DWLP_USER)));
            return TRUE;
        }
        if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
        {
            ChooseData* data = reinterpret_cast<ChooseData*>(GetWindowLongPtrW(dialog, DWLP_USER));
            if (data->shown)
            {
                SendDlgItemMessageW(dialog, IDC_MOVE_PREVIEW, STM_SETIMAGE, IMAGE_BITMAP, 0);
                DeleteObject(data->shown);
                data->shown = nullptr;
            }
            const LRESULT at = SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_GETCURSEL, 0, 0);
            if (LOWORD(wParam) == IDOK && at >= 0)
                data->chosen = int(SendDlgItemMessageW(dialog, IDC_MOVE_TEAM, CB_GETITEMDATA, WPARAM(at), 0));
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;
    }

    bool NeedPlayer()
    {
        if (g.selected >= 0 && !g.coach)
            return true;
        MessageBoxW(g.window, L"Select a player first. Coaches stay with their teams.", L"HC09 Roster Editor", MB_ICONINFORMATION);
        return false;
    }

    void MovePlayer(int teamId)
    {
        const uint32_t r = uint32_t(g.selected);
        const std::wstring who = Wide(g.roster.PlayerName(r));
        g.roster.MovePlayer(r, teamId);
        FillPeople();
        UpdateTitle();
        Status(who + L" is now with the " + Wide(g.roster.TeamName(teamId)) + L". Depth charts and team ratings were rebuilt.");
    }

    void MoveDialog()
    {
        if (!NeedPlayer())
            return;
        EndEdit(true);
        ChooseData data;
        data.title = L"Move to a team";
        data.text = L"Move " + Wide(g.roster.PlayerName(uint32_t(g.selected))) + L" to:";
        const int current = int(g.roster.play->Get(uint32_t(g.selected), "TGID"));
        for (const TeamEntry& t : g.roster.teams)
            if (t.id != current)
                data.options.push_back({ t.id, Wide(t.name) });
        if (DialogBoxParamW(g.instance, MAKEINTRESOURCEW(IDD_MOVE), g.window, ChooseProc, LPARAM(&data)) == IDOK && data.chosen >= 0)
            MovePlayer(data.chosen);
    }

    void PresetDialog()
    {
        if (g.selected < 0 || !g.coach || g.game.coachPresets.empty())
        {
            MessageBoxW(g.window, g.game.coachPresets.empty() ? L"The create-a-coach appearances are read from the game's disc image, which was not found."
                                                              : L"Select a coach first.", L"HC09 Roster Editor", MB_ICONINFORMATION);
            return;
        }
        EndEdit(true);
        ChooseData data;
        data.title = L"Coach appearance";
        data.text = L"Give him the head, skin and build of create-a-coach appearance:";
        for (int i = 0; i < int(g.game.coachPresets.size()); i++)
            data.options.push_back({ i, L"Appearance " + std::to_wstring(i + 1) + (g.game.coachPresets[size_t(i)].skin ? L" (dark)" : L" (light)") });
        data.preview = [](int option, int size) -> HBITMAP {
            Pixels pixels;
            return g.game.images.Portrait(true, g.game.coachPresets[size_t(option)].portrait, pixels) ? ToBitmap(pixels, size, kPortraitBack) : nullptr;
        };
        if (DialogBoxParamW(g.instance, MAKEINTRESOURCEW(IDD_PRESET), g.window, ChooseProc, LPARAM(&data)) != IDOK || data.chosen < 0)
            return;
        const CoachPreset& preset = g.game.coachPresets[size_t(data.chosen)];
        const uint32_t r = uint32_t(g.selected);
        g.roster.coch->Set(r, "CFSH", preset.face);
        g.roster.coch->Set(r, "CSKI", preset.skin);
        g.roster.coch->Set(r, "CBSZ", preset.body);
        g.roster.coch->Set(r, "CThg", 0);
        g.roster.dirty = true;
        ShowPerson();
        UpdateTitle();
    }

    void ChangePicture()
    {
        if (g.selected < 0)
            return;
        EndEdit(true);
        wchar_t file[1024] = L"";
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = g.window;
        dialog.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = DWORD(std::size(file));
        dialog.lpstrTitle = L"Choose a picture (a PNG with a transparent background looks like the game's own)";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&dialog))
            return;
        const uint32_t r = uint32_t(g.selected);
        const int id = g.roster.OwnPortrait(g.coach, r, g.coach ? g.game.coachPortraits : g.game.playerPortraits);
        if (id < 0)
        {
            MessageBoxW(g.window, L"No portrait of the game's is left to replace: every one belongs to somebody.", L"Picture", MB_ICONERROR);
            return;
        }
        const std::filesystem::path target = g.roster.PortraitFolder(g.coach, g.root) / (std::to_wstring(id) + L".png");
        if (!ImportPicture(file, target, g.coach ? 256 : 351))
        {
            MessageBoxW(g.window, L"That picture could not be read.", L"Picture", MB_ICONERROR);
            return;
        }
        ShowPerson();
        UpdateTitle();
        Status(L"Picture saved as " + target.wstring() + (g.roster.dirty ? L". Save the roster too: his portrait number changed." : L""));
    }

    void RemovePicture()
    {
        const std::filesystem::path picture = PicturePath();
        if (picture.empty() || !std::filesystem::exists(picture))
            return;
        if (MessageBoxW(g.window, (L"Delete " + picture.wstring() + L"?\n\nThe game then shows its own portrait.").c_str(), L"Picture",
                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        std::error_code ec;
        std::filesystem::remove(picture, ec);
        ShowPerson();
    }

    // ---- draft picks (HC27: the career template's DRPK, and hc27\draft_picks.txt for the draft after) ----

    struct Pick
    {
        int draft = 0, round = 0, slot = 0, original = 0, owner = 0;
        uint32_t record = 0;
    };

    struct PicksData
    {
        std::vector<Pick> picks;
        std::vector<int> shown;  // indices into picks, as listed
    };

    constexpr int kFirstDraft = 2026;  // HC27's season: the template's two drafts are this year's and the next

    void LoadPicks(PicksData& data)
    {
        Table* t = g.game.templates[0]->db.Find("DRPK");
        for (uint32_t i = 0; i < t->count; i++)
        {
            const int number = int(t->Get(i, "DPNM"));
            data.picks.push_back({ int(t->Get(i, "DPYO")), number / 32 + 1, number % 32, int(t->Get(i, "DPOD")), int(t->Get(i, "DPID")), i });
        }
        // The draft after those: every team's own picks, then the trades in hc27\draft_picks.txt
        // (draft round from to [original]; src/common/draft_picks.cpp).
        const size_t first = data.picks.size();
        for (int round = 1; round <= 7; round++)
            for (int team = 1; team <= 32; team++)
                data.picks.push_back({ 2, round, team - 1, team, team, 0 });
        std::ifstream file(g.root / L"hc27" / L"draft_picks.txt");
        for (std::string line; std::getline(file, line);)
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream fields(line);
            int draft = 0, round = 0, from = 0, to = 0, original = 0;
            if (!(fields >> draft >> round >> from >> to) || draft != kFirstDraft + 2)
                continue;
            fields >> original;
            Pick* found = nullptr;
            for (size_t i = first; i < data.picks.size() && !found; i++)
                if (data.picks[i].round == round && data.picks[i].owner == from && data.picks[i].original == (original ? original : from))
                    found = &data.picks[i];
            for (size_t i = first; i < data.picks.size() && !found; i++)
                if (data.picks[i].round == round && data.picks[i].owner == from)
                    found = &data.picks[i];
            if (found)
                found->owner = to;
        }
    }

    bool SavePicks(const PicksData& data)
    {
        for (auto& d : g.game.templates)
        {
            Table* t = d->db.Find("DRPK");
            for (const Pick& pick : data.picks)
                if (pick.draft < 2 && pick.record < t->count)
                    t->Set(pick.record, "DPID", pick.owner);
        }
        std::wstring error;
        if (!g.game.WritePatches(g.game.templates, "DRPK", error))
        {
            MessageBoxW(g.window, error.c_str(), L"Draft picks", MB_ICONERROR);
            return false;
        }
        std::ofstream file(g.root / L"hc27" / L"draft_picks.txt", std::ios::trunc | std::ios::binary);
        file << "# Draft pick trades HC27 applies to a career's " << kFirstDraft + 2 << " picks (written by HC09RosterEditor).\n"
             << "# draft round from to [original], team ids as in the roster's TEAM table\n";
        for (const Pick& pick : data.picks)
            if (pick.draft == 2 && pick.owner != pick.original)
                file << kFirstDraft + 2 << ' ' << pick.round << ' ' << pick.original << ' ' << pick.owner << '\n';
        return bool(file);
    }

    void ListPicks(HWND dialog, PicksData& data)
    {
        const HWND list = GetDlgItem(dialog, IDC_PICKS_LIST);
        const int draft = int(SendDlgItemMessageW(dialog, IDC_PICKS_DRAFT, CB_GETCURSEL, 0, 0));
        ListView_DeleteAllItems(list);
        data.shown.clear();
        for (int i = 0; i < int(data.picks.size()); i++)
            if (data.picks[size_t(i)].draft == draft)
                data.shown.push_back(i);
        std::sort(data.shown.begin(), data.shown.end(), [&](int a, int b) {
            const Pick& x = data.picks[size_t(a)];
            const Pick& y = data.picks[size_t(b)];
            return x.round != y.round ? x.round < y.round : x.slot < y.slot;
        });
        for (int at = 0; at < int(data.shown.size()); at++)
        {
            const Pick& pick = data.picks[size_t(data.shown[size_t(at)])];
            const std::wstring round = std::to_wstring(pick.round), slot = draft == 2 ? L"" : std::to_wstring(pick.slot + 1),
                               original = Wide(g.roster.TeamName(pick.original)),
                               owner = pick.owner == pick.original ? L"" : Wide(g.roster.TeamName(pick.owner));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = at;
            item.pszText = const_cast<wchar_t*>(round.c_str());
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, at, 1, const_cast<wchar_t*>(slot.c_str()));
            ListView_SetItemText(list, at, 2, const_cast<wchar_t*>(original.c_str()));
            ListView_SetItemText(list, at, 3, const_cast<wchar_t*>(owner.c_str()));
        }
    }

    INT_PTR CALLBACK PicksProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        PicksData* data = reinterpret_cast<PicksData*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            data = reinterpret_cast<PicksData*>(lParam);
            const HWND list = GetDlgItem(dialog, IDC_PICKS_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const std::pair<const wchar_t*, int> columns[] = { { L"Round", 50 }, { L"Pick", 45 }, { L"Team's own pick", 180 }, { L"Traded to", 180 } };
            for (int i = 0; i < 4; i++)
            {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(columns[i].first);
                column.cx = MulDiv(columns[i].second, GetDpiForWindow(dialog), 96);
                ListView_InsertColumn(list, i, &column);
            }
            for (int year = 0; year < 3; year++)
            {
                const std::wstring label = std::to_wstring(kFirstDraft + year) + (year == 0 ? L" draft (careers started in the off-season)" : L" draft");
                SendDlgItemMessageW(dialog, IDC_PICKS_DRAFT, CB_ADDSTRING, 0, LPARAM(label.c_str()));
            }
            SendDlgItemMessageW(dialog, IDC_PICKS_DRAFT, CB_SETCURSEL, 1, 0);
            for (const TeamEntry& t : g.roster.teams)
                if (t.id <= 32)
                {
                    const LRESULT at = SendDlgItemMessageW(dialog, IDC_PICKS_OWNER, CB_ADDSTRING, 0, LPARAM(Wide(t.name).c_str()));
                    SendDlgItemMessageW(dialog, IDC_PICKS_OWNER, CB_SETITEMDATA, WPARAM(at), t.id);
                }
            SendDlgItemMessageW(dialog, IDC_PICKS_OWNER, CB_SETCURSEL, 0, 0);
            SetDlgItemTextW(dialog, IDC_PICKS_NOTE, L"These are the picks a new HC27 career starts with. They are not part of the roster file: "
                                                    L"Save writes them to the hc27 folder, for every roster. Careers already started keep theirs.");
            ListPicks(dialog, *data);
            return TRUE;
        }
        if (message != WM_COMMAND)
            return FALSE;
        if (LOWORD(wParam) == IDC_PICKS_DRAFT && HIWORD(wParam) == CBN_SELCHANGE)
            ListPicks(dialog, *data);
        else if (LOWORD(wParam) == IDC_PICKS_GIVE)
        {
            const HWND list = GetDlgItem(dialog, IDC_PICKS_LIST);
            const LRESULT at = SendDlgItemMessageW(dialog, IDC_PICKS_OWNER, CB_GETCURSEL, 0, 0);
            const int owner = int(SendDlgItemMessageW(dialog, IDC_PICKS_OWNER, CB_GETITEMDATA, WPARAM(at), 0));
            std::vector<int> selected;
            for (int item = -1; (item = ListView_GetNextItem(list, item, LVNI_SELECTED)) >= 0;)
                selected.push_back(item);
            for (const int item : selected)
                data->picks[size_t(data->shown[size_t(item)])].owner = owner;
            const int top = ListView_GetTopIndex(list);
            ListPicks(dialog, *data);
            for (const int item : selected)
                ListView_SetItemState(list, item, LVIS_SELECTED, LVIS_SELECTED);
            ListView_EnsureVisible(list, std::min(top + ListView_GetCountPerPage(list) - 1, ListView_GetItemCount(list) - 1), FALSE);
        }
        else if (LOWORD(wParam) == IDOK)
        {
            if (SavePicks(*data))
                EndDialog(dialog, IDOK);
        }
        else if (LOWORD(wParam) == IDCANCEL)
            EndDialog(dialog, IDCANCEL);
        return TRUE;
    }

    void PicksDialog()
    {
        if (g.game.templates.empty() || !g.game.CanPatchDisc())
        {
            MessageBoxW(g.window, L"Draft picks can be changed for HC27 only, and need the game's disc image (IsoPath in hc09.ini): "
                                  L"a career takes its picks from the game's own database, which HC27 lets the hc27 folder change.",
                L"Draft picks", MB_ICONINFORMATION);
            return;
        }
        PicksData data;
        LoadPicks(data);
        if (DialogBoxParamW(g.instance, MAKEINTRESOURCEW(IDD_PICKS), g.window, PicksProc, LPARAM(&data)) == IDOK)
            Status(L"Draft picks saved to hc27\\disc and hc27\\draft_picks.txt. New HC27 careers start with them.");
    }

    // ---- the window ----

    void Layout()
    {
        RECT client{};
        GetClientRect(g.window, &client);
        SendMessageW(g.status, WM_SIZE, 0, 0);
        RECT bar{};
        GetWindowRect(g.status, &bar);
        const int dpi = int(GetDpiForWindow(g.window));
        auto px = [&](int value) { return MulDiv(value, dpi, 96); };
        const int margin = px(8), left = px(330), top = px(8), bottom = client.bottom - (bar.bottom - bar.top) - margin;
        const int picture = px(kPortraitSize);
        MoveWindow(g.team, margin, top, left - margin, px(400), TRUE);
        MoveWindow(g.people, margin, top + px(30), left - margin, bottom - top - px(30), TRUE);
        const int x = left + margin;
        MoveWindow(g.portrait, x, top, picture, picture, TRUE);
        const int lookLeft = client.right - margin - picture, textWidth = lookLeft - margin - (x + picture + margin);
        MoveWindow(g.name, x + picture + margin, top, textWidth, px(34), TRUE);
        MoveWindow(g.info, x + picture + margin, top + px(38), textWidth, px(40), TRUE);
        MoveWindow(g.lookLabel, x + picture + margin, top + picture - px(34), textWidth, px(34), TRUE);
        MoveWindow(g.look, lookLeft, top, picture, picture, TRUE);
        const int buttonTop = top + px(86);
        int at = x + picture + margin;
        for (const int id : { IDM_PICTURE, IDM_MOVE, IDM_RELEASE })
        {
            const int width = px(id == IDM_RELEASE ? 170 : 130);
            MoveWindow(GetDlgItem(g.window, id), at, buttonTop, width, px(26), TRUE);
            at += width + margin;
        }
        MoveWindow(g.grid, x, top + picture + margin, client.right - x - margin, bottom - top - picture - margin, TRUE);
        ListView_SetColumnWidth(g.grid, 1, std::max<int>(px(160), client.right - x - margin - px(330) - GetSystemMetrics(SM_CXVSCROLL) - 4));
    }

    void AddColumns(HWND list, std::initializer_list<std::pair<const wchar_t*, int>> columns)
    {
        const int dpi = int(GetDpiForWindow(g.window));
        int i = 0;
        for (const auto& [title, width] : columns)
        {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(title);
            column.cx = MulDiv(width, dpi, 96);
            ListView_InsertColumn(list, i++, &column);
        }
    }

    void Create(HWND window)
    {
        g.window = window;
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        g.font = CreateFontIndirectW(&metrics.lfMessageFont);
        metrics.lfMessageFont.lfHeight = metrics.lfMessageFont.lfHeight * 2;
        metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
        g.bigFont = CreateFontIndirectW(&metrics.lfMessageFont);
        auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD extended = 0) {
            const HWND control = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                HMENU(INT_PTR(id)), g.instance, nullptr);
            SendMessageW(control, WM_SETFONT, WPARAM(g.font), TRUE);
            return control;
        };
        g.team = child(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_TEAM);
        g.people = child(WC_LISTVIEWW, nullptr, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_PEOPLE, WS_EX_CLIENTEDGE);
        g.grid = child(WC_LISTVIEWW, nullptr, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, IDC_GRID, WS_EX_CLIENTEDGE);
        g.portrait = child(L"STATIC", nullptr, SS_BITMAP | SS_CENTERIMAGE | SS_SUNKEN, IDC_PORTRAIT);
        g.look = child(L"STATIC", nullptr, SS_BITMAP | SS_CENTERIMAGE | SS_SUNKEN, IDC_LOOK);
        g.lookLabel = child(L"STATIC", L"", SS_NOPREFIX | SS_RIGHT, IDC_LOOK_LABEL);
        g.name = child(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, IDC_NAME);
        g.info = child(L"STATIC", L"", SS_NOPREFIX, IDC_INFO);
        SendMessageW(g.name, WM_SETFONT, WPARAM(g.bigFont), TRUE);
        child(L"BUTTON", L"Change picture...", WS_TABSTOP, IDM_PICTURE);
        child(L"BUTTON", L"Move to a team...", WS_TABSTOP, IDM_MOVE);
        child(L"BUTTON", L"Release to free agents", WS_TABSTOP, IDM_RELEASE);
        g.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, window,
            HMENU(INT_PTR(IDC_STATUS)), g.instance, nullptr);
        for (const HWND list : { g.people, g.grid })
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        AddColumns(g.people, { { L"Pos", 50 }, { L"#", 36 }, { L"Name", 170 }, { L"Ovr", 42 } });
        AddColumns(g.grid, { { L"Property", 330 }, { L"Value", 200 } });
        ListView_EnableGroupView(g.people, TRUE);
        ListView_EnableGroupView(g.grid, TRUE);
        for (int i = 0; i < 2; i++)
        {
            LVGROUP group{ sizeof(group) };
            group.mask = LVGF_HEADER | LVGF_GROUPID;
            group.pszHeader = const_cast<wchar_t*>(i ? L"Players" : L"Coaches");
            group.iGroupId = i;
            ListView_InsertGroup(g.people, -1, &group);
        }
        SetWindowSubclass(g.grid, GridProc, 1, 0);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            Create(window);
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { 900, 560 };
            return 0;
        case WM_APP:  // the in-place combo box closed
            EndEdit(wParam != 0);
            return 0;
        case WM_CTLCOLORSTATIC:
            if (HWND(lParam) == g.portrait || HWND(lParam) == g.look)
            {
                static const HBRUSH brush = CreateSolidBrush(kPortraitBack);
                return LRESULT(brush);
            }
            break;
        case WM_NOTIFY:
        {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->hwndFrom == g.people && header->code == LVN_ITEMCHANGED)
            {
                const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                {
                    EndEdit(true);
                    const uint32_t value = uint32_t(change->lParam);
                    const bool coach = (value & kCoachFlag) != 0;
                    const int64_t record = value & ~kCoachFlag;
                    if (coach != g.coach || record != g.selected)
                    {
                        g.coach = coach;
                        g.selected = record;
                        ShowPerson();
                    }
                }
            }
            else if (header->hwndFrom == g.grid && (header->code == NM_DBLCLK || header->code == NM_RETURN))
            {
                BeginEdit(ListView_GetNextItem(g.grid, -1, LVNI_SELECTED));
                return 0;
            }
            else if (header->hwndFrom == g.grid && header->code == LVN_KEYDOWN)
            {
                const WORD key = reinterpret_cast<const NMLVKEYDOWN*>(lParam)->wVKey;
                if (key == VK_F2 || key == VK_SPACE)
                    BeginEdit(ListView_GetNextItem(g.grid, -1, LVNI_SELECTED));
            }
            break;
        }
        case WM_COMMAND:
            if (HWND(lParam) == g.team && HIWORD(wParam) == CBN_SELCHANGE && g.loaded)
            {
                EndEdit(true);
                FillPeople();
                return 0;
            }
            if (lParam && HIWORD(wParam) != BN_CLICKED)
                break;
            switch (LOWORD(wParam))
            {
            case IDM_OPEN_DEFAULT:
                if (ConfirmDiscard())
                    OpenRoster(g.root / L"hc27" / L"ROSTER");
                break;
            case IDM_OPEN:
                OpenDialog();
                break;
            case IDM_SAVE:
                EndEdit(true);
                if (g.loaded)
                    SaveAll();
                break;
            case IDM_EXIT:
                SendMessageW(window, WM_CLOSE, 0, 0);
                break;
            case IDM_PICTURE:
                if (g.loaded)
                    ChangePicture();
                break;
            case IDM_PICTURE_REMOVE:
                if (g.loaded)
                    RemovePicture();
                break;
            case IDM_MOVE:
                if (g.loaded)
                    MoveDialog();
                break;
            case IDM_RELEASE:
                if (g.loaded && NeedPlayer() && g.roster.play->Get(uint32_t(g.selected), "TGID") != kFreeAgents)
                {
                    EndEdit(true);
                    MovePlayer(kFreeAgents);
                }
                break;
            case IDM_PRESET:
                if (g.loaded)
                    PresetDialog();
                break;
            case IDM_DEPTH:
                if (g.loaded && CurrentTeam() <= 32)
                {
                    g.roster.RebuildDepthChart(CurrentTeam());
                    UpdateTitle();
                    Status(L"Depth chart rebuilt: at each position the best overall rating starts.");
                }
                break;
            case IDM_PICKS:
                if (g.loaded)
                    PicksDialog();
                break;
            }
            return 0;
        case WM_CLOSE:
            EndEdit(true);
            if (ConfirmDiscard())
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    // The install folder: the nearest one above the program that holds hc09.ini or the game's sources.
    std::filesystem::path FindRoot()
    {
        wchar_t module[MAX_PATH * 2] = L"";
        GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
        std::filesystem::path folder = std::filesystem::path(module).parent_path();
        for (std::filesystem::path at = folder; !at.empty(); at = at.parent_path())
        {
            if (std::filesystem::exists(at / L"hc09.ini") || std::filesystem::exists(at / L"hc27" / L"ROSTER"))
                return at;
            if (at == at.root_path())
                break;
        }
        return folder;
    }

    // --check <roster>: loads it and reports, without a window (for testing).
    int Check(const std::filesystem::path& path, const std::filesystem::path& out)
    {
        FILE* log = _wfopen(out.c_str(), L"w");
        if (!log)
            return 2;
        std::wstring error;
        Roster roster;
        if (!roster.Load(path, error))
        {
            std::fprintf(log, "load failed: %ls\n", error.c_str());
            std::fclose(log);
            return 1;
        }
        std::fprintf(log, "roster: %u players, %u coaches, %zu teams, checksums %s, hc27 %d\n", roster.play->count, roster.coch->count,
            roster.teams.size(), roster.save.ChecksumsValid() ? "valid" : "INVALID", int(roster.hc27));
        GameData game;
        const bool disc = game.Load(g.root, error);
        std::fprintf(log, "disc: %d (%ls) playbooks %zu/%zu colleges %zu heads %zu presets %zu skills %zu templates %zu statics %zu portraits %zu/%zu\n",
            int(disc), error.c_str(), game.offensePlaybooks.size(), game.defensePlaybooks.size(), game.colleges.size(), game.heads.size(),
            game.coachPresets.size(), game.specialSkills.size(), game.templates.size(), game.statics.size(), game.playerPortraits.size(),
            game.coachPortraits.size());
        for (size_t i = 0; i < game.specialSkills.size() && i < 4; i++)
            std::fprintf(log, "  skill %d: %s (roles %X)\n", game.specialSkills[i].id, game.specialSkills[i].name.c_str(), game.specialSkills[i].roles);
        // A move there and back, a rename, a portrait of his own; saved beside the report.
        const std::vector<uint32_t> bears = roster.Players(1);
        if (!bears.empty())
        {
            const uint32_t r = bears[0];
            std::fprintf(log, "first Bear: %s (#%lld), %zu Bears\n", roster.PlayerName(r).c_str(), static_cast<long long>(roster.play->Get(r, "PJEN")), bears.size());
            roster.MovePlayer(r, 2);
            std::fprintf(log, "moved to %s: now %zu Bears, %zu Bengals, number %lld\n", roster.TeamName(2).c_str(), roster.Players(1).size(),
                roster.Players(2).size(), static_cast<long long>(roster.play->Get(r, "PJEN")));
            roster.MovePlayer(bears.back(), kFreeAgents);
            roster.play->SetText(r, "PFNA", "Edited");
            std::fprintf(log, "portrait of his own: %d\n", roster.OwnPortrait(false, r, game.playerPortraits));
        }
        const std::filesystem::path picture = roster.PortraitFolder(false, g.root) / L"1018.png";
        std::filesystem::path imported = out;
        imported += L".png";
        std::fprintf(log, "picture import: %d%c", int(ImportPicture(picture, imported, 351)), 10);
        roster.path = out;
        roster.path += L".roster";
        std::filesystem::copy_file(path, roster.path, std::filesystem::copy_options::overwrite_existing);
        const bool saved = roster.Save(error);
        Pixels a, b, c;
        const bool pa = game.images.Portrait(false, 1018, a), pb = game.images.Portrait(true, 559, b),
                   pc = !game.heads.empty() && game.images.Head(game.heads[0].name, c);
        std::fprintf(log, "disc pictures: player %d %dx%d, coach %d %dx%d, head %d %dx%d%c", int(pa), a.width, a.height, int(pb), b.width, b.height,
            int(pc), c.width, c.height, 10);
        std::fprintf(log, "saved %d %ls\n", int(saved), error.c_str());
        std::fclose(log);
        return saved ? 0 : 1;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    g.instance = instance;
    g.root = FindRoot();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 4 && std::wstring(argv[1]) == L"--check")
        return Check(argv[2], argv[3]);

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{ sizeof(type) };
    type.lpfnWndProc = WindowProc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    type.lpszClassName = L"HC09RosterEditor";
    type.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&type);
    CreateWindowExW(0, type.lpszClassName, L"HC09 Roster Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820,
        nullptr, nullptr, instance, nullptr);
    ShowWindow(g.window, show);

    std::wstring error;
    if (!g.game.Load(g.root, error))
        Status(error);
    if (argc >= 2 && std::filesystem::exists(argv[1]))
        OpenRoster(argv[1]);
    else if (std::filesystem::exists(g.root / L"hc27" / L"ROSTER"))
        OpenRoster(g.root / L"hc27" / L"ROSTER");
    else
        Status(L"Open a saved roster with File > Open (save one in the game: My Coach > File Management > Save).");

    const HACCEL keys = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_MENU));
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (g.editor && (message.hwnd == g.editor || IsChild(g.editor, message.hwnd)))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && message.hwnd == g.grid)
        {
            BeginEdit(ListView_GetNextItem(g.grid, -1, LVNI_SELECTED));  // Enter would otherwise go to the dialog keys
            continue;
        }
        if (!TranslateAcceleratorW(g.window, keys, &message) && !IsDialogMessageW(g.window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}
