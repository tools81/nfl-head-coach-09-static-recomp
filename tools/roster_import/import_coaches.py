"""Puts current coaching staffs into an NFL Head Coach 09 roster save.

    python tools/roster_import/import_coaches.py --roster tools/roster_import/data/ROSTER-M27 \
        --coaches tools/roster_import/data/coaches --heads tools/roster_import/data/coaches.csv \
        --iso "iso/NFL Head Coach 09.iso"

Each coach is a photo in the coaches folder named <team>_<role>_<first>_<last>.png, for example
bears_hc_Ben_Johnson.png (team nickname, lower case; role: hc head coach, oc offensive
coordinator, dc defensive coordinator, sc special teams coordinator). The coach takes over the
record of that team's coach in that role: name, and the 3D head given in the heads CSV
(columns team, role, head), as one of

    preset N     create-a-coach appearance N (1-165), as listed in the game's Appearance menu
    likeness N   the head of a 2008 head coach (roster head id 0-36)

A preset's head, skin and build are read from the game's own preset table on the disc image
(--iso); a likeness head keeps the skin and build of the 2008 coach it belongs to. A coach without
a heads row gets a preset of his skin tone (light or dark, measured on his photo's face) when
--iso is given, and otherwise keeps his record's head.

The photos also replace the coaches' pictures that HC27 draws (in hc27/textures/swappables): each
coach's portrait on the career screens such as Staff Status (CoachPortraits/<portrait id>.png,
head and shoulders), and for head coaches the cut-out pictures beside the team on Coach Now's
Team Select (TeamSelect/<2 x team id - 1>.png at home and <2 x team id - 2>.png away). Those are
made from the photo by removing its plain background (a photo with transparency is used as it
is) and framing the coach like the game's own pictures, which needs Pillow (pip install pillow).
With --ratings, each coach listed (columns team, role, then any of age, overall, offense,
defense, knowledge, motivation, charisma, ethic, approval, scheme (43 or 34), wins, losses, ties,
po_wins, po_losses, po_seasons, sb, win_seasons, years, strengths, salary, contract) also gets
those ratings, that career record and that contract; strengths lists the position groups he
develops best (qb rb wr ol dl lb db s k p), salary is in dollars a year and contract the seasons
left on it. Edits the roster in place unless --out is given (for HC27: --out hc27/ROSTER).
"""

import argparse
import csv
import mmap
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb import Database, SaveFile  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

ROLES = {"hc": 0, "oc": 1, "dc": 2, "sc": 3}  # file name role -> COCH.COPS
# File name team -> its other name in TEAM.TDNA (lower case): import_roster.py renames the Redskins.
TEAM_ALIASES = {"commanders": "redskins", "redskins": "commanders"}
LIKENESS_HEADS = 37  # roster head ids 0-36 are the 2008 head coaches' own heads
PHOTO = re.compile(r"^([a-z0-9]+)_([a-z]+)_(.+)\.png$", re.IGNORECASE)

# Coach Now's Team Select pictures: 512x512 cut-outs from the top down to a flat cut, as the game's
# own. Each team has two: TeamSelect/<2 x team id - 1> for the home side (the coach right of centre,
# like Tony Dungy's) and TeamSelect/<2 x team id - 2> for the away side (left, like Scott Linehan's).
TEAM_SELECT_SIZE = 512
TEAM_SELECT_HOME = (190, 10, 512, 321)  # left, top, right, bottom of the coach
TEAM_SELECT_AWAY = (31, 10, 353, 321)
# Every coach is framed by his head: this wide (ears included), its top at the top of the box,
# centred on the box; below it the photo is cut at the bottom of the box (one too short to reach
# it is made larger until it does).
TEAM_SELECT_HEAD_WIDTH = 140

# The coach portraits (Staff Status and the other career screens): 256x256 cut-outs, head and
# shoulders down to the bottom edge, framed like Lovie Smith's: the head 100 pixels wide (ears
# included), centred, its top 42 pixels down.
PORTRAIT_SIZE = 256
PORTRAIT_HEAD_WIDTH = 100
PORTRAIT_HEAD_TOP = 42
PORTRAIT_HEAD_TOP_LOWEST = 64  # for a photo that ends too soon to reach the bottom edge

# Coach skin (CSKI): 0 light, 1 dark. Measured on the portrait as for players (fetch_portraits.py),
# with its own threshold: photos of real people measure warmer than the Madden images.
SKIN_LIGHT, SKIN_DARK = 0, 1
DARK_BELOW = 19


