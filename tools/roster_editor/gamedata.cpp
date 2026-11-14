#include "gamedata.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "disc.h"

namespace hc09::editor
{
    namespace
    {
        const char* const kArchives[] = { "qkl_boot.ast", "qkl_fe2ig.ast", "qkl_interface.ast", "qkl_misc.ast" };

        // The disc image mapped into memory (read-only).
        struct Mapping
        {
            HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
            const uint8_t* data = nullptr;
            uint64_t size = 0;

            bool Open(const std::filesystem::path& path)
            {
                file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                    return false;
                LARGE_INTEGER length{};
                GetFileSizeEx(file, &length);
                size = uint64_t(length.QuadPart);
                mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
                data = mapping ? static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
                return data != nullptr;
            }
            ~Mapping()
            {
                if (data)
                    UnmapViewOfFile(data);
                if (mapping)
                    CloseHandle(mapping);
                if (file != INVALID_HANDLE_VALUE)
                    CloseHandle(file);
            }
        };

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

        struct Hit
        {
            int archive = 0;
            uint64_t offset = 0;  // in the archive
        };

        // Every database in the archives: "DB" 00 08 with a size that fits. Scanning the disc takes a few
        // seconds, so the list is kept in build\roster_editor.cache for the same image.
        std::vector<Hit> FindDatabases(const Mapping& iso, const uint64_t starts[], const uint64_t sizes[],
            const std::filesystem::path& cache)
        {
            std::vector<Hit> hits;
            {
                std::ifstream file(cache);
                uint64_t size = 0;
                size_t count = 0;
                if (file >> size >> count && size == iso.size)
                {
                    for (size_t i = 0; i < count; i++)
                    {
                        Hit hit;
                        if (file >> hit.archive >> hit.offset && hit.archive >= 0 && hit.archive < int(std::size(kArchives)))
                            hits.push_back(hit);
                    }
                    if (hits.size() == count && count)
                        return hits;
                    hits.clear();
                }
            }
            for (int a = 0; a < int(std::size(kArchives)); a++)
            {
                if (!sizes[a])
                    continue;
                const uint8_t* begin = iso.data + starts[a];
                const uint8_t* end = begin + sizes[a];
                for (const uint8_t* p = begin; p + 24 < end;)
                {
                    p = static_cast<const uint8_t*>(std::memchr(p, 'D', size_t(end - 24 - p)));
                    if (!p)
                        break;
                    if (p[1] == 'B' && p[2] == 0 && p[3] == 8)
                    {
                        const uint64_t size = Be32(p + 8), tables = Be32(p + 16);
                        if (size >= 64 && p + size <= end && tables && tables < 2048 && Be32(p + 12) == 0)
                            hits.push_back({ a, uint64_t(p - begin) });
                    }
                    p++;
                }
            }
            std::error_code ec;
            std::filesystem::create_directories(cache.parent_path(), ec);
            std::ofstream file(cache, std::ios::trunc);
            file << iso.size << ' ' << hits.size() << '\n';
            for (const Hit& hit : hits)
                file << hit.archive << ' ' << hit.offset << '\n';
            return hits;
        }

        // Lays hc27\disc's files over a database just read from the disc.
        void ApplyPatches(DiscDatabase& d, const std::filesystem::path& folder)
        {
            for (const Table& t : d.db.Tables())
            {
                char name[96];
                std::snprintf(name, sizeof(name), "%s@%llX.bin", d.archive.c_str(), static_cast<unsigned long long>(d.offset + t.dataStart));
                std::ifstream file(folder / name, std::ios::binary);
                if (!file)
                    continue;
                const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (bytes.size() == d.db.TableEnd(t) - t.dataStart)
                    std::memcpy(&d.data[t.dataStart], bytes.data(), bytes.size());
            }
            d.db.Open(&d.data, 0);  // counts are in the headers, which patches leave alone; names stay valid
        }

        std::string Trim(std::string text)
        {
            while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
                text.pop_back();
            return text;
        }
    }

