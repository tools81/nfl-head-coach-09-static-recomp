"""Gives HC27's players their real NFL statistics in the career database.

    python tools/roster_import/player_stats.py --iso "iso/NFL Head Coach 09.iso"

A new career takes its players' past statistics from the career database on the disc: season lines
(PSOF offense, PSDE defense, PSKI kicking and punting, PSKP returns, PSNG games; SEYR the season, -1
the last) and career totals (PCOF, PCDE, PCKI, PCKP, PCNG), found by player id (PGID). HC27's
players took over the 2008 players' records, ids included, so each started with the statistics of the
2008 player he replaced, and the game's player evaluation counts production: the 49ers' Kurtis Rourke
(a 2008 starter's 4,679 passing yards) rated 81 on the career's roster screen, Brock Purdy (a backup's
501) 73.

This fills the tables with the players' own regular-season statistics from nflverse
(https://github.com/nflverse/nflverse-data, CC-BY 4.0), the stats_player_reg_<season>.csv files
for 1999 to 2025 in --stats (default tools/roster_import/data/nflverse; download them from the
release "stats_player"). A player is found by his name, then his position and team. The tables
cannot grow (the patches leave their headers alone), so the rows are dealt out newest season first:
every player's 2025 before anyone's 2024. Longest plays, quarterback wins and kickoffs, which nflverse
does not have, stay 0. The tables go to hc27/disc (see disc_patches.py); --report lists the players
matched and the roster's players left without statistics.
"""

import argparse
import csv
import glob
import os
import re
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disc_patches  # noqa: E402
from import_roster import POSITION_NAMES  # noqa: E402
from tdb import SaveFile  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LAST_SEASON = 2025
NO_PLAYER = 32767  # PGID of the rows no player needs
TEAMS = {  # nflverse team -> TGID of the 2008 roster
    "CHI": 1, "CIN": 2, "BUF": 3, "DEN": 4, "CLE": 5, "TB": 6, "ARI": 7, "LAC": 8, "SD": 8, "KC": 9, "IND": 10,
    "DAL": 11, "MIA": 12, "PHI": 13, "ATL": 14, "SF": 15, "NYG": 16, "JAX": 17, "NYJ": 18, "DET": 19, "GB": 20,
    "CAR": 21, "NE": 22, "LV": 23, "OAK": 23, "LA": 24, "LAR": 24, "STL": 24, "BAL": 25, "WAS": 26, "NO": 27,
    "SEA": 28, "PIT": 29, "TEN": 30, "MIN": 31, "HOU": 32,
}
# nflverse position -> the roster positions (POSITION_NAMES) it can be.
GROUPS = {
    "QB": {"QB"}, "RB": {"HB", "FB"}, "HB": {"HB", "FB"}, "FB": {"FB", "HB"}, "WR": {"WR"}, "TE": {"TE"},
    "T": {"LT", "RT", "LG", "RG", "C"}, "OT": {"LT", "RT", "LG", "RG", "C"}, "G": {"LG", "RG", "LT", "RT", "C"},
    "OG": {"LG", "RG", "LT", "RT", "C"}, "C": {"C", "LG", "RG"}, "OL": {"LT", "LG", "C", "RG", "RT"},
    "DE": {"LE", "RE", "DT", "LOLB", "ROLB"}, "DT": {"DT", "LE", "RE"}, "NT": {"DT"}, "DL": {"LE", "RE", "DT"},
    "OLB": {"LOLB", "ROLB", "LE", "RE", "MLB"}, "ILB": {"MLB", "LOLB", "ROLB"}, "MLB": {"MLB", "LOLB", "ROLB"},
    "LB": {"LOLB", "MLB", "ROLB", "LE", "RE"}, "CB": {"CB", "FS", "SS"}, "DB": {"CB", "FS", "SS"},
    "S": {"FS", "SS", "CB"}, "FS": {"FS", "SS", "CB"}, "SS": {"SS", "FS", "CB"}, "SAF": {"FS", "SS", "CB"},
    "K": {"K", "P"}, "P": {"P", "K"},
}


def num(row, *names):
    total = 0.0
    for name in names:
        value = row.get(name, "")
        if value not in ("", "NA"):
            total += float(value)
    return total