def read_presets(iso_path):
    """Create-a-coach appearance presets from the disc image: the CCMS table (one record per
    preset, in menu order) of the game database that holds it."""
    with open(iso_path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        at = m.find(b"SMCC")  # the table name in the database directory, stored reversed
        while at >= 0:
            start = m.rfind(b"DB\x00\x08", max(0, at - 0x10000), at)
            if start >= 0:
                size = struct.unpack_from(">I", m, start + 8)[0]
                try:
                    table = Database(bytearray(m[start:start + size]), 0).tables.get("CCMS")
                except (ValueError, struct.error):
                    table = None
                if table is not None:
                    return [{f: table.get(i, f) for f in ("CFSH", "CSKI", "CBSZ")} for i in range(table.count)]
            at = m.find(b"SMCC", at + 4)
    raise SystemExit(f"no create-a-coach presets found in {iso_path}")


def parse_photo(name):
    match = PHOTO.match(name)
    if not match:
        return None
    team, role, rest = match.groups()
    first, _, last = rest.partition("_")
    return team.lower(), role.lower(), first.replace("_", " "), last.replace("_", " ")


def remove_background(image, tolerance=40):
    """Clears the plain light background around the coach: the light pixels connected to the
    photo's top and side edges (not its bottom, where a white shirt may touch it)."""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
    image = image.convert("RGBA")
    if image.getextrema()[3][0] < 255:
        return image  # already has transparency
    width, height = image.size
    marked = image.convert("RGB")
    key = (255, 0, 255)
    edges = [(x, 0) for x in range(0, width, 8)] + [(x, y) for y in range(0, height, 8) for x in (0, width - 1)]
    for seed in edges:
        pixel = marked.getpixel(seed)
        if pixel != key and min(pixel) > 255 - 2 * tolerance:
            ImageDraw.floodfill(marked, seed, key, thresh=tolerance)
    kept = ImageChops.difference(marked, Image.new("RGB", marked.size, key)).convert("L").point(lambda v: 255 if v else 0)
    # Pull the edge in a pixel and soften it, so no light fringe is left around the coach.
    image.putalpha(kept.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1)))
    return image


def cut_out(photo_path):
    """The coach cut out of his photo, and the bounding box of what is left."""
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("coach pictures need Pillow: pip install pillow")
    coach = remove_background(Image.open(photo_path))
    box = coach.getchannel("A").point(lambda v: 255 if v > 32 else 0).getbbox()
    if not box:
        raise SystemExit(f"{photo_path}: nothing left after removing the background")
    return coach, box


def find_head(coach, box):
    """Left and right of the head at its widest: the widest row from the top down to the neck,
    where the outline narrows and then widens past the head into the shoulders."""
    alpha = coach.getchannel("A").point(lambda v: 255 if v > 128 else 0)
    pixels = alpha.load()
    head, narrowed = None, False
    smallest_head = 0.3 * (box[2] - box[0])  # narrower is the top of the hair (a lock, a parting)
    for y in range(box[1], box[3]):
        xs = [x for x in range(box[0], box[2]) if pixels[x, y]]
        if not xs:
            continue
        span = (xs[0], xs[-1] + 1)
        width = span[1] - span[0]
        if head and head[1] - head[0] >= smallest_head and width < 0.9 * (head[1] - head[0]):
            narrowed = True
        elif narrowed and width > head[1] - head[0]:
            return head
        if not narrowed and (not head or width > head[1] - head[0]):
            head = span
    # No shoulders below the neck (a close-up cut off at the chin or neck): the head is the widest
    # part found.
    return head or (box[0], box[2])


