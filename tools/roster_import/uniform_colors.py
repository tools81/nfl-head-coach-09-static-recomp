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

Every team (all of TEAMS_ALL, colour changes or not) also gets the 2026 look of the cloth: the
Reebok mark on both sleeves and the pants becomes a Nike swoosh of the same colour, and the
jersey's mesh (a lattice of dots, removed by notching its frequencies out) becomes solid fabric.
Those edits re-encode only the 4x4 blocks they change, in the texture's full-size level (its
smaller mip levels, seen from far away, keep the old cloth).

Needs numpy, scipy and opencv-python (and Pillow).
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
WHITE, BLACK, GREY = (236, 237, 243), (18, 18, 20), (165, 172, 175)


def _stripes(*colours, wide=12, gap_colour=None, gap=3):
    """Sleeve bands: each colour wide, with gap of gap_colour between (none if gap_colour is None)."""
    out = []
    for i, c in enumerate(colours):
        if i and gap_colour is not None:
            out.append((gap_colour, gap))
        out.append((c, wide))
    return out


ROYAL, BILLS_RED = (0, 51, 141), (198, 12, 48)
CARD_RED = (151, 35, 63)
FALC_RED = (167, 25, 48)
BENG_ORANGE = (251, 79, 20)
BROWN, BROWNS_ORANGE = (56, 32, 20), (255, 80, 0)
BRONCO_ORANGE, BRONCO_NAVY = (251, 79, 20), (12, 35, 64)
TEX_NAVY, TEX_RED = (12, 32, 64), (200, 16, 46)
JAG_TEAL = (0, 103, 120)
PAT_NAVY, PAT_RED, PAT_SILVER = (0, 34, 68), (198, 12, 48), (176, 183, 188)
BUCS_RED, PEWTER, BUCS_ORANGE = (213, 10, 10), (52, 48, 43), (255, 121, 0)
TITANS_BLUE, TITANS_NAVY, TITANS_RED = (75, 146, 219), (12, 35, 64), (200, 16, 46)
BURGUNDY, GOLD = (90, 20, 20), (255, 182, 18)

