"""Converts a madden.tools player export into the ratings CSV the importer reads.

    python tools/roster_import/madden_tools.py --export data/madden_tools_fa.txt \
        --out data/free_agents.csv [--team "Free Agents"]

madden.tools (https://madden.tools/players) has every Madden player's ratings, free agents
included, which EA's ratings site leaves out. Its pages are Next.js: each player's ratings are at
/_next/data/<build id>/players/<slug>.json. To make the export, open https://madden.tools/players
in a browser and run EXPORT_JS (below) in its console; it fetches the free agents' pages (the best
--limit by overall, long snappers left out) and returns the export text, which goes in --export.

The export is a header line ("MTFA1"), a line naming the fields, then one line per player:
the fields joined by | and then the ratings joined by commas. The output has the columns of EA's
CSV (firstName, lastName, position/id, stats/<rating>/value, avatarUrl, ...), so load_players in
import_roster.py reads it; the player ids are madden.tools' plus 10,000,000, so they never clash
with EA's in the portrait cache.
"""

import argparse
import csv

EXPORT_JS = r"""
const limit = 512;
const b = window.__NEXT_DATA__.buildId;
const fa = window.__NEXT_DATA__.props.pageProps.teamPlayerData.find(x => x.team.acronym === 'FA');
const list = Object.values(fa.playersByPosition).flat().filter(p => p.position !== 'LS')
  .sort((a, c) => c.rating_overall - a.rating_overall).slice(0, limit);
const det = {}; let i = 0;
async function worker() { while (i < list.length) { const p = list[i++];
  for (let k = 0; k < 3 && !det[p.id]; k++) { try { const r = await fetch(`/_next/data/${b}/players/${p.slug}.json`);
    if (r.ok) det[p.id] = (await r.json()).pageProps.player; } catch (e) {} } } }
await Promise.all(Array.from({length: 8}, worker));
const d = Object.values(det);
const rk = Object.keys(d[0]).filter(k => k.startsWith('rating_')).sort();
const fk = ['id','first_name','last_name','position','age','height','weight','years_pro','jersey_number','portrait_id','team_id','home_state'];
'MTFA1\n' + fk.join('|') + '|' + rk.join(',') + '\n' + d.map(p => fk.map(k => String(p[k] ?? '').replace(/[|\n]/g, ' ')).join('|')
  + '|' + rk.map(k => p[k]).join(',')).join('\n')
"""

PORTRAIT_URL = "https://cdn.madden.tools/madden-27/assets/images/player-portraits/256/{}.png"
ID_OFFSET = 10_000_000

# madden.tools position -> EA's position/id (the keys of import_roster.POSITIONS)
POSITIONS = {"LEDGE": "LEDG", "REDGE": "REDG"}

# madden.tools rating (after "rating_") -> EA stat name, where it is not just the camelCase form
STATS = {"ball_carrier_vision": "bCVision", "impact_block": "impactBlocking",
         "throw_accuracy_short": "throwAccuracyShort", "throw_accuracy_medium": "throwAccuracyMid",
         "throw_accuracy_deep": "throwAccuracyDeep", "route_running_short": "shortRouteRunning",
         "route_running_medium": "mediumRouteRunning", "route_running_deep": "deepRouteRunning"}


def camel(name):
    head, *rest = name.split("_")
    return head + "".join(w.title() for w in rest)


def convert(export, out, team):
    with open(export, encoding="utf-8") as f:
        lines = f.read().strip().split("\n")
    if lines[0] != "MTFA1":
        raise SystemExit("not a madden.tools export (first line should be MTFA1)")
    header = lines[1].split("|")
    fields, ratings = header[:-1], header[-1].split(",")
    stat_names = [STATS.get(r[7:], camel(r[7:])) for r in ratings]
    columns = ["id", "firstName", "lastName", "team/label", "position/id", "birthdate", "height", "weight",
               "age", "jerseyNum", "yearsPro", "handedness", "college", "avatarUrl"] + \
              [f"stats/{s}/value" for s in stat_names]
    rows = []
    for line in lines[2:]:
        parts = line.split("|")
        p = dict(zip(fields, parts[:-1]))
        values = parts[-1].split(",")
        portrait = int(p["portrait_id"] or 0)
        rows.append([int(p["id"]) + ID_OFFSET, p["first_name"], p["last_name"], team,
                     POSITIONS.get(p["position"], p["position"]), "", p["height"], p["weight"], p["age"],
                     p["jersey_number"], p["years_pro"], "1", "",
                     PORTRAIT_URL.format(portrait) if portrait else ""] + values)
    with open(out, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(columns)
        w.writerows(rows)
    print(f"{len(rows)} players written to {out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--export", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--team", default="Free Agents", help="the team/label column")
    args = parser.parse_args()
    convert(args.export, args.out, args.team)
