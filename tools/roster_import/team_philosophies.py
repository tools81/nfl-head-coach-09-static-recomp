"""Sets each team's position philosophies to fit its depth chart.

A team rates its players by its philosophy at each position (Workhorse HB, Pass Block Specialist
Tackle, ...): the head coach's (COCH, the PP fields of the head coach's record) and the general
manager's (GMVW). The game's player evaluation weighs a player's ratings differently for each
philosophy: the PEIW (skills) and PEAW (physical) tables on the disc give, for each of the 81
philosophies (GMPP, the EGPP enum below), a weight per rating. The 2026 coaches took over the
2008 coaches' records, philosophies included, so under them a backup can rate above the starter
the depth chart (built from the players' overall ratings) puts first: the Bears' Workhorse HB rates
Kyle Monangai above D'Andre Swift, where Balanced HB puts Swift first.

For each team and philosophy this scores the team's players at the position as the game weighs
them (a weighted average of their ratings) and picks the philosophy under which the starters (the
top of each depth chart: three WRs, two DTs and two CBs, one elsewhere) rate highest against the
rest. Of those, it takes the one under which the players rate closest to their Madden overalls
(from the import reports, players.csv and free_agents.csv): the philosophies differ by much more
than taste. Mobile/Speed QB, which weighs speed and arm over awareness and accuracy, rates
backups 10 to 15 points above their Madden overall (Kurtis Rourke, 63, at 78), while Field General
QB keeps them within a few points. Then the one that best keeps the depth charts' order, then the
current one. The head coach and the general manager both get it. Kickers and punters, usually one
a team, keep theirs.

A career takes its head coaches from the roster but its general managers from the career database
on the disc, which has the 2008 GMs' philosophies, and in a career a team rates its players by the
GM's: the 49ers' 2008 Strong Arm/Tools QB, which values size, put the 6'3" Mac Jones above Brock
Purdy. So the GMs' philosophies are also written to the disc's databases, as patches in hc27/disc
(see disc_patches.py).

The weighted average is close to the game's evaluation but not the same: the game also counts
things such as height and the condition of each body part. The game's own scores (the overall a
career's roster screens show) are saved with each career: when it begins, it evaluates every player
under each of the 81 philosophies. With --career, a career save begun from this roster (any team),
the choice uses those scores instead.

    python tools/roster_import/team_philosophies.py --iso "iso/NFL Head Coach 09.iso"   # hc27/ROSTER, in place
    python tools/roster_import/team_philosophies.py --iso "iso/NFL Head Coach 09.iso" --career CAREER-BEARS

--report writes every team's choice and the starters' ranks (default
tools/roster_import/data/philosophies.csv). Run it again after the depth charts are rebuilt
(roster_moves.py). A career keeps the philosophies it began with.
"""

import argparse
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disc_patches import databases, write_patches  # noqa: E402
from import_roster import POSITION_NAMES  # noqa: E402
from tdb import Database, SaveFile  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DATA = os.path.join(ROOT, "tools", "roster_import", "data")
TEAMS = 32
HEAD_COACH = 0  # COCH.COPS