# team: {"colours": [(old rgb, new rgb)], "numbers": extra pairs for the numbers and names
#        ("numbers_H"/"numbers_A": for one kit only), "kit": {"H"|"A": redesign, see redesign();
#        "num": [fill, outline] for the numbers and names}, "shell": helmet colour,
#        "facemask": facemask colour} (any of them optional)
TEAMS = {
    "BILLS": {"colours": [((20, 25, 50), ROYAL), ((25, 45, 100), ROYAL), ((190, 40, 40), BILLS_RED)],
              "numbers_H": [((0, 48, 112), BILLS_RED)], "numbers_A": [((0, 0, 32), BILLS_RED)],
              "kit": {"H": {"body": ROYAL, "collar": WHITE, "socks": ROYAL, "pants": WHITE,
                            "pants_stripes": [(ROYAL, 8), (BILLS_RED, 10), (ROYAL, 8)],
                            "stripes": _stripes(WHITE, BILLS_RED, WHITE, BILLS_RED, WHITE, gap_colour=ROYAL)},
                      "A": {"body": WHITE, "collar": ROYAL, "socks": WHITE, "pants": ROYAL,
                            "pants_stripes": [(WHITE, 6), (BILLS_RED, 10), (WHITE, 6)],
                            "stripes": _stripes(ROYAL, BILLS_RED, ROYAL, BILLS_RED, ROYAL, gap_colour=WHITE)}},
              "shell": (242, 242, 242), "facemask": (242, 242, 242)},
    "CARDINALS": {"kit": {"H": {"body": CARD_RED, "collar": (110, 20, 45), "wordmark": WHITE, "pants": CARD_RED,
                                "socks": CARD_RED, "num": [WHITE, BLACK]},
                          "A": {"body": WHITE, "collar": CARD_RED, "wordmark": None, "stripes": [(BLACK, 26)],
                                "pants": WHITE, "pants_stripes": [(GREY, 8), (CARD_RED, 12), (GREY, 8)],
                                "socks": WHITE, "num": [CARD_RED, BLACK]}},
                  "facemask": GREY},
    "FALCONS": {"kit": {"H": {"body": FALC_RED, "collar": BLACK, "wordmark": WHITE, "pants": WHITE,
                              "pants_stripes": [(BLACK, 10), (GREY, 12)], "socks": FALC_RED, "num": [WHITE, BLACK]},
                        "A": {"body": WHITE, "collar": BLACK, "wordmark": BLACK, "pants": BLACK,
                              "pants_stripes": [(WHITE, 8), (GREY, 12)], "socks": WHITE, "num": [FALC_RED, BLACK]}},
                "shell": BLACK, "facemask": GREY},
    "BENGALS": {"kit": {"H": {"body": BLACK, "collar": BLACK, "wordmark": BENG_ORANGE, "socks": BENG_ORANGE,
                              "stripes": _stripes(BENG_ORANGE, BENG_ORANGE, BENG_ORANGE, wide=10, gap_colour=BLACK, gap=6),
                              "stripes_at": 60, "num": [WHITE, BENG_ORANGE]},
                        "A": {"body": WHITE, "collar": WHITE, "wordmark": BENG_ORANGE, "socks": WHITE,
                              "stripes": _stripes(BLACK, BLACK, BLACK, wide=10, gap_colour=WHITE, gap=6),
                              "stripes_at": 60, "num": [BLACK, BENG_ORANGE]}}},
    "BROWNS": {"kit": {"H": {"body": BROWN, "collar": WHITE, "wordmark": None, "pants": WHITE,
                             "pants_stripes": [(BROWN, 8), (BROWNS_ORANGE, 12), (BROWN, 8)], "socks": BROWN,
                             "stripes": _stripes(BROWNS_ORANGE, WHITE, BROWNS_ORANGE, WHITE, BROWNS_ORANGE, wide=10),
                             "num": [WHITE, WHITE]},
                       "A": {"body": WHITE, "collar": WHITE, "wordmark": None, "pants": BROWNS_ORANGE,
                             "pants_stripes": [(BROWN, 8), (WHITE, 8), (BROWN, 8)], "socks": WHITE,
                             "stripes": _stripes(BROWN, BROWNS_ORANGE, BROWN, wide=12, gap_colour=WHITE),
                             "num": [BROWN, BROWN]}},
               "facemask": (242, 242, 242)},
    "BRONCOS": {"kit": {"H": {"body": BRONCO_ORANGE, "collar": BRONCO_NAVY, "wordmark": BRONCO_NAVY, "pants": WHITE,
                              "pants_stripes": [(BRONCO_ORANGE, 10), (BRONCO_NAVY, 10)], "socks": BRONCO_ORANGE,
                              "stripes": [(BRONCO_NAVY, 12), (WHITE, 6), (BRONCO_NAVY, 12)], "num": [WHITE, BRONCO_NAVY]},
                        "A": {"body": WHITE, "collar": BRONCO_NAVY, "wordmark": BRONCO_ORANGE, "pants": BRONCO_ORANGE,
                              "pants_stripes": [(WHITE, 8), (BRONCO_NAVY, 10)], "socks": WHITE,
                              "stripes": [(BRONCO_NAVY, 12), (BRONCO_ORANGE, 6), (BRONCO_NAVY, 12)],
                              "num": [BRONCO_NAVY, BRONCO_ORANGE]}}},
    "TEXANS": {"kit": {"H": {"body": TEX_NAVY, "collar": TEX_RED, "wordmark": TEX_RED, "pants": WHITE,
                             "pants_stripes": [(TEX_NAVY, 8), (TEX_RED, 10), (TEX_NAVY, 8)], "socks": TEX_NAVY,
                             "num": [WHITE, TEX_RED]},
                       "A": {"body": WHITE, "collar": TEX_NAVY, "wordmark": TEX_RED, "pants": TEX_NAVY,
                             "pants_stripes": [(WHITE, 6), (TEX_RED, 10), (WHITE, 6)], "socks": WHITE,
                             "num": [TEX_NAVY, TEX_RED]}}},
    "JAGUARS": {"kit": {"H": {"body": JAG_TEAL, "collar": BLACK, "wordmark": None, "pants": WHITE, "socks": JAG_TEAL,
                              "stripes": [(BLACK, 30)], "stripes_at": 210, "num": [WHITE, WHITE]},
                        "A": {"body": WHITE, "collar": BLACK, "wordmark": None, "pants": JAG_TEAL, "socks": BLACK,
                              "stripes": [(BLACK, 30)], "stripes_at": 210, "num": [BLACK, BLACK]}},
                "shell": BLACK},
    "PATRIOTS": {"kit": {"H": {"body": PAT_NAVY, "collar": PAT_RED, "wordmark": WHITE, "pants": PAT_SILVER,
                               "pants_stripes": [(PAT_NAVY, 8), (PAT_RED, 10), (PAT_NAVY, 8)], "socks": PAT_NAVY,
                               "stripes": [(PAT_RED, 10), (WHITE, 6), (PAT_RED, 10)], "stripes_at": 70,
                               "num": [WHITE, PAT_RED]},
                         "A": {"body": WHITE, "collar": PAT_NAVY, "wordmark": PAT_NAVY, "pants": PAT_SILVER,
                               "pants_stripes": [(PAT_NAVY, 8), (PAT_RED, 10), (PAT_NAVY, 8)], "socks": WHITE,
                               "stripes": [(PAT_NAVY, 10), (PAT_RED, 6), (PAT_NAVY, 10)], "stripes_at": 70,
                               "num": [PAT_NAVY, PAT_RED]}},
                 "shell": PAT_SILVER, "facemask": PAT_RED},
    "BUCCANEERS": {"kit": {"H": {"body": BUCS_RED, "collar": BLACK, "pants": WHITE, "socks": BLACK,
                                 "pants_stripes": [(BLACK, 6), (BUCS_RED, 10), (BUCS_ORANGE, 6), (BLACK, 6)],
                                 "stripes": [(BLACK, 26)], "stripes_at": 214, "num": [WHITE, BUCS_RED]},
                           "A": {"body": WHITE, "collar": BLACK, "pants": PEWTER, "socks": BLACK,
                                 "pants_stripes": [(BLACK, 6), (BUCS_RED, 10), (BUCS_ORANGE, 6), (BLACK, 6)],
                                 "stripes": [(BLACK, 26)], "stripes_at": 214, "num": [BUCS_RED, BLACK]}},
                   "shell": PEWTER, "facemask": BLACK},
    "TITANS": {"kit": {"H": {"body": TITANS_BLUE, "collar": TITANS_NAVY, "wordmark": WHITE, "pants": WHITE,
                             "pants_stripes": [(TITANS_NAVY, 8), (TITANS_RED, 8)], "socks": TITANS_BLUE,
                             "stripes": _stripes(WHITE, WHITE, WHITE, WHITE, wide=5, gap_colour=TITANS_NAVY, gap=4),
                             "num": [WHITE, TITANS_RED]},
                       "A": {"body": WHITE, "collar": TITANS_NAVY, "wordmark": TITANS_BLUE, "pants": TITANS_BLUE,
                             "pants_stripes": [(TITANS_NAVY, 8), (TITANS_RED, 8)], "socks": WHITE,
                             "stripes": _stripes(TITANS_BLUE, TITANS_BLUE, TITANS_BLUE, TITANS_BLUE, wide=6,
                                                 gap_colour=WHITE, gap=4),
                             "num": [TITANS_BLUE, TITANS_RED]}},
               "shell": (242, 242, 242), "facemask": (242, 242, 242)},
    "REDSKINS": {"kit": {"H": {"body": BURGUNDY, "collar": (60, 12, 12), "wordmark": None, "pants": WHITE,
                               "pants_stripes": [(BURGUNDY, 8), (GOLD, 10)], "socks": BURGUNDY,
                               "stripes": [(WHITE, 8), (GOLD, 10)], "num": [WHITE, GOLD]},
                         "A": {"body": WHITE, "collar": BURGUNDY, "wordmark": None, "pants": BURGUNDY,
                               "pants_stripes": [(WHITE, 8), (GOLD, 10)], "socks": WHITE,
                               "stripes": [(BURGUNDY, 8), (GOLD, 10)], "num": [BURGUNDY, GOLD]}},
                 "shell": BURGUNDY, "facemask": GOLD},
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
}
TEAMS_ALL = ["49ERS", "BEARS", "BENGALS", "BILLS", "BRONCOS", "BROWNS", "BUCCANEERS", "CARDINALS", "CHARGERS",
             "CHIEFS", "COLTS", "COWBOYS", "DOLPHINS", "EAGLES", "FALCONS", "GIANTS", "JAGUARS", "JETS", "LIONS",
             "PACKERS", "PANTHERS", "PATRIOTS", "RAIDERS", "RAMS", "RAVENS", "REDSKINS", "SAINTS", "SEAHAWKS",
             "STEELERS", "TEXANS", "TITANS", "VIKINGS"]
