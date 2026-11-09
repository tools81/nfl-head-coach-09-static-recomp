"""Replaces the 2008 free agents in a roster with the league's real free agents.

    python tools/roster_import/free_agents.py --fetch

The roster's free-agent pool (team 1009) is the 2008 game's unsigned players, so a career's free
agency offered players who retired long ago. EA's ratings site keeps a list of players for each
week's ratings: the launch list (1-base) has every player with Madden ratings, free agents
included, and each week's list the players on a team that week. A player is a free agent now when
no team has him in the latest week's list nor in the roster, and the transactions (roster_moves.py's
CSV) do not put him on a team:

    latest transaction                      free agent?
    released, waived, terminated            yes
    signed to a practice squad              yes (HC09 has no practice squads; any team may sign him)
    signed, claimed off waivers, traded     no
    waived injured                          no (he reverts to his team's injured reserve)
    none, and has a contract (contracts.csv) no (on a reserve list: injured, PUP, suspended)
    none, and no contract                   yes (unsigned all year)

Each player takes his ratings from the latest list that has him. The best of them at each
position (each position keeping its share of the pool) take over the records of the 2008 free
agents (those of the same position first, so equipment and build fit), unsigned like the game's
own free agents. Free agents the roster already has (players released since the import) stay.
The roster has a fixed number of player records, so the pool keeps its size (512): the weakest
free agents are left out.

--fetch downloads the lists into --ratings (one JSON file per list); without it the files there
are used. Then give the free agents their portraits and heads, and their statistics:

    python tools/roster_import/fetch_portraits.py --report tools/roster_import/data/players.csv \\
        --report tools/roster_import/data/free_agents.csv --roster hc27/ROSTER --looks tools/roster_import/data/looks.csv
    python tools/roster_import/player_stats.py --iso "iso/NFL Head Coach 09.iso"
"""

import argparse
import csv
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from import_roster import CONTRACT_FIELDS, FREE_AGENTS, POSITION_NAMES, player_from_row, write_player  # noqa: E402
from fetch_portraits import download  # noqa: E402
from roster_moves import destination, name_key  # noqa: E402
from tdb import SaveFile  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SITE = "https://www.ea.com/en/games/madden-nfl/ratings"
LAUNCH = "1-base"


