"""The roster's free-agent pool: real free agents where there are ratings for them, made-up
players for the rest.

    python tools/roster_import/free_agents.py --roster hc27/ROSTER [--players data/free_agents.csv] \
        [--report data/free_agents_report.csv] [--out hc27/ROSTER] [--seed 2026]

After import_roster.py the free-agent pool (TGID 1009) is the 2008 game's: its own free agents,
its 2008 draft class and the 2008 players the import left over (Keith Rivers, Peyton Hillis, ...).

--players is a ratings CSV of today's free agents (madden_tools.py makes one from madden.tools,
which lists them; EA's ratings site does not). Each takes over a free-agent record, one of his
position first, exactly as import_roster.py fills the teams' records: name, body, ratings, a
portrait id of his own (the record's 2008 one where nobody else has it), and no contract, like the
game's own free agents. --report lists them in import_roster.py's report format, for
fetch_portraits.py --keep, which installs their pictures and matches their heads.

The records left over get a made-up name (drawn from the lists below, never one a real player,
2008 or current, has, never two the same) and the generic portrait. Their 2008 ratings are kept.
Run it on the roster import_coaches.py wrote, before any earlier run of this (it needs the 2008
records' portrait ids); it changes the file in place unless --out.
"""

import argparse
import collections
import csv
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402
import import_roster as importer  # noqa: E402

FREE_AGENTS = 1009

FIRST = """Aaron Adrian Aiden Alex Andre Andrew Anthony Antonio Ashton Austin Avery Bailey Blake
Brady Brandon Brayden Brennan Brock Bryce Byron Caden Calvin Camden Carson Carter Cedric Chance
Chase Christian Clayton Cody Colby Cole Colton Connor Cooper Corey Cortez Curtis Dakota Dalton
Damian Damon Dante Darius Darnell Darren Davion Dawson Deandre Declan Demarcus Deon Derek Desmond
Devin Devonte Dexter Dominic Donovan Drake Dylan Easton Eli Elijah Emmanuel Eric Ethan Evan
Ezra Garrett Gavin Grady Graham Grant Griffin Hayden Hunter Isaac Isaiah Jace Jackson Jalen
Jamal Jamari Jameson Jaden Jarrett Jason Javon Jaylen Jaxon Jeremiah Jermaine Jordan Josiah
Julian Justin Kaden Kaleb Kameron Keenan Keith Kendall Kendrick Keon Kevin Khalil Kieran Kobe
Kyler Lamar Landon Lane Lawson Levi Logan Lucas Luke Malik Marcus Mason Mateo Maxwell Micah
Miles Nash Nathan Nico Noah Nolan Omar Owen Parker Peyton Preston Quentin Quinton Rashad Reggie
Reid Remy Rhett Ricky Riley Rodney Roman Ryder Sawyer Silas Spencer Sterling Tanner Tariq Terrell
Theo Tobias Trent Trevon Tristan Troy Tucker Tyler Tyrell Tyson Victor Wade Warren Wesley Xavier
Zach Zane""".split()

LAST = """Abbott Acosta Adair Alston Ambrose Archer Atwood Avery Baines Baldwin Banks Barlow Barrett
Baxter Beasley Beckett Bell Benson Bishop Blackwell Blair Bolden Bowers Boyd Bradford Bratton
Brewer Bridges Briggs Brock Brooks Buckley Burke Burrell Byers Calhoun Callahan Carrington
Carver Cassidy Chambers Chandler Childs Clayton Cobb Colvin Conley Copeland Corbin Crane
Crawford Crosby Cullen Dalton Daniels Darby Dawkins Deacon Decker Dennison Dillard Dixon
Donnelly Dorsey Draper Driscoll Dudley Dunbar Duncan Eaton Ellison Emery Espinoza Farley Faulk
Fenwick Fields Finley Fitzgerald Fleming Fontaine Forbes Ford Fowler Frazier Gaines Galloway
Gentry Gibbs Gilmore Glover Goodwin Granger Grayson Greer Griffith Haley Halsey Hammond Hardin
Harmon Hartley Hatcher Hawkins Hayes Henley Hensley Hickman Hines Holloway Holt Hopkins Horton
Huff Hughes Humphrey Ingram Irwin Jacobs Jarvis Jennings Kaiser Keller Kendrick Kerr Kimble
Kirby Knox Lambert Landry Langston Larkin Lawson Leach Lester Lindsey Lockhart Lowery Lyons
Maddox Malone Manning Marsh Mathis Maxey McBride McCall McCoy McKnight Mercer Merritt Monroe
Morrow Mullins Nash Nelson Noble Norwood Oakley Odom Oliver Osborne Pace Padilla Parrish Patton
Payne Pearce Pennington Pittman Pope Prescott Pruitt Quarles Radford Ramsey Randle Ransom
Reese Rhodes Riddle Rivers Roach Rollins Rowe Rucker Salazar Sanders Sawyer Sellers Shaffer
Sheppard Shields Simms Sims Skinner Slater Solomon Spears Stafford Stanton Stokes Strickland
Sutton Swanson Talbot Tate Teague Thornton Tillman Townsend Trammell Tucker Underwood Vance
Vaughn Wade Walton Ware Watkins Weaver Webster Whitfield Wilder Winslow Wooten Yates York""".split()


