"""The teams' colours in the game's menus (TEAM.TBC* the primary, TB2* the secondary), where they
changed since 2008. import_roster.py sets them; team_uniforms.py --roster sets them in a roster save.
"""

# By TEAM.TDNA.
TEAM_COLOURS = {
    "Seahawks": ("#002244", "#69BE28"),
    "Rams": ("#003594", "#FFA300"),
    "Bills": ("#00338D", "#C60C30"),
    "Dolphins": ("#008E97", "#FC4C02"),
    "Chargers": ("#0080C6", "#FFC20E"),
    "Jets": ("#046A38", "#FFFFFF"),
    "49ers": ("#AA0000", "#B3995D"),
    "Broncos": ("#FB4F14", "#002244"),
    "Jaguars": ("#006778", "#D7A22A"),
    "Browns": ("#FF3C00", "#311D00"),
}


def set_team_colours(team_table):
    for i in range(team_table.count):
        colours = TEAM_COLOURS.get(team_table.get(i, "TDNA"))
        if not colours:
            continue
        for prefix, colour in zip(("TBC", "TB2"), colours):
            r, g, b = (int(colour[k:k + 2], 16) for k in (1, 3, 5))
            team_table.set(i, prefix + "R", r)
            team_table.set(i, prefix + "G", g)
            team_table.set(i, prefix + "B", b)
