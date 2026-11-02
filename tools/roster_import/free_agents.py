"""Turns the roster's free agents into made-up players.

    python tools/roster_import/free_agents.py --roster hc27/ROSTER [--out hc27/ROSTER] [--seed 2026]

After import_roster.py the free-agent pool (TGID 1009) is the 2008 game's: its own free agents,
its 2008 draft class and the 2008 players the import left over (Keith Rivers, Peyton Hillis, ...).
This gives each of them a made-up name (first and last name drawn from the lists below, never one
an imported player has, never two the same) and the game's generic portrait, so the pool reads
as present-day unknowns. Ratings, ages, positions, bodies and colleges stay as they are. Run it
after import_coaches.py has written hc27/ROSTER (it changes the file in place unless --out).
"""

import argparse
import collections
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import SaveFile  # noqa: E402

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


def rename_free_agents(roster, out, seed):
    save = SaveFile(roster)
    play = save.db.tables["PLAY"]
    fa = [i for i in range(play.count) if play.get(i, "TGID") == FREE_AGENTS]
    taken = {(play.get(i, "PFNA"), play.get(i, "PLNA")) for i in range(play.count)}  # nobody real, 2008 or now
    counts = collections.Counter(play.get(i, "PSXP") for i in range(play.count))
    generic = max((k for k in counts if k), key=lambda k: counts[k])
    rng = random.Random(seed)
    for i in fa:
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
    print(f"{len(fa)} free agents renamed; wrote {out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", required=True)
    parser.add_argument("--out", help="defaults to --roster (in place)")
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    rename_free_agents(args.roster, args.out or args.roster, args.seed)