def portrait_picture(photo_path, out_path):
    """The coach cut out of his photo and framed as the game frames its own coach portraits."""
    from PIL import Image
    coach, box = cut_out(photo_path)
    head_left, head_right = find_head(coach, box)
    scale = PORTRAIT_HEAD_WIDTH / (head_right - head_left)
    top = PORTRAIT_HEAD_TOP
    # A photo that ends at the shoulders or chest has to reach the bottom of the picture: the coach
    # is moved down a little, then scaled up.
    if box[3] == coach.height and top + (box[3] - box[1]) * scale < PORTRAIT_SIZE:
        top = min(PORTRAIT_HEAD_TOP_LOWEST, round(PORTRAIT_SIZE - (box[3] - box[1]) * scale))
        scale = max(scale, (PORTRAIT_SIZE - top) / (box[3] - box[1]))
    coach = coach.resize((max(1, round(coach.width * scale)), max(1, round(coach.height * scale))), Image.LANCZOS)
    x = round(PORTRAIT_SIZE / 2 - (head_left + head_right) / 2 * scale)
    y = round(top - box[1] * scale)
    picture = Image.new("RGBA", (PORTRAIT_SIZE, PORTRAIT_SIZE), (0, 0, 0, 0))
    picture.alpha_composite(coach.crop((-x, -y, PORTRAIT_SIZE - x, PORTRAIT_SIZE - y)))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    picture.save(out_path)


def team_select_picture(photo_path, out_path, placement):
    """The coach cut out of his photo, placed (box left, top, right, bottom) as the game places its
    own Team Select pictures."""
    from PIL import Image
    coach, box = cut_out(photo_path)
    head_left, head_right = find_head(coach, box)
    left, top, right, bottom = placement
    # A photo that would end above the bottom of the box (a close-up) is made larger to fill it.
    scale = max(TEAM_SELECT_HEAD_WIDTH / (head_right - head_left), (bottom - top) / (box[3] - box[1]))
    coach = coach.resize((max(1, round(coach.width * scale)), max(1, round(coach.height * scale))), Image.LANCZOS)
    # The head's top at the top of the box and its middle at the box's middle; whatever falls
    # outside the box (below it, or a wide shot's arms past the picture's edges) is cut off.
    x = round((left + right) / 2 - (head_left + head_right) / 2 * scale)
    y = round(top - box[1] * scale)
    picture = Image.new("RGBA", (TEAM_SELECT_SIZE, TEAM_SELECT_SIZE), (0, 0, 0, 0))
    picture.alpha_composite(coach.crop((-x, -y, TEAM_SELECT_SIZE - x, bottom - y)))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    picture.save(out_path)


def skin_preset(portrait_path, presets, name):
    """A create-a-coach preset of the skin tone measured on the coach's portrait, picked by his
    name so that every run gives him the same one; None if the face cannot be measured."""
    from fetch_portraits import skin_angle
    angle = skin_angle(portrait_path)
    if angle is None:
        return None
    skin = SKIN_DARK if angle < DARK_BELOW else SKIN_LIGHT
    choices = [n + 1 for n, preset in enumerate(presets) if preset["CSKI"] == skin]
    return "preset", choices[zlib.crc32(name.encode()) % len(choices)]


def assign_portrait_ids(coch, imported):
    """Gives every imported coach a portrait id (PNPt) of his own, so his picture replaces only his
    portrait. Some 2008 coaches have none (the generic picture, shared by many records) or share
    one; those imported coaches take the id of a free agent's 2008 portrait, and that free agent
    gets the generic picture."""
    counts = {}
    for i in range(coch.count):
        counts[coch.get(i, "PNPt")] = counts.get(coch.get(i, "PNPt"), 0) + 1
    generic = max(counts, key=lambda k: counts[k])
    imported_set = set(imported)
    spare = [i for i in range(coch.count) if i not in imported_set and coch.get(i, "TGID") > 32
             and counts[coch.get(i, "PNPt")] == 1 and coch.get(i, "PNPt") != generic]
    taken = set()
    for i in imported:
        pid = coch.get(i, "PNPt")
        if pid == generic or counts[pid] > 1 or pid in taken:
            if not spare:
                raise SystemExit("ran out of coach portrait ids")
            donor = spare.pop(0)
            pid = coch.get(donor, "PNPt")
            coch.set(donor, "PNPt", generic)
            coch.set(i, "PNPt", pid)
        taken.add(pid)
    # Coaches not imported who still share an id with an imported one would show his picture.
    for i in range(coch.count):
        if i not in imported_set and coch.get(i, "PNPt") in taken:
            coch.set(i, "PNPt", generic)


