"""Brings an HC27 roster up to date with the league's transactions since it was made.

The transactions are NFL.com's (https://www.nfl.com/transactions/league/<category>/<year>/<month>,
categories signings, waivers, trades, terminations), saved as a CSV with the columns category, date
(YYYY/MM/DD), from, to (team slugs such as chicago-bears), name and text. Each player's latest
transaction after --since decides where he is now:

    signed, claimed or traded to a team   on that team
    released or waived (and not since)    a free agent

A player is found in the roster by name: on the team he left when the transaction names it, else
by name alone when only one rostered player has it. Players not in the roster (signed from outside
the league, or not in the ratings the roster was built from) are listed but cannot be added: the
roster has no ratings for them. Depth charts and team ratings are rebuilt for every team that changed.

    python tools/roster_import/roster_moves.py --roster hc27/ROSTER --transactions tx.csv \\
        --since 2026/09/08 --out tools/roster_import/data/ROSTER-M27_week8 --report moves.csv
"""

import argparse
import csv
import os
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from import_roster import FREE_AGENTS, build_depth_charts, rate_teams  # noqa: E402
from tdb import SaveFile  # noqa: E402

TEAM_SLUGS = {
    "chicago-bears": 1, "cincinnati-bengals": 2, "buffalo-bills": 3, "denver-broncos": 4, "cleveland-browns": 5,
    "tampa-bay-buccaneers": 6, "arizona-cardinals": 7, "los-angeles-chargers": 8, "kansas-city-chiefs": 9,
    "indianapolis-colts": 10, "dallas-cowboys": 11, "miami-dolphins": 12, "philadelphia-eagles": 13,
    "atlanta-falcons": 14, "san-francisco-49ers": 15, "new-york-giants": 16, "jacksonville-jaguars": 17,
    "new-york-jets": 18, "detroit-lions": 19, "green-bay-packers": 20, "carolina-panthers": 21,
    "new-england-patriots": 22, "las-vegas-raiders": 23, "los-angeles-rams": 24, "baltimore-ravens": 25,
    "washington-commanders": 26, "new-orleans-saints": 27, "seattle-seahawks": 28, "pittsburgh-steelers": 29,
    "tennessee-titans": 30, "minnesota-vikings": 31, "houston-texans": 32,
}
SUFFIXES = {"jr", "sr", "ii", "iii", "iv", "v"}


def name_key(name):
    """Lower case, no accents, punctuation or suffix: "Ja'Marr Chase Jr." -> "jamarr chase"."""
    text = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode().lower()
    words = ["".join(c for c in w if c.isalnum()) for w in text.replace("-", " ").split()]
    words = [w for w in words if w and w not in SUFFIXES]
    return " ".join(words)


