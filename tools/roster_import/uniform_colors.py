"""Recolours teams' uniforms and helmets to their current colours, straight from the disc's files.

    python tools/roster_import/uniform_extract.py "iso/NFL Head Coach 09.iso"
    python tools/roster_import/uniform_colors.py [--team RAMS ...] [--preview]
    python tools/roster_import/uniform_patches.py "iso/NFL Head Coach 09.iso"

A team's uniform is a set of textures (jersey, sleeves, pants, socks, the numbers and the name
letters, home and away) and its helmet a material file (<TEAM>HELMET, with the shell's and the
facemask's base colours) and decals, all compressed in the disc's archives (see
uniform_extract.py, which copies them to build/uniforms_raw). The textures are Xbox 360 DXT1/DXT3
(tiled, 16-bit swapped); numbers and names are stacks of glyphs (the texture dump in
build/hashed does not show those).

For each team in TEAMS this moves each colour of its uniform near one of the team's old colours
to the matching new one, keeping its shading (it changes the two end colours of every 4x4 block, so
the cloth's folds, mesh and lettering stay as they are), sets the helmet's shell and facemask
colours, and writes each changed file, compressed again into no more room than the original
(zopfli if installed, `pip install zopfli`, else zlib), to tools/roster_import/data/uniforms as
<NAME>.orig / <NAME>.new for uniform_patches.py. --preview also writes before/after sheets to
build/uniform_preview. Logos keep their 2008 shapes; only their colours change.

Needs numpy (and Pillow for --preview).
"""

import argparse
import glob
import os
import re
import struct
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RAW = os.path.join(ROOT, "build", "uniforms_raw")
OUT = os.path.join(HERE, "data", "uniforms")
PREVIEW = os.path.join(ROOT, "build", "uniform_preview")

SILVER = (150, 157, 163)
# team: {"colours": [(old rgb, new rgb)], "numbers": extra pairs for the numbers and names,
#        "shell": helmet colour, "facemask": facemask colour} (any of them optional)
TEAMS = {
    "BILLS": {"colours": [((20, 25, 50), (0, 51, 141)), ((25, 45, 100), (0, 51, 141)), ((190, 40, 40), (198, 12, 48))],
              "shell": (242, 242, 242), "facemask": (242, 242, 242)},
    "BROWNS": {"facemask": (242, 242, 242)},
    "CHARGERS": {"colours": [((15, 30, 60), (0, 128, 198)), ((10, 25, 50), (0, 110, 180)), ((200, 150, 40), (255, 194, 14))],
                 "facemask": (255, 194, 14)},
    "DOLPHINS": {"colours": [((20, 80, 80), (0, 142, 151)), ((230, 90, 20), (252, 76, 2)), ((15, 20, 40), (0, 100, 110))],
                 "facemask": (0, 142, 151)},
    "JETS": {"colours": [((20, 50, 10), (18, 87, 64)), ((30, 70, 20), (18, 87, 64))],
             "shell": (18, 87, 64), "facemask": (242, 242, 242)},
    "LIONS": {"numbers": [((16, 52, 155), (0, 118, 182)), ((0, 0, 0), SILVER), ((30, 30, 30), SILVER)],
              "shell": (204, 209, 214), "facemask": (0, 118, 182)},
    "RAMS": {"colours": [((25, 35, 60), (0, 53, 148)), ((140, 115, 60), (255, 163, 0)), ((105, 90, 50), (230, 150, 0))],
             "shell": (0, 53, 148), "facemask": (0, 53, 148)},
    "SEAHAWKS": {"colours": [((45, 70, 100), (0, 34, 68)), ((20, 30, 45), (0, 34, 68)), ((60, 120, 60), (105, 190, 40)),
                             ((100, 140, 180), (165, 172, 175))],
                 "shell": (0, 34, 68), "facemask": (0, 34, 68)},
    "TITANS": {"shell": (12, 35, 64), "facemask": (12, 35, 64)},
}
UNIFORM_PARTS = ("TORSOBAKED", "SLEEVEBAKED", "RSLEEVEBAKED", "PANTSBAKED", "SOCKS", "NUM", "NAME")
HEADER = 0x60  # XPR2 header and texture fetch constant before the texels


# --- textures ---------------------------------------------------------------------------------

def fetch(x):
    """(format, width, height, depth) from an XPR2 texture's fetch constant."""
    d = [struct.unpack(">I", x[0x48 + 4 * i:0x4C + 4 * i])[0] for i in range(6)]
    return d[1] & 0x3F, (d[2] & 0x1FFF) + 1, ((d[2] >> 13) & 0x1FFF) + 1, ((d[2] >> 26) & 0x3F) + 1


