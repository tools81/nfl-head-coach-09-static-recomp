#pragma once

// A roster save and the changes the editor makes to it beyond single fields: moving players between teams
// and the free agent pool (contract, jersey number, depth charts, team ratings) and giving a player or coach
// a portrait id of his own, so a picture can replace his portrait alone.

#include <filesystem>
#include <string>
#include <vector>

#include "gamedata.h"
#include "tdb.h"

namespace hc09::editor
{
    constexpr int kFreeAgents = 1009;      // PLAY.TGID of the free agent pool
    constexpr int kCoachFreeAgents = 1023;  // COCH.TGID of coaches without a team

    extern const char* const kPositions[21];  // PLAY.PPOS -> "QB", ...
    extern const char* const kCoachRoles[11];  // COCH.COPS -> "Head Coach", ...

    struct TeamEntry
    {
        int id = 0;
        std::string name;  // "Chicago Bears"
    };

    class Roster
    {
    public:
        bool Load(const std::filesystem::path& path, std::wstring& error);
        bool Save(std::wstring& error);  // keeps the file as it was in <name>.bak the first time

        std::filesystem::path path;
        SaveFile save;
        Table* play = nullptr;
        Table* coch = nullptr;
        Table* team = nullptr;
        Table* dcht = nullptr;
        Table* cskl = nullptr;
        std::vector<TeamEntry> teams;  // the 32 teams by name, then the free agents
        bool dirty = false;
        bool hc27 = false;  // an HC27 roster (hc27\ROSTER or a save of HC27's): which folders its pictures go to

        std::string TeamName(int id) const;
        std::vector<uint32_t> Players(int teamId) const;  // by position, best first
        std::vector<uint32_t> Coaches(int teamId) const;  // by role
        std::string PlayerName(uint32_t record) const;
        std::string CoachName(uint32_t record) const;

        // Puts the player on the team (or kFreeAgents). A free agent signs a one-year deal at the league's
        // lowest salary; a released player loses his contract; a player moved between teams keeps his (a
        // trade). A jersey number a new teammate wears is changed to a free one. Both teams' depth charts
        // are rebuilt by overall rating and every team's ratings are worked out again.
        void MovePlayer(uint32_t record, int teamId);
        void RebuildDepthChart(int teamId);
        void RateTeams();

        // The id his picture is stored under, after making it his alone: an id nobody else has, taken from
        // those the game's portrait library holds (`library`; empty when the disc image is not known) or
        // from the free agent who has had one to himself. -1 when none is left.
        int OwnPortrait(bool coach, uint32_t record, const std::set<int>& library);
        int Portrait(bool coach, uint32_t record) const;
        // textures\swappables\<library> of the game this roster belongs to.
        std::filesystem::path PortraitFolder(bool coach, const std::filesystem::path& root) const;
    };
}