# Each table's fields: (season field, career field, how to compute it from a season row). Career
# totals are the sums of the seasons'.
OFFENSE = [
    ("saat", "caat", lambda r: num(r, "attempts")), ("sacm", "cacm", lambda r: num(r, "completions")),
    ("saya", "caya", lambda r: num(r, "passing_yards")), ("satd", "catd", lambda r: num(r, "passing_tds")),
    ("sain", "cain", lambda r: num(r, "passing_interceptions")), ("suat", "cuat", lambda r: num(r, "carries")),
    ("suya", "cuya", lambda r: num(r, "rushing_yards")), ("sutd", "cutd", lambda r: num(r, "rushing_tds")),
    ("sufu", "cufu", lambda r: num(r, "rushing_fumbles", "receiving_fumbles", "sack_fumbles")),
    ("scca", "ccca", lambda r: num(r, "receptions")), ("scya", "ccya", lambda r: num(r, "receiving_yards")),
    ("sctd", "cctd", lambda r: num(r, "receiving_tds")),
]
DEFENSE = [
    ("sdta", "cdta", lambda r: num(r, "def_tackles_solo", "def_tackle_assists")),
    ("slsk", "clsk", lambda r: num(r, "def_sacks")), ("slff", "clff", lambda r: num(r, "def_fumbles_forced")),
    ("ssin", "csin", lambda r: num(r, "def_interceptions")), ("ssiy", "csiy", lambda r: num(r, "def_interception_yards")),
    ("sdpd", "cdpd", lambda r: num(r, "def_pass_defended")), ("sdtl", "cdtl", lambda r: num(r, "def_tackles_for_loss")),
    ("slfr", "clfr", lambda r: num(r, "fumble_recovery_opp")),
]
KICKING = [
    ("skea", "ckea", lambda r: num(r, "pat_att")), ("skem", "ckem", lambda r: num(r, "pat_made")),
    ("skfa", "ckfa", lambda r: num(r, "fg_att")), ("skfm", "ckfm", lambda r: num(r, "fg_made")),
    ("skaa", "cka0", lambda r: num(r, "fg_made_0_19", "fg_made_20_29", "fg_missed_0_19", "fg_missed_20_29")),
    ("skma", "ckma", lambda r: num(r, "fg_made_0_19", "fg_made_20_29")),
    ("skac", "ckac", lambda r: num(r, "fg_made_30_39", "fg_missed_30_39")), ("skmc", "ckmc", lambda r: num(r, "fg_made_30_39")),
    ("skad", "ckad", lambda r: num(r, "fg_made_40_49", "fg_missed_40_49")), ("skmd", "ckmd", lambda r: num(r, "fg_made_40_49")),
    ("skae", "ckae", lambda r: num(r, "fg_made_50_59", "fg_made_60_", "fg_missed_50_59", "fg_missed_60_")),
    ("skme", "ckme", lambda r: num(r, "fg_made_50_59", "fg_made_60_")),
    ("spat", "cpat", lambda r: num(r, "pt_att")), ("spya", "cpya", lambda r: num(r, "pt_yards")),
    ("sppt", "cppt", lambda r: num(r, "pt_inside_20")), ("sptb", "cptb", lambda r: num(r, "pt_touchback")),
    ("spbl", "cpbl", lambda r: num(r, "pt_blocked")), ("spny", "cpny", lambda r: num(r, "pt_net_yards")),
]
RETURNS = [
    ("srka", "crka", lambda r: num(r, "kickoff_returns")), ("srky", "crky", lambda r: num(r, "kickoff_return_yards")),
    ("srpa", "crpa", lambda r: num(r, "punt_returns")), ("srpy", "crpy", lambda r: num(r, "punt_return_yards")),
]
GAMES = [("sgmp", "cgmp", lambda r: num(r, "games"))]
# (season table, career table, fields)
TABLES = [
    ("PSOF", "PCOF", OFFENSE), ("PSDE", "PCDE", DEFENSE), ("PSKI", "PCKI", KICKING), ("PSKP", "PCKP", RETURNS),
    ("PSNG", "PCNG", GAMES),
]
# The roster positions a table keeps lines for (the tables are full: a lineman's fake-punt catch or a
# receiver's special-teams tackle would take a defender's or a receiver's row).
TABLE_POSITIONS = {
    "PSOF": {"QB", "HB", "FB", "WR", "TE"}, "PSDE": {"LE", "RE", "DT", "LOLB", "MLB", "ROLB", "CB", "FS", "SS"},
    "PSKI": {"K", "P"},
}
# The longest field of the season and the career (the best season's), from an nflverse column.
LONGEST = {"PSKI": [("skfL", "ckfL", "fg_long"), ("splN", "cplN", "pt_long")]}