def destination(row):
    """The team the transaction puts the player on (FREE_AGENTS when it lets him go), or None when
    it does not move him (reserve lists, exemptions, injured players waived to injured reserve)."""
    text, category = row["text"].lower(), row["category"]
    to = TEAM_SLUGS.get(row["to"])
    if "injured" in text and "settlement" not in text:
        return None  # "Waived, Injured": he reverts to the team's injured reserve
    if text.startswith(("released", "waived", "terminated")):
        return FREE_AGENTS
    if category in ("signings", "waivers", "trades") and to:
        return to
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", required=True)
    parser.add_argument("--transactions", required=True)
    parser.add_argument("--since", required=True, help="first date to apply (YYYY/MM/DD)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--report", help="CSV of every change and every player not found")
    parser.add_argument("--no-releases", action="store_true", help="keep released players on their teams")
    args = parser.parse_args()

    save = SaveFile(args.roster)
    db = save.db
    play, team_table, dcht = db.tables["PLAY"], db.tables["TEAM"], db.tables["DCHT"]
    names = {team_table.get(i, "TGID"): team_table.get(i, "TDNA") for i in range(team_table.count)}
    names[FREE_AGENTS] = "Free Agents"

    by_name = {}
    for i in range(play.count):
        if 1 <= play.get(i, "TGID") <= 32:
            by_name.setdefault(name_key(f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}"), []).append(i)

    rows = [r for r in csv.DictReader(open(args.transactions, encoding="utf-8")) if r["date"] >= args.since]
    # Within a day: releases, then trades and waiver claims, then signings (a player let go and
    # signed elsewhere the same day).
    rows.sort(key=lambda r: (r["date"], 0 if destination(r) == FREE_AGENTS else 2 if r["category"] == "signings" else 1))
    # Each player's team as the transactions go by. NFL.com tells namesakes apart by their profile
    # (slug): the first transaction that finds a roster player ties him to its slug, and from then
    # on only that slug's transactions apply to him. To find him, a trade or release must name the
    # team he is on; a waiver claim the team that waived him (he is a free agent by then); a
    # signing must find him a free agent, already on that team, or on a practice squad (from where
    # any team may sign him). Anything else is reported, not applied.
    team = {i: play.get(i, "TGID") for records in by_name.values() for i in records}
    practice = {}  # record -> on a practice squad
    slug_of, record_of = {}, {}
    latest = {}  # record -> (row, destination)
    missing, conflicts = {}, []
    for row in rows:
        where = destination(row)
        if where is None or (where == FREE_AGENTS and args.no_releases):
            continue
        slug = row["slug"]
        if slug in record_of:
            candidates = [record_of[slug]]
        else:
            candidates = [i for i in by_name.get(name_key(row["name"]), []) if i not in slug_of]
        frm = TEAM_SLUGS.get(row["from"])
        claim = row["category"] == "waivers" and where != FREE_AGENTS
        if claim:
            chosen = [i for i in candidates if team[i] in (frm, FREE_AGENTS)]
        elif frm:
            chosen = [i for i in candidates if team[i] == frm]
        else:
            chosen = [i for i in candidates if team[i] in (FREE_AGENTS, where) or practice.get(i)]
        if len(chosen) != 1:
            if not by_name.get(name_key(row["name"])) and where != FREE_AGENTS:
                missing.setdefault(row["name"], (row, 0))
            elif candidates and where != FREE_AGENTS and not any(team[i] == where for i in candidates):
                conflicts.append((row, [names.get(team[i]) for i in candidates]))
            continue
        i = chosen[0]
        slug_of[i], record_of[slug] = slug, i
        team[i] = where
        practice[i] = "practice squad" in row["text"].lower()
        latest[i] = (row, where)

    changes = []
    touched = set()
    for i, (row, where) in latest.items():
        before = play.get(i, "TGID")
        if where == before:
            continue
        changes.append((play.get(i, "PFNA"), play.get(i, "PLNA"), names.get(before), names.get(where), row["date"], row["text"]))
        play.set(i, "TGID", where)
        touched |= {before, where}

    records_by_team = {}
    for i in range(play.count):
        t = play.get(i, "TGID")
        if 1 <= t <= 32:
            records_by_team.setdefault(t, []).append(i)
    build_depth_charts(play, dcht, records_by_team)
    rate_teams(play, team_table)
    save.fix_checksums()
    save.save(args.out)

    moved = sum(1 for c in changes if c[3] != "Free Agents")
    print(f"{moved} players moved to new teams, {len(changes) - moved} released, "
          f"{len(missing)} signed players not in the roster, {len(conflicts)} signings of a namesake on another team skipped")
    for row, where in conflicts:
        print(f"  skipped: {row['date']} {row['name']} {row['text']} -> {row['to']} (roster has him on {', '.join(where)})")
    for first, last, before, after, date, text in sorted(changes, key=lambda c: c[4]):
        if after != "Free Agents":
            print(f"  {date} {first} {last}: {before} -> {after} ({text})")
    if args.report:
        with open(args.report, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["first", "last", "from", "to", "date", "transaction"])
            w.writerows(sorted(changes, key=lambda c: c[4]))
            for name, (row, count) in sorted(missing.items()):
                w.writerow(["", name, row["from"], row["to"], row["date"],
                            f"{row['text']} (not in the roster)" if not count else f"{row['text']} ({count} players of that name)"])
    counts = {names[t]: len(r) for t, r in sorted(records_by_team.items()) if t in touched}
    print("roster sizes now:", ", ".join(f"{k} {v}" for k, v in counts.items()))


if __name__ == "__main__":
    main()