# The Reebok mark: part -> (where it is on the 49ers' home texture, which serves as the template
# for finding it on the others, angle of the swoosh, mirrored). Teams have it in different
# places and sizes, so each texture is searched; MARK_BOXES pins the ones the search misses.
SWOOSHES = {"SLEEVEBAKED": ((8, 488, 27, 531), -90, False),
            "RSLEEVEBAKED": ((8, 495, 27, 538), 90, True),
            "PANTSBAKED": ((624, 97, 720, 132), -8, False)}
MARK_TEMPLATE = "49ERSH{part}_COL"
MARK_BOXES = {"CHARGERSASLEEVEBAKED": (122, 487, 166, 573), "CHARGERSARSLEEVEBAKED": (122, 487, 166, 573),
              "CHARGERSHSLEEVEBAKED": (122, 487, 166, 573), "CHARGERSHRSLEEVEBAKED": (122, 487, 166, 573),
              "JETSHRSLEEVEBAKED": (15, 501, 37, 547), "JETSHSLEEVEBAKED": (15, 501, 37, 547),
              **{f"PANTHERS{k}{p}SLEEVEBAKED": (54, 481, 85, 543) for k in "HA" for p in ("", "R")},
              "SEAHAWKSASLEEVEBAKED": (67, 495, 93, 552),
              "TEXANSHSLEEVEBAKED": (15, 496, 37, 543), "TEXANSHRSLEEVEBAKED": (15, 496, 37, 543),
              **{f"TITANS{k}{p}SLEEVEBAKED": (15, 496, 37, 542) for k in "HA" for p in ("", "R")},
              **{f"VIKINGS{k}{p}SLEEVEBAKED": (83, 491, 145, 625) for k in "HA" for p in ("", "R")}}
MESH_PARTS = ("TORSOBAKED",)
# Parts HC27 replaces by content hash instead (hc27/textures/hashed): their swoosh and fabric are
# done there.
HASHED_PARTS = {"LIONS": ("TORSOBAKED", "SLEEVEBAKED", "RSLEEVEBAKED", "PANTSBAKED")}
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


# --- cloth: swoosh and fabric ------------------------------------------------------------------

SWOOSH = [(1.00, 0.00), (0.62, 0.24), (0.40, 0.36), (0.24, 0.43), (0.13, 0.45), (0.06, 0.42), (0.03, 0.34),
          (0.07, 0.22), (0.17, 0.08), (0.12, 0.22), (0.12, 0.31), (0.17, 0.355), (0.27, 0.35), (0.42, 0.30)]


def swoosh_mask(w, h, angle=0, mirror=False, ss=8):
    """Coverage (0-1) of a swoosh fitted in a w x h box, turned by angle degrees."""
    from PIL import Image, ImageDraw
    p = np.array(SWOOSH) - [0.5, 0.225]
    if mirror:
        p[:, 0] = -p[:, 0]
    t = np.deg2rad(angle)
    p = p @ np.array([[np.cos(t), -np.sin(t)], [np.sin(t), np.cos(t)]]).T
    span = p.max(0) - p.min(0)
    p = p * min(w * ss / span[0], h * ss / span[1]) * 0.92 + [w * ss / 2, h * ss / 2]
    im = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(im).polygon([tuple(q) for q in p], fill=255)
    return np.array(im.resize((w, h), Image.LANCZOS)).astype(float) / 255