def page_data(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (hc09-roster-import)"})
    with urllib.request.urlopen(request, timeout=60) as response:
        html = response.read().decode("utf-8")
    found = re.search(r'<script id="__NEXT_DATA__"[^>]*>(.*?)</script>', html, re.S)
    if not found:
        raise SystemExit(f"{url}: no ratings data in the page")
    return json.loads(found.group(1))["props"]["pageProps"]


def fetch(folder, delay):
    """Downloads every week's list (and the launch list) into folder, one JSON file each, and
    index.json naming them oldest first."""
    os.makedirs(folder, exist_ok=True)
    iterations = [i["id"] for i in page_data(SITE)["ratingsFilters"]["iterations"]]
    for iteration in iterations:
        players, page, total = {}, 1, None
        while total is None or len(players) < total:
            query = urllib.parse.urlencode({"iteration": iteration, "page": page})
            details = page_data(f"{SITE}?{query}")["ratingDetails"]
            total = details["totalItems"]
            if not details["items"]:
                break
            for item in details["items"]:
                players[item["id"]] = item
            page += 1
            time.sleep(delay)
        with open(os.path.join(folder, f"{iteration}.json"), "w", encoding="utf-8") as f:
            json.dump(list(players.values()), f)
        print(f"{iteration}: {len(players)} players")
    with open(os.path.join(folder, "index.json"), "w", encoding="utf-8") as f:
        json.dump(list(reversed(iterations)), f)  # the site lists the newest first


def flatten(item):
    """A ratings site player as a row of its CSV export (see player_from_row)."""
    row = {k: v for k, v in item.items() if not isinstance(v, (dict, list))}
    row["team/label"] = (item.get("team") or {}).get("label") or ""
    row["position/id"] = (item.get("position") or {}).get("id") or ""
    for name, stat in (item.get("stats") or {}).items():
        row[f"stats/{name}/value"] = stat.get("value") if isinstance(stat, dict) else stat
    return row


def latest_status(transactions):
    """name key -> how the player's latest transaction leaves him: "free", "practice", "team",
    "reserve" (waived injured, back on his team's injured reserve)."""
    with open(transactions, encoding="utf-8", newline="") as f:
        rows = sorted(csv.DictReader(f), key=lambda r: r["date"])
    status = {}
    for row in rows:
        text = row["text"].lower()
        where = destination(row)
        if where is None:
            if row["category"] == "waivers" and "injured" in text:
                status[name_key(row["name"])] = "reserve"
            continue
        if where == FREE_AGENTS:
            status[name_key(row["name"])] = "free"
        else:
            status[name_key(row["name"])] = "practice" if "practice squad" in text else "team"
    return status


def pick(pool, places):
    """The best of pool (sorted best first) for the places, each position getting its share of the
    pool, so the pool keeps the league's mix of positions (by overall alone, the ratings' scale puts
    a hundred receivers and three quarterbacks in it)."""
    by_position = {}
    for p in pool:
        by_position.setdefault(p["position"], []).append(p)
    share = {k: places * len(v) / len(pool) for k, v in by_position.items()}
    quota = {k: min(int(v), len(by_position[k])) for k, v in share.items()}
    for k in sorted(share, key=lambda k: quota[k] - share[k]):  # the largest remainders first
        if sum(quota.values()) >= min(places, len(pool)):
            break
        if quota[k] < len(by_position[k]):
            quota[k] += 1
    chosen = [p for k, players in by_position.items() for p in players[:quota[k]]]
    return sorted(chosen, key=lambda p: (-p["stats"]["overall"], p["last"], p["first"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--out", help="where to write the roster (default: over --roster)")
    parser.add_argument("--players", default=os.path.join(HERE, "data", "players.csv"),
                        help="import_roster.py's report: the roster's imported players and their site ids")
    parser.add_argument("--ratings", default=os.path.join(HERE, "data", "ea_ratings"))
    parser.add_argument("--fetch", action="store_true", help="download the ratings site's lists first")
    parser.add_argument("--transactions", default=os.path.join(HERE, "data", "transactions_2026.csv"))
    parser.add_argument("--contracts", default=os.path.join(HERE, "data", "contracts.csv"))
    parser.add_argument("--no-practice-squads", action="store_true",
                        help="leave out players signed to a practice squad")
    parser.add_argument("--report", default=os.path.join(HERE, "data", "free_agents.csv"),
                        help="the free agents written, in import_roster.py's report format (for fetch_portraits.py)")
    parser.add_argument("--cache", default=os.path.join(HERE, "data", "portraits"),
                        help="fetch_portraits.py's image cache (the players' images are downloaded into it)")
    parser.add_argument("--delay", type=float, default=0.2, help="seconds between page downloads")
    args = parser.parse_args()

    if args.fetch:
        fetch(args.ratings, args.delay)
    with open(os.path.join(args.ratings, "index.json"), encoding="utf-8") as f:
        iterations = json.load(f)
    if iterations[0] != LAUNCH:
        raise SystemExit(f"{args.ratings}: the launch list ({LAUNCH}) comes first")
    latest = {}  # site id -> the latest list's entry
    for iteration in iterations:
        with open(os.path.join(args.ratings, f"{iteration}.json"), encoding="utf-8") as f:
            for item in json.load(f):
                latest[item["id"]] = item
    with open(os.path.join(args.ratings, f"{iterations[-1]}.json"), encoding="utf-8") as f:
        signed = {item["id"] for item in json.load(f) if item.get("team")}

    save = SaveFile(args.roster)
    play = save.db.tables["PLAY"]
    with open(args.players, encoding="utf-8", newline="") as f:
        imported = {int(r["pgid"]): r for r in csv.DictReader(f)}
    have = set()
    old_free = []  # the 2008 free agents' records
    for i in range(play.count):
        row = imported.get(play.get(i, "PGID"))
        if row:
            have.add(int(row["madden_id"]))
        elif play.get(i, "TGID") == FREE_AGENTS:
            old_free.append(i)

    status = latest_status(args.transactions)
    with open(args.contracts, encoding="utf-8-sig", newline="") as f:
        under_contract = {name_key(r["name"]) for r in csv.DictReader(f)}
    free_status = {"free", "practice"} - ({"practice"} if args.no_practice_squads else set())
    pool, reasons = [], {}
    for site_id, item in latest.items():
        if site_id in signed or site_id in have:
            continue  # on a team, or already in the roster (on a team or a free agent)
        key = name_key(f"{item['firstName']} {item['lastName']}")
        state = status.get(key) or ("reserve" if key in under_contract else "free")
        reasons[state] = reasons.get(state, 0) + 1
        if state not in free_status:
            continue
        p = player_from_row(flatten(item))
        if p and "overall" in p["stats"]:
            p["status"] = state
            pool.append(p)
    pool.sort(key=lambda p: (-p["stats"]["overall"], p["last"], p["first"]))
    print(f"{len(latest)} players on the ratings site; {len(signed)} on a team in {iterations[-1]}; "
          f"of the rest, not in the roster: " + ", ".join(f"{n} {s}" for s, n in sorted(reasons.items())))
    chosen = pick(pool, len(old_free))

    # Same position first, so equipment and build fit; then any record.
    available = list(old_free)
    records = {}
    for p in chosen:
        match = next((i for i in available if play.get(i, "PPOS") == p["position"]), None)
        if match is not None:
            available.remove(match)
            records[id(p)] = match
    for p in chosen:
        if id(p) not in records:
            records[id(p)] = available.pop(0)
    for p in chosen:
        i = records[id(p)]
        write_player(play, i, p, FREE_AGENTS)
        for field in CONTRACT_FIELDS:  # unsigned, like the game's own free agents
            play.set(i, field, 0)

    # Images, into fetch_portraits.py's cache: the free agents', and those of imported players the
    # cache lacks (the site had no Madden 27 image of some; fetch_portraits.py now takes Madden 26's).
    os.makedirs(args.cache, exist_ok=True)

    def has_image(site_id, url):
        path = os.path.join(args.cache, f"{site_id}.png")
        if url and not os.path.exists(path):
            try:
                download(url, path)
            except Exception as error:
                print(f"no image for site player {site_id}: {error}")
        return os.path.exists(path)

    # Portrait ids. Only ids the game's portrait library has can be replaced, and those not used by
    # the imported players are the ids the 2008 free agents had to themselves: too few for every free
    # agent. They go first to imported players with an image but no id of their own (the site had no
    # Madden 27 image of some, see fetch_portraits.py), then to the free agents, the best first; the
    # rest show the silhouette (their image still sets their head's skin tone).
    counts = {}
    for i in range(play.count):
        counts[play.get(i, "PSXP")] = counts.get(play.get(i, "PSXP"), 0) + 1
    generic = max((k for k in counts if k), key=lambda k: counts[k])
    by_pgid = {play.get(i, "PGID"): i for i in range(play.count)}
    team = [by_pgid[g] for g in imported if g in by_pgid]
    held = {play.get(i, "PSXP") for i in team}
    spare = sorted(play.get(i, "PSXP") for i in old_free
                   if counts[play.get(i, "PSXP")] == 1 and play.get(i, "PSXP") not in held | {0, generic})
    needing = [i for i in team if play.get(i, "PSXP") == generic and has_image(imported[play.get(i, "PGID")]["madden_id"],
                                                                          imported[play.get(i, "PGID")]["avatarUrl"])]
    pictured = [records[id(p)] for p in chosen if has_image(p["id"], p["avatar"])]
    for i in old_free:
        play.set(i, "PSXP", generic)
    given = 0
    for i in needing + pictured:
        if not spare:
            break
        play.set(i, "PSXP", spare.pop(0))
        given += 1
    silhouettes = len(needing) + len(pictured) - given
    save.fix_checksums()
    save.save(args.out or args.roster)
    with open(args.report, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["pgid", "portrait_id", "madden_id", "first", "last", "team", "position", "overall", "avatarUrl",
                    "status"])
        for p in chosen:
            i = records[id(p)]
            w.writerow([play.get(i, "PGID"), play.get(i, "PSXP"), p["id"], p["first"], p["last"], FREE_AGENTS,
                        POSITION_NAMES[p["position"]], p["stats"]["overall"], p["avatar"], p["status"]])
    counts = {}
    for p in chosen:
        counts[POSITION_NAMES[p["position"]]] = counts.get(POSITION_NAMES[p["position"]], 0) + 1
    print(f"{len(pool)} free agents; the best {len(chosen)} replace the 2008 free agents: "
          + ", ".join(f"{n} {k}" for k, n in sorted(counts.items(), key=lambda kv: -kv[1])))
    print(f"portraits: {min(given, len(needing))} imported players without one get theirs; "
          f"{max(given - len(needing), 0)} free agents get one, {silhouettes} with an image show the silhouette "
          f"(no portrait id left), {len(chosen) - len(pictured)} have no image")
    print(f"wrote {args.out or args.roster} and {args.report}")


if __name__ == "__main__":
    main()
