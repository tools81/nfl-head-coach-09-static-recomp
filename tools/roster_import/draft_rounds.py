"""The draft round and pick of the players still on their rookie contracts (PDRO, PDPI).

The imported players keep the draft round and pick of the 2008 players whose records they took
over, which HC27 careers need for the fifth-year option of first-round picks
(src/common/fifth_year_option.cpp). A drafted player's contract is set by his pick, so ranking a
draft class's four-year rookie contracts by value gives back the order they were picked in: the
largest is the first pick, the 33rd largest the first of the second round. The classes are the
players with up to three years in the league (the 2023 to 2026 drafts; a 2023 first-round pick
whose option was exercised has five years). A deal's value is its total as signed, from the
contracts CSV (otc_contracts.py), since a player traded since then carries only part of it in the
roster; a player missing from the CSV is ranked by the roster's. Rounds count 32 picks each, which
is exact for the first round (the one the option needs, short of first-round picks no longer on
their rookie deals) and close for the later ones, which also have compensatory picks.

    python tools/roster_import/draft_rounds.py                   # hc27/ROSTER, in place
    python tools/roster_import/draft_rounds.py --save <career>   # a career save, in place

(--contracts names the CSV, by default tools/roster_import/data/contracts.csv.)

import_roster.py runs it on the roster it builds.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEAMS = 32
ROUNDS = 7
LAST_TEAM = 32  # team ids 1-32; free agents and others are higher


def set_draft_rounds(play, totals=None):
    """Sets PDRO and PDPI of the players on rookie contracts; returns how many by class. `totals`
    maps a player's name ("First Last") to his contract's total value in dollars as signed."""
    totals = totals or {}

    def value(i):
        name = f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}"
        return totals.get(name, play.get(i, "PTSA") * 10_000)

    classes = {}
    for i in range(play.count):
        years = play.get(i, "PYRP")
        length = play.get(i, "PCON")
        if years > 3 or play.get(i, "TGID") > LAST_TEAM:
            continue
        # Four years for every pick, signed as a rookie (as many years run as he has played); a
        # first-round pick's fifth year once his option is exercised.
        if (length == 4 or (length == 5 and years == 3)) and length - play.get(i, "PCYL") == years:
            classes.setdefault(years, []).append(i)
    counts = {}
    for years, players in sorted(classes.items()):
        players.sort(key=lambda i: (-value(i), -play.get(i, "PSBO")))
        for rank, i in enumerate(players):
            play.set(i, "PDRO", min(ROUNDS, rank // TEAMS + 1))
            play.set(i, "PDPI", rank % TEAMS + 1 if rank < ROUNDS * TEAMS else TEAMS)
        counts[years] = len(players)
    return counts


def contract_totals(path, play):
    """Name -> total value of the four- or five-year contract the CSV lists under that name (one
    only), for the players of `play`."""
    import csv
    from import_roster import name_key

    by_key = {}
    with open(path, encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            if row["length"] in ("4", "5"):
                by_key.setdefault(name_key(row["name"]), []).append(int(row["total"]))
    totals = {}
    for i in range(play.count):
        name = f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}"
        found = by_key.get(name_key(name), [])
        if len(found) == 1:
            totals[name] = found[0]
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--save", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--contracts", default=os.path.join(ROOT, "tools", "roster_import", "data", "contracts.csv"))
    args = parser.parse_args()
    save = SaveFile(args.save)
    play = save.db.tables["PLAY"]
    counts = set_draft_rounds(play, contract_totals(args.contracts, play))
    save.fix_checksums()
    save.save(args.save)
    print(f"{args.save}: rookie contracts by years in the league {counts}")


if __name__ == "__main__":
    main()