def replace_mark(img, box, angle=0, mirror=False, threshold=60, cloth_pct=None):
    """In img (HxWx3 floats), the mark in box (what stands out from the box's cloth) is painted
    out and a swoosh of its colour drawn in its place, shaded like the cloth. False if no mark."""
    import cv2
    x0, y0, x1, y1 = box
    pad = 6 if cloth_pct is None else 16
    X0, Y0, X1, Y1 = max(0, x0 - pad), max(0, y0 - pad), min(img.shape[1], x1 + pad), min(img.shape[0], y1 + pad)
    region = img[Y0:Y1, X0:X1]
    flat = region.reshape(-1, 3)
    # striped cloth: its brightest rows (cloth_pct percentile) stand for the cloth instead
    cloth = np.median(flat, 0) if cloth_pct is None else flat[np.argsort(flat.sum(-1))[int(len(flat) * cloth_pct / 100)]]
    mark = np.abs(region - cloth).sum(-1) > threshold
    inside = np.zeros_like(mark)  # only the mark itself, never art next to it
    inside[max(y0 - Y0 - 4, 0):y1 - Y0 + 4, max(x0 - X0 - 4, 0):x1 - X0 + 4] = True
    mark &= inside
    if mark.sum() < 20 or mark.mean() > 0.35:  # nothing there, or a pattern rather than a mark
        return False
    contrast = np.abs(region - cloth).sum(-1)
    colour = np.median(region[mark & (contrast >= np.percentile(contrast[mark], 70))], 0)  # its solid core, not its soft edge
    mask = cv2.dilate(mark.astype(np.uint8), np.ones((5, 5), np.uint8))
    if cloth_pct is None:
        clean = cv2.inpaint(np.clip(region, 0, 255).astype(np.uint8), mask, 5, cv2.INPAINT_TELEA).astype(float)
    else:  # striped across: each row is bridged from the cloth either side of the mark
        clean = region.copy()
        cols = np.nonzero(mask.any(0))[0]
        a, b = max(cols[0] - 3, 0), min(cols[-1] + 3, region.shape[1] - 1)
        t = np.linspace(0, 1, b - a + 1)[None, :, None]
        clean[:, a:b + 1] = region[:, a:a + 1] * (1 - t) + region[:, b:b + 1] * t
    cover = np.zeros(region.shape[:2])
    cover[y0 - Y0:y1 - Y0, x0 - X0:x1 - X0] = swoosh_mask(x1 - x0, y1 - y0, angle, mirror)
    shade = np.clip(clean.mean(-1, keepdims=True) / max(cloth.mean(), 1), 0.7, 1.3)
    img[Y0:Y1, X0:X1] = clean * (1 - cover[..., None]) + np.clip(colour * shade, 0, 255) * cover[..., None]
    return True


def _edges(img):
    import cv2
    g = img.mean(-1).astype(np.float32)
    return np.hypot(cv2.Sobel(g, cv2.CV_32F, 1, 0), cv2.Sobel(g, cv2.CV_32F, 0, 1))


_templates = {}


