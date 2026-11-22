"""Gives HC27's careers a 17-game regular season.

    python tools/roster_import/schedule17.py --iso "iso/NFL Head Coach 09.iso"

A career's first season is played to one of two schedules the disc holds (the SCSA and SCHD tables, 332 games
each: four preseason weeks, a 17-week regular season of 16 games a team, and the playoff games to be filled in).
This makes each a 17-game season in 18 weeks with the same number of rows: the fourth preseason week's 16 games
become a regular season week 18 in which every team plays a team of the other conference that it does not meet
otherwise, and the playoff weeks come a week later. The changed tables are written to hc27/disc (see
disc_patches.py).
"""

import argparse
import os
import sys

import disc_patches
from tdb import SaveFile

TABLES = ("SCSA", "SCHD")


def conferences():
    """TEAM.TGID -> TEAM.CGID, from HC27's roster."""
    team = SaveFile(os.path.join(disc_patches.ROOT, "hc27", "ROSTER")).db.tables["TEAM"]
    return {team.get(i, "TGID"): team.get(i, "CGID") for i in range(team.count) if 1 <= team.get(i, "TGID") <= 32}


def lengthen(t, conference):
    """One schedule table: False if it is not a 16-game schedule (already changed, or another kind of table)."""
    changed = False
    for template in sorted(set(t.get(i, "SESI") for i in range(t.count))):
        rows = [i for i in range(t.count) if t.get(i, "SESI") == template]
        regular = [i for i in rows if t.get(i, "SEST") == 0 and t.get(i, "SEWT") == 1]
        spare = [i for i in rows if t.get(i, "SEST") == 9 and t.get(i, "SEWN") == 3]
        if len(regular) != 256 or len(spare) != 16 or max(t.get(i, "SEWN") for i in regular) != 16:
            continue
        meets = set()
        for i in regular:
            a, h = t.get(i, "GATG"), t.get(i, "GHTG")
            meets.add((a, h))
            meets.add((h, a))
        groups = sorted(set(conference.values()))
        one = sorted(g for g, c in conference.items() if c == groups[0])
        other = sorted(g for g, c in conference.items() if c == groups[1])
        # A perfect matching between the conferences among teams that do not meet (augmenting paths).
        match = {}

        def place(a, seen):
            for b in other:
                if (a, b) in meets or b in seen:
                    continue
                seen.add(b)
                if b not in match or place(match[b], seen):
                    match[b] = a
                    return True
            return False

        for a in one:
            if not place(a, set()):
                raise SystemExit(f"no seventeenth opponent for team {a}")
        pairs = sorted((a, b) for b, a in match.items())
        for k, (i, (a, b)) in enumerate(zip(spare, pairs)):
            away, home = (a, b) if (k + template) % 2 == 0 else (b, a)
            for name, value in (("SEST", 0), ("SEWT", 1), ("SEWN", 17), ("SGNM", k), ("GATG", away), ("GHTG", home),
                                ("GTOD", 780), ("GDow", 3)):
                t.set(i, name, value)
        for i in rows:
            if t.get(i, "SEST") == 0 and t.get(i, "SEWT") >= 2:
                t.set(i, "SEWN", t.get(i, "SEWN") + 1)
        changed = True
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True)
    args = parser.parse_args()
    conference = conferences()
    patches = []
    for path, offset, db in disc_patches.databases(args.iso, TABLES):
        changed = [name for name in TABLES if name in db.tables and db.tables[name].count >= 332 and lengthen(db.tables[name], conference)]
        if changed:
            patches.append((path, offset, db, changed))
            print(f"{os.path.basename(path)} @ {offset:#x}: {', '.join(changed)}")
    print(disc_patches.write_patches("schedule17", patches), "files written to", disc_patches.OUT)


if __name__ == "__main__":
    sys.exit(main())
