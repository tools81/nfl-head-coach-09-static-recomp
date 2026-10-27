"""Gives HC27's teams the real record of the season before the career starts (2025).

    python tools/roster_import/last_season.py --iso "iso/NFL Head Coach 09.iso"

The game keeps fifteen past seasons of every team in its career database (the OTRS table: SEYR the
season, -1 the last; TSRW wins, TSRO losses, TSRI ties, TSRL how it ended: 0 won the Super Bowl,
1 lost it, 2 out earlier in the playoffs, 3 no playoffs). New Career's Team Select shows the last
one ("Last season: 13-3-0" for the 2007 Colts). This replaces season -1 with RECORDS and writes
the table to hc27/disc (see disc_patches.py); the older seasons are left as they are.
"""

import argparse

import disc_patches

# Team (TEAM.TDNA of the 2008 roster, as the career database knows them) -> 2025 wins, losses,
# ties and how the season ended.
WON_SUPER_BOWL, LOST_SUPER_BOWL, PLAYOFFS, NO_PLAYOFFS = 0, 1, 2, 3
RECORDS = {
    "Patriots": (14, 3, 0, LOST_SUPER_BOWL), "Bills": (12, 5, 0, PLAYOFFS), "Dolphins": (7, 10, 0, NO_PLAYOFFS),
    "Jets": (3, 14, 0, NO_PLAYOFFS),
    "Steelers": (10, 7, 0, PLAYOFFS), "Ravens": (8, 9, 0, NO_PLAYOFFS), "Bengals": (6, 11, 0, NO_PLAYOFFS),
    "Browns": (5, 12, 0, NO_PLAYOFFS),
    "Jaguars": (13, 4, 0, PLAYOFFS), "Texans": (12, 5, 0, PLAYOFFS), "Colts": (8, 9, 0, NO_PLAYOFFS),
    "Titans": (3, 14, 0, NO_PLAYOFFS),
    "Broncos": (14, 3, 0, PLAYOFFS), "Chargers": (11, 6, 0, PLAYOFFS), "Chiefs": (6, 11, 0, NO_PLAYOFFS),
    "Raiders": (3, 14, 0, NO_PLAYOFFS),
    "Eagles": (11, 6, 0, PLAYOFFS), "Cowboys": (7, 9, 1, NO_PLAYOFFS), "Redskins": (5, 12, 0, NO_PLAYOFFS),
    "Giants": (4, 13, 0, NO_PLAYOFFS),
    "Bears": (11, 6, 0, PLAYOFFS), "Packers": (9, 7, 1, PLAYOFFS), "Vikings": (9, 8, 0, NO_PLAYOFFS),
    "Lions": (9, 8, 0, NO_PLAYOFFS),
    "Panthers": (8, 9, 0, PLAYOFFS), "Buccaneers": (8, 9, 0, NO_PLAYOFFS), "Falcons": (8, 9, 0, NO_PLAYOFFS),
    "Saints": (6, 11, 0, NO_PLAYOFFS),
    "Seahawks": (14, 3, 0, WON_SUPER_BOWL), "Rams": (12, 5, 0, PLAYOFFS), "49ers": (12, 5, 0, PLAYOFFS),
    "Cardinals": (3, 14, 0, NO_PLAYOFFS),
}
# TEAM.TGID of the 2008 roster (the career database has no team names of its own).
TEAM_IDS = {
    "Bears": 1, "Bengals": 2, "Bills": 3, "Broncos": 4, "Browns": 5, "Buccaneers": 6, "Cardinals": 7, "Chargers": 8,
    "Chiefs": 9, "Colts": 10, "Cowboys": 11, "Dolphins": 12, "Eagles": 13, "Falcons": 14, "49ers": 15, "Giants": 16,
    "Jaguars": 17, "Jets": 18, "Lions": 19, "Packers": 20, "Panthers": 21, "Patriots": 22, "Raiders": 23, "Rams": 24,
    "Ravens": 25, "Redskins": 26, "Saints": 27, "Seahawks": 28, "Steelers": 29, "Titans": 30, "Vikings": 31, "Texans": 32,
}


def update(db):
    t = db.tables.get("OTRS")
    if not t or not t.count:
        return []
    records = {TEAM_IDS[name]: record for name, record in RECORDS.items()}
    for i in range(t.count):
        if t.get(i, "SEYR") == -1 and t.get(i, "TGID") in records:
            wins, losses, ties, ending = records[t.get(i, "TGID")]
            t.set(i, "TSRW", wins)
            t.set(i, "TSRO", losses)
            t.set(i, "TSRI", ties)
            t.set(i, "TSRL", ending)
    return ["OTRS"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--out", default=disc_patches.OUT)
    args = parser.parse_args()
    found = disc_patches.databases(args.iso, ["OTRS"])
    if not found:
        raise SystemExit(f"no team history found in {args.iso}")
    written = disc_patches.write_patches("last_season", [(path, offset, db, update(db)) for path, offset, db in found], args.out)
    print(f"2025 records written to {written} tables in {args.out}")


if __name__ == "__main__":
    main()