NICKNAMES = {"mike": "michael", "matt": "matthew", "chris": "christopher", "nick": "nicholas", "josh": "joshua",
             "jon": "jonathan", "will": "william", "bill": "william", "bob": "robert", "rob": "robert", "tony": "anthony",
             "dan": "daniel", "danny": "daniel", "jim": "james", "jimmy": "james", "joe": "joseph", "tom": "thomas",
             "ben": "benjamin", "sam": "samuel", "zach": "zachary", "alex": "alexander", "andy": "andrew", "drew": "andrew",
             "greg": "gregory", "jeff": "jeffrey", "ken": "kenneth", "pat": "patrick", "rick": "richard", "steve": "steven"}


def name_key(name):
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()  # Estimé -> Estime
    name = re.sub(r"[^a-z ]", "", name.lower().replace("-", " "))
    words = [w for w in name.split() if w not in {"jr", "sr", "ii", "iii", "iv", "v"}]
    return " ".join(words)


def load_seasons(stats_dir):
    """player_id -> {season: row}, and player_id -> (name, position, the latest season's team)."""
    seasons, info = {}, {}
    for path in sorted(glob.glob(os.path.join(stats_dir, "stats_player_reg_*.csv"))):
        season = int(re.search(r"(\d{4})\.csv$", path).group(1))
        if season > LAST_SEASON:
            continue
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                if row.get("season_type", "REG") != "REG":
                    continue
                pid = row["player_id"]
                seasons.setdefault(pid, {})[season] = row
                if pid not in info or season >= info[pid][3]:
                    info[pid] = (row["player_display_name"], row["position"], row["recent_team"], season)
    if not seasons:
        raise SystemExit(f"no nflverse statistics (stats_player_reg_<season>.csv) in {stats_dir}")
    return seasons, info


