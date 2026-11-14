#pragma once

// What the editor reads from the game's disc image: the names behind the roster's numbers (playbooks,
// colleges, 3D heads, create-a-coach appearances), the portrait ids the game's libraries hold, and the two
// databases a new career starts from besides the roster: the career template (its draft picks) and the
// static database (each coach's special skills).
//
// Those two are edited as HC27 does every disc change: the changed table is written to hc27\disc as
// <archive>@<offset>.bin, which HC27.exe lays over the disc's files at startup (tools/roster_import/
// disc_patches.py). The original game (HC09.exe) has no such folder, so there they cannot be changed.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "disc_images.h"
#include "tdb.h"

namespace hc09::editor
{
    struct Named
    {
        int id = 0;
        std::string name;
    };

    // A database on the disc, with hc27\disc's changes already applied.
    struct DiscDatabase
    {
        std::string archive;  // e.g. "qkl_fe2ig.ast"
        uint64_t offset = 0;  // in the archive
        std::vector<uint8_t> data;
        Database db;
    };

    struct CoachPreset
    {
        int face = 0, skin = 0, body = 0;  // COCH CFSH, CSKI, CBSZ
        int portrait = 0;                  // the picture of it in the coaches' portrait library
    };

    struct SpecialSkill
    {
        int id = 0;
        uint32_t roles = 0;  // bit COCH.COPS (0 head coach ... 10 defensive backs coach) that can hold it
        int tree = 0;
        std::string name, description;
    };

    class GameData
    {
    public:
        // `root` is the install folder (the one holding hc09.ini and hc27\). False when there is no usable
        // disc image: the editor then shows numbers where it has no names.
        bool Load(const std::filesystem::path& root, std::wstring& error);
        bool Loaded() const { return loaded_; }

        std::vector<Named> offensePlaybooks, defensePlaybooks, colleges, heads;
        std::vector<CoachPreset> coachPresets;
        std::vector<SpecialSkill> specialSkills;
        std::set<int> playerPortraits, coachPortraits;  // ids the 2008 roster used: those the libraries hold
        DiscImages images;                               // the game's own portraits and head textures

        // Every copy of the career template (DRPK with both drafts) and of the static database (PSSS).
        std::vector<std::unique_ptr<DiscDatabase>> templates, statics;
        bool CanPatchDisc() const { return std::filesystem::exists(root_ / L"hc27"); }
        // Writes `table` of every database in `copies` to hc27\disc.
        bool WritePatches(std::vector<std::unique_ptr<DiscDatabase>>& copies, const std::string& table, std::wstring& error);
        const std::filesystem::path& Root() const { return root_; }

    private:
        std::filesystem::path root_;
        bool loaded_ = false;
    };
}
