"""Builds an NFL Head Coach 09 roster save from a Madden ratings CSV.

    python tools/roster_import/import_roster.py --csv "Madden 27 Ratings.csv" \
        --roster ROSTER-DEFAULT --out ROSTER-M27 [--report players.csv]

The CSV is the export of EA's Madden ratings site (one row per player: names, team/label,
position/id, physicals, stats/<rating>/value, avatarUrl). The roster is a roster save made in
HC09 (My Coach > rosters); it is used as the template, so fields the CSV has no data for
(appearance, equipment, contract, college, personality) come from the HC09 player whose record
an imported player takes over.

Every player on the 32 teams is replaced: an imported player reuses a record of the same team
(same position first), then records from the free-agent pool; team records left over become free
agents. Record counts and table layouts never change, so only the save container's checksums
are recomputed. The output's player ids (PGID) are those of the reused records; --report writes
the mapping, including each player's portrait id (PSXP, inherited from the reused record), which
fetch_portraits.py uses to name the replacement images.
"""

import argparse
import csv
import datetime
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

FREE_AGENTS = 1009
NO_PLAYER = 32767

POSITIONS = {  # Madden position id -> HC09 PPOS (None: no HC09 equivalent, skipped)
    "QB": 0, "HB": 1, "FB": 2, "WR": 3, "TE": 4, "LT": 5, "LG": 6, "C": 7, "RG": 8, "RT": 9,
    "LE": 10, "LEDG": 10, "RE": 11, "REDG": 11, "DT": 12, "LOLB": 13, "SAM": 13, "MLB": 14, "MIKE": 14,
    "ROLB": 15, "WILL": 15, "CB": 16, "FS": 17, "SS": 18, "K": 19, "P": 20, "LS": None,
}
POSITION_NAMES = ["QB", "HB", "FB", "WR", "TE", "LT", "LG", "C", "RG", "RT", "LE", "RE", "DT",
                  "LOLB", "MLB", "ROLB", "CB", "FS", "SS", "K", "P"]

# Team names that changed since 2008 (CSV label -> HC09 TEAM.TDNA); others match by nickname.
TEAM_ALIASES = {"Las Vegas Raiders": "Raiders", "Los Angeles Chargers": "Chargers",
                "Los Angeles Rams": "Rams", "Washington Commanders": "Redskins",
                "NY Giants": "Giants", "NY Jets": "Jets"}
# Teams renamed in the roster (HC09 TEAM.TDNA -> new name, also the TMNC short name). The game
# names a team's files after TDNA (its uniforms, helmets, career campus), so HC27 opens the old
# name's files for the new name (kRenamedTeams in src/common/edition.cpp, which must list the
# same teams).
TEAM_RENAMES = {"Redskins": "Commanders"}

# HC09 rating -> (its progression ceiling field, how to compute it from the CSV's stats)
def avg(*names):
    return lambda s: round(sum(s[n] for n in names) / len(names))


def stat(name):
    return lambda s: s[name]