def match(play, info):
    """PLAY index -> nflverse player_id, for the roster's players."""
    roster = {}
    for i in range(play.count):
        if 1 <= play.get(i, "TGID") <= 32 or play.get(i, "TGID") == 1009:
            roster.setdefault(name_key(f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}"), []).append(i)
    by_name = {}
    for pid, (name, position, team, season) in info.items():
        by_name.setdefault(name_key(name), []).append(pid)
    by_last = {}
    for pid, (name, _, _, _) in info.items():
        words = name_key(name).split()
        if words:
            by_last.setdefault(words[-1][:3], []).append(pid)

    def close(roster_key, pid):
        """The roster's name cut short (its last names are, past a length) or a short first name (Cam for
        Cameron, Dax for Daxton): the last name starts the same, the first names start alike."""
        first, last = roster_key.split()[0], roster_key.split()[-1]
        first = NICKNAMES.get(first, first)
        words = name_key(info[pid][0]).split()
        return len(words) >= 2 and words[-1].startswith(last) and (words[0].startswith(first) or first.startswith(words[0])
                                                                       or (len(first) <= 3 and words[0][0] == first[0]))

    found = {}
    for key, indexes in roster.items():
        pids = by_name.get(key, [])
        for i in indexes:
            position = POSITION_NAMES[play.get(i, "PPOS")]
            fits = [p for p in pids if position in GROUPS.get(info[p][1], {position})]
            if not fits and len(key.split()) >= 2:
                near = [p for p in by_last.get(key.split()[-1][:3], []) if position in GROUPS.get(info[p][1], {position})]
                fits = [p for p in near if close(key, p)]
                if not fits:  # another first name (Andy for Andres): the one of that last name on his team
                    last = key.split()[-1]
                    fits = [p for p in near if name_key(info[p][0]).split()[-1].startswith(last)
                            and TEAMS.get(info[p][2]) == play.get(i, "TGID")]
                    fits = fits if len(fits) == 1 else []
            if len(fits) > 1:  # the same name: the one on this team last, else the most recent
                same_team = [p for p in fits if TEAMS.get(info[p][2]) == play.get(i, "TGID")]
                fits = same_team or sorted(fits, key=lambda p: -info[p][3])[:1]
            if fits:
                found[i] = fits[0]
    return found


def clamp(table, field_name, value):
    field = table.field(field_name)
    value = int(round(value))
    if field.kind == 2:
        low, high = -(1 << (field.bits - 1)), (1 << (field.bits - 1)) - 1
    else:
        low, high = 0, (1 << field.bits) - 1
    return max(low, min(high, value))


def fill(db, play, found, seasons):
    """Rewrites the statistics tables of one database; returns their names and the rows written."""
    pgid = {i: play.get(i, "PGID") for i in found}
    written = {}
    for season_name, career_name, fields in TABLES:
        st, ct = db.tables[season_name], db.tables[career_name]
        longest = LONGEST.get(season_name, [])

        def has_stats(row):
            return any(compute(row) for _, _, compute in fields)
        positions = TABLE_POSITIONS.get(season_name)
        lines = {}  # PLAY index -> [(season, row)] with something to show, newest first
        for i, pid in found.items():
            if positions and POSITION_NAMES[play.get(i, "PPOS")] not in positions:
                continue
            rows = [(s, r) for s, r in sorted(seasons[pid].items(), reverse=True) if has_stats(r)]
            if rows:
                lines[i] = rows
        # Career rows, as many as fit: the players on teams before the free agents, and of each, the most
        # games in the last three seasons first, then in all.
        def playing_time(i):
            return (not 1 <= play.get(i, "TGID") <= 32, -sum(num(r, "games") for s, r in lines[i] if s > LAST_SEASON - 3),
                    -sum(num(r, "games") for _, r in lines[i]))
        ranked = sorted(lines, key=playing_time)[:ct.count]
        careers = sorted(ranked, key=lambda i: pgid[i])
        # Season rows: newest first across the players with a career row.
        picks, depth = [], 0
        while len(picks) < st.count:
            added = False
            for i in careers:
                if depth < len(lines[i]) and len(picks) < st.count:
                    picks.append((i, lines[i][depth]))
                    added = True
            if not added:
                break
            depth += 1
        picks.sort(key=lambda p: (pgid[p[0]], -p[1][0]))

        def clear_row(t, r):
            for f in t.fields:
                if f.kind in (2, 3) and f.name != "STKF":
                    t.set(r, f.name, 0)
            t.set(r, "PGID", NO_PLAYER)
        for r in range(st.count):
            clear_row(st, r)
            if r < len(picks):
                i, (season, row) = picks[r]
                st.set(r, "PGID", pgid[i])
                st.set(r, "SEYR", season - LAST_SEASON - 1)
                for field, _, compute in fields:
                    st.set(r, field, clamp(st, field, compute(row)))
                for field, _, column in longest:
                    st.set(r, field, clamp(st, field, num(row, column)))
                if season_name == "PSNG":
                    st.set(r, "sgYY", TEAMS.get(row.get("recent_team", ""), 0))
        for r in range(ct.count):
            clear_row(ct, r)
            if r < len(careers):
                i = careers[r]
                rows = [row for _, row in lines[i]]
                ct.set(r, "PGID", pgid[i])
                for _, field, compute in fields:
                    ct.set(r, field, clamp(ct, field, sum(compute(row) for row in rows)))
                for _, field, column in longest:
                    ct.set(r, field, clamp(ct, field, max(num(row, column) for row in rows)))
        written[season_name] = len(picks)
        written[career_name] = len(careers)
    return [name for name, _, _ in TABLES] + [name for _, name, _ in TABLES], written


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--roster", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--stats", default=os.path.join(ROOT, "tools", "roster_import", "data", "nflverse"))
    parser.add_argument("--out", default=disc_patches.OUT)
    parser.add_argument("--report", default=os.path.join(ROOT, "tools", "roster_import", "data", "player_stats.csv"))
    args = parser.parse_args()
    play = SaveFile(args.roster).db.tables["PLAY"]
    seasons, info = load_seasons(args.stats)
    found = match(play, info)
    patches, counts = [], None
    for path, offset, db in disc_patches.databases(args.iso, [t for t, _, _ in TABLES]):
        changed, counts = fill(db, play, found, seasons)
        patches.append((path, offset, db, changed))
    if not patches:
        raise SystemExit(f"no career statistics found in {args.iso}")
    written = disc_patches.write_patches("player_stats", patches, args.out)
    with open(args.report, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["team", "position", "player", "overall", "nflverse player", "seasons"])
        for i in sorted(range(play.count), key=lambda i: -play.get(i, "POVR")):
            if not (1 <= play.get(i, "TGID") <= 32 or play.get(i, "TGID") == 1009):
                continue
            pid = found.get(i)
            w.writerow([play.get(i, "TGID"), POSITION_NAMES[play.get(i, "PPOS")], f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}",
                        play.get(i, "POVR"), info[pid][0] if pid else "", len(seasons[pid]) if pid else 0])
    on_teams = sum(1 for i in range(play.count) if 1 <= play.get(i, "TGID") <= 32)
    matched = sum(1 for i in found if 1 <= play.get(i, "TGID") <= 32)
    free = sum(1 for i in range(play.count) if play.get(i, "TGID") == 1009)
    print(f"{matched} of {on_teams} team players and {sum(1 for i in found if play.get(i, 'TGID') == 1009)} of {free} "
          f"free agents matched to nflverse; rows written: {counts}")
    print(f"{written} tables written to {args.out}; report in {args.report}")


if __name__ == "__main__":
    main()
