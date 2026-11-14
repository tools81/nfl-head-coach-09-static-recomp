"""Downloads the league's transactions from NFL.com into the CSV roster_moves.py reads.

    python tools/roster_import/fetch_transactions.py --months 2026/10 2026/11

Each month's pages (https://www.nfl.com/transactions/league/<category>/<year>/<month>, categories
signings, waivers, trades, terminations, other) are read to their last page and added to --out
(default data/transactions_2026.csv); rows already there are not repeated, so a month can be
fetched again as it goes on.
"""

import argparse
import csv
import html
import os
import re
import time
import urllib.request

CATEGORIES = ["signings", "waivers", "trades", "terminations", "other"]
COLUMNS = ["category", "date", "from", "to", "slug", "name", "position", "text"]
HERE = os.path.dirname(os.path.abspath(__file__))


def text_of(cell):
    return html.unescape(re.sub(r"<[^>]+>", "", cell)).strip()


def team_of(cell):
    found = re.search(r'href="/teams/([^/"]+)/', cell)
    return found.group(1) if found else ""


def page_rows(category, year, page):
    for row in re.findall(r"<tr>(.*?)</tr>", page, re.S):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)
        if len(cells) < 6:
            continue
        month, day = text_of(cells[2]).split("/")
        slug = re.search(r'href="/players/([^/"]+)/', cells[3])
        yield {
            "category": category, "date": f"{year}/{month}/{day}", "from": team_of(cells[0]), "to": team_of(cells[1]),
            "slug": slug.group(1) if slug else "", "name": text_of(cells[3]), "position": text_of(cells[4]),
            "text": text_of(cells[5]),
        }


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read().decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--months", nargs="+", required=True, help="YYYY/MM ...")
    parser.add_argument("--out", default=os.path.join(HERE, "data", "transactions_2026.csv"))
    args = parser.parse_args()

    rows = []
    if os.path.exists(args.out):
        rows = list(csv.DictReader(open(args.out, encoding="utf-8")))
    seen = {tuple(r[c] for c in COLUMNS) for r in rows}
    added = 0
    for month in args.months:
        year, number = month.split("/")
        for category in CATEGORIES:
            url = f"https://www.nfl.com/transactions/league/{category}/{year}/{int(number)}"
            pages = 0
            while url and pages < 200:
                page = fetch(url)
                pages += 1
                for row in page_rows(category, year, page):
                    key = tuple(row[c] for c in COLUMNS)
                    if key not in seen:
                        seen.add(key)
                        rows.append(row)
                        added += 1
                after = re.search(r'href="(/transactions/league/[^"]*\?after=[^"]*)"', page)
                url = "https://www.nfl.com" + html.unescape(after.group(1)) if after else None
                time.sleep(0.3)
            print(f"{month} {category}: {pages} pages")
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"{added} new transactions, {len(rows)} in {args.out}")


if __name__ == "__main__":
    main()