# The --ratings CSV's columns -> COCH fields. Ratings are 0-99; the record counts are careers
# (regular season, playoffs), years the coach has been with his team, and his age; contract is
# the seasons left on his deal.
RATING_FIELDS = {
    "age": "CAGE", "overall": "POVS", "offense": "COFF", "defense": "CDEF", "knowledge": "CKNW",
    "motivation": "CMOT", "charisma": "CCHM", "ethic": "CETH", "approval": "HDAp",
    "wins": "CCWI", "losses": "CCLO", "ties": "CCTI", "po_wins": "CCPW", "po_losses": "CCPL",
    "po_seasons": "CCPM", "sb": "CSBW", "win_seasons": "CCWS", "years": "CCYT",
    "contract": "CCLN",
}
SCHEMES = {"43": 95, "34": 5}  # CDTY: the base defense, 4-3 or 3-4
# Position groups -> the coach's rating for developing them, and which side they belong to.
POSITION_FIELDS = {
    "qb": ("CRQB", "offense"), "rb": ("CRRB", "offense"), "wr": ("CRWR", "offense"), "ol": ("CROL", "offense"),
    "dl": ("CRDL", "defense"), "lb": ("CRLB", "defense"), "db": ("CRDB", "defense"), "s": ("CRSA", "defense"),
    "k": ("CRKS", None), "p": ("CRPS", None),
}
SKILL_FIELDS = ("SKPA", "SKPC", "SKPF", "SKCM", "SKSM", "SKCR", "SKST", "SKPX")  # 1-5 each


def load_ratings(path):
    ratings = {}
    if not path:
        return ratings
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            key = (row["team"].strip().lower(), row["role"].strip().lower())
            values = {}
            for column in list(RATING_FIELDS) + ["scheme", "strengths", "salary"]:
                value = (row.get(column) or "").strip()
                if not value:
                    continue
                if column == "strengths":
                    unknown = [p for p in value.split() if p not in POSITION_FIELDS]
                    if unknown:
                        raise SystemExit(f"{path}: {key}: unknown position group(s) {unknown}")
                    values[column] = value.split()
                elif column == "scheme":
                    if value not in SCHEMES:
                        raise SystemExit(f"{path}: {key}: scheme must be 43 or 34")
                    values[column] = value
                elif not value.isdigit():
                    raise SystemExit(f"{path}: {key}: {column} {value!r} is not a number")
                else:
                    values[column] = int(value)
            ratings[key] = values
    return ratings


def apply_ratings(coch, record, values):
    """Writes a coach's ratings and record. The position groups he develops follow his offense
    and defense ratings (kickers and punters a low base), his strengths ten points better; his
    skills (1-5) follow his overall rating."""
    for column, field in RATING_FIELDS.items():
        if column in values:
            limit = 99 if column in ("overall", "offense", "defense", "knowledge", "motivation", "charisma",
                                     "ethic", "approval") else 7 if column == "contract" else None
            coch.set(record, field, min(values[column], limit) if limit else values[column])
    if "scheme" in values:
        coch.set(record, "CDTY", SCHEMES[values["scheme"]])
    if "salary" in values:  # CSAL is in $10,000s
        coch.set(record, "CSAL", min(16383, round(values["salary"] / 10000)))
    offense = values.get("offense", coch.get(record, "COFF"))
    defense = values.get("defense", coch.get(record, "CDEF"))
    strengths = values.get("strengths", [])
    if "offense" in values or "defense" in values or strengths:
        for group, (field, side) in POSITION_FIELDS.items():
            base = {"offense": offense, "defense": defense}.get(side, 40) - 8
            if group in strengths:
                base = max(base, offense if side == "offense" else defense if side == "defense"
                           else values.get("overall", 70)) + 10
            coch.set(record, field, max(35, min(99, base)))
    if "overall" in values:
        level = max(1, min(5, round(1 + values["overall"] * 4 / 99)))
        for field in SKILL_FIELDS:
            coch.set(record, field, level)


def load_heads(path):
    heads = {}
    if not path:
        return heads
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            kind, _, number = row["head"].strip().lower().partition(" ")
            if kind not in ("preset", "likeness") or not number.strip().isdigit():
                raise SystemExit(f"{path}: head {row['head']!r} is not 'preset N' or 'likeness N'")
            heads[(row["team"].strip().lower(), row["role"].strip().lower())] = (kind, int(number))
    return heads


