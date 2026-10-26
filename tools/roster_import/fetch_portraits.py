"""Installs player portraits for an imported roster.

    python tools/roster_import/fetch_portraits.py --report tools/roster_import/data/players.csv

Each player's image (the CSV's avatarUrl) is downloaded once into the cache folder, named by the
ratings site's player id, then copied into the game's replacement folder as <portrait id>.png,
the name HC27 looks up for that player (hc27/textures/swappables/PlayerPortraits). Portrait ids
can change each time the roster is rebuilt, so the folder is rebuilt from the report every run:
images no longer in the report are removed. Re-running after an interruption only downloads what
is missing.

With --roster, that roster save is updated too:
- Players whose image cannot be had are switched to the game's generic silhouette (otherwise they
  would show the 2008 portrait their id came from).
- Every player with an image gets a 3D head that matches the skin tone in his image, instead of
  the head of the 2008 player whose record he took over. The game's generic heads come in three
  skin tones (PSKI 0 light, 1 dark, 2 medium). The tone is measured on the face in the image (the
  individual typology angle of the lit skin between forehead and cheeks). The head is one the 2008
  roster gives players of that tone, picked by the player's ratings site id so that a rebuilt
  roster gives each player the same head. The 2008 stars' scanned faces (PUHE) are cleared.
  Skin colour alone cannot tell every player's tone (a light-skinned black player can measure
  like a tanned white one), so --looks names players to set by hand: a CSV with columns first,
  last, skin (light, medium or dark) and, optionally, head (a PGHE number).
  This needs Pillow (pip install pillow).
"""

import argparse
import csv
import math
import os
import shutil
import time
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Skin tone (PSKI) by individual typology angle, in degrees: below DARK dark, below MEDIUM
# medium, light above. Set by sorting the Madden 27 images by angle and looking at them.
SKIN_DARK, SKIN_LIGHT, SKIN_MEDIUM = 1, 0, 2
DARK_BELOW = 22
MEDIUM_BELOW = 36