def _tiled(wb, hb, log):
    y, x = np.mgrid[0:hb, 0:wb]
    aw = (wb + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (log + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)


def decode(x, layer=0):
    """RGBA image of a DXT1 (18) or DXT3 (19) texture (one layer of a stack), for previews."""
    fmt, w, h, _ = fetch(x)
    bpb = 8 if fmt == 18 else 16
    wb, hb = w // 4, h // 4
    size = ((wb + 31) // 32 * 32) * ((hb + 31) // 32 * 32) * bpb
    data = np.frombuffer(x, np.uint8)[HEADER + layer * size:]
    blocks = data[_tiled(wb, hb, 3 if bpb == 8 else 4)[..., None] + np.arange(bpb)]
    blocks = blocks.reshape(hb, wb, bpb // 2, 2)[..., ::-1].reshape(hb, wb, bpb)
    col = blocks[..., -8:].astype(np.uint32)
    c0, c1 = col[..., 0] | col[..., 1] << 8, col[..., 2] | col[..., 3] << 8
    bits = col[..., 4] | col[..., 5] << 8 | col[..., 6] << 16 | col[..., 7] << 24
    rgb = lambda c: np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.int32)
    p0, p1 = rgb(c0), rgb(c1)
    four = (c0 > c1) | (fmt != 18)
    p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
    idx = (bits[..., None] >> (2 * np.arange(16))) & 3
    px = np.take_along_axis(np.stack([p0, p1, p2, p3], 2), idx[..., None].repeat(3, -1), 2)
    if fmt == 18:
        a = np.where((~four[..., None]) & (idx == 3), 0, 255).astype(np.uint8)
    else:
        al = blocks[..., :8].astype(np.uint64)
        av = sum(al[..., i] << np.uint64(8 * i) for i in range(8))
        a = (((av[..., None] >> (np.uint64(4) * np.arange(16, dtype=np.uint64))) & np.uint64(15)) * 17).astype(np.uint8)
    img = np.concatenate([px.astype(np.uint8), a[..., None]], -1)
    return img.reshape(hb, wb, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)


def _unpack565(c):
    return np.stack([((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31], -1)


def _pack565(rgb):
    r, g, b = [np.clip(np.round(rgb[..., i] * m / 255), 0, m).astype(np.uint32) for i, m in ((0, 31), (1, 63), (2, 31))]
    return (r << 11) | (g << 5) | b


def map_colours(rgb, pairs, tolerance=70):
    """Colours near one of the old colours (by hue and brightness) move to its new colour, as much
    brighter or darker than it as they were than the old one."""
    old = np.array([a for a, _ in pairs], float)
    new = np.array([b for _, b in pairs], float)

    def key(c):
        light = c.max(-1, keepdims=True) + 1e-3
        return np.concatenate([c / light * 120, light * 0.5], -1)

    dist = np.linalg.norm(key(rgb)[:, None, :] - key(old)[None], axis=-1)
    nearest, close = dist.argmin(-1), dist.min(-1) < tolerance
    ratio = np.clip((rgb.max(-1) + 1) / (old.max(-1) + 1)[nearest], 0.35, 1.6)
    out = rgb.copy()
    out[close] = np.clip(new[nearest] * ratio[:, None], 0, 255)[close]
    return out


def recolour(x, pairs):
    """The texture with both end colours of every block mapped (DXT1 blocks keep their mode)."""
    fmt = fetch(x)[0]
    bpb = 8 if fmt == 18 else 16
    b = np.frombuffer(x, np.uint8).copy()
    n = (len(b) - HEADER) // bpb
    blk = b[HEADER:HEADER + n * bpb].reshape(n, bpb)
    col = blk[:, bpb - 8:]
    word = lambda i: (col[:, i].astype(np.uint32) << 8) | col[:, i + 1]
    c0, c1, w0, w1 = word(0), word(2), word(4), word(6)
    mapped = map_colours(np.concatenate([_unpack565(c0), _unpack565(c1)]), pairs)
    n0, n1 = _pack565(mapped[:n]), _pack565(mapped[n:])
    if fmt == 18:  # 4-colour blocks need c0 > c1, 3-colour blocks c0 <= c1: swap and reindex
        four = c0 > c1
        equal = four & (n0 == n1)
        n0 = np.where(equal & (n0 < 0xFFFF), n0 + 1, n0)
        n1 = np.where(equal & (n0 == 0xFFFF), n1 - 1, n1)
        bits = w0 | (w1 << 16)
        swap4, swap3 = four & (n0 < n1), ~four & (n0 > n1)
        flip3 = bits ^ (~((bits >> 1) & 0x55555555) & 0x55555555)
        bits = np.where(swap4, bits ^ 0x55555555, np.where(swap3, flip3, bits))
        n0, n1 = np.where(swap4 | swap3, n1, n0), np.where(swap4 | swap3, n0, n1)
        w0, w1 = bits & 0xFFFF, bits >> 16
    for i, v in ((0, n0), (2, n1), (4, w0), (6, w1)):
        col[:, i], col[:, i + 1] = v >> 8, v & 255
    blk[:, bpb - 8:] = col
    b[HEADER:HEADER + n * bpb] = blk.reshape(-1)
    return b.tobytes()


# --- helmet material --------------------------------------------------------------------------

def set_helmet(x, shell=None, facemask=None):
    """The helmet material file with its shells' and facemask's baseColor set (0-255 rgb)."""
    x = bytearray(x)
    for material, colour in ((b"helmetLeftShell_mat", shell), (b"helmetRightShell_mat", shell), (b"facemask_mat", facemask)):
        if colour is None:
            continue
        at = x.find(material + b"\0")
        at = x.find(b"baseColor\0", at) if at >= 0 else -1
        if at < 0:
            raise ValueError(f"no {material.decode()} baseColor")
        struct.pack_into(">3f", x, at + 10, *(c / 255 for c in colour))
    return bytes(x)


# --- writing ----------------------------------------------------------------------------------

def compress(data, room):
    try:
        import zopfli.zlib as zopfli
        tries = [lambda: zopfli.compress(data, numiterations=i) for i in (1, 5, 15)]
    except ImportError:
        tries = []
    tries.insert(0, lambda: zlib.compress(data, 9))
    for attempt in tries:
        packed = attempt()
        if len(packed) <= room:
            return packed + bytes(room - len(packed))
    return None


def write_pair(name, orig, data):
    packed = compress(data, len(orig))
    if packed is None:
        return "does not fit (install zopfli: pip install zopfli)"
    assert zlib.decompressobj().decompress(packed) == data
    open(os.path.join(OUT, name + ".orig"), "wb").write(orig)
    open(os.path.join(OUT, name + ".new"), "wb").write(packed)
    return "written"


def preview(team, pairs_by_name):
    from PIL import Image
    tiles = []
    for name, (before, after) in sorted(pairs_by_name.items()):
        if "HELMET" in name and "DECAL" not in name:
            continue
        row = []
        for data in (before, after):
            im = Image.fromarray(decode(data))
            bg = Image.new("RGBA", im.size, (0, 150, 0, 255))
            bg.alpha_composite(im)
            im = bg.convert("RGB")
            im.thumbnail((160, 320))
            row.append(im)
        tiles.append(row)
    if not tiles:
        return
    sheet = Image.new("RGB", (170 * len(tiles), 660), (255, 0, 255))
    for i, (a, b) in enumerate(tiles):
        sheet.paste(a, (i * 170, 0))
        sheet.paste(b, (i * 170, 330))
    os.makedirs(PREVIEW, exist_ok=True)
    sheet.save(os.path.join(PREVIEW, f"{team}.png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--team", action="append", help="only this team (as named on the disc, e.g. RAMS)")
    parser.add_argument("--preview", action="store_true", help="before/after sheets in build/uniform_preview")
    args = parser.parse_args()
    if not os.path.isdir(RAW):
        raise SystemExit(f"{RAW} is missing: run uniform_extract.py first")
    os.makedirs(OUT, exist_ok=True)
    for team, spec in TEAMS.items():
        if args.team and team not in args.team:
            continue
        shown = {}
        files = []
        for path in sorted(glob.glob(os.path.join(RAW, team + "*_COL.orig")) + glob.glob(os.path.join(RAW, f"HELMET_{team}*_DECAL.orig"))):
            name = os.path.basename(path)[:-5]
            m = re.match(rf"^{team}[HA]({'|'.join(UNIFORM_PARTS)})_COL$", name)
            if not m and "DECAL" not in name:
                continue
            pairs = list(spec.get("colours", []))
            if m and m.group(1) in ("NUM", "NAME"):
                pairs += spec.get("numbers", [])
            if pairs:
                files.append((name, path, pairs))
        for name, path, pairs in files:
            orig = open(path, "rb").read()
            data = zlib.decompress(orig)
            new = recolour(data, pairs)
            if new != data:
                print(f"{name}: {write_pair(name, orig, new)}")
                shown[name] = (data, new)
        if "shell" in spec or "facemask" in spec:
            path = os.path.join(RAW, team + "HELMET.orig")
            orig = open(path, "rb").read()
            data = zlib.decompress(orig)
            print(f"{team}HELMET: {write_pair(team + 'HELMET', orig, set_helmet(data, spec.get('shell'), spec.get('facemask')))}")
        if args.preview and shown:
            preview(team, shown)
    print(f"done; now run uniform_patches.py to lay them over the disc")


if __name__ == "__main__":
    main()