# The philosophies (GMPP), as the disc's EGPP enum names them.
PHILOSOPHIES = [
    "Mobile/Speed QB", "Strong Arm/Tools QB", "Pure Passer QB", "Field General QB", "Balanced QB",
    "Speed HB", "Moves HB", "Power HB", "Receiving HB", "Workhorse HB", "Balanced HB",
    "Ball Carrier FB", "Blocking FB", "Receiving FB", "Balanced FB",
    "Strong/Blocking WR", "Tall/Redzone Threat WR", "Speed WR", "Quick/Route Runner WR", "Balanced WR",
    "Soft Hands/Routes TE", "Blocking TE", "Playmaker TE", "H-back TE", "Balanced TE",
    "Size/Strength/Engulfer Tackle", "Run Block Specialist Tackle", "Pass Block Specialist Tackle",
    "Zone Blocking Tackle", "Balanced Tackle",
    "Size/Strength/Mauler Guard", "Run Block Specialist Guard", "Pass Block Specialist Guard", "Zone Blocking Guard",
    "Balanced Guard",
    "Size/Strength/Mauler Center", "Run Block Specialist Center", "Pass Block Specialist Center",
    "Zone Blocking Center", "Balanced Center",
    "Balanced DE", "Prototype DE", "Undersized Speed Rusher DE", "Run Stopper DE", "Versatile/3-4 DE",
    "Balanced DT", "Prototype DT", "One Gap Penetrator DT", "Run Stopper DT", "Versatile/3-4 DT",
    "Balanced OLB", "Coverage/Cover 2 OLB", "Run Stopper OLB", "PassRusher/3-4 Blitzer OLB", "Prototype OLB",
    "Balanced MLB", "Coverage/Speed MLB", "Power/Blitzer MLB", "3-4/Tackler MLB", "Prototype MLB",
    "Balanced CB", "Man Coverage/Ballhawk CB", "Strong/Press/Run Support CB", "Prototype CB", "Cover 2 CB",
    "Balanced FS", "Man Coverage FS", "Centerfielder FS", "Prototype FS", "Run Support/Blitzer FS",
    "Balanced SS", "Coverage SS", "Prototype SS", "Run Support/Big Hitter SS", "Smart/Productive SS",
    "Big Foot/Distance K", "Pinpoint Accuracy K", "Balanced K",
    "Big Foot/Distance P", "Pinpoint Accuracy P", "Balanced P",
]

# Philosophy field -> (its philosophies, the depth chart positions (PPOS) it rates, starters per chart).
GROUPS = {
    "QBPP": (range(0, 5), [0], 1), "HBPP": (range(5, 11), [1], 1), "FBPP": (range(11, 15), [2], 1),
    "WRPP": (range(15, 20), [3], 3), "TEPP": (range(20, 25), [4], 1), "TKPP": (range(25, 30), [5, 9], 1),
    "GDPP": (range(30, 35), [6, 8], 1), "CRPP": (range(35, 40), [7], 1), "DEPP": (range(40, 45), [10, 11], 1),
    "DTPP": (range(45, 50), [12], 2), "OLPP": (range(50, 55), [13, 15], 1), "MLPP": (range(55, 60), [14], 1),
    "CBPP": (range(60, 65), [16], 2), "FSPP": (range(65, 70), [17], 1), "SsPP": (range(70, 75), [18], 1),
    "KKPP": (range(75, 78), [19], 1), "KPPP": (range(78, 81), [20], 1),
}

# How much of the starters' lead over the rest (the share of starter-backup pairs the starter wins)
# a philosophy may give up to be closer to the players' Madden overalls.
SLACK = 0.0

# Evaluation weight field (PEIW, PEAW) -> the rating it weighs (PLAY).
WEIGHT_RATINGS = {
    "THAW": "PTHA", "STAW": "PSTA", "IMBW": "PLIB", "RNBW": "PRBK", "KACW": "PKAC", "TACW": "PTAK", "SPCW": "PLSC",
    "PBFW": "PPBF", "RBFW": "PRBF", "PRGW": "PLPR", "TGHW": "PTGH", "INJW": "PINJ", "PBKW": "PPBK", "TRKW": "PLTR",
    "RELW": "PLRL", "STMW": "PLSA", "PWMW": "PLPm", "LRNW": "PLRN", "IMPW": "PIMP", "MORW": "PMOR", "RORW": "PLRR",
    "PURW": "PLPU", "AWRW": "PAWR", "BKSW": "PBSG", "CATW": "PCTH", "CITW": "PLCI", "KRTW": "PKRT", "BCVW": "PBCV",
    "MCVW": "PLMC", "ZCVW": "PLZC", "CARw": "PCAR", "PRSw": "PLPE",
    "ACCW": "PACC", "SPDW": "PSPD", "AGLW": "PAGI", "FIMW": "PFMS", "SPMW": "PLSM", "THPW": "PTHP", "JMPW": "PJMP",
    "STRW": "PSTR", "RBSW": "PRBS", "ELUW": "PELU", "JMVW": "PLJM", "HPWW": "PLHT", "KPWW": "PKPR", "PBSw": "PPBS",
}