RATINGS = {
    "POVR": (None, stat("overall")),
    "PSPD": ("PSDX", stat("speed")),
    "PSTR": ("PSTX", stat("strength")),
    "PAWR": ("PAWX", stat("awareness")),
    "PAGI": ("PAGX", stat("agility")),
    "PACC": ("PACX", stat("acceleration")),
    "PCAR": ("PCAX", stat("carrying")),
    "PCTH": ("PCTX", stat("catching")),
    "PJMP": ("PJMX", stat("jumping")),
    "PTAK": ("PTKX", stat("tackle")),
    "PTHP": ("PTPX", stat("throwPower")),
    "PTHA": ("PTAX", lambda s: round(0.4 * s["throwAccuracyShort"] + 0.35 * s["throwAccuracyMid"]
                                     + 0.25 * s["throwAccuracyDeep"])),
    "PKPR": ("PKPX", stat("kickPower")),
    "PKAC": ("PKAX", stat("kickAccuracy")),
    "PKRT": ("PKRX", stat("kickReturn")),
    "PSTA": ("PSAX", stat("stamina")),
    "PINJ": ("PINX", stat("injury")),
    "PTGH": ("PTGX", stat("toughness")),
    "PELU": ("PELX", avg("jukeMove", "spinMove", "changeOfDirection")),
    "PBCV": ("PBCX", stat("bCVision")),
    "PLTR": ("PLTX", stat("trucking")),
    "PLSA": ("PLSX", stat("stiffArm")),
    "PLSM": ("PSMx", stat("spinMove")),
    "PLJM": ("PLJX", stat("jukeMove")),
    "PLRR": ("PRRX", avg("shortRouteRunning", "mediumRouteRunning", "deepRouteRunning")),
    "PLRL": ("PRLX", stat("release")),
    "PLCI": ("PLCX", stat("catchInTraffic")),
    "PLSC": ("PSCX", stat("spectacularCatch")),
    "PLIB": ("PIBX", stat("impactBlocking")),
    "PPBK": ("PPBX", stat("passBlock")),
    "PPBF": ("PPFX", stat("passBlockFinesse")),
    "PPBS": ("PPSX", stat("passBlockPower")),
    "PRBK": ("PRBX", stat("runBlock")),
    "PRBF": ("PRFX", stat("runBlockFinesse")),
    "PRBS": ("PRSX", stat("runBlockPower")),
    "PBSG": ("PBSX", stat("blockShedding")),
    "PFMS": ("PFMX", stat("finesseMoves")),
    "PLPm": ("PPMX", stat("powerMoves")),
    "PLHT": ("PLHX", stat("hitPower")),
    "PLMC": ("PLMX", stat("manCoverage")),
    "PLZC": ("PLZX", stat("zoneCoverage")),
    "PLPE": ("PPEX", stat("press")),
    "PLPR": ("PPRX", stat("playRecognition")),
    "PLPU": ("PPUX", stat("pursuit")),
}
# A contract: cap hit, total and signing bonus (in $10,000s), length, years left, and the per-year
# salary (PSA) and bonus (PSB) schedule.
CONTRACT_FIELDS = ["PCSA", "PTSA", "PVTS", "PVSB", "PSBO", "PCON", "PCYL"] + \
    [f"PS{k}{y}" for y in range(7) for k in "AB"]

SLOTS = 7  # contract years the game stores (PSA0-6, PSB0-6)
NAME_SUFFIXES = {"jr", "sr", "ii", "iii", "iv", "v"}


def name_key(name):
    words = "".join(c for c in name.lower().replace("-", " ") if c.isalnum() or c == " ").split()
    return " ".join(w for w in words if w not in NAME_SUFFIXES)


# Nicknames the ratings use for players whose contracts are under their given names.
NICKNAMES = {"sauce": "ahmad", "bam": "zonovan", "kiko": "francisco", "juju": "julius"}


def first_names_fit(short, full):
    """Whether two first names can be the same person's: the same initial, one inside the other
    (Drew, Andrew), initials (C.J.) or a known nickname."""
    return (short[0] == full[0] or short in full or full in short or len(short) <= 2
            or NICKNAMES.get(short) == full)


def load_contracts(path):
    """Contracts from otc_contracts.py's CSV: (name key, team nickname) -> deal, and name key ->
    deals (to find players the two sources place on different teams). A deal has its length and the
    (cap hit, prorated bonus) of each season still to run, the current season first."""
    by_team, by_name = {}, {}
    with open(path, encoding="utf-8-sig", newline="") as f:
        rows = csv.DictReader(f)
        seasons = sorted({int(c[4:]) for c in rows.fieldnames if c.startswith("cap_")})
        for row in rows:
            years = [(int(row[f"cap_{s}"]), int(row[f"bonus_{s}"] or 0)) for s in seasons if row[f"cap_{s}"]]
            if not years:
                continue
            deal = {"length": int(row["length"]), "years": years}
            key = name_key(row["name"])
            by_team[(key, row["team"].lower())] = deal
            by_name.setdefault(key, []).append(deal)
    return by_team, by_name


def write_contract(play, index, deal):
    """A contract in the game: slot y of PSA (salary) and PSB (prorated signing bonus) is year y of
    the deal, in $10,000s, and their sum is that year's cap hit; PCON is the length, PCYL the years
    left including this one, so this season is slot PCON - PCYL. Years already played are not in
    the sources and repeat the first one left."""
    years = deal["years"][:SLOTS]
    left = len(years)
    length = clamp(deal["length"], left, SLOTS)
    years = [years[0]] * (length - left) + years
    salaries = [clamp(round((cap - bonus) / 10000), 0, 16383) for cap, bonus in years]
    bonuses = [clamp(round(bonus / 10000), 0, 8191) for _, bonus in years]
    for y in range(SLOTS):
        play.set(index, f"PSA{y}", salaries[y] if y < length else 0)
        play.set(index, f"PSB{y}", bonuses[y] if y < length else 0)
    bonus = sum(bonuses)
    total = clamp(sum(salaries) + bonus, 0, 65535)
    now = length - left
    for field, value in (("PCON", length), ("PCYL", left), ("PCSA", min(16383, salaries[now] + bonuses[now])),
                         ("PSBO", min(bonus, 8191)), ("PVSB", min(bonus, 16383)), ("PTSA", total), ("PVTS", total)):
        play.set(index, field, value)


