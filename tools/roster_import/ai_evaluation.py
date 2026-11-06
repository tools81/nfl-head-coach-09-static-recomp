"""Retunes how the game's AI weighs players, so AI teams start the players their ratings favour.

    python tools/roster_import/ai_evaluation.py --iso "iso/NFL Head Coach 09.iso"

The AI ranks a depth chart by the game's own overall for the position, which it computes from seven
component scores (sub_825E3A88; HC09_PROBE_EVAL in src/debug/depth_chart_probe.cpp logs it). Each
component's weight is a field of table PEOW in the disc's databases, one record per evaluation
slot (GMPP). Every position has five slots (QB 0-4, HB 5-10, FB 11-14, WR 15-19, TE 20-24,
OT 25-29, OG 30-34, C 35-39, DE 40-44, DT 45-49, OLB 50-54, MLB 55-59, CB 60-64, FS 65-69,
SS 70-74, K 75-77, P 78-80) and each team ranks a position by one of them, its scheme
(HC09_PROBE_SLOTS logs which). The components:

    OSZW  size: height and weight on curves (LPHG, LPWG, weighted by PESW), not ratings
    ODRW  durability: the body-part ratings (LPHC weights)
    OLNW  learning (PLRN)
    OATW  athletic: for QBs throw power (PEAW: THP 31, SPD/ACC/AGL/ELU 1)
    OINW  intangibles: throw accuracy, awareness and learning (PEIW: THA, AWR, LRN 20 each)
    OPTW  a physical score (follows strength, r = 0.97 over ten QBs)
    OPDW  production: from the player's stats, filled in during the season

    overall = floor(sum(weight x score) / sum(weights))

For QBs the 2008 weights are OSZW 8, OATW 15, OPTW 15, OPDW 6, OINW 3, ODRW 1, OLNW 1: accuracy
and awareness together count for 3/49 and size for 8/49, so a tall, heavy, strong-armed backup
outranks a more accurate starter (Rourke over Purdy; Wentz over Murray). Other positions lean on
size, durability and the physical score in the same way (DT, DE, WR, CB most). WEIGHTS below was
fitted, slot by slot, to the imported overall (POVR) of the players at that position (the
production weight kept as it was): mostly less size, durability and physical score, more
learning and intangibles. The overall is also the one the game shows for players and that other
AI decisions read, so displayed overalls move closer to the imported ones.

PEOW is written, changed, to hc27/disc (one file per database copy, see disc_patches.py), which
HC27 lays over the disc's files at startup. Rerun after changing WEIGHTS; its earlier patches are
replaced. Delete the files listed in hc27/disc/ai_evaluation.files to go back to the 2008 weights.
"""

import argparse

import disc_patches