def read_weights(iso_path):
    """GMPP -> {rating: weight}, from the disc's evaluation tables."""
    weights, done = {}, set()
    for _, _, db in databases(iso_path, ["PEIW", "PEAW"]):
        for name in ("PEIW", "PEAW"):
            table = db.tables.get(name)
            if name in done or not table or not table.count:
                continue
            done.add(name)
            for i in range(table.count):
                w = weights.setdefault(table.get(i, "GMPP"), {})
                for field in table.fields:
                    value = table.get(i, field.name) if field.name in WEIGHT_RATINGS else 0
                    if value:
                        w[WEIGHT_RATINGS[field.name]] = w.get(WEIGHT_RATINGS[field.name], 0) + value
    if done != {"PEIW", "PEAW"}:
        raise SystemExit(f"{iso_path}: the player evaluation tables (PEIW, PEAW) were not found")
    return weights


def madden_overalls(paths):
    """{PGID: Madden overall}, from the import reports (players.csv, free_agents.csv)."""
    overalls = {}
    for path in paths:
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                overalls.update((int(r["pgid"]), int(r["overall"])) for r in csv.DictReader(f) if r.get("overall"))
    return overalls


def career_scores(path):
    """GMPP -> {PGID: the game's score}, from a career save's evaluation lists: one a philosophy, in
    GMPP order, each a u16 count and that many records of a u16 PGID and 8 bytes, the score last
    (the bytes before it are the parts it is weighed from)."""
    data = open(path, "rb").read()
    play = Database(bytearray(data), data.find(b"DB\x00\x08\x01")).tables["PLAY"]
    known = {play.get(i, "PGID") for i in range(play.count)}

    def lists_at(at):
        found = []
        while len(found) < len(PHILOSOPHIES) and at + 12 <= len(data):
            count = struct.unpack_from(">H", data, at)[0]
            records = [data[at + 2 + 10 * k:at + 12 + 10 * k] for k in range(count)]
            if not count or len(records[-1]) < 10 or any(struct.unpack_from(">H", r)[0] not in known for r in records):
                break
            found.append({struct.unpack_from(">H", r)[0]: r[9] for r in records})
            at += 2 + 10 * count
        return found

    for at in range(len(data) - 14):
        count, first, second = struct.unpack_from(">HH8xH", data, at)
        if 10 <= count <= 1000 and first in known and second in known:
            found = lists_at(at)
            if len(found) == len(PHILOSOPHIES):
                return dict(enumerate(found))
    raise SystemExit(f"{path}: no player evaluations found (is it a career save?)")


def evaluate(play, index, weights):
    total = sum(weights.values())
    return sum(play.get(index, rating) * w for rating, w in weights.items()) / total if total else 0.0


def fit(scores, charts, starters_per_chart):
    """(starters rated above the others, depth chart pairs in order): higher is a better fit."""
    starters = {p for chart in charts for p in chart[:starters_per_chart]}
    others = [p for p in scores if p not in starters]
    above = sum(1.0 if scores[s] > scores[o] else 0.5 if scores[s] == scores[o] else 0.0 for s in starters for o in others)
    ordered = sum(1 for chart in charts for a in range(len(chart)) for b in range(a + 1, len(chart))
                  if scores[chart[a]] >= scores[chart[b]])
    return above, ordered


def gap(scores, madden, play):
    """How far the scores are from the players' Madden overalls, on average."""
    known = [abs(scores[i] - madden[play.get(i, "PGID")]) for i in scores if play.get(i, "PGID") in madden]
    return sum(known) / len(known) if known else 0.0