def skin_angle(path):
    """Individual typology angle of the face in a portrait image (centred, as the ratings site
    crops them): atan((L* - 50) / b*) over the lit skin between forehead and cheeks, or None."""
    from PIL import Image, ImageCms
    image = Image.open(path).convert("RGBA")
    width, height = image.size
    region = image.crop((round(width * .36), round(height * .30), round(width * .64), round(height * .50)))
    to_lab = ImageCms.buildTransform(ImageCms.createProfile("sRGB"), ImageCms.createProfile("LAB"), "RGB", "LAB")
    lab = ImageCms.applyTransform(region.convert("RGB"), to_lab)
    pixels, alphas = lab.tobytes(), region.getchannel("A").tobytes()
    skin = []
    for n, alpha in enumerate(alphas):
        lightness = pixels[3 * n] * 100 / 255
        a, b = (v - 256 if v > 127 else v for v in pixels[3 * n + 1: 3 * n + 3])  # stored signed
        if alpha > 200 and a > 3 and b > 3:  # reddish and yellowish: skin, not hair or eyes
            skin.append((lightness, b))
    if len(skin) < 50:
        return None
    skin.sort()
    lit = skin[len(skin) // 2: len(skin) * 9 // 10]  # above the shadows, below the highlights
    lightness = sum(s[0] for s in lit) / len(lit)
    yellow = sum(s[1] for s in lit) / len(lit)
    return math.degrees(math.atan2(lightness - 50, yellow))


def skin_tone(angle):
    return SKIN_DARK if angle < DARK_BELOW else SKIN_MEDIUM if angle < MEDIUM_BELOW else SKIN_LIGHT


def heads(spec):
    """'a-b c ...' -> [a, ..., b, c, ...]"""
    numbers = []
    for part in spec.split():
        first, _, last = part.partition("-")
        numbers.extend(range(int(first), int(last or first) + 1))
    return numbers


# The generic heads (PGHE) the 2008 roster gives players of each skin tone, and their hair colour
# (PHCL): black (0) for the dark and medium heads; the light heads' as the 2008 players have it,
# brown (2) unless listed.
TONE_HEADS = {
    SKIN_LIGHT: heads("0-1 3-6 10 15-37 44-48 50 57 63 65 154-175"),
    SKIN_DARK: heads("43 59-61 66-67 71 74-81 86 88-92 94 96-101 103 105 107 109-111 116 118-119 123-126 "
                     "128-134 141 143-145 149-150 176-217 219-226 228-233"),
    SKIN_MEDIUM: heads("65-66 77 175-178"),
}
LIGHT_HAIR = {**{head: 0 for head in heads("4 15-16 18-20 32-33 37 47 50 63 65 154 159 162 172")},
              6: 1, 155: 4, 157: 4}


TONE_NAMES = {"light": SKIN_LIGHT, "dark": SKIN_DARK, "medium": SKIN_MEDIUM}


def load_looks(path):
    """The --looks CSV: (first, last) in lower case -> (skin tone, head or None)."""
    looks = {}
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            skin = row["skin"].strip().lower()
            head = (row.get("head") or "").strip()
            if skin not in TONE_NAMES or (head and not head.isdigit()):
                raise SystemExit(f"{path}: {row['first']} {row['last']}: skin must be light, medium or dark "
                                 f"and head a number")
            looks[(row["first"].strip().lower(), row["last"].strip().lower())] = (TONE_NAMES[skin],
                                                                                 int(head) if head else None)
    return looks


def match_heads(play, images, looks):
    """Gives each player in images (PGID -> (ratings site id, image path)) a generic head of his
    skin tone, or the skin tone and head looks (see load_looks) gives him."""
    counts = {}
    for i in range(play.count):
        site_id, path = images.get(play.get(i, "PGID"), (0, None))
        tone, head = looks.get((play.get(i, "PFNA").lower(), play.get(i, "PLNA").lower()), (None, None))
        if tone is None:
            angle = skin_angle(path) if path else None
            if angle is None:
                continue
            tone = skin_tone(angle)
        if head is None:
            choices = TONE_HEADS[tone]
            head = choices[site_id * 2654435761 % 2 ** 32 % len(choices)]  # the same head every rebuild
        hair = LIGHT_HAIR.get(head, 2) if tone == SKIN_LIGHT else 0
        play.set(i, "PSKI", tone)
        play.set(i, "PGHE", head)
        play.set(i, "PHCL", hair)
        play.set(i, "PUHE", 0)
        counts[tone] = counts.get(tone, 0) + 1
    return counts


def download(url, path):
    request = urllib.request.Request(url, headers={"User-Agent": "hc09-roster-import"})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read()
    if not data.startswith(b"\x89PNG"):
        raise ValueError("not a PNG")
    with open(path + ".part", "wb") as out:
        out.write(data)
    os.replace(path + ".part", path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--report", required=True, help="import_roster.py --report output")
    parser.add_argument("--cache", default=os.path.join(HERE, "data", "portraits"))
    parser.add_argument("--out", default=os.path.join(ROOT, "hc27", "textures", "swappables", "PlayerPortraits"))
    parser.add_argument("--roster", help="the imported roster save: players without an image get the silhouette")
    parser.add_argument("--looks", help="CSV of first, last, skin (light, medium or dark) and optionally head "
                                        "(PGHE): overrides the skin tone measured from those players' images")
    parser.add_argument("--delay", type=float, default=0.05, help="seconds between downloads")
    args = parser.parse_args()

    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    with open(args.report, encoding="utf-8", newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["avatarUrl"]]

    fetched = failed = 0
    wanted = set()
    missing = []  # pgids of players without an image
    images = {}  # pgid -> (ratings site id, cached image)
    for row in rows:
        cached = os.path.join(args.cache, f"{row['madden_id']}.png")
        if not os.path.exists(cached):
            try:
                download(row["avatarUrl"], cached)
                fetched += 1
                time.sleep(args.delay)
            except Exception as error:  # keep going; a re-run retries the failures
                print(f"{row['first']} {row['last']}: {error}")
                failed += 1
                missing.append(int(row["pgid"]))
                continue
        images[int(row["pgid"])] = (int(row["madden_id"]), cached)
        name = f"{row['portrait_id']}.png"
        wanted.add(name)
        shutil.copyfile(cached, os.path.join(args.out, name))

    removed = 0
    for name in os.listdir(args.out):
        if name.lower().endswith(".png") and name not in wanted:
            os.remove(os.path.join(args.out, name))
            removed += 1
    if args.roster:
        from tdb import SaveFile
        save = SaveFile(args.roster)
        play = save.db.tables["PLAY"]
        if missing:
            counts = {}
            for i in range(play.count):
                counts[play.get(i, "PSXP")] = counts.get(play.get(i, "PSXP"), 0) + 1
            generic = max((k for k in counts if k), key=lambda k: counts[k])
            wanted_pgids = set(missing)
            for i in range(play.count):
                if play.get(i, "PGID") in wanted_pgids:
                    play.set(i, "PSXP", generic)
            print(f"{len(missing)} players without an image now show the silhouette")
        tones = match_heads(play, images, load_looks(args.looks) if args.looks else {})
        print(f"heads by skin tone: {tones.get(SKIN_LIGHT, 0)} light, {tones.get(SKIN_MEDIUM, 0)} medium, "
              f"{tones.get(SKIN_DARK, 0)} dark")
        save.fix_checksums()
        save.save(args.roster)
        print(f"{args.roster} updated")
    print(f"{len(wanted)} portraits installed in {args.out} ({fetched} downloaded, {failed} failed, "
          f"{removed} stale removed)")


if __name__ == "__main__":
    main()
