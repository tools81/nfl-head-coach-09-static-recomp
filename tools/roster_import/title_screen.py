"""Makes HC27's title screen (the one with Press Start) from the game's own.

    python tools/roster_import/title_screen.py --coach tools/roster_import/data/ui/title_ben_johnson.png \
        --title tools/roster_import/data/ui/title.png

The screen's pictures are textures inside the disc's archives, so, as for team logos
(team_logos.py), they are replaced by their contents: run HC27 once with HC09_DUMP_HASHED=1 to the
title screen first, which saves them in build/hashed. The background (a stadium and Tony Dungy)
is drawn as eight tiles; they are put together, the coach is replaced (the stadium on his side is
the other side's, mirrored, fading to black) and the result is cut into tiles again. The game's
title badge is replaced by the title (on a white banner, for dark lettering) and the copyright
line under it is removed. The results go to hc27/textures/hashed. Needs Pillow.
"""

import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The background's tiles: content hash -> (left, top) on the screen.
BACKGROUND = {
    "E133152BCFEB173A": (0, 0), "E6D5CBED5E236C0B": (1024, 0),
    "A405842DB054DB29": (0, 512), "2FB492269C235316": (1024, 512),
    "2A0725C15257BC19": (0, 640), "E4A611FB581388B3": (1024, 640),
    "E53F32EA2EF6882D": (0, 704), "20C0FFB785B22999": (1024, 704),
}
SCREEN = (1280, 720)
COACH_FROM = 490  # the coach is right of this column; the stadium is left of it
# The title badge with the copyright line under it (rows 236 on, removed).
BADGE = "760C850D96DBBF0A"
BADGE_BOX = (19, 1, 365, 174)
COPYRIGHT_TOP = 200


def background(dump, coach):
    from PIL import Image, ImageOps
    tiles = {name: Image.open(os.path.join(dump, f"{name}.png")).convert("RGBA") for name in BACKGROUND}
    width = max(BACKGROUND[n][0] + t.width for n, t in tiles.items())
    height = max(BACKGROUND[n][1] + t.height for n, t in tiles.items())
    screen = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    for name, tile in tiles.items():
        screen.paste(tile, BACKGROUND[name])

    # The stadium mirrored over the coach, fading to black toward the right.
    stadium = screen.crop((0, 0, COACH_FROM, height))
    mirrored = ImageOps.mirror(stadium)
    covered = Image.new("RGBA", (width - COACH_FROM, height), (0, 0, 0, 255))
    covered.paste(mirrored, (0, 0))
    fade = Image.new("L", covered.size, 0)
    fade.putdata([max(0, 255 - 255 * x // (mirrored.width * 9 // 10)) for y in range(covered.height)
                  for x in range(covered.width)])
    black = Image.new("RGBA", covered.size, (0, 0, 0, 255))
    covered = Image.composite(covered, black, fade)
    # A soft seam: the first columns blend from the original.
    seam = 24
    blend = Image.new("L", (seam, height))
    blend.putdata([255 * x // seam for y in range(height) for x in range(seam)])
    screen.paste(covered.crop((0, 0, seam, height)), (COACH_FROM, 0), blend)
    screen.paste(covered.crop((seam, 0, covered.width, height)), (COACH_FROM + seam, 0))

    # The new coach on the right, standing on the bottom of the screen.
    person = coach.crop(coach.getchannel("A").point(lambda v: 255 if v > 16 else 0).getbbox())
    scale = SCREEN[1] * 0.95 / person.height
    person = person.resize((round(person.width * scale), round(person.height * scale)), Image.LANCZOS)
    screen.alpha_composite(person, (SCREEN[0] - person.width - SCREEN[0] // 25, SCREEN[1] - person.height))

    return {name: screen.crop((x, y, x + tiles[name].width, y + tiles[name].height)) for name, (x, y) in BACKGROUND.items()}


def badge(dump, title):
    from PIL import Image, ImageDraw, ImageFilter
    original = Image.open(os.path.join(dump, f"{BADGE}.png")).convert("RGBA")
    picture = Image.new("RGBA", original.size, (0, 0, 0, 0))
    title = title.crop(title.getchannel("A").point(lambda v: 255 if v > 16 else 0).getbbox())
    left, top, right, bottom = BADGE_BOX
    pad = (bottom - top) // 10
    # The title fitted inside the banner's padding.
    scale = min((right - left - 4 * pad) / title.width, (bottom - top - 4 * pad) / title.height)
    title = title.resize((round(title.width * scale), round(title.height * scale)), Image.LANCZOS)
    plate = (left + (right - left - title.width) // 2 - 2 * pad, top + (bottom - top - title.height) // 2 - pad,
             left + (right - left + title.width) // 2 + 2 * pad, top + (bottom - top + title.height) // 2 + pad)
    shadow = Image.new("RGBA", original.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((plate[0] + 3, plate[1] + 4, plate[2] + 3, plate[3] + 4), radius=pad,
                                             fill=(0, 0, 0, 160))
    picture.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4)))
    ImageDraw.Draw(picture).rounded_rectangle(plate, radius=pad, fill=(255, 255, 255, 240))
    picture.alpha_composite(title, ((plate[0] + plate[2] - title.width) // 2, (plate[1] + plate[3] - title.height) // 2))
    return picture  # nothing below COPYRIGHT_TOP: the copyright line is gone


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--coach", required=True, help="the coach's picture: a PNG with a transparent background")
    parser.add_argument("--title", required=True, help="the game's title: a PNG with a transparent background")
    parser.add_argument("--dump", default=os.path.join(ROOT, "build", "hashed"), help="HC09_DUMP_HASHED's folder")
    parser.add_argument("--out", default=os.path.join(ROOT, "hc27", "textures", "hashed"))
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("title_screen.py needs Pillow: pip install pillow")
    missing = [n for n in list(BACKGROUND) + [BADGE] if not os.path.exists(os.path.join(args.dump, f"{n}.png"))]
    if missing:
        raise SystemExit(f"not in {args.dump} (run HC27 to the title screen with HC09_DUMP_HASHED=1): " + ", ".join(missing))
    os.makedirs(args.out, exist_ok=True)
    for name, tile in background(args.dump, Image.open(args.coach).convert("RGBA")).items():
        tile.save(os.path.join(args.out, f"{name}.png"))
    badge(args.dump, Image.open(args.title).convert("RGBA")).save(os.path.join(args.out, f"{BADGE}.png"))
    print(f"title screen written to {args.out}")


if __name__ == "__main__":
    main()