def build_free_agents(roster, out, seed, players_path=None, report_path=None):
    save = SaveFile(roster)
    play = save.db.tables["PLAY"]
    records = [i for i in range(play.count) if play.get(i, "TGID") == FREE_AGENTS]
    real = importer.load_players(players_path) if players_path else []
    # Not one who is on a team in this roster (signed since the ratings were taken).
    rostered_names = {importer.name_key(f"{play.get(i, 'PFNA')} {play.get(i, 'PLNA')}") for i in range(play.count)
                      if 1 <= play.get(i, "TGID") <= 32}
    signed = [p for p in real if importer.name_key(f"{p['first']} {p['last']}") in rostered_names]
    if signed:
        print(f"{len(signed)} of the free agents are on a team in the roster, left out")
    real = [p for p in real if p not in signed]
    real.sort(key=lambda p: -p["stats"]["overall"])
    if len(real) > len(records):
        print(f"{len(real) - len(records)} free agents left out: the pool has {len(records)} records")
        real = real[:len(records)]

    # Real free agents into records of their position first, then any.
    available = list(records)
    chosen = {}
    for p in real:
        match = next((i for i in available if play.get(i, "PPOS") == p["position"]), None)
        if match is not None:
            available.remove(match)
            chosen[id(p)] = match
    for p in real:
        if id(p) not in chosen:
            chosen[id(p)] = available.pop(0)
    assignments = [(chosen[id(p)], p) for p in real]
    for i, p in assignments:
        importer.write_player(play, i, p, FREE_AGENTS)
        for field in importer.CONTRACT_FIELDS:  # unsigned
            play.set(i, field, 0)
    # Portrait ids: the rostered players keep theirs, each real free agent gets one of his own.
    # Only ids the game's portrait library has can show a picture, and those are the ids the 2008
    # roster uses: the ones no rostered player holds go to the best free agents (the retired
    # legends' too, who then show the silhouette); the rest get the silhouette.
    counts = collections.Counter(play.get(i, "PSXP") for i in range(play.count))
    generic = max((k for k in counts if k), key=lambda k: counts[k])
    rostered = [i for i in range(play.count) if 1 <= play.get(i, "TGID") <= 32
                and play.get(i, "PSXP") not in (0, generic)]  # the silhouette ones stay so
    held = {play.get(i, "PSXP") for i in rostered}
    room = sum(1 for k, n in counts.items() if k and n == 1 and k not in held)
    pictured = [i for i, p in assignments if p["avatar"]][:room]
    unpictured = [i for i, _ in assignments if i not in set(pictured)]
    importer.assign_portrait_ids(play, rostered + pictured, unpictured)
    pictured = set(pictured)

    # The rest: made-up names, the generic portrait.
    taken = {(play.get(i, "PFNA"), play.get(i, "PLNA")) for i in range(play.count)}
    counts = collections.Counter(play.get(i, "PSXP") for i in range(play.count))
    generic = max((k for k in counts if k), key=lambda k: counts[k])
    rng = random.Random(seed)
    for i in available:
        while True:
            name = (rng.choice(FIRST), rng.choice(LAST))
            if name not in taken:
                break
        taken.add(name)
        play.set(i, "PFNA", name[0])
        play.set(i, "PLNA", name[1])
        play.set(i, "PSXP", generic)  # no 2008 player's photo
    save.fix_checksums()
    save.save(out)
    print(f"{len(assignments)} real free agents ({len(pictured)} with their picture), {len(available)} made up; "
          f"wrote {out}")

    if report_path:
        with open(report_path, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f)
            w.writerow(["pgid", "portrait_id", "madden_id", "first", "last", "team", "position", "overall", "avatarUrl"])
            for i, p in assignments:
                w.writerow([play.get(i, "PGID"), play.get(i, "PSXP"), p["id"], p["first"], p["last"], FREE_AGENTS,
                            importer.POSITION_NAMES[p["position"]], p["stats"]["overall"],
                            p["avatar"] if i in pictured else ""])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", required=True)
    parser.add_argument("--players", help="ratings CSV of today's free agents (madden_tools.py)")
    parser.add_argument("--report", help="write the real free agents with their player and portrait ids")
    parser.add_argument("--out", help="defaults to --roster (in place)")
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    build_free_agents(args.roster, args.out or args.roster, args.seed, args.players, args.report)
