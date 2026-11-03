"""Real ownership of the 2027 and 2028 draft picks for HC27 careers (trades as of week 8 of 2026).

A career keeps the picks of the next two drafts in its DRPK table: one record per pick, DPYO 0 the
nearer draft and 1 the one after, DPOD the team the pick originally belongs to, DPID the team that
holds it, DPNM its number in the draft. A new career takes the table from a template on the disc,
which holds the 2008 and 2009 drafts with that year's trades: an off-season start keeps both (the
2026 and 2027 drafts in HC27), a pre-season start drops the first, so the second becomes the next
draft, and makes the one after (2028) with every team holding its own picks. So:

    python tools/roster_import/career_picks.py --iso "iso/NFL Head Coach 09.iso"

writes the template to hc27/disc (see disc_patches.py) with no trades in its first draft (2026) and
the real ones in its second (2027), and hc27/draft_picks.txt, the trades HC27 applies to the 2028
picks when a career saves in its first season (src/common/draft_picks.cpp). An existing career
saved in its first season can be given both drafts' trades directly:

    python tools/roster_import/career_picks.py --career build/userdata-hc27/content/00000001/CAREER-<name>/CAREER-<name>

Each trade "A B" moves a pick of that round from A to B: A's own pick while A holds it, else one A
acquired (or the named original team's in a chain). Trades are listed in the order they were made,
so a pick traded on ends up with the last team. Conditional picks are counted as traded. Sources:
Wikipedia's 2027 and 2028 NFL draft pages.
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

TEAM_IDS = {
    "CHI": 1, "CIN": 2, "BUF": 3, "DEN": 4, "CLE": 5, "TB": 6, "ARI": 7, "LAC": 8, "KC": 9, "IND": 10,
    "DAL": 11, "MIA": 12, "PHI": 13, "ATL": 14, "SF": 15, "NYG": 16, "JAX": 17, "NYJ": 18, "DET": 19,
    "GB": 20, "CAR": 21, "NE": 22, "LV": 23, "LAR": 24, "BAL": 25, "WAS": 26, "NO": 27, "SEA": 28,
    "PIT": 29, "TEN": 30, "MIN": 31, "HOU": 32,
}

# (draft, round, from, to[, original team when a chain names it]), oldest trade first within each round.
TRADES = [
    # 2027
    (2027, 1, "GB", "DAL"),    # Micah Parsons
    (2027, 1, "IND", "NYJ"),   # Sauce Gardner
    (2027, 1, "DAL", "NYJ"),   # Quinnen Williams: the earlier of Dallas' and Green Bay's (Dallas' own, by the
                               # standings at week 8; Dallas keeps Green Bay's)
    (2027, 1, "LAR", "CLE"),   # Myles Garrett
    (2027, 1, "CHI", "TEN"),   # Jeffery Simmons
    (2027, 2, "HOU", "ARI"),   # Marvin Harrison Jr.
    (2027, 3, "LAR", "KC"),    # Trent McDuffie
    (2027, 3, "PHI", "MIN"),   # Jonathan Greenard
    (2027, 3, "DAL", "PIT"),   # Broderick Jones
    (2027, 3, "SEA", "IND"),   # Quenton Nelson
    (2027, 4, "MIN", "CAR"),   # Adam Thielen
    (2027, 4, "DAL", "GB"),    # Rashan Gary
    (2027, 4, "NYG", "CLE"),
    (2027, 4, "SEA", "CLE"),
    (2027, 4, "PIT", "DAL"),   # Broderick Jones
    (2027, 4, "SF", "IND"),    # DeForest Buckner
    (2027, 4, "CHI", "TEN"),   # Jeffery Simmons
    (2027, 5, "HOU", "CLE"),
    (2027, 5, "DAL", "PIT"),   # George Pickens
    (2027, 5, "PIT", "MIA"),   # Minkah Fitzpatrick
    (2027, 5, "CAR", "MIN"),   # Adam Thielen
    (2027, 5, "CHI", "NE"),    # Garrett Bradbury
    (2027, 5, "NE", "PHI"),    # A. J. Brown
    (2027, 5, "NYG", "MIN"),   # J. J. McCarthy
    (2027, 5, "BAL", "PHI"),   # Cam Jurgens
    (2027, 6, "PIT", "DAL"),   # George Pickens
    (2027, 6, "SF", "KC"),     # Skyy Moore
    (2027, 6, "NYJ", "MIN"),   # Harrison Phillips
    (2027, 6, "GB", "PHI"),    # Darian Kinnard
    (2027, 6, "NO", "NE"),     # Ja'Lynn Polk
    (2027, 6, "CLE", "HOU"),   # Cam Robinson
    (2027, 6, "PHI", "NYJ"),   # Michael Carter II
    (2027, 6, "LAC", "NO"),    # Trevor Penning
    (2027, 6, "KC", "NYJ"),    # Justin Fields
    (2027, 6, "PHI", "GB"),    # Dontayvion Wicks
    (2027, 6, "BAL", "SF"),
    (2027, 6, "MIN", "NE"),
    (2027, 6, "ATL", "KC"),    # Wanya Morris
    (2027, 6, "JAX", "NYG"),   # Daniel Faalele
    (2027, 6, "DAL", "PIT"),   # Broderick Jones
    (2027, 6, "DAL", "PIT"),   # Joey Porter Jr. (conditional)
    (2027, 6, "ARI", "HOU"),   # Marvin Harrison Jr.
    (2027, 7, "LAR", "BAL"),   # Tre'Davious White
    (2027, 7, "BAL", "LAC", "LAR"),   # Odafe Oweh: the Rams' pick
    (2027, 7, "MIA", "PIT"),   # Minkah Fitzpatrick
    (2027, 7, "NO", "DEN"),    # Devaughn Vele
    (2027, 7, "KC", "SF"),     # Skyy Moore
    (2027, 7, "MIN", "NYJ"),   # Harrison Phillips
    (2027, 7, "PHI", "MIN"),   # Sam Howell
    (2027, 7, "LAC", "HOU"),   # Austin Deculus (conditional)
    (2027, 7, "ATL", "SEA"),   # Michael Jerrell (conditional)
    (2027, 7, "NYG", "MIA"),   # Darren Waller (conditional)
    (2027, 7, "HOU", "CLE"),   # Cam Robinson
    (2027, 7, "NYJ", "PHI"),   # Michael Carter II
    (2027, 7, "BAL", "PHI"),   # Jaire Alexander
    (2027, 7, "HOU", "DET"),   # David Montgomery
    (2027, 7, "PHI", "CAR"),   # Andy Dalton
    (2027, 7, "DAL", "PHI"),
    (2027, 7, "LV", "BUF"),
    (2027, 7, "KC", "ATL"),    # Wanya Morris
    (2027, 7, "SF", "HOU"),    # Jarrett Patterson
    (2027, 7, "HOU", "ARI", "SF"),    # Marvin Harrison Jr.: San Francisco's pick
    (2027, 7, "PHI", "BAL"),   # Cam Jurgens
    # 2028
    (2028, 1, "NE", "PHI"),    # A. J. Brown
    (2028, 1, "CHI", "TEN"),   # Jeffery Simmons
    (2028, 2, "LAR", "CLE"),   # Myles Garrett
    (2028, 2, "DAL", "PIT"),   # Joey Porter Jr.
    (2028, 2, "BAL", "PHI"),   # Cam Jurgens
    (2028, 5, "SEA", "IND"),   # Quenton Nelson
    (2028, 5, "TEN", "CHI"),   # Jeffery Simmons
    (2028, 6, "NO", "DAL"),    # Asim Richards
    (2028, 6, "JAX", "MIA"),   # Quinn Ewers
    (2028, 7, "NE", "NO"),     # Ja'Lynn Polk
    (2028, 7, "CLE", "LAR"),   # KT Leveston
    (2028, 7, "DAL", "NO"),    # Asim Richards
    (2028, 7, "NYJ", "LAC"),   # Ja'Sir Taylor (conditional)
    (2028, 7, "SEA", "NYJ"),   # Irvin Charles (conditional)
    (2028, 7, "HOU", "NE"),    # Kayshon Boutte
]

HOOK_DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "hc27", "draft_picks.txt")


def apply(t, first_draft):
    """Applies TRADES to DRPK table t whose DPYO 0 is the `first_draft`, after giving every team its
    own picks."""
    rounds = {}  # (draft, round) -> {original team: record}
    for i in range(t.count):
        draft = first_draft + t.get(i, "DPYO")
        rnd = t.get(i, "DPNM") // 32 + 1
        rounds.setdefault((draft, rnd), {})[t.get(i, "DPOD")] = i
        t.set(i, "DPID", t.get(i, "DPOD"))
    names = {v: k for k, v in TEAM_IDS.items()}
    moved, skipped = 0, []
    for draft, rnd, frm, to, *original in TRADES:
        if (draft, rnd) not in rounds:
            continue
        picks = rounds[(draft, rnd)]
        a, b = TEAM_IDS[frm], TEAM_IDS[to]
        own = TEAM_IDS[original[0]] if original else a
        record = picks.get(own) if picks.get(own) is not None and t.get(picks[own], "DPID") == a else None
        if record is None:  # a pick A acquired
            record = next((i for i in picks.values() if t.get(i, "DPID") == a), None)
        if record is None:
            skipped.append(f"{draft} round {rnd} {frm} -> {to}")
            continue
        t.set(record, "DPID", b)
        moved += 1
    held = {}
    for (draft, rnd), picks in sorted(rounds.items()):
        for orig, i in picks.items():
            if t.get(i, "DPID") != orig:
                held.setdefault((draft, names[t.get(i, "DPID")]), []).append(f"R{rnd} ({names[orig]})")
    return moved, skipped, held


def report(moved, skipped, held):
    print(f"{moved} picks moved")
    for line in skipped:
        print(f"  not applied (no such pick held): {line}")
    for (draft, team), picks in sorted(held.items()):
        print(f"  {draft} {team} holds {', '.join(picks)}")


def career(path):
    save = SaveFile(path)
    t = save.db.tables.get("DRPK")
    if t is None or t.count != 448:
        raise SystemExit(f"{path}: not a career save with two drafts of picks")
    if save.db.tables["SEAI"].get(0, "SEYR") != 0:
        raise SystemExit(f"{path}: the career is past its first season; the trades are for 2027 and 2028")
    # Which draft is the nearer one: an off-season career before its draft still has 2026's, which
    # the template (written by --iso) leaves without trades, and 2027's, which has Chicago's first
    # rounder held by Tennessee.
    chicago_first = [i for i in range(t.count) if t.get(i, "DPOD") == TEAM_IDS["CHI"] and t.get(i, "DPNM") < 32]
    later = any(t.get(i, "DPYO") == 1 and t.get(i, "DPID") == TEAM_IDS["TEN"] for i in chicago_first)
    result = apply(t, 2026 if later else 2027)
    save.fix_checksums()
    backup = path + ".bak"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    save.save(path)
    print(f"backup: {backup}")
    report(*result)


def template(iso):
    import disc_patches
    found = disc_patches.databases(iso, ["DRPK"])
    if not found:
        raise SystemExit(f"no draft picks found in {iso}")
    for _, _, db in found:
        result = apply(db.tables["DRPK"], 2026)
    written = disc_patches.write_patches("career_picks", [(path, offset, db, ["DRPK"]) for path, offset, db in found])
    print(f"template picks written to {written} tables")
    report(*result)
    with open(HOOK_DATA, "w", newline="\n") as f:
        f.write("# Draft pick trades HC27 applies to a career's 2028 picks (tools/roster_import/career_picks.py).\n")
        f.write("# draft round from to [original], team ids as in the roster's TEAM table\n")
        for draft, rnd, frm, to, *original in TRADES:
            if draft == 2028:
                f.write(" ".join(str(v) for v in [draft, rnd, TEAM_IDS[frm], TEAM_IDS[to]] + [TEAM_IDS[o] for o in original]) + "\n")
    print(f"2028 trades written to {HOOK_DATA}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--iso", help="the game's disc image: write the template and hc27/draft_picks.txt")
    group.add_argument("--career", help="a career save (CAREER-<name>/CAREER-<name>) to give both drafts' trades")
    args = parser.parse_args()
    if args.iso:
        template(args.iso)
    else:
        career(args.career)


if __name__ == "__main__":
    main()
