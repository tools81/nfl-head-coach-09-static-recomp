"""The rookie wage scale HC27 careers sign their draft picks to: hc27/rookie_scale.txt.

Since 2011 a drafted player's contract is set by where he is picked: four years, with a total value
and signing bonus fixed for each pick, falling from the first pick to the last. The game instead
has each team negotiate with its picks' agents after the draft. HC27 signs every pick as the draft
ends instead (src/common/rookie_scale.cpp), to the scale this script writes:

    python tools/roster_import/rookie_scale.py

The scale comes from the 2026 draft class in the roster (hc27/ROSTER, with the real contracts of
otc_contracts.py): the rookies on four-year deals, ranked by total value, so the first is the first
pick's deal, the 33rd the first of the second round, and so on. Each line is a pick: total value,
signing bonus and the four years' salaries, in $10,000s as the game stores them. The salaries
follow the scale's shape: the league minimum in the first year, rising by equal steps to make up
the deal (and never below the minimum for each year); the bonus is spread evenly over the four
years when a pick is signed.
"""

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
YEARS = 4
PICKS = 7 * 32  # the game's draft: seven rounds of one pick per team


def rookie_deals(path):
    """(total, bonus) of each four-year contract signed by a rookie, largest first."""
    play = SaveFile(path).db.tables["PLAY"]
    deals, salaries = [], []
    for i in range(play.count):
        if play.get(i, "PYRP") == 0 and play.get(i, "PCON") == YEARS and play.get(i, "PCYL") == YEARS:
            deals.append((play.get(i, "PTSA"), play.get(i, "PSBO")))
            salaries.append([play.get(i, f"PSA{y}") for y in range(YEARS)])
    deals.sort(reverse=True)
    # Each year's minimum: the salary most of the deals pay that year (the late picks' all).
    minimums = [Counter(s[y] for s in salaries).most_common(1)[0][0] for y in range(YEARS)]
    return deals, minimums


def schedule(total, bonus, minimums):
    """The four years' salaries of a deal: the first year's minimum, then equal raises, the last
    year taking what rounding leaves so that they add up to the deal."""
    first = minimums[0]
    step = max(0.0, (total - bonus - YEARS * first) / (YEARS * (YEARS - 1) / 2))
    salaries = [max(minimums[y], round(first + y * step)) for y in range(YEARS - 1)]
    salaries.append(max(minimums[-1], total - bonus - sum(salaries)))
    return salaries


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--roster", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--out", default=os.path.join(ROOT, "hc27", "rookie_scale.txt"))
    args = parser.parse_args()

    deals, minimums = rookie_deals(args.roster)
    if len(deals) < PICKS:
        sys.exit(f"{args.roster}: {len(deals)} rookie contracts, fewer than the draft's {PICKS} picks")
    with open(args.out, "w", newline="\n") as out:
        out.write("# Rookie wage scale HC27 signs draft picks to (tools/roster_import/rookie_scale.py).\n")
        out.write("# pick total bonus salary1 salary2 salary3 salary4, in $10,000s; four years\n")
        for pick, (total, bonus) in enumerate(deals[:PICKS], 1):
            # A few late deals pay less than the minimums besides their bonus: they keep their
            # value, with the bonus making up what the minimums leave.
            bonus = max(0, min(bonus, total - sum(minimums)))
            salaries = schedule(total, bonus, minimums)
            out.write(f"{pick} {sum(salaries) + bonus} {bonus} {' '.join(map(str, salaries))}\n")
    first, last = deals[0], deals[PICKS - 1]
    print(f"{args.out}: {PICKS} picks, ${first[0] / 100:.2f}M (${first[1] / 100:.2f}M bonus) down to "
          f"${last[0] / 100:.2f}M; yearly minimums {minimums}")


if __name__ == "__main__":
    main()