    bool GameData::Load(const std::filesystem::path& root, std::wstring& error)
    {
        root_ = root;
        wchar_t path[1024] = L"";
        GetPrivateProfileStringW(L"Game", L"IsoPath", L"", path, DWORD(std::size(path)), (root / L"hc09.ini").c_str());
        if (!path[0])
            GetPrivateProfileStringW(L"Settings", L"IsoPath", L"", path, DWORD(std::size(path)), (root / L"hc09.ini").c_str());
        if (!path[0])
        {
            // Any section: the key is unique in the file.
            std::wifstream ini(root / L"hc09.ini");
            for (std::wstring line; std::getline(ini, line);)
                if (_wcsnicmp(line.c_str(), L"IsoPath=", 8) == 0)
                    wcsncpy_s(path, line.c_str() + 8, _TRUNCATE);
            while (path[0] && (path[wcslen(path) - 1] == L'\r' || path[wcslen(path) - 1] == L' '))
                path[wcslen(path) - 1] = 0;
        }
        std::filesystem::path isoPath = path;
        if (isoPath.is_relative())
            isoPath = root / isoPath;
        if (!path[0] || !std::filesystem::exists(isoPath))
        {
            error = L"The game's disc image was not found (IsoPath in hc09.ini). Names of playbooks, colleges and heads, "
                    L"special skills and draft picks need it.";
            return false;
        }
        uint64_t starts[std::size(kArchives)]{}, sizes[std::size(kArchives)]{};
        for (size_t a = 0; a < std::size(kArchives); a++)
        {
            std::wstring ignored;
            if (auto found = setup::FindDiscFile(isoPath, kArchives[a], ignored))
            {
                starts[a] = found->offset;
                sizes[a] = found->size;
            }
        }
        Mapping iso;
        if (!iso.Open(isoPath) || !sizes[0] || !sizes[1])
        {
            error = L"The disc image could not be read: " + isoPath.wstring();
            return false;
        }
        for (size_t a = 0; a < std::size(kArchives); a++)
            if (starts[a] + sizes[a] > iso.size)
                sizes[a] = 0;

        const std::filesystem::path patches = root / L"hc27" / L"disc";
        const bool patched = CanPatchDisc();
        auto read = [&](const Hit& hit) {
            auto d = std::make_unique<DiscDatabase>();
            d->archive = kArchives[hit.archive];
            d->offset = hit.offset;
            const uint8_t* p = iso.data + starts[hit.archive] + hit.offset;
            d->data.assign(p, p + Be32(p + 8));
            if (!d->db.Open(&d->data, 0))
                return std::unique_ptr<DiscDatabase>();
            if (patched)
                ApplyPatches(*d, patches);
            return d;
        };
        // What a database is, from its directory alone (the table names, reversed, after the header).
        auto has = [&](const Hit& hit, const char* table) {
            const uint8_t* p = iso.data + starts[hit.archive] + hit.offset;
            const uint32_t count = Be32(p + 16);
            if (24 + uint64_t(count) * 8 > Be32(p + 8))
                return false;
            for (uint32_t i = 0; i < count; i++)
            {
                const uint8_t* name = p + 24 + i * 8;
                if (name[0] == table[3] && name[1] == table[2] && name[2] == table[1] && name[3] == table[0])
                    return true;
            }
            return false;
        };

        bool playbooks = false, roster = false, names = false;
        for (const Hit& hit : FindDatabases(iso, starts, sizes, root / L"build" / L"roster_editor.cache"))
        {
            if (!playbooks && has(hit, "PLAB"))
            {
                if (auto d = read(hit))
                    if (Table* t = d->db.Find("PLAB"); t && t->count && t->Has("BNAM"))
                    {
                        for (uint32_t i = 0; i < t->count; i++)
                            (t->Get(i, "BOFF") ? offensePlaybooks : defensePlaybooks).push_back({ int(t->Get(i, "BGID")), Trim(t->Text(i, "BNAM")) });
                        playbooks = true;
                    }
            }
            if (has(hit, "DRPK") && has(hit, "SEAI"))
            {
                if (auto d = read(hit))
                    if (Table* t = d->db.Find("DRPK"); t && t->count == 448)
                        templates.push_back(std::move(d));
            }
            if (has(hit, "PSSS") && has(hit, "LSSD"))
            {
                auto d = read(hit);
                if (!d)
                    continue;
                if (!names)
                {
                    names = true;
                    if (Table* t = d->db.Find("COLL"))
                        for (uint32_t i = 0; i < t->count; i++)
                            colleges.push_back({ int(t->Get(i, "COID")), t->Text(i, "CONA") });
                    if (Table* t = d->db.Find("PGHE"))
                        for (uint32_t i = 0; i < t->count; i++)
                            heads.push_back({ int(t->Get(i, "PGHE")), t->Text(i, "PFCG") });
                    if (Table* t = d->db.Find("CCMS"))
                        for (uint32_t i = 0; i < t->count; i++)
                            coachPresets.push_back({ int(t->Get(i, "CFSH")), int(t->Get(i, "CSKI")), int(t->Get(i, "CBSZ")), int(t->Get(i, "CSXP")) });
                    if (Table* t = d->db.Find("COCH"))
                        for (uint32_t i = 0; i < t->count; i++)
                            coachPortraits.insert(int(t->Get(i, "PNPt")));
                    if (Table* t = d->db.Find("LSSD"))
                    {
                        // Who can hold a skill: the head coach, the coordinators and each position's coach.
                        static const char* const kRoles[] = { "ATHC", "ATOC", "ATDC", "ATST", "ATQB", "ATRB", "ATWR", "ATOL", "ATDL", "ATLB", "ATDB" };
                        for (uint32_t i = 0; i < t->count; i++)
                        {
                            SpecialSkill skill;
                            skill.id = int(t->Get(i, "SPSK"));
                            skill.tree = int(t->Get(i, "SKTR"));
                            for (int r = 0; r < int(std::size(kRoles)); r++)
                                if (t->Get(i, kRoles[r]))
                                    skill.roles |= 1u << r;
                            specialSkills.push_back(skill);
                        }
                        if (Table* e = d->db.Find("ESPS"))  // their names
                            for (uint32_t i = 0; i < e->count; i++)
                                for (SpecialSkill& skill : specialSkills)
                                    if (skill.id == int(e->Get(i, "ENUV")))
                                    {
                                        skill.name = e->Text(i, "ENDN");
                                        skill.description = e->Text(i, "SPDC");
                                    }
                        std::sort(specialSkills.begin(), specialSkills.end(), [](const SpecialSkill& a, const SpecialSkill& b) { return a.name < b.name; });
                    }
                }
                statics.push_back(std::move(d));
            }
            if (!roster && has(hit, "PLAY") && has(hit, "DCHT") && has(hit, "TEAM") && !has(hit, "SEAI"))
            {
                // The 2008 roster: its portrait ids are the ones the game's portrait library holds.
                if (auto d = read(hit))
                    if (Table* t = d->db.Find("PLAY"); t && t->count > 1500)
                    {
                        for (uint32_t i = 0; i < t->count; i++)
                            playerPortraits.insert(int(t->Get(i, "PSXP")));
                        roster = true;
                    }
            }
        }
        std::sort(colleges.begin(), colleges.end(), [](const Named& a, const Named& b) { return a.name < b.name; });
        images.Open(isoPath, root / L"build" / L"roster_editor_images.cache");
        loaded_ = playbooks || names;
        if (!loaded_)
            error = L"The disc image holds none of the game's databases: " + isoPath.wstring();
        return loaded_;
    }

    bool GameData::WritePatches(std::vector<std::unique_ptr<DiscDatabase>>& copies, const std::string& table, std::wstring& error)
    {
        const std::filesystem::path folder = root_ / L"hc27" / L"disc";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        // The files this tool wrote are listed in roster_editor.files, as the Python tools list theirs.
        std::set<std::string> listed;
        {
            std::ifstream list(folder / L"roster_editor.files");
            for (std::string line; std::getline(list, line);)
                if (!Trim(line).empty())
                    listed.insert(Trim(line));
        }
        for (auto& d : copies)
        {
            Table* t = d->db.Find(table);
            if (!t)
                continue;
            d->db.FixTableCrcs();
            char name[96];
            std::snprintf(name, sizeof(name), "%s@%llX.bin", d->archive.c_str(), static_cast<unsigned long long>(d->offset + t->dataStart));
            std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(&d->data[t->dataStart]), std::streamsize(d->db.TableEnd(*t) - t->dataStart));
            if (!file)
            {
                error = L"Could not write " + (folder / name).wstring();
                return false;
            }
            listed.insert(name);
        }
        std::ofstream list(folder / L"roster_editor.files", std::ios::trunc);
        for (const std::string& name : listed)
            list << name << '\n';
        return true;
    }
}
