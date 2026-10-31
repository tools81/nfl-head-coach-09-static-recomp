"""Contact sheets of the textures HC27 dumped (HC09_DUMP_HASHED=1), to find a team's logos without
opening hundreds of files.

    python tools/roster_import/dump_sheets.py

Reads build/hashed and writes build/hashed_sheets/sheet_NN.png (112 numbered thumbnails each) and
index.txt (number -> texture hash and size). Textures already shown on an earlier run are left
out (build/hashed_sheets/seen.txt), so each run shows only what the latest games added; --all
shows everything again. Fonts and normal maps are left out. Needs Pillow.

--recent MINUTES instead shows what the game drew in the last MINUTES minutes (it saves a
texture again each run), whether shown before or not, leaving out textures already replaced in
hc27/textures/hashed and colourless ones (fonts, frames, glows): the quickest way to find what is
still old on screens just visited.
"""

import time

import argparse
import os

from PIL import Image, ImageDraw, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DUMP = os.path.join(ROOT, "build", "hashed")
OUT = os.path.join(ROOT, "build", "hashed_sheets")
REPLACED = os.path.join(ROOT, "hc27", "textures", "hashed")
CELL, COLS, PER_SHEET = 140, 14, 112


def boring(im):
    small = im.convert("RGBA")
    small.thumbnail((48, 48))
    rgb = small.convert("RGB")
    r, g, b = ImageStat.Stat(rgb).mean
    if abs(r - g) < 12 and b < r - 40:          # olive normal map
        return True
    if b > 200 and r > 100 and g < 160 and abs(r - g) > 30:   # violet normal map
        return True
    alpha = small.getchannel("A")
    # white glyphs on transparency (font pages)
    if ImageStat.Stat(alpha).mean[0] < 140 and min(r, g, b) > 200:
        return True
    return False


def colourless(im):
    small = im.convert("RGBA")
    small.thumbnail((64, 64))
    spread = [max(p[:3]) - min(p[:3]) for p in small.getdata() if p[3] > 64]
    return not spread or sorted(spread)[len(spread) * 99 // 100] < 24


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--all", action="store_true", help="include textures shown before")
    parser.add_argument("--recent", type=float, metavar="MINUTES",
                        help="only what the game drew in the last MINUTES minutes and is not replaced yet")
    parser.add_argument("--grey", action="store_true",
                        help="with --recent, keep colourless textures too (numbers, silver helmets)")
    parser.add_argument("--fonts", action="store_true",
                        help="with --recent, show only white-on-transparent pages (fonts, jersey numbers)")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    seen_path = os.path.join(OUT, "seen.txt")
    seen = set() if args.all or not os.path.exists(seen_path) else set(open(seen_path).read().split())
    for old in os.listdir(OUT):
        if old.startswith("sheet_"):
            os.remove(os.path.join(OUT, old))
    if args.recent:
        since = time.time() - args.recent * 60
        done = {f[:-4].upper() for f in os.listdir(REPLACED)} if os.path.isdir(REPLACED) else set()
        files = sorted(f for f in os.listdir(DUMP) if f.endswith(".png") and f[:-4].upper() not in done
                       and os.path.getmtime(os.path.join(DUMP, f)) >= since)
    else:
        files = sorted(f for f in os.listdir(DUMP) if f.endswith(".png") and f[:-4] not in seen)
    shown, lines = [], []
    for f in files:
        try:
            im = Image.open(os.path.join(DUMP, f))
            im.load()
        except OSError:
            continue
        seen.add(f[:-4])
        if args.fonts:
            if min(im.size) < 16 or not boring(im):
                continue
        elif min(im.size) < 16 or boring(im) or (args.recent and not args.grey and colourless(im)):
            continue
        shown.append((f[:-4], im))
    for start in range(0, len(shown), PER_SHEET):
        chunk = shown[start:start + PER_SHEET]
        rows = (len(chunk) + COLS - 1) // COLS
        sheet = Image.new("RGB", (CELL * COLS, (CELL + 14) * rows), (105, 105, 105))
        draw = ImageDraw.Draw(sheet)
        for i, (name, im) in enumerate(chunk):
            n = start + i
            thumb = im.convert("RGBA")
            thumb.thumbnail((CELL - 8, CELL - 8))
            x, y = (i % COLS) * CELL, (i // COLS) * (CELL + 14)
            sheet.paste(thumb, (x + 4, y + 4), thumb)
            draw.text((x + 3, y + CELL - 2), f"{n} {im.size[0]}x{im.size[1]}", fill=(255, 255, 0))
            lines.append(f"{n} {name} {im.size[0]}x{im.size[1]}")
        sheet.save(os.path.join(OUT, f"sheet_{start // PER_SHEET:02d}.png"))
    open(os.path.join(OUT, "index.txt"), "w").write("\n".join(lines) + "\n")
    if not args.recent:
        open(seen_path, "w").write("\n".join(sorted(seen)) + "\n")
    print(f"{len(files)} new textures, {len(shown)} on {(len(shown) + PER_SHEET - 1) // PER_SHEET} sheets in {OUT}")


if __name__ == "__main__":
    main()