def choose(save, weights, exact=None, madden=None, slack=SLACK):
    """Sets the head coaches' and general managers' philosophies; returns the report rows and
    {team: {field: philosophy}}. exact: career_scores, used instead of the weights. madden: {PGID:
    Madden overall}; of the philosophies within slack of the best fit, the one closest to it."""
    tables = save.db.tables
    play, depth, coach, gm = tables["PLAY"], tables["DCHT"], tables["COCH"], tables["GMVW"]
    team_name = {tables["TEAM"].get(i, "TGID"): tables["TEAM"].get(i, "TDNA") for i in range(tables["TEAM"].count)}
    players = {}
    for i in range(play.count):
        if 1 <= play.get(i, "TGID") <= TEAMS:
            players.setdefault((play.get(i, "TGID"), play.get(i, "PPOS")), []).append(i)
    index_of = {play.get(i, "PGID"): i for i in range(play.count)}
    charts = {}
    for i in range(depth.count):
        charts.setdefault((depth.get(i, "TGID"), depth.get(i, "PPOS")), []).append((depth.get(i, "ddep"), depth.get(i, "PGID")))
    head = {coach.get(i, "TGID"): i for i in range(coach.count) if coach.get(i, "COPS") == HEAD_COACH}
    manager = {gm.get(i, "TGID"): i for i in range(gm.count)}
    rows, chosen = [], {}

    def scored(pool, g):
        if exact is not None:
            return {i: exact[g].get(play.get(i, "PGID"), 0) for i in pool}
        return {i: evaluate(play, i, weights.get(g, {})) for i in pool}

    for team in range(1, TEAMS + 1):
        if team not in head:
            continue
        for field, (options, positions, starters) in GROUPS.items():
            pool = [i for p in positions for i in players.get((team, p), [])]
            team_charts = [[index_of[g] for _, g in sorted(charts.get((team, p), [])) if g in index_of and index_of[g] in pool]
                           for p in positions]
            current = coach.get(head[team], field)
            if len(pool) < 2 or not any(team_charts):
                best = current
            else:
                ranked = []
                for g in options:
                    scores = scored(pool, g)
                    above, ordered = fit(scores, team_charts, starters)
                    ranked.append((above / max(1, starters * (len(pool) - starters)), ordered, g == current, g,
                                   gap(scores, madden or {}, play)))
                top = max(r[0] for r in ranked)
                best = max(ranked, key=lambda r: (r[0] >= top - slack, -r[4] if madden else 0, r[:4]))[3]
            coach.set(head[team], field, best)
            if team in manager:
                gm.set(manager[team], field, best)
            chosen.setdefault(team, {})[field] = best
            scores = scored(pool, best)
            order = sorted(pool, key=lambda i: -scores[i])
            names = []
            for chart in team_charts:
                for i in chart[:starters]:
                    names.append(f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')} ({order.index(i) + 1} of {len(pool)})")
            rows.append([team_name.get(team, team), "/".join(POSITION_NAMES[p] for p in positions),
                         PHILOSOPHIES[current] if current < len(PHILOSOPHIES) else current, PHILOSOPHIES[best],
                         "; ".join(names), f"{gap(scores, madden or {}, play):.1f}" if madden else ""])
    return rows, chosen


def disc_managers(iso_path, chosen):
    """The disc's general managers get the chosen philosophies: write_patches' list."""
    patches = []
    for path, offset, db in databases(iso_path, ["GMVW"]):
        gm = db.tables["GMVW"]
        for i in range(gm.count):
            for field, value in chosen.get(gm.get(i, "TGID"), {}).items():
                gm.set(i, field, value)
        patches.append((path, offset, db, {"GMVW"}))
    return patches


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image (the player evaluation tables)")
    parser.add_argument("--roster", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--out", help="where to write the roster (default: in place)")
    parser.add_argument("--report", default=os.path.join(DATA, "philosophies.csv"))
    parser.add_argument("--career", help="a career save begun from this roster: choose by the game's own scores")
    parser.add_argument("--players", nargs="*", default=[os.path.join(DATA, "players.csv"), os.path.join(DATA, "free_agents.csv")],
                        help="the import reports with the players' Madden overalls (pgid, overall)")
    args = parser.parse_args()
    save = SaveFile(args.roster)
    rows, chosen = choose(save, read_weights(args.iso), career_scores(args.career) if args.career else None,
                          madden_overalls(args.players))
    save.fix_checksums()
    save.save(args.out or args.roster)
    written = write_patches("team_philosophies", disc_managers(args.iso, chosen))
    print(f"{written} general manager tables written to hc27/disc")
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["team", "position", "was", "now", "starters (rank under the new philosophy)",
                    "average distance from the Madden overalls"])
        w.writerows(rows)
    changed = sum(1 for r in rows if r[2] != r[3])
    print(f"{changed} of {len(rows)} team philosophies changed; report in {args.report}")


if __name__ == "__main__":
    main()
