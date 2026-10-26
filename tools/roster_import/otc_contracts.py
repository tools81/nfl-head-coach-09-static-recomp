"""Turns Over The Cap's contract pages into the contracts CSV import_roster.py reads (--contracts).

    python tools/roster_import/otc_contracts.py --pages tools/roster_import/data/otc \
        --out tools/roster_import/data/contracts.csv

--pages is a folder of pages saved from overthecap.com: the contracts list
(https://overthecap.com/contracts, saved as contracts.html) and each team's salary cap page
(https://overthecap.com/salary-cap/<team>, e.g. chicago-bears.html). A team page shows the team's
contracts year by year (base salary, prorated signing bonus, cap number); the list gives each
contract's total value and its average per year, from which its length follows.

Each row of the CSV is a player under contract: name, team (nickname, e.g. bears), position,
length (years), total and guaranteed value (dollars), then for each season of the contract still to
run, from the first season the pages show, cap_<season> (the cap number) and bonus_<season> (the
part of it that is prorated signing bonus).
"""

import argparse
import csv
import html
import os
import re

PLAYER = re.compile(r'href="/player/[^/]+/(\d+)/">([^<]+)</a>')
CELL = re.compile(r"<td[^>]*>(.*?)</td>", re.S)


def money(text):
    digits = re.sub(r"[^\d]", "", text)
    return int(digits) if digits else 0


def cells(row):
    return [re.sub(r"<[^>]+>", "", c).strip() for c in CELL.findall(row)]


def read_list(path):
    """Player id -> (position, total, apy, guaranteed) from the contracts list."""
    contracts = {}
    text = open(path, encoding="utf-8").read()
    for row in re.findall(r"<tr.*?</tr>", text, re.S):
        player = PLAYER.search(row)
        values = cells(row)
        if player and len(values) >= 6:
            contracts.setdefault(player.group(1), (values[1], money(values[3]), money(values[4]), money(values[5])))
    return contracts


def read_team(path):
    """Player id -> (name, {season: (cap number, prorated bonus)}) from a team's salary cap page."""
    text = open(path, encoding="utf-8").read()
    marks = [(m.start(), int(m.group(1))) for m in re.finditer(r'id="y(20\d\d)"', text)]
    players = {}
    for (start, season), (end, _) in zip(marks, marks[1:] + [(len(text), 0)]):
        section = text[start:end]
        after = 0
        for table in re.finditer(r'<table class="salary-cap-table ([^"]+)">(.*?)</table>', section, re.S):
            heading = re.sub(r"<[^>]+>", " ", section[after:table.start()])
            after = table.end()
            active = table.group(1) == "contracted-players"
            # The other tables are players under contract but not on the active roster (practice
            # squad, injured reserve, ...), with only their cap number, and released players'
            # dead money, which is skipped.
            if not active and "Dead Money" in heading:
                continue
            for row in re.findall(r"<tr.*?</tr>", table.group(2), re.S):
                player = PLAYER.search(row)
                values = cells(row)
                if not player or len(values) < (6 if active else 2):
                    continue
                name = html.unescape(player.group(2)).strip()
                # Active: player, base salary, prorated bonus, ..., cap number, (spacer), dead
                # money, cap savings. Others: name, cap number.
                year = (money(values[-4]), money(values[2])) if active else (money(values[1]), 0)
                players.setdefault(player.group(1), (name, {}))[1].setdefault(season, year)
    return players


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pages", required=True, help="folder of saved overthecap.com pages")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    contracts = read_list(os.path.join(args.pages, "contracts.html"))
    rows, seasons = [], set()
    for page in sorted(os.listdir(args.pages)):
        if page == "contracts.html" or not page.endswith(".html"):
            continue
        team = os.path.splitext(page)[0].split("-")[-1]
        for player_id, (name, years) in read_team(os.path.join(args.pages, page)).items():
            position, total, apy, guaranteed = contracts.get(player_id, ("", 0, 0, 0))
            caps = {s: v for s, v in years.items() if v[0] > 0}
            if not caps:
                continue
            length = max(len(caps), round(total / apy)) if apy else len(caps)
            rows.append((name, team, position, length, total or sum(c for c, _ in caps.values()), guaranteed, caps))
            seasons |= set(caps)
    seasons = sorted(seasons)
    with open(args.out, "w", encoding="utf-8", newline="") as f:
        out = csv.writer(f)
        out.writerow(["name", "team", "position", "length", "total", "guaranteed"] +
                     [f"{k}_{s}" for s in seasons for k in ("cap", "bonus")])
        for name, team, position, length, total, guaranteed, caps in rows:
            out.writerow([name, team, position, length, total, guaranteed] +
                         [v for s in seasons for v in (caps.get(s, ("", "")))])
    print(f"{len(rows)} contracts ({seasons[0]}-{seasons[-1]}) written to {args.out}")


if __name__ == "__main__":
    main()