def find_mark(part, img, margin=6):
    """The Reebok mark's box in img (HxWx3), found by matching the edges of the 49ers' one over
    a range of sizes, or None. Flat cloth scores low, so a plain area never wins."""
    import cv2
    if part not in _templates:
        ref = decode(zlib.decompress(open(os.path.join(RAW, MARK_TEMPLATE.format(part=part) + ".orig"), "rb").read()))
        x0, y0, x1, y1 = SWOOSHES[part][0]
        _templates[part] = _edges(ref[..., :3].astype(float))[y0 - margin:y1 + margin, x0 - margin:x1 + margin]
    t0 = _templates[part]
    edges = _edges(img)
    best = (0.3, None)
    for scale in (0.8, 0.9, 1.0, 1.12, 1.25, 1.4, 1.6, 1.8, 2.0, 2.25, 2.5, 2.8):
        t = cv2.resize(t0, None, fx=scale, fy=scale)
        if t.shape[0] >= edges.shape[0] or t.shape[1] >= edges.shape[1]:
            continue
        r = cv2.matchTemplate(edges, t, cv2.TM_CCOEFF_NORMED)
        energy = cv2.boxFilter(edges * edges, -1, (t.shape[1], t.shape[0]), anchor=(0, 0), borderType=cv2.BORDER_CONSTANT)
        r = r * np.clip(np.sqrt(np.maximum(energy[:r.shape[0], :r.shape[1]], 0) / (t * t).mean()), 0, 1)
        _, v, _, (x, y) = cv2.minMaxLoc(r)
        if v > best[0]:
            m = round(margin * scale)
            best = (v, (x + m, y + m, x + t.shape[1] - m, y + t.shape[0] - m))
    if best[1] is None:
        return None
    # snap to what stands out from the cloth around where it matched
    x0, y0, x1, y1 = best[1]
    pad = max(4, (x1 - x0) // 4)
    X0, Y0 = max(x0 - pad, 0), max(y0 - pad, 0)
    region = img[Y0:y1 + pad, X0:x1 + pad]
    mark = np.abs(region - np.median(region.reshape(-1, 3), 0)).sum(-1) > 60
    ys, xs = np.nonzero(mark)
    if len(ys) < 20 or mark.mean() > 0.35:
        return best[1]
    snapped = (X0 + xs.min(), Y0 + ys.min(), X0 + xs.max() + 1, Y0 + ys.max() + 1)
    area = lambda b: (b[2] - b[0]) * (b[3] - b[1])
    return snapped if area(snapped) < 1.5 * area(best[1]) else best[1]  # not if it took in other art


def remove_mesh(img, ratio=3.0, rmin=30, even=5):
    """The jersey's mesh is a regular lattice of dots: its peaks are notched out of the image's
    spectrum, which leaves the folds, lettering and edges as they are."""
    import cv2
    from scipy import ndimage
    h, w, _ = img.shape
    lum = img.mean(-1)
    mag = np.log1p(np.abs(np.fft.fftshift(np.fft.fft2(lum - lum.mean()))))
    background = ndimage.median_filter(mag.astype(np.float32), size=15)
    yy, xx = np.mgrid[0:h, 0:w]
    radius = np.hypot((yy - h / 2) / h * 1024, (xx - w / 2) / w * 1024)
    peaks = (mag - background > np.log(ratio)) & (radius > rmin)
    notch = cv2.GaussianBlur(cv2.dilate(peaks.astype(np.uint8), np.ones((5, 5), np.uint8)).astype(np.float32), (0, 0), 1.5)
    keep = 1 - np.clip(notch * 1.5, 0, 1)
    out = np.empty_like(img)
    for c in range(3):
        out[..., c] = np.real(np.fft.ifft2(np.fft.ifftshift(np.fft.fftshift(np.fft.fft2(img[..., c])) * keep)))
    # 4x4 blocks that are now nearly even become one flat colour: the fabric reads as solid and
    # the texture packs small enough to fit back in the disc's room for it
    b = np.clip(out, 0, 255).reshape(h // 4, 4, w // 4, 4, 3)
    m = b.mean((1, 3), keepdims=True)
    b = np.where(np.abs(b - m).max((1, 3, 4), keepdims=True) < even, m, b)
    return b.reshape(h, w, 3)


def encode_changed(x, before, after, all_blocks=False, level=None):
    """x (a DXT1 texture) with the 4x4 blocks of its full-size level (or of level, a mip level's
    (offset, width, height)) where after (RGB) differs from before re-encoded from after."""
    fmt, w, h, _ = fetch(x)
    base = 0
    if level:
        base, w, h = level
    if fmt != 18:
        raise ValueError("cloth edits need a DXT1 texture")
    wb, hb = w // 4, h // 4
    diff = np.abs(after[..., :3].astype(int) - before[..., :3].astype(int)).max(-1) > 2
    changed = diff.reshape(hb, 4, wb, 4).any((1, 3)) | all_blocks
    if before.shape[-1] == 4:  # leave blocks with see-through texels as they are
        changed &= ~(before[..., 3] < 255).reshape(hb, 4, wb, 4).any((1, 3))
    offs = _tiled(wb, hb, 3)
    b = bytearray(x)
    ys, xs = np.nonzero(changed)
    if not len(ys):
        return x
    px = after[..., :3].astype(float).reshape(hb, 4, wb, 4, 3).transpose(0, 2, 1, 3, 4)[ys, xs].reshape(-1, 16, 3)
    mean = px.mean(1, keepdims=True)
    centred = px - mean
    axis = np.array([np.linalg.svd(c, full_matrices=False)[2][0] if np.abs(c).sum() else [1, 1, 1] for c in centred])
    proj = (centred * axis[:, None, :]).sum(-1)
    hi = mean[:, 0] + axis * proj.max(1, keepdims=True)
    lo = mean[:, 0] + axis * proj.min(1, keepdims=True)
    c0, c1 = _pack565(np.clip(hi, 0, 255)), _pack565(np.clip(lo, 0, 255))
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = _unpack565(c0), _unpack565(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    idx = np.abs(px[:, :, None, :] - palette[:, None, :, :]).sum(-1).argmin(-1)
    idx = np.where((c0 == c1)[:, None], 0, idx)
    bits = (idx.astype(np.uint32) << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    for k, (y, xb) in enumerate(zip(ys, xs)):
        o = HEADER + base + offs[y, xb]
        struct.pack_into(">HHHH", b, o, c0[k], c1[k], bits[k] & 0xFFFF, bits[k] >> 16)
    return bytes(b)


def mip_levels(x):
    """(offset after the header, width, height) of each mip level below the full size that has
    tiles of its own: the levels down to 32 texels on the short side. The smaller ones share the
    packed mip tail, which is left as it is (they are only seen from far away)."""
    fmt, w, h, _ = fetch(x)
    bpb = 8 if fmt == 18 else 16
    offset = (struct.unpack(">I", x[0x5C:0x60])[0] >> 12) << 12  # the fetch constant's mip address
    levels = []
    w, h = w // 2, h // 2
    while offset and min(w, h) > 16:
        levels.append((offset, w, h))
        offset += ((w // 4 + 31) // 32 * 32) * ((h // 4 + 31) // 32 * 32) * bpb
        w, h = w // 2, h // 2
    return levels


def decode_level(x, level):
    """RGBA image of one mip level, (offset, width, height)."""
    offset, w, h = level
    header = bytearray(x[:HEADER])
    d2 = struct.unpack(">I", header[0x50:0x54])[0]
    struct.pack_into(">I", header, 0x50, (d2 & ~((1 << 26) - 1)) | ((h - 1) << 13) | (w - 1))
    return decode(bytes(header) + x[HEADER + offset:])


def _even(img, even):
    """img with its nearly even 4x4 blocks (no channel further than even from the mean) made one
    colour: they pack smaller."""
    if not even:
        return img
    h, w = img.shape[:2]
    b = img.reshape(h // 4, 4, w // 4, 4, -1)
    m = b.mean((1, 3), keepdims=True)
    return np.where(np.abs(b - m).max((1, 3, 4), keepdims=True) < even, m, b).reshape(img.shape)


def rebuild_mips(x, even=0):
    """x (DXT1) with its mip levels made again from its full-size level, so a design changed there
    shows from a distance too (the game draws players on the field from the smaller levels)."""
    if fetch(x)[0] != 18:
        return x
    full = decode(x).astype(float)
    for level in mip_levels(x):
        _, w, h = level
        f = full.shape[0] // h
        small = _even(full.reshape(h, f, w, f, 4).mean((1, 3)), even)
        before = decode_level(x, level)
        x = encode_changed(x, before, np.clip(np.round(small), 0, 255).astype(np.uint8), all_blocks=True, level=level)
    return x


def cloth(team, part, x, even=5, name="", original=None):
    """x with the 2026 cloth: the swoosh for the Reebok mark, solid fabric for the mesh."""
    if part in HASHED_PARTS.get(team, ()):
        return x, ""
    if part not in SWOOSHES and part not in MESH_PARTS:
        return x, ""
    before = decode(x)
    img = before[..., :3].astype(float)
    note = []
    if part in SWOOSHES:
        _, angle, mirror = SWOOSHES[part]
        box = MARK_BOXES.get(name[:-4]) or find_mark(part, decode(original or x)[..., :3].astype(float))
        note.append("swoosh" if box and replace_mark(img, box, angle, mirror) else "no Reebok mark found")
    if part in MESH_PARTS:
        img = remove_mesh(img, even=even)
        note.append("solid fabric")
    return encode_changed(x, before, np.clip(np.round(img), 0, 255).astype(np.uint8)), ", ".join(note)


# --- redesigns ---------------------------------------------------------------------------------

def _norm_blur(v, mask, sigma):
    import cv2
    m = mask.astype(np.float32)
    return cv2.GaussianBlur(v * m, (0, 0), sigma) / np.maximum(cv2.GaussianBlur(m, (0, 0), sigma), 1e-3)


def plain_torso(img, body, collar, wordmark="keep", keep_box=(468, 1140, 560, 1236),
                wordmark_box=(330, 1236, 694, 1350), neck_reach=60):
    """The jersey (original colours, HxWx3 floats) as one plain body colour, folds kept: yokes,
    side panels and piping all go; the ring round the neck becomes collar; the NFL shield in
    keep_box stays; the chest wordmark in wordmark_box stays as it is ("keep"), goes (None) or
    takes a colour."""
    import cv2
    h, w, _ = img.shape
    lum = img.mean(-1).astype(np.float32)
    for dark in (10, 18):  # the neck hole: the big near-black blob in the middle
        hole = cv2.morphologyEx((lum < dark).astype(np.uint8), cv2.MORPH_OPEN, np.ones((9, 9), np.uint8))
        n, lab, st, _ = cv2.connectedComponentsWithStats(hole)
        neck = np.isin(lab, [i for i in range(1, n) if 20000 < st[i, 4] < 400000 and 300 < st[i, 0] + st[i, 2] / 2 < 724])
        if neck.sum() > 40000:
            break
    near_neck = cv2.dilate(neck.astype(np.uint8), np.ones((2 * neck_reach + 1,) * 2, np.uint8)).astype(bool)
    # the cloth's colours: k-means on a sample, the commonest one is the body
    sample = img[~neck][:: 37].astype(np.float32)
    _, labels, centres = cv2.kmeans(sample, 6, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 20, 1.0),
                                    3, cv2.KMEANS_PP_CENTERS)
    main = np.bincount(labels.ravel(), minlength=6).argmax()
    cls = np.linalg.norm(img[:, :, None, :] - centres[None, None], axis=-1).argmin(-1)
    keep = np.zeros((h, w), bool)
    x0, y0, x1, y1 = keep_box
    keep[y0:y1, x0:x1] = True
    keep &= np.linalg.norm(img - centres[main], axis=-1) > 60  # by the shield only the shield stays
    mark = np.zeros((h, w), bool)
    if wordmark is not None:
        x0, y0, x1, y1 = wordmark_box
        smooth = cv2.medianBlur(np.clip(img[y0:y1, x0:x1], 0, 255).astype(np.uint8), 5).astype(float)  # not the mesh
        letters = (np.linalg.norm(smooth - centres[main], axis=-1) > 60).astype(np.uint8)
        n, lab, st, _ = cv2.connectedComponentsWithStats(letters)
        mark[y0:y1, x0:x1] = np.isin(lab, [i for i in range(1, n) if st[i, 4] >= 15])
        mark = cv2.dilate(mark.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(bool) & ~neck
    # folds: each colour's smoothed light against its broad average, as strong as the body's
    ratio, spread = np.ones((h, w), np.float32), {}
    for k in range(len(centres)):
        mk = (cls == k) & ~neck
        if mk.sum() < 200:
            continue
        r = _norm_blur(lum, mk, 4) / np.maximum(_norm_blur(lum, mk, 40), 1)
        ratio[mk], spread[k] = r[mk], np.std(r[mk])
    shade = np.ones((h, w), np.float32)
    for k, sd in spread.items():
        mk = (cls == k) & ~neck
        shade[mk] = 1 + (ratio[mk] - 1) * min(1.0, spread.get(main, 0.04) / max(sd, 1e-3))
    shade = np.clip(shade, 0.8, 1.08)
    # the collar: what is not body right round the neck hole, filled in to the hole
    ring = near_neck & (cls != main) & ~neck & ~keep & (lum > 12)
    ring = cv2.morphologyEx((ring | neck).astype(np.uint8), cv2.MORPH_CLOSE, np.ones((25, 25), np.uint8))
    filled = ring.copy()
    cv2.floodFill(filled, np.zeros((h + 2, w + 2), np.uint8), (0, 0), 1)
    collar_m = ((ring > 0) | (filled == 0)) & ~neck & ~keep & near_neck
    body_m = ~neck & ~keep & ~collar_m & ~mark
    out = img.copy()
    out[body_m] = np.array(body, float) * shade[body_m, None]
    lit = collar_m & (lum > 30)
    fall = np.clip(lum / max(np.median(lum[lit]) if lit.any() else 1, 1), 0, 1.1)  # into the neck's shadow
    out[collar_m] = np.array(collar, float) * fall[collar_m, None]
    if wordmark not in (None, "keep"):
        out[mark] = np.array(wordmark, float) * np.clip(lum / max(np.median(lum[mark]), 1), 0.7, 1.2)[mark, None] \
            if mark.any() else out[mark]
    return np.clip(out, 0, 255)


def _cloth_shade(img, sigma=3):
    """Folds of the main cloth (the colour most of img is), spread smoothly over the rest."""
    import cv2
    med = np.median(img.reshape(-1, 3), 0)
    main = np.linalg.norm(img - med, axis=-1) < 40
    lum = img.mean(-1).astype(np.float32)
    reach = cv2.GaussianBlur(main.astype(np.float32), (0, 0), sigma)
    ratio = _norm_blur(lum, main, sigma) / max(float(np.median(lum[main])), 1)
    return np.clip(np.where(reach > 0.05, ratio, 1.0), 0.8, 1.1)


def _keep_mark(part, img, name, plain):
    """plain without the Reebok mark's own pixels (the swoosh is drawn there later)."""
    box = MARK_BOXES.get(name) or find_mark(part, img)
    if box:
        x0, y0, x1, y1 = box
        region = img[y0:y1, x0:x1]
        plain[y0:y1, x0:x1] &= np.abs(region - np.median(region.reshape(-1, 3), 0)).sum(-1) < 60
    return plain


def _bands(out, shade, bands, at, axis=1):
    """Stripes [(rgb, width), ...] side by side from at along axis (1: across x, 0: across y)."""
    for colour, wide in bands:
        sl = (slice(None), slice(at, at + wide)) if axis == 1 else (slice(at, at + wide), slice(None))
        out[sl] = np.array(colour, float) * shade[sl][..., None]
        at += wide


PANTS_SEAMS = (492, 1542)  # x of the side seams on the pants texture
PANTS_KEEP = [(700, 0, 1350, 70), (900, 0, 1150, 330), (1220, 60, 1330, 150)]  # belt, fly, NFL shield


def redesign(part, img, kit, name=""):
    """The part (original colours, HxWx3 floats) in the kit's design: a TEAMS[...]["kit"] entry,
    {"body", "collar", "wordmark", "sleeve", "stripes", "stripes_at", "pants", "pants_stripes",
    "socks"} (any optional but body for the jersey)."""
    out = img.copy()
    if part == "TORSOBAKED":
        return plain_torso(img, kit["body"], kit.get("collar", kit["body"]), kit.get("wordmark", "keep"))
    shade = _cloth_shade(img)
    if part in ("SLEEVEBAKED", "RSLEEVEBAKED"):
        plain = _keep_mark(part, img, name, np.ones(img.shape[:2], bool))
        out[plain] = np.array(kit.get("sleeve", kit["body"]), float) * shade[plain, None]
        _bands(out, shade, kit.get("stripes", []), kit.get("stripes_at", 156))
    elif part == "PANTSBAKED" and "pants" in kit:
        import cv2
        plain = _keep_mark(part, img, name, np.ones(img.shape[:2], bool))
        # in the belt, fly and shield boxes only what is not cloth stays (the cloth colours being
        # those covering much of the pants)
        sample = img.reshape(-1, 3)[::53].astype(np.float32)
        _, labels, centres = cv2.kmeans(sample, 5, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 20, 1.0),
                                        3, cv2.KMEANS_PP_CENTERS)
        cloth = centres[np.bincount(labels.ravel(), minlength=5) > 0.06 * len(labels)]
        for x0, y0, x1, y1 in PANTS_KEEP:
            box = img[y0:y1, x0:x1]
            near = np.linalg.norm(box[:, :, None, :] - cloth[None, None], axis=-1).min(-1) < 60
            plain[y0:y1, x0:x1] &= near
        out[plain] = np.array(kit["pants"], float) * shade[plain, None]
        bands = [(c, 4 * w) for c, w in kit.get("pants_stripes", [])]  # 2048 texels across the pants
        width = sum(w for _, w in bands)
        for seam in PANTS_SEAMS:
            _bands(out, shade, bands, seam - width // 2)
    elif part == "SOCKS" and "socks" in kit:  # the sock above the white sanitary
        top = int(img.shape[0] * 0.44)
        s = shade.copy()
        s[:top] = _cloth_shade(img[:top])
        out[:top] = np.array(kit["socks"], float) * s[:top, :, None]
    return np.clip(out, 0, 255)


def number_pairs(x, colours):
    """Colour pairs taking the number (or name) texture x's own colours, fill first then the
    outlines outward, to colours (as many as given; the last repeats)."""
    import cv2
    from scipy import ndimage
    d = decode(x)
    solid = d[..., 3] > 200
    if solid.sum() < 50:
        return []
    depth = ndimage.distance_transform_edt(solid)
    px = d[..., :3][solid].astype(np.float32)
    k = min(3, max(2, len(colours)))
    _, labels, centres = cv2.kmeans(px, k, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 20, 1.0), 3,
                                    cv2.KMEANS_PP_CENTERS)
    order = np.argsort([-depth[solid][labels.ravel() == i].mean() for i in range(k)])
    return [(tuple(int(v) for v in centres[i]), tuple(colours[min(j, len(colours) - 1)])) for j, i in enumerate(order)]


# --- far-away models --------------------------------------------------------------------------

def _resized(img, w, h):
    from PIL import Image
    return np.array(Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize((w, h), Image.BOX)).astype(float)


def _relit(lod, fake_before, fake_after):
    """fake_after (an image laid out like lod) with lod's own baked light: lod's brightness against
    what the same layout made from the original full-size textures (fake_before) gives."""
    lum = lambda im: im.mean(-1, keepdims=True)
    ratio = np.clip((lum(lod) + 1) / (lum(fake_before) + 1), 0, 1.5)
    return np.clip(fake_after * ratio, 0, 255)


def far_models(team, kit_side, spec, textures, raw):
    """The far-away (LOD) versions of a kit made again from its new full-size textures:
    <team><H|A>PANTSSLEEVES (both sleeves above the pants) and the socks band of
    <team><H|A>HELMETSOCKSSHOES, whose helmet shell also takes the team's new shell colour.
    textures: name -> (original, new) decompressed full-size textures. Returns name -> new
    decompressed LOD texture."""
    out = {}
    side = team + kit_side
    def pair(part):
        name = f"{side}{part}_COL"
        if name in textures:
            return [decode(t)[..., :3].astype(float) for t in textures[name]]
        path = os.path.join(raw, name + ".orig")
        if not os.path.exists(path):
            return None
        img = decode(zlib.decompress(open(path, "rb").read()))[..., :3].astype(float)
        return [img, img]
    def lod_data(name):  # the texture as the loop left it (recoloured), else as on the disc
        if name in textures:
            return textures[name][1]
        return zlib.decompress(open(os.path.join(raw, name + ".orig"), "rb").read())
    path = os.path.join(raw, f"{side}PANTSSLEEVES_COL.orig")
    sleeve, rsleeve, pants = pair("SLEEVEBAKED") or pair("RSLEEVEBAKED"), pair("RSLEEVEBAKED") or pair("SLEEVEBAKED"), pair("PANTSBAKED")
    if os.path.exists(path) and sleeve and pants:
        data = lod_data(f"{side}PANTSSLEEVES_COL")
        lod = decode(data)
        h, w = lod.shape[:2]
        fakes = []
        for k in (0, 1):
            fake = np.zeros((h, w, 3))
            fake[:h // 2, :w // 2] = _resized(sleeve[k], w // 2, h // 2)
            fake[:h // 2, w // 2:] = _resized(rsleeve[k], w // 2, h // 2)
            fake[h // 2:] = _resized(pants[k], w, h // 2)
            fakes.append(fake)
        img = _relit(lod[..., :3].astype(float), *fakes)
        room = len(open(path, "rb").read())
        for even in (0, 6, 10, 16, 24):
            new = encode_changed(data, lod, np.round(_even(img, even)).astype(np.uint8), all_blocks=True)
            new = rebuild_mips(new, even)
            if compress(new, room) is not None:
                break
        out[f"{side}PANTSSLEEVES_COL"] = (data, new)
    path = os.path.join(raw, f"{side}HELMETSOCKSSHOES_COL.orig")
    socks = pair("SOCKS")
    if os.path.exists(path) and (socks or spec.get("shell")):
        import cv2
        data = lod_data(f"{side}HELMETSOCKSSHOES_COL")
        lod = decode(data)
        img = lod[..., :3].astype(float).copy()
        h, w = img.shape[:2]
        if socks:  # rows h/2 .. 3h/4: the socks texture squeezed in
            band = slice(h // 2, h * 3 // 4)
            img[band] = _relit(img[band], _resized(socks[0], w, h // 4), _resized(socks[1], w, h // 4))
        if spec.get("shell"):  # the top half: the helmet, its commonest colour the shell
            top = img[:h // 2]
            sample = top.reshape(-1, 3).astype(np.float32)
            _, labels, centres = cv2.kmeans(sample, 4, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 20, 1.0),
                                            3, cv2.KMEANS_PP_CENTERS)
            shell = centres[np.bincount(labels.ravel(), minlength=4).argmax()]
            near = np.linalg.norm(top - shell, axis=-1) < 70
            ratio = np.clip((top.mean(-1) + 1) / (shell.mean() + 1), 0.5, 1.4)
            top[near] = np.clip(np.array(spec["shell"], float) * ratio[near, None], 0, 255)
        # Only the blocks that changed, evened out (nearly even 4x4 blocks made one colour) as far
        # as it takes to pack into the little room the file has.
        room = len(open(path, "rb").read())
        for even in (0, 6, 10, 16, 24, 40):
            new = encode_changed(data, lod, np.round(_even(img, even)).astype(np.uint8))
            if compress(new, room) is not None:
                break
        out[f"{side}HELMETSOCKSSHOES_COL"] = (data, new)
    return out


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
    parser.add_argument("--part", action="append", help="only this part (e.g. SLEEVEBAKED); helmets are left out")
    parser.add_argument("--preview", action="store_true", help="before/after sheets in build/uniform_preview")
    args = parser.parse_args()
    if not os.path.isdir(RAW):
        raise SystemExit(f"{RAW} is missing: run uniform_extract.py first")
    os.makedirs(OUT, exist_ok=True)
    for team in TEAMS_ALL:
        spec = TEAMS.get(team, {})
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
                pairs = spec.get("numbers_" + name[len(team)], []) + pairs + spec.get("numbers", [])
            files.append((name, path, pairs, m.group(1) if m else None))
        for name, path, pairs, part in files:
            if args.part and part not in args.part:
                continue
            orig = open(path, "rb").read()
            data = zlib.decompress(orig)
            new = recolour(data, pairs) if pairs else data
            kit = spec.get("kit", {}).get(name[len(team)]) if part else None
            if kit and part in ("NUM", "NAME") and "num" in kit:
                new = recolour(data, number_pairs(data, kit["num"]))
            if kit and {"TORSOBAKED": "body", "SLEEVEBAKED": "body", "RSLEEVEBAKED": "body", "PANTSBAKED": "pants",
                        "SOCKS": "socks"}.get(part) in kit:
                img = redesign(part, decode(data)[..., :3].astype(float), kit, name[:-4])
                new = encode_changed(new, decode(new), img, all_blocks=True)
            recoloured = new
            for even in (5, 8, 12, 16):  # a jersey that packs too big is evened out further
                new, note = cloth(team, part, recoloured, even, name, data)
                if new == data:
                    break
                result = "does not fit"
                # what changed at full size, at a distance too: the mip levels made again (evened
                # out if need be to fit, or left as they were as a last resort)
                mips = part in ("TORSOBAKED", "SLEEVEBAKED", "RSLEEVEBAKED", "PANTSBAKED", "SOCKS")
                for attempt in ([lambda: rebuild_mips(new), lambda: rebuild_mips(new, 8), lambda: rebuild_mips(new, 16)]
                                if mips else []) + [lambda: new]:
                    candidate = attempt()
                    if compress(candidate, len(orig)) is not None:
                        new = candidate
                        break
                result = write_pair(name, orig, new)
                if result == "written" or part not in MESH_PARTS:
                    break
            if new != data:
                print(f"{name}: {result}{' (' + note + ')' if note else ''}", flush=True)
                shown[name] = (data, new)
        if not args.part:
            # the far-away models of each kit, from the new full-size textures
            full = dict(shown)
            for side in "HA":
                if not (spec.get("kit", {}).get(side) or spec.get("shell")):
                    continue
                for name, (_, new) in far_models(team, side, spec, full, RAW).items():
                    orig = open(os.path.join(RAW, name + ".orig"), "rb").read()
                    print(f"{name}: {write_pair(name, orig, new)} (far model)", flush=True)
                    shown[name] = (zlib.decompress(orig), new)
        if (spec.get("shell") or spec.get("facemask")) and not args.part:
            path = os.path.join(RAW, team + "HELMET.orig")
            orig = open(path, "rb").read()
            data = zlib.decompress(orig)
            print(f"{team}HELMET: {write_pair(team + 'HELMET', orig, set_helmet(data, spec.get('shell'), spec.get('facemask')))}")
        if args.preview and shown:
            preview(team, shown)
    print(f"done; now run uniform_patches.py to lay them over the disc")


if __name__ == "__main__":
    main()