# Evaluation slot (PEOW.GMPP) -> new weights (each 0-31). Fields left out keep their 2008 value.
WEIGHTS = {
    0: {"OSZW": 1, "ODRW": 0, "OLNW": 19, "OATW": 15, "OINW": 20, "OPTW": 0, "OPDW": 5},  # QB
    1: {"OSZW": 1, "ODRW": 1, "OLNW": 1, "OATW": 12, "OINW": 24, "OPTW": 2, "OPDW": 4},  # QB
    2: {"OSZW": 3, "ODRW": 11, "OLNW": 23, "OATW": 15, "OINW": 23, "OPTW": 14, "OPDW": 10},  # QB
    3: {"OSZW": 0, "ODRW": 2, "OLNW": 26, "OATW": 16, "OINW": 28, "OPTW": 10, "OPDW": 8},  # QB
    4: {"OSZW": 5, "ODRW": 8, "OLNW": 15, "OATW": 12, "OINW": 22, "OPTW": 0, "OPDW": 8},  # QB
    5: {"OSZW": 0, "ODRW": 3, "OLNW": 27, "OATW": 15, "OINW": 23, "OPTW": 12, "OPDW": 5},  # HB
    6: {"OSZW": 0, "ODRW": 0, "OLNW": 16, "OATW": 15, "OINW": 22, "OPTW": 15, "OPDW": 4},  # HB
    7: {"OSZW": 0, "ODRW": 9, "OLNW": 15, "OATW": 11, "OINW": 15, "OPTW": 5, "OPDW": 5},  # HB
    8: {"OSZW": 0, "ODRW": 5, "OLNW": 5, "OATW": 5, "OINW": 25, "OPTW": 10, "OPDW": 5},  # HB
    9: {"OSZW": 0, "ODRW": 15, "OLNW": 27, "OATW": 23, "OINW": 27, "OPTW": 5, "OPDW": 10},  # HB
    10: {"OSZW": 0, "ODRW": 15, "OLNW": 15, "OATW": 10, "OINW": 22, "OPTW": 0, "OPDW": 6},  # HB
    11: {"OSZW": 0, "ODRW": 0, "OLNW": 9, "OATW": 0, "OINW": 17, "OPTW": 7, "OPDW": 1},  # FB
    12: {"OSZW": 5, "ODRW": 2, "OLNW": 20, "OATW": 0, "OINW": 15, "OPTW": 12, "OPDW": 1},  # FB
    13: {"OSZW": 3, "ODRW": 0, "OLNW": 8, "OATW": 10, "OINW": 9, "OPTW": 0, "OPDW": 1},  # FB
    14: {"OSZW": 5, "ODRW": 0, "OLNW": 8, "OATW": 7, "OINW": 31, "OPTW": 10, "OPDW": 1},  # FB
    15: {"OSZW": 0, "ODRW": 0, "OLNW": 11, "OATW": 7, "OINW": 31, "OPTW": 0, "OPDW": 5},  # WR
    16: {"OSZW": 0, "ODRW": 0, "OLNW": 5, "OATW": 12, "OINW": 15, "OPTW": 0, "OPDW": 6},  # WR
    17: {"OSZW": 2, "ODRW": 0, "OLNW": 0, "OATW": 15, "OINW": 26, "OPTW": 0, "OPDW": 5},  # WR
    18: {"OSZW": 1, "ODRW": 0, "OLNW": 10, "OATW": 21, "OINW": 15, "OPTW": 0, "OPDW": 6},  # WR
    19: {"OSZW": 1, "ODRW": 0, "OLNW": 7, "OATW": 15, "OINW": 29, "OPTW": 0, "OPDW": 8},  # WR
    20: {"OSZW": 1, "ODRW": 3, "OLNW": 24, "OATW": 0, "OINW": 25, "OPTW": 0, "OPDW": 4},  # TE
    21: {"OSZW": 0, "ODRW": 0, "OLNW": 31, "OATW": 0, "OINW": 0, "OPTW": 3, "OPDW": 1},  # TE
    22: {"OSZW": 1, "ODRW": 3, "OLNW": 28, "OATW": 0, "OINW": 21, "OPTW": 0, "OPDW": 4},  # TE
    23: {"OSZW": 1, "ODRW": 0, "OLNW": 18, "OATW": 0, "OINW": 23, "OPTW": 0, "OPDW": 4},  # TE
    24: {"OSZW": 1, "ODRW": 3, "OLNW": 23, "OATW": 0, "OINW": 22, "OPTW": 0, "OPDW": 4},  # TE
    25: {"OSZW": 0, "ODRW": 0, "OLNW": 5, "OATW": 31, "OINW": 5, "OPTW": 0, "OPDW": 1},  # OT
    26: {"OSZW": 3, "ODRW": 5, "OLNW": 10, "OATW": 26, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OT
    27: {"OSZW": 8, "ODRW": 0, "OLNW": 0, "OATW": 30, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OT
    28: {"OSZW": 1, "ODRW": 0, "OLNW": 0, "OATW": 27, "OINW": 1, "OPTW": 0, "OPDW": 1},  # OT
    29: {"OSZW": 0, "ODRW": 0, "OLNW": 0, "OATW": 25, "OINW": 4, "OPTW": 0, "OPDW": 1},  # OT
    30: {"OSZW": 0, "ODRW": 0, "OLNW": 5, "OATW": 23, "OINW": 5, "OPTW": 0, "OPDW": 1},  # OG
    31: {"OSZW": 3, "ODRW": 5, "OLNW": 10, "OATW": 30, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OG
    32: {"OSZW": 8, "ODRW": 5, "OLNW": 10, "OATW": 31, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OG
    33: {"OSZW": 1, "ODRW": 0, "OLNW": 0, "OATW": 30, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OG
    34: {"OSZW": 8, "ODRW": 0, "OLNW": 0, "OATW": 25, "OINW": 31, "OPTW": 0, "OPDW": 1},  # OG
    35: {"OSZW": 0, "ODRW": 0, "OLNW": 5, "OATW": 31, "OINW": 5, "OPTW": 5, "OPDW": 1},  # C
    36: {"OSZW": 3, "ODRW": 0, "OLNW": 10, "OATW": 28, "OINW": 15, "OPTW": 0, "OPDW": 1},  # C
    37: {"OSZW": 8, "ODRW": 0, "OLNW": 10, "OATW": 31, "OINW": 15, "OPTW": 0, "OPDW": 1},  # C
    38: {"OSZW": 1, "ODRW": 0, "OLNW": 0, "OATW": 27, "OINW": 15, "OPTW": 0, "OPDW": 1},  # C
    39: {"OSZW": 8, "ODRW": 0, "OLNW": 0, "OATW": 25, "OINW": 31, "OPTW": 0, "OPDW": 1},  # C
    40: {"OSZW": 8, "ODRW": 0, "OLNW": 30, "OATW": 25, "OINW": 25, "OPTW": 0, "OPDW": 1},  # DE
    41: {"OSZW": 0, "ODRW": 0, "OLNW": 26, "OATW": 15, "OINW": 15, "OPTW": 0, "OPDW": 1},  # DE
    42: {"OSZW": 2, "ODRW": 0, "OLNW": 20, "OATW": 15, "OINW": 3, "OPTW": 0, "OPDW": 1},  # DE
    43: {"OSZW": 0, "ODRW": 0, "OLNW": 29, "OATW": 0, "OINW": 15, "OPTW": 0, "OPDW": 1},  # DE
    44: {"OSZW": 0, "ODRW": 0, "OLNW": 25, "OATW": 0, "OINW": 24, "OPTW": 0, "OPDW": 1},  # DE
    45: {"OSZW": 0, "ODRW": 0, "OLNW": 18, "OATW": 10, "OINW": 15, "OPTW": 0, "OPDW": 1},  # DT
    46: {"OSZW": 0, "ODRW": 0, "OLNW": 23, "OATW": 15, "OINW": 31, "OPTW": 0, "OPDW": 1},  # DT
    47: {"OSZW": 3, "ODRW": 1, "OLNW": 30, "OATW": 0, "OINW": 22, "OPTW": 10, "OPDW": 1},  # DT
    48: {"OSZW": 0, "ODRW": 0, "OLNW": 25, "OATW": 0, "OINW": 15, "OPTW": 10, "OPDW": 1},  # DT
    49: {"OSZW": 0, "ODRW": 0, "OLNW": 10, "OATW": 10, "OINW": 26, "OPTW": 0, "OPDW": 1},  # DT
    50: {"OSZW": 6, "ODRW": 0, "OLNW": 15, "OATW": 0, "OINW": 31, "OPTW": 15, "OPDW": 1},  # OLB
    51: {"OSZW": 0, "ODRW": 1, "OLNW": 5, "OATW": 3, "OINW": 15, "OPTW": 0, "OPDW": 1},  # OLB
    52: {"OSZW": 0, "ODRW": 0, "OLNW": 12, "OATW": 3, "OINW": 15, "OPTW": 5, "OPDW": 1},  # OLB
    53: {"OSZW": 0, "ODRW": 0, "OLNW": 11, "OATW": 0, "OINW": 12, "OPTW": 5, "OPDW": 1},  # OLB
    54: {"OSZW": 0, "ODRW": 0, "OLNW": 21, "OATW": 15, "OINW": 23, "OPTW": 0, "OPDW": 1},  # OLB
    55: {"OSZW": 5, "ODRW": 10, "OLNW": 25, "OATW": 0, "OINW": 31, "OPTW": 15, "OPDW": 1},  # MLB
    56: {"OSZW": 5, "ODRW": 5, "OLNW": 25, "OATW": 0, "OINW": 15, "OPTW": 10, "OPDW": 1},  # MLB
    57: {"OSZW": 8, "ODRW": 0, "OLNW": 26, "OATW": 2, "OINW": 12, "OPTW": 3, "OPDW": 1},  # MLB
    58: {"OSZW": 5, "ODRW": 5, "OLNW": 12, "OATW": 0, "OINW": 24, "OPTW": 5, "OPDW": 1},  # MLB
    59: {"OSZW": 0, "ODRW": 0, "OLNW": 22, "OATW": 0, "OINW": 18, "OPTW": 15, "OPDW": 1},  # MLB
    60: {"OSZW": 3, "ODRW": 0, "OLNW": 27, "OATW": 0, "OINW": 25, "OPTW": 15, "OPDW": 3},  # CB
    61: {"OSZW": 3, "ODRW": 0, "OLNW": 10, "OATW": 0, "OINW": 25, "OPTW": 10, "OPDW": 3},  # CB
    62: {"OSZW": 5, "ODRW": 0, "OLNW": 23, "OATW": 12, "OINW": 12, "OPTW": 0, "OPDW": 3},  # CB
    63: {"OSZW": 0, "ODRW": 0, "OLNW": 18, "OATW": 0, "OINW": 15, "OPTW": 15, "OPDW": 3},  # CB
    64: {"OSZW": 1, "ODRW": 2, "OLNW": 8, "OATW": 2, "OINW": 25, "OPTW": 0, "OPDW": 3},  # CB
    65: {"OSZW": 5, "ODRW": 0, "OLNW": 28, "OATW": 0, "OINW": 31, "OPTW": 15, "OPDW": 1},  # FS
    66: {"OSZW": 3, "ODRW": 0, "OLNW": 26, "OATW": 15, "OINW": 31, "OPTW": 10, "OPDW": 1},  # FS
    67: {"OSZW": 3, "ODRW": 5, "OLNW": 12, "OATW": 0, "OINW": 31, "OPTW": 8, "OPDW": 1},  # FS
    68: {"OSZW": 0, "ODRW": 0, "OLNW": 27, "OATW": 15, "OINW": 21, "OPTW": 15, "OPDW": 1},  # FS
    69: {"OSZW": 0, "ODRW": 0, "OLNW": 20, "OATW": 0, "OINW": 15, "OPTW": 6, "OPDW": 1},  # FS
    70: {"OSZW": 5, "ODRW": 10, "OLNW": 22, "OATW": 0, "OINW": 31, "OPTW": 15, "OPDW": 1},  # SS
    71: {"OSZW": 1, "ODRW": 2, "OLNW": 19, "OATW": 6, "OINW": 21, "OPTW": 0, "OPDW": 1},  # SS
    72: {"OSZW": 0, "ODRW": 6, "OLNW": 26, "OATW": 0, "OINW": 20, "OPTW": 10, "OPDW": 1},  # SS
    73: {"OSZW": 0, "ODRW": 8, "OLNW": 27, "OATW": 0, "OINW": 15, "OPTW": 6, "OPDW": 1},  # SS
    74: {"OSZW": 1, "ODRW": 6, "OLNW": 15, "OATW": 0, "OINW": 31, "OPTW": 10, "OPDW": 1},  # SS
    75: {"OSZW": 1, "ODRW": 1, "OLNW": 5, "OATW": 30, "OINW": 25, "OPTW": 8, "OPDW": 3},  # K
    76: {"OSZW": 1, "ODRW": 1, "OLNW": 0, "OATW": 18, "OINW": 15, "OPTW": 8, "OPDW": 3},  # K
    77: {"OSZW": 1, "ODRW": 1, "OLNW": 0, "OATW": 25, "OINW": 31, "OPTW": 10, "OPDW": 3},  # K
    78: {"OSZW": 1, "ODRW": 1, "OLNW": 5, "OATW": 15, "OINW": 18, "OPTW": 8, "OPDW": 3},  # P
    79: {"OSZW": 1, "ODRW": 1, "OLNW": 5, "OATW": 14, "OINW": 15, "OPTW": 8, "OPDW": 3},  # P
    80: {"OSZW": 1, "ODRW": 1, "OLNW": 0, "OATW": 25, "OINW": 31, "OPTW": 10, "OPDW": 3},  # P
}
TABLES = ("PEOW",)


def update(db):
    """Sets the weights in the database's PEOW; returns the names of the tables it changed."""
    t = db.tables.get("PEOW")
    if not t or not t.count:
        return []
    changed = False
    for i in range(t.count):
        for field, value in WEIGHTS.get(t.get(i, "GMPP"), {}).items():
            if not 0 <= value <= 31:
                raise ValueError(f"PEOW.{field} must be 0-31, not {value}")
            if t.get(i, field) != value:
                t.set(i, field, value)
                changed = True
    return ["PEOW"] if changed else []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--out", default=disc_patches.OUT)
    args = parser.parse_args()

    found = disc_patches.databases(args.iso, TABLES)
    if not found:
        raise SystemExit("No database with PEOW found on this disc image.")
    written = disc_patches.write_patches("ai_evaluation", [(path, offset, db, update(db)) for path, offset, db in found],
                                         args.out)
    print(f"AI evaluation weights set for slots {sorted(WEIGHTS)}; {written} tables in {len(found)} databases "
          f"patched in {args.out}")


if __name__ == "__main__":
    main()