def import_coaches(roster_path, coaches_dir, heads_path, iso_path, out_path, textures_dir, ratings_path=None):
    save = SaveFile(roster_path)
    coch, team_table = save.db.tables["COCH"], save.db.tables["TEAM"]
    teams = {team_table.get(i, "TDNA").lower(): team_table.get(i, "TGID") for i in range(team_table.count)}
    heads = load_heads(heads_path)
    ratings = load_ratings(ratings_path)
    presets = []
    if any(kind == "preset" for kind, _ in heads.values()) and not iso_path:
        raise SystemExit("preset heads need --iso (the game's disc image)")
    if iso_path:
        presets = read_presets(iso_path)

    # The 2008 head coaches' own skin and build go with their likeness heads.
    likeness = {}
    for i in range(coch.count):
        head = coch.get(i, "CFSH")
        if head < LIKENESS_HEADS and coch.get(i, "COPS") == 0:
            likeness.setdefault(head, {"CFSH": head, "CSKI": coch.get(i, "CSKI"), "CBSZ": coch.get(i, "CBSZ")})

    coaches = []
    for name in sorted(os.listdir(coaches_dir)):
        parsed = parse_photo(name)
        if not parsed:
            continue
        team, role, first, last = parsed
        tgid = teams.get(team, teams.get(TEAM_ALIASES.get(team)))
        if tgid is None or tgid > 32:
            raise SystemExit(f"{name}: unknown team {team!r}")
        if role not in ROLES:
            raise SystemExit(f"{name}: unknown role {role!r} (known: {', '.join(ROLES)})")
        record = next((i for i in range(coch.count)
                       if coch.get(i, "TGID") == tgid and coch.get(i, "COPS") == ROLES[role]), None)
        if record is None:
            raise SystemExit(f"{name}: the roster has no {role} for the {team}")
        coaches.append((name, team, role, first, last, tgid, record))
    assign_portrait_ids(coch, [coach[-1] for coach in coaches])

    # The folder holds only the pictures of this run: one left from an earlier run could be a
    # portrait id that is no longer this coach's.
    portraits_dir = os.path.join(textures_dir, "CoachPortraits")
    os.makedirs(portraits_dir, exist_ok=True)
    for old in os.listdir(portraits_dir):
        if old.lower().endswith(".png"):
            os.remove(os.path.join(portraits_dir, old))
    done = 0
    for name, team, role, first, last, tgid, record in coaches:
        coch.set(record, "CFNM", first)
        coch.set(record, "CLNM", last)
        if (team, role) in ratings:
            apply_ratings(coch, record, ratings[(team, role)])
        photo = os.path.join(coaches_dir, name)
        portrait = os.path.join(portraits_dir, f"{coch.get(record, 'PNPt')}.png")
        portrait_picture(photo, portrait)
        choice = heads.get((team, role))
        if not choice and presets:
            choice = skin_preset(portrait, presets, f"{first} {last}")
        if choice:
            kind, number = choice
            if kind == "preset":
                if not 1 <= number <= len(presets):
                    raise SystemExit(f"{name}: preset {number} is not 1-{len(presets)}")
                look = presets[number - 1]
            else:
                if number not in likeness:
                    raise SystemExit(f"{name}: likeness {number} is not a 2008 head coach's head")
                look = likeness[number]
            for field, value in look.items():
                coch.set(record, field, value)
        if role == "hc":
            team_select_picture(photo, os.path.join(textures_dir, "TeamSelect", f"{2 * tgid - 1}.png"), TEAM_SELECT_HOME)
            team_select_picture(photo, os.path.join(textures_dir, "TeamSelect", f"{2 * tgid - 2}.png"), TEAM_SELECT_AWAY)
        print(f"{first} {last}: {team} {role}" + (f", {choice[0]} {choice[1]}" if choice else ""))
        done += 1

    save.fix_checksums()
    save.save(out_path or roster_path)
    print(f"{done} coaches imported into {out_path or roster_path}; pictures in {textures_dir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--roster", required=True, help="the roster save to update (import_roster.py --out)")
    parser.add_argument("--coaches", default=os.path.join(HERE, "data", "coaches"), help="folder of coach photos")
    parser.add_argument("--heads", help="CSV of team, role, head ('preset N' or 'likeness N')")
    parser.add_argument("--iso", help="the game's disc image (needed for preset heads)")
    parser.add_argument("--ratings", help="CSV of each coach's ratings and career record (see apply_ratings)")
    parser.add_argument("--out", help="write here instead of updating --roster")
    parser.add_argument("--textures", default=os.path.join(ROOT, "hc27", "textures", "swappables"),
                        help="where the coaches' pictures go (CoachPortraits and TeamSelect folders)")
    args = parser.parse_args()
    import_coaches(args.roster, args.coaches, args.heads, args.iso, args.out, args.textures, args.ratings)
