"""Two fixes for rosters built by import_roster.py: the 2008 players left in free agency, and
teams with nobody at left outside linebacker or fullback.

    python tools/roster_import/roster_fixes.py --roster hc27/ROSTER --out hc27/ROSTER

import_roster.py applies both itself (see its --keep-2008-players); this command applies them to a
roster that was built without them.

**2008 players.** Records the import doesn't reuse keep their 2008 players, and they stay in free
agency with their 2008 ratings: Shaun Alexander 89, Ty Law 89, Junior Seau, Trent Dilfer 77, the
2008 draft class (whose overall is stored as 0 but whose ratings are real), 294 of them rated 70
or more. When a career starts, the game fills every AI roster to 75 from free agency, so about 290
of them join teams and 40-50 start (in tests, Trent Dilfer started for Miami). camp_level() lowers
each to a camp player (overall 40-52, fixed per player id; no skill rating above 8 over it) with no
room to grow, so they still fill camp rosters but are cut or sit. Durability ratings, names, ids and
looks are left alone. In a built roster they are found as free agents without a contract (the
import, with --contracts, zeroes theirs; released 2026 players keep theirs).

**Empty LOLB and FB.** Madden lists outside linebackers and fullbacks differently, so 20 teams come
out of the import with no left outside linebacker and 18 with no fullback, and the game fills those
spots from free agency, i.e. with the players above. fill_empty_positions() moves one backup to the
empty position, before the depth charts are built: for LOLB the best ROLB or MLB other than that
position's starter (from whichever group is larger), for FB the best run-blocking TE other than the
starter (or a third HB when the team has fewer than three TEs).
"""

import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

FREE_AGENTS = 1009
LOLB, MLB, ROLB, FB, HB, TE = 13, 14, 15, 2, 1, 4

# Every rating with its ceiling (the import's RATINGS, plus PLRN).
RATING_PAIRS = [
    ("PSPD", "PSDX"), ("PSTR", "PSTX"), ("PAWR", "PAWX"), ("PAGI", "PAGX"), ("PACC", "PACX"),
    ("PCAR", "PCAX"), ("PCTH", "PCTX"), ("PJMP", "PJMX"), ("PTAK", "PTKX"), ("PTHP", "PTPX"),
    ("PTHA", "PTAX"), ("PKPR", "PKPX"), ("PKAC", "PKAX"), ("PKRT", "PKRX"), ("PSTA", "PSAX"),
    ("PINJ", "PINX"), ("PTGH", "PTGX"), ("PELU", "PELX"), ("PBCV", "PBCX"), ("PLTR", "PLTX"),
    ("PLSA", "PLSX"), ("PLSM", "PSMx"), ("PLJM", "PLJX"), ("PLRR", "PRRX"), ("PLRL", "PRLX"),
    ("PLCI", "PLCX"), ("PLSC", "PSCX"), ("PLIB", "PIBX"), ("PPBK", "PPBX"), ("PPBF", "PPFX"),
    ("PPBS", "PPSX"), ("PRBK", "PRBX"), ("PRBF", "PRFX"), ("PRBS", "PRSX"), ("PBSG", "PBSX"),
    ("PFMS", "PFMX"), ("PLPm", "PPMX"), ("PLHT", "PLHX"), ("PLMC", "PLMX"), ("PLZC", "PLZX"),
    ("PLPE", "PPEX"), ("PLPR", "PPRX"), ("PLPU", "PPUX"), ("PLRN", "PLRX"),
]
DURABILITY = {"PSTA", "PINJ", "PTGH"}
CONTRACT = ["PCSA", "PTSA", "PVTS", "PVSB", "PSBO", "PCON", "PCYL"]


def leftover_records(play):
    """The 2008 players in a built roster: free agents with no contract."""
    return [i for i in range(play.count)
            if play.get(i, "TGID") == FREE_AGENTS and all(play.get(i, f) == 0 for f in CONTRACT)]


def camp_level(play, records):
    """Lower the given players to camp level (overall 40-52) with no room to grow."""
    for i in records:
        pgid = play.get(i, "PGID")
        target = 40 + hashlib.md5(str(pgid).encode()).digest()[0] % 13
        drop = max(0, play.get(i, "POVR") - target)
        top = target + 8
        for rating, cap in RATING_PAIRS:
            value = play.get(i, rating)
            if rating not in DURABILITY:
                value = max(min(value, 20), min(value - drop, top))
                play.set(i, rating, value)
            play.set(i, cap, value)
        play.set(i, "POVR", target)
        play.set(i, "PIMP", max(5, min(99, round((target - 45) * 1.6))))
        play.set(i, "PFPB", 0)
    return len(records)


def fill_empty_positions(play, records_by_team):
    """Move one backup to LOLB / FB on each team with nobody there. Returns the moves made."""
    moves = []
    for team, records in sorted(records_by_team.items()):
        def at(position):
            return sorted((i for i in records if play.get(i, "PPOS") == position),
                          key=lambda i: -play.get(i, "POVR"))

        def move(i, position):
            moves.append((team, play.get(i, "PFNA"), play.get(i, "PLNA"), play.get(i, "PPOS"), position))
            play.set(i, "PPOS", position)

        if not at(LOLB):
            for group in sorted((ROLB, MLB), key=lambda p: -len(at(p))):
                backups = at(group)[1:]
                if backups:
                    move(backups[0], LOLB)
                    break
        if not at(FB):
            tes, hbs = at(TE), at(HB)
            pools = [tes[1:], hbs[2:]] if len(tes) >= 3 else [hbs[2:], tes[1:]]
            for pool in pools:
                if pool:
                    move(max(pool, key=lambda i: (play.get(i, "PRBK"), play.get(i, "POVR"))), FB)
                    break
    return moves


def main():
    from import_roster import POSITION_NAMES, build_depth_charts, rate_teams

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    save = SaveFile(args.roster)
    db = save.db
    play = db.tables["PLAY"]
    lowered = camp_level(play, leftover_records(play))
    records_by_team = {}
    for i in range(play.count):
        t = play.get(i, "TGID")
        if 1 <= t <= 32:
            records_by_team.setdefault(t, []).append(i)
    moves = fill_empty_positions(play, records_by_team)
    build_depth_charts(play, db.tables["DCHT"], records_by_team)
    rate_teams(play, db.tables["TEAM"])
    save.fix_checksums()
    save.save(args.out)
    print(f"{lowered} 2008 players lowered to camp level; {len(moves)} players moved to empty positions; wrote {args.out}")
    for team, first, last, before, after in moves:
        print(f"  team {team}: {first} {last} {POSITION_NAMES[before]} -> {POSITION_NAMES[after]}")


if __name__ == "__main__":
    main()