def minimum_deal(years_pro):
    """A player without a contract in the sources: the league minimum for his experience, for three
    years as a rookie (an undrafted free agent's deal) and one otherwise."""
    from league_salaries import minimum_salary
    length = 3 if years_pro == 0 else 1
    return {"length": length, "years": [(minimum_salary(years_pro + y), 0) for y in range(length)]}


# Ratings that keep improving with experience; the rest are mostly physical.
MENTAL = {"PAWR", "PLPR", "PLRR", "PTHA", "PBCV", "PLMC", "PLZC", "PPBK", "PRBK", "PLRL"}


def ceiling(field, value, age):
    years = max(0, 27 - age)
    growth = years * (2.0 if field in MENTAL else 0.75)
    return min(99, round(value + growth))


def clamp(value, low, high):
    return max(low, min(high, value))


def load_players(path):
    players = []
    with open(path, encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            position = POSITIONS.get(row["position/id"], "unknown")
            if position is None:
                continue
            if position == "unknown":
                print(f"skipping {row['firstName']} {row['lastName']}: unknown position {row['position/id']}")
                continue
            stats = {}
            for key, value in row.items():
                if key.startswith("stats/") and key.endswith("/value"):
                    try:
                        stats[key[6:-6]] = int(value)
                    except ValueError:
                        pass
            players.append({
                "id": row["id"], "first": row["firstName"], "last": row["lastName"],
                "team": row["team/label"], "position": position,
                "birth": datetime.date.fromisoformat(row["birthdate"][:10]) if row["birthdate"] else None,
                "height": int(row["height"]), "weight": int(row["weight"]), "age": int(row["age"]),
                "jersey": int(row["jerseyNum"] or 0), "years": int(row["yearsPro"] or 0),
                "right_handed": row["handedness"] != "0", "college": row["college"],
                "avatar": row.get("avatarUrl", ""), "stats": stats,
            })
    return players


def write_player(play, index, p, team):
    s = p["stats"]
    play.set(index, "PFNA", p["first"])
    play.set(index, "PLNA", p["last"])
    play.set(index, "TGID", team)
    play.set(index, "PPOS", p["position"])
    play.set(index, "POPS", p["position"])
    play.set(index, "PJEN", clamp(p["jersey"], 0, 99))
    play.set(index, "PHGT", clamp(p["height"], 0, 127))
    play.set(index, "PWGT", clamp(p["weight"] - 160, 0, 255))
    play.set(index, "PAGE", clamp(p["age"], 18, 63))
    play.set(index, "PYRP", clamp(p["years"], 0, 31))
    play.set(index, "PHAN", 0 if p["right_handed"] else 1)
    if p["birth"]:
        play.set(index, "PBDA", p["birth"].day)
        play.set(index, "PBMO", p["birth"].month)
    play.set(index, "PHTN", "")
    play.set(index, "PFPB", 1 if s["overall"] >= 90 else 0)  # Pro Bowl badge
    for field, (twin, compute) in RATINGS.items():
        value = clamp(compute(s), 0, 99)
        play.set(index, field, value)
        if twin:
            play.set(index, twin, ceiling(field, value, p["age"]))
    play.set(index, "PIMP", clamp(round((s["overall"] - 45) * 1.6), 5, 99))
    # HC09-only traits the CSV has no counterpart for. Left alone they would be those of the 2008
    # player whose record this was, and PLRN in particular (football IQ, which the game's displayed
    # overall leans on heavily) swings that overall by 20 points. PLRN follows awareness in the
    # 2008 roster (least-squares fit: 11.9 + 0.86 x awareness, mean error 7); the rest take the
    # 2008 roster's median (morale, approval, PCEL, and full confidence).
    learning = clamp(round(11.9 + 0.86 * s["awareness"]), 1, 99)
    play.set(index, "PLRN", learning)
    play.set(index, "PLRX", ceiling("PAWR", learning, p["age"]))
    for field, value in (("PMOR", 91), ("HDAp", 85), ("PCEL", 77), ("PCAF", 100), ("PSEF", 100), ("PCUF", 100)):
        play.set(index, field, value)


def clear_draft_class(db):
    """DRPP lists the 2008 draft class by player id (with team, pick and round). A career started
    in the preseason takes some of those players off their teams as draft picks still to be
    signed, and one started in the off-season puts the class back in the draft. The ids now belong
    to the imported players who took over those records, mostly veterans under contract, so every
    row is pointed at no player (32767, the id the game's own tables use for none)."""
    drpp = db.tables["DRPP"]
    for i in range(drpp.count):
        drpp.set(i, "PGID", NO_PLAYER)


# The team ratings Team Select and the career screens show (TEAM table): position groups from the
# team's best players at each position (position code, how many), then offense and defense from
# the groups and overall from those and special teams.
TEAM_GROUPS = {"TRQB": [(0, 1)], "TRRB": [(1, 1)], "TWRR": [(3, 3), (4, 1)],
               "TROL": [(5, 1), (6, 1), (7, 1), (8, 1), (9, 1)], "TRDL": [(10, 1), (11, 1), (12, 2)],
               "TRLB": [(13, 1), (14, 1), (15, 1)], "TRDB": [(16, 2), (17, 1), (18, 1)], "TRST": [(19, 1), (20, 1)]}
TEAM_OFFENSE = {"TRQB": 0.35, "TWRR": 0.25, "TROL": 0.25, "TRRB": 0.15}
TEAM_DEFENSE = {"TRDL": 0.35, "TRLB": 0.25, "TRDB": 0.40}


def rate_teams(play, team_table):
    """Rewrites the 32 teams' ratings from their players. The 2008 roster's ratings were set by
    hand; the new ones keep their range (the best team gets the 2008 best value, the worst the
    2008 worst) with the teams ordered by their players' overall ratings."""
    by_team = {}
    for i in range(play.count):
        by_team.setdefault(play.get(i, "TGID"), {}).setdefault(play.get(i, "PPOS"), []).append(play.get(i, "POVR"))
    rows = [i for i in range(team_table.count) if 1 <= team_table.get(i, "TGID") <= 32]
    raw = {}
    for i in rows:
        players = by_team.get(team_table.get(i, "TGID"), {})
        r = {}
        for group, spec in TEAM_GROUPS.items():
            values = [v for position, n in spec for v in sorted(players.get(position, []), reverse=True)[:n]]
            r[group] = sum(values) / len(values) if values else 0
        r["TROF"] = sum(r[g] * w for g, w in TEAM_OFFENSE.items())
        r["TRDE"] = sum(r[g] * w for g, w in TEAM_DEFENSE.items())
        r["TROV"] = 0.45 * r["TROF"] + 0.45 * r["TRDE"] + 0.10 * r["TRST"]
        raw[i] = r
    for field in list(TEAM_GROUPS) + ["TROF", "TRDE", "TROV"]:
        old = [team_table.get(i, field) for i in rows]
        new = [raw[i][field] for i in rows]
        low, high = min(new), max(new)
        for i in rows:
            share = (raw[i][field] - low) / (high - low) if high > low else 0.5
            team_table.set(i, field, round(min(old) + share * (max(old) - min(old))))


def build_depth_charts(play, dcht, records_by_team):
    """Rewrites DCHT in place: every rostered player at his position, best first, plus the
    special-teams slots (21 KR, 22 PR, 23 kickoff, 24 long snapper, 25 third-down back).
    The table keeps its record count: lower slots are trimmed or the deepest positions padded
    with repeats until the total matches."""
    def rating(i, field):
        return play.get(i, field)

    charts = []
    for team, records in sorted(records_by_team.items()):
        entries = []
        by_position = {}
        for i in records:
            by_position.setdefault(play.get(i, "PPOS"), []).append(i)
        for position in range(21):
            ranked = sorted(by_position.get(position, []), key=lambda i: -rating(i, "POVR"))
            entries += [(position, depth, play.get(i, "PGID")) for depth, i in enumerate(ranked)]
        returners = sorted((i for i in records if play.get(i, "PPOS") in (1, 3, 16, 17, 18)),
                           key=lambda i: -(rating(i, "PKRT") * 2 + rating(i, "PSPD")))
        kickers = sorted(by_position.get(19, []), key=lambda i: -rating(i, "POVR"))
        centers = sorted(by_position.get(7, []), key=lambda i: rating(i, "POVR"))  # backup snaps
        backs = sorted(by_position.get(1, []), key=lambda i: -(rating(i, "PCTH") + rating(i, "PPBK")))
        special = [(21, returners[:2]), (22, returners[:1]), (23, kickers[:1]), (24, centers[:1]), (25, backs[:1])]
        for position, chosen in special:
            entries += [(position, depth, play.get(i, "PGID")) for depth, i in enumerate(chosen)]
        charts.append((team, entries))

    total = sum(len(e) for _, e in charts)
    # Trim the deepest slots first (never starters) until the totals match.
    while total > dcht.count:
        team, entries = max(charts, key=lambda c: max(d for _, d, _ in c[1]))
        deepest = max(range(len(entries)), key=lambda k: entries[k][1])
        entries.pop(deepest)
        total -= 1
    rows = [(team, pos, depth, pgid) for team, entries in charts for pos, depth, pgid in entries]
    if len(rows) < dcht.count:
        # Pad by repeating teams' backup kickoff entries (harmless duplicates the game re-sorts).
        filler = [r for r in rows if r[1] == 23]
        rows += [filler[k % len(filler)] for k in range(dcht.count - len(rows))]
    for k, (team, pos, depth, pgid) in enumerate(rows):
        dcht.set(k, "TGID", team)
        dcht.set(k, "PPOS", pos)
        dcht.set(k, "ddep", depth)
        dcht.set(k, "PGID", pgid)
        dcht.set(k, "DCLK", 7)


def assign_portrait_ids(play, imported, without_image=()):
    """Gives every imported player with an image a portrait id (PSXP) of his own, so a replacement
    image can be keyed by it; players without one get the generic silhouette. Records keep the id they had where it is unique; the rest take ids of 2008
    portraits nobody imported uses (those images exist in the game's portrait library). Players
    not imported who still share an id with an imported one get the generic silhouette."""
    counts = {}
    for i in range(play.count):
        counts[play.get(i, "PSXP")] = counts.get(play.get(i, "PSXP"), 0) + 1
    generic = max((k for k in counts if k), key=lambda k: counts[k])
    imported_set = set(imported)
    for i in without_image:
        play.set(i, "PSXP", generic)
    taken = set()
    pending = []
    for i in imported:
        pid = play.get(i, "PSXP")
        if pid and pid != generic and counts[pid] == 1 and pid not in taken:
            taken.add(pid)
        else:
            pending.append(i)
    spare = sorted(k for k, n in counts.items() if k and k != generic and n == 1 and k not in taken)
    # Prefer ids whose only owner is not being imported (a single real 2008 player's photo).
    for i in pending:
        if not spare:
            raise SystemExit("ran out of portrait ids")
        pid = spare.pop(0)
        play.set(i, "PSXP", pid)
        taken.add(pid)
    for i in range(play.count):
        if i not in imported_set and play.get(i, "PSXP") in taken:
            play.set(i, "PSXP", generic)


def import_roster(csv_path, roster_path, out_path, report_path=None, contracts_path=None):
    save = SaveFile(roster_path)
    db = save.db
    play, team_table, dcht = db.tables["PLAY"], db.tables["TEAM"], db.tables["DCHT"]

    teams = {}
    for i in range(team_table.count):
        teams[team_table.get(i, "TDNA")] = team_table.get(i, "TGID")

    def team_id(label):
        name = TEAM_ALIASES.get(label)
        if name is None:
            name = next((n for n in teams if label.endswith(" " + n)), None)
        if name not in teams or teams[name] > 32:
            raise SystemExit(f"cannot match team {label!r}")
        return teams[name]

    players = load_players(csv_path)
    by_team = {}
    for p in players:
        by_team.setdefault(team_id(p["team"]), []).append(p)

    records = {}
    free = []
    for i in range(play.count):
        t = play.get(i, "TGID")
        if 1 <= t <= 32:
            records.setdefault(t, []).append(i)
        elif t == FREE_AGENTS:
            free.append(i)

    assignments = []  # (record, player, team)
    leftovers = []
    for t in sorted(by_team):
        incoming = sorted(by_team[t], key=lambda p: -p["stats"]["overall"])
        available = list(records.get(t, []))
        chosen = {}
        # Same position first, so equipment and build fit; then any record of the team.
        for p in incoming:
            match = next((i for i in available if play.get(i, "PPOS") == p["position"]), None)
            if match is not None:
                available.remove(match)
                chosen[id(p)] = match
        for p in incoming:
            if id(p) in chosen:
                continue
            if available:
                chosen[id(p)] = available.pop(0)
            elif free:
                chosen[id(p)] = free.pop(0)
            else:
                raise SystemExit("not enough player records for the imported roster")
        leftovers += available
        assignments += [(chosen[id(p)], p, t) for p in incoming]

    # Contracts: without --contracts, each team's 2008 contracts are dealt out again by rank (best
    # player, biggest cap hit), so every team stays under the 2008 cap. Read them before any record
    # changes.
    deals = {}
    for t, team_records in records.items():
        bundles = [{f: play.get(i, f) for f in CONTRACT_FIELDS} for i in team_records]
        deals[t] = sorted(bundles, key=lambda b: -b["PCSA"])

    for i in leftovers:
        play.set(i, "TGID", FREE_AGENTS)
    for i, p, t in assignments:
        write_player(play, i, p, t)
    if contracts_path:
        # Real contracts: by name on the player's team, else by name alone when only one player
        # has it; players without one get a minimum deal.
        by_team_deals, by_name_deals = load_contracts(contracts_path)
        nickname = {}
        for i in range(team_table.count):
            name = team_table.get(i, "TDNA")
            nickname[team_table.get(i, "TGID")] = TEAM_RENAMES.get(name, name).lower()
        team_names = {}
        for key, team in by_team_deals:
            team_names.setdefault(team, []).append(key)
        found = 0
        for i, p, t in assignments:
            key = name_key(f"{p['first']} {p['last']}")
            deal = by_team_deals.get((key, nickname[t]))
            if deal is None and len(by_name_deals.get(key, [])) == 1:
                deal = by_name_deals[key][0]
            if deal is None:
                # A short or nickname first name (Cam Heyward, Drew Ogletree, C.J. Okoye, Sauce
                # Gardner): the one player of that last name on the team whose first name fits.
                namesakes = [k for k in team_names.get(nickname[t], ())
                             if k.split()[-1] == key.split()[-1] and first_names_fit(key.split()[0], k.split()[0])]
                if len(namesakes) == 1:
                    deal = by_team_deals[(namesakes[0], nickname[t])]
            found += deal is not None
            write_contract(play, i, deal or minimum_deal(p["years"]))
        print(f"contracts: {found} players matched, {len(assignments) - found} on minimum deals")
        for i in leftovers:  # the 2008 players left as free agents, unsigned like the game's own
            for field in CONTRACT_FIELDS:
                play.set(i, field, 0)
    else:
        for t in sorted(by_team):
            ranked = sorted((a for a in assignments if a[2] == t), key=lambda a: -a[1]["stats"]["overall"])
            for (i, p, _), bundle in zip(ranked, deals.get(t, [])):
                for field, value in bundle.items():
                    play.set(i, field, value)
                # Rookies keep at most a rookie-length deal.
                if p["years"] == 0:
                    play.set(i, "PCON", min(bundle["PCON"], 4))
                    play.set(i, "PCYL", min(bundle["PCYL"], 4))

    assign_portrait_ids(play, [i for i, p, _ in assignments if p["avatar"]],
                        [i for i, p, _ in assignments if not p["avatar"]])

    roster = {}
    for i, _, t in assignments:
        roster.setdefault(t, []).append(i)
    build_depth_charts(play, dcht, roster)

    clear_draft_class(db)
    rate_teams(play, team_table)

    for i in range(team_table.count):
        new_name = TEAM_RENAMES.get(team_table.get(i, "TDNA"))
        if new_name:
            team_table.set(i, "TDNA", new_name)
            team_table.set(i, "TMNC", new_name)

    save.fix_checksums()
    save.save(out_path)
    print(f"{len(assignments)} players imported into {len(roster)} teams; "
          f"{len(leftovers)} original players moved to free agency; wrote {out_path}")

    if report_path:
        with open(report_path, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f)
            w.writerow(["pgid", "portrait_id", "madden_id", "first", "last", "team", "position", "overall", "avatarUrl"])
            for i, p, t in assignments:
                w.writerow([play.get(i, "PGID"), play.get(i, "PSXP"), p["id"], p["first"], p["last"], t,
                            POSITION_NAMES[p["position"]], p["stats"]["overall"], p["avatar"]])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--roster", required=True, help="an HC09 roster save to use as the template")
    parser.add_argument("--out", required=True)
    parser.add_argument("--report", help="write the imported players with their HC09 player ids")
    parser.add_argument("--contracts", help="real contracts: the CSV otc_contracts.py writes")
    args = parser.parse_args()
    import_roster(args.csv, args.roster, args.out, args.report, args.contracts)
