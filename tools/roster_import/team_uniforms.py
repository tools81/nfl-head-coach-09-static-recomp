"""Updates teams' uniforms, helmets and team colours to the ones they wear now.

    python tools/roster_import/team_uniforms.py [--iso "iso/NFL Head Coach 09.iso"]
    python tools/roster_import/team_uniforms.py --inspect seahawks [--preview <folder>]
    python tools/roster_import/team_uniforms.py --roster hc27/ROSTER     (the menus' team colours only)
    python tools/roster_import/team_uniforms.py --export <folder>        (the logo textures, for team_logos.py)

A team's uniforms are packages in the disc's qkl_fe2ig.ast, one a kit (<TEAM>H, <TEAM>A and their
_ALTn: the sideline uniform, torso, sleeves, pants, socks, numbers, the masks and normal maps that go
with them), and its helmet's decal another (<TEAM>HELMET). Each colour texture of them is recoloured
(RECOLOURS: colours of its 2008 textures, the official colour each stood for and the one it stands
for now, moved in OKLab, so the cloth's shading and stitching stay) and written to
hc27/textures/hashed/<content hash>.png, which HC27 draws instead (see xpr.py: the same hash as the
game's). Normal maps, masks and flat maps are left.

A helmet's shell and facemask are colours of its material (an RSF stream after the helmet's
textures: the baseColor of helmetLeftShell_mat, helmetRightShell_mat and the facemask materials).
SHELLS changes them: the material is compressed again into the original's room and written to
hc27/disc as qkl_fe2ig.ast@<offset>.bin, which HC27 lays over the disc (see disc_patches.py).

Some of a team's other images (the menus' logos, the score bug's helmets) are recoloured too
(UI_RECOLOURS; UI_TEXTURES lists them, found by their order among the other teams'). Teams with a
new logo get it from team_logos.py, which takes the original images --export writes.

--inspect lists the main colours of each of a team's kit textures, to choose its old colours.
--roster sets the team colours of the game's menus (team_colours.py) in a roster save;
import_roster.py sets them too.

Needs numpy, xxhash and Pillow (pip install numpy xxhash pillow).
"""

import argparse
import os
import re
import struct
import zlib

import numpy as np

import xpr
from team_colours import set_team_colours
from team_logos import BACKDROPS as LOGO_BACKDROPS, TEAMS as LOGO_TEXTURES

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "hc27", "textures", "hashed")


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


# Team (its files' name) -> its helmet shell's colour now, or (shell, facemask), where they changed.
SHELLS = {
    "seahawks": "#06182F",
    "rams": "#0A2A85",
    "bills": ("#E6E6E6", "#0C2A6A"),
    "jets": ("#0B5A30", "#E2E2E2"),
    "browns": "#EF4A12",
}

# Team (its files' name) -> {"kits": [(a colour as drawn in its 2008 textures, the team's official
# colour it stands for then, the official colour it stands for now), or (a colour as drawn, the colour
# to draw instead)], "helmet": the same for its
# helmet's decal (default: the kits'), "<KIT>": one kit's own (e.g. "BRONCOSH", the home kit)}. A
# texture's pixels near the first colour take the third's hue, keeping their lightness and
# colourfulness in proportion (the textures are drawn darker and greyer than the colours they stand for).
_NAVY_08 = "#0C2340"
RECOLOURS = {
    # 2012: college navy, action green and wolf grey (the 2008 blue became navy).
    "seahawks": {
        "kits": [("#233549", "#0A2342", "#002244"), ("#314259", "#0A2342", "#002244"), ("#121726", "#0A2342", "#002244"),
                 ("#1B4275", "#1F3048"), ("#204B86", "#22344E"),   # the blue: the home kit's navy as drawn
                 ("#397732", "#5BAE3A", "#69BE28")],
        "helmet": [("#283C62", "#3B6AA0", "#A5ACAF"), ("#000C20", "#0A2342", "#002244")],
    },
    # 2020: royal and sol.
    "rams": {
        # Royal as the game draws it (its 1970s throwback kit's, #19225A).
        "kits": [("#181B29", "#1B2560"), ("#131524", "#172156"), ("#040817", "#0C1440"),
                 ("#957B46", "#B3995D", "#FFA300"), ("#817236", "#B3995D", "#FFA300")],
    },
    # 2011: royal blue for navy.
    "bills": {
        "kits": [("#151929", _NAVY_08, "#00338D"), ("#1C202D", _NAVY_08, "#00338D"), ("#202335", _NAVY_08, "#00338D")],
        "helmet": [],
    },
    # 2013: the brighter orange.
    "dolphins": {
        "kits": [("#DD5020", "#F58220", "#FC4C02"), ("#FF4000", "#F58220", "#FC4C02")],
        "helmet": [],   # a new logo (team_logos.py)
    },
    # 2020: powder blue for navy, sunshine gold.
    "chargers": {
        "kits": [("#0A1939", _NAVY_08, "#0080C6"), ("#041539", _NAVY_08, "#0080C6"), ("#0B1A34", _NAVY_08, "#0080C6"),
                 ("#CC8C00", "#FFB81C", "#FFC20E")],
        "helmet": [],   # a new logo (team_logos.py)
    },
    # 2024: legacy green.
    "jets": {
        "kits": [("#112B01", "#0C371D", "#046A38"), ("#133207", "#0C371D", "#046A38"), ("#122512", "#0C371D", "#046A38")],
    },
    # 2009: the classic red.
    "49ers": {
        "kits": [("#761616", "#8E1C1C", "#AA0000"), ("#690908", "#8E1C1C", "#AA0000"), ("#722120", "#8E1C1C", "#AA0000"),
                 ("#660C0A", "#8E1C1C", "#AA0000")],
        "helmet": [("#7C0009", "#8E1C1C", "#AA0000")],
    },
    # 2012: orange home jerseys, navy trim (the home kit's navy and orange trade places).
    "broncos": {
        "kits": [],
        "helmet": [],
        "BRONCOSH": [("#141836", "#002244", "#FB4F14"), ("#161A32", "#002244", "#FB4F14"), ("#231D41", "#002244", "#FB4F14"),
                     ("#201C39", "#002244", "#FB4F14"), ("#A83308", "#FB4F14", "#002244"), ("#DB5607", "#FB4F14", "#002244"),
                     ("#A53B20", "#FB4F14", "#002244"), ("#792209", "#FB4F14", "#002244")],
    },
    # 2013: black home jerseys, the brighter gold.
    "jaguars": {
        "kits": [("#C78721", "#9F792C", "#D7A22A"), ("#C49707", "#9F792C", "#D7A22A"), ("#CBA126", "#9F792C", "#D7A22A")],
        "helmet": [],
        "JAGUARSH": [("#105665", "#006778", "#101820"), ("#094B5A", "#006778", "#101820"), ("#0A5263", "#006778", "#101820"),
                     ("#315A65", "#006778", "#101820"), ("#2A5665", "#006778", "#101820"),
                     ("#C78721", "#9F792C", "#D7A22A"), ("#C49707", "#9F792C", "#D7A22A")],
    },
    # 2015: the brighter orange, a warmer brown.
    "browns": {
        "kits": [("#BC2700", "#E35F1E", "#FF3C00"), ("#A43308", "#E35F1E", "#FF3C00"), ("#B83507", "#E35F1E", "#FF3C00"),
                 ("#312020", "#382F2D", "#311D00")],
        "helmet": [],
    },
}

# The menus' images of the teams changed here: (archive, offset of its zlib stream), by content hash.
UI_TEXTURES = {
    "seahawks": {
        "38424377B24AE938": ("qkl_boot.ast", 0x16033B38, "large"),
        "11BF384F47C617DB": ("qkl_boot.ast", 0x161048E8, "small"),
        "13A6800F0944E2CB": ("qkl_fe2ig.ast", 0x537ABE40, "flat"),
        "CDC0DD91DA45B9B3": ("qkl_interface.ast", 0x29B4740, "score bug helmet"),
        "5822853C0AC17D41": ("qkl_interface.ast", 0x29EC630, "score bug helmet"),
    },
    "rams": {
        "8276266CCBC63D52": ("qkl_interface.ast", 0x29AD428, "score bug helmet"),
        "06040080E0A6CB4F": ("qkl_interface.ast", 0x29E50D8, "score bug helmet"),
    },
    "dolphins": {
        "D1DA611C23E9EE0D": ("qkl_boot.ast", 0x15F2F198, "large"),
        "FC1881A7129AA603": ("qkl_boot.ast", 0x160E8580, "small"),
        "E424803D7189F411": ("qkl_fe2ig.ast", 0x53779A20, "flat"),
        "1E4F7C5060619E24": ("qkl_interface.ast", 0x2998840, "score bug helmet"),
        "7D6D7A6129C3F961": ("qkl_interface.ast", 0x29CFF38, "score bug helmet"),
        "E41A97891C456E34": ("qkl_misc.ast", 0x280B300, "field"),
    },
    "chargers": {
        "9EF3F3026E965245": ("qkl_boot.ast", 0x15EF3500, "large"),
        "5695BF9684F8886E": ("qkl_boot.ast", 0x160E1440, "small"),
        "B23457CCD6B03EC2": ("qkl_fe2ig.ast", 0x5376EF20, "flat"),
        "F3FF604EEEEB837E": ("qkl_interface.ast", 0x2991E30, "score bug helmet"),
        "1AC4FCC4C499B678": ("qkl_interface.ast", 0x29C9570, "score bug helmet"),
        "B73A500772A1D6AE": ("qkl_misc.ast", 0x1FE5864, "field"),
    },
    "jets": {
        "3C9DCE9A65C5C976": ("qkl_boot.ast", 0x15F93878, "large"),
        "4849D2A237EDAA3F": ("qkl_boot.ast", 0x160F2CB8, "small"),
        "DCB5DCD8374BE2A9": ("qkl_fe2ig.ast", 0x5378B6D0, "flat"),
    },
    "jaguars": {
        "E9B5F584D2E8828F": ("qkl_boot.ast", 0x15F840E0, "large"),
        "D818446A0440AA2A": ("qkl_boot.ast", 0x160F1160, "small"),
        "511B589CF7E7EF46": ("qkl_fe2ig.ast", 0x53787710, "flat"),
        "3909F9C1F016B6B8": ("qkl_interface.ast", 0x29A1110, "score bug helmet"),
        "D7B9644298F1FB37": ("qkl_interface.ast", 0x29D8A50, "score bug helmet"),
        "B851C053ADAD99E6": ("qkl_misc.ast", 0x378762C, "field"),
    },
    "browns": {
        "1E170335E9B75E8F": ("qkl_boot.ast", 0x15EC10C8, "large"),
        "4D0C2360B3F896FA": ("qkl_boot.ast", 0x160DBC50, "small"),
        "1BFADA57EDF4A5F6": ("qkl_fe2ig.ast", 0x537625F0, "flat"),
        "32F0C69C972A1A95": ("qkl_interface.ast", 0x298CD68, "score bug helmet"),
        "1FD3A7152355B8DD": ("qkl_interface.ast", 0x29C4378, "score bug helmet"),
    },
    "falcons": {
        "5116494A23719290": ("qkl_boot.ast", 0x15F50898, "large"),
        "BB973DC788C0476C": ("qkl_boot.ast", 0x160EBF90, "small"),
        "7C44B63D045A970E": ("qkl_fe2ig.ast", 0x53782C50, "flat"),
        "DD4AA3481127E5DA": ("qkl_interface.ast", 0x299C1D8, "score bug helmet"),
        "9D90738A0BA084C1": ("qkl_interface.ast", 0x29D3980, "score bug helmet"),
        "2C2B5F6B2257581A": ("qkl_misc.ast", 0x30DE808, "field"),
    },
}

# Team -> (the recolouring to use, the roles of UI_TEXTURES it applies to). The rest of a team's
# images there (a new logo) are team_logos.py's.
UI_RECOLOURS = {
    "seahawks": ("helmet", ("large", "small", "flat")),
    "rams": ("kits", ("score bug helmet",)),
    "jets": ("kits", ("large", "small", "flat")),
    "browns": ("kits", ("score bug helmet",)),
}


# ---- colour -----------------------------------------------------------------------------------

def _lin(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _unlin(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0


# OKLab (Ottosson 2020; its hues stay put as lightness changes, CIELab's blues turn purple), scaled
# to CIELab's ranges (L 0..100, a and b about +-100).
_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])
_SCALE = np.array([100.0, 250.0, 250.0])


def to_lab(c):
    lms = np.cbrt(_lin(np.asarray(c, np.float64)) @ _M1.T)
    return (lms @ _M2.T) * _SCALE


def from_lab(lab):
    lms = (np.asarray(lab, np.float64) / _SCALE) @ np.linalg.inv(_M2).T
    return _unlin((lms ** 3) @ np.linalg.inv(_M1).T)


def anchor(drawn, then, now):
    """What a texture's colour `drawn` (Lab), standing for the official colour `then`, becomes for
    `now`: its lightness and colourfulness kept in proportion to the official ones (the textures are
    drawn darker and greyer than the colours they show once lit), the new colour's hue."""
    lightness = now[0] * drawn[0] / max(then[0], 1.0)
    c_drawn, c_then, c_now = (np.hypot(c[1], c[2]) for c in (drawn, then, now))
    chroma = c_drawn * c_now / c_then if c_then > 8 else c_now * lightness / max(now[0], 1.0)
    hue = np.arctan2(now[2], now[1])
    lightness = min(lightness, 100.0)
    # Less colourful until it is a colour sRGB has (else it clips to another hue).
    for _ in range(40):
        lab = np.array([lightness, chroma * np.cos(hue), chroma * np.sin(hue)])
        linear = ((lab / _SCALE) @ np.linalg.inv(_M2).T) ** 3 @ np.linalg.inv(_M1).T
        if linear.min() >= -0.001 and linear.max() <= 1.001:
            break
        chroma *= 0.92
    return lab


def recolour(px, pairs, reach=14.0):
    """px: (h, w, 4) uint8. Each pixel moves by the change of the old colours near it (OKLab distance,
    lightness counted half: the cloth's shading), weighted so a pixel exactly an old colour becomes
    the new one; its lightness relative to the old colour's stays."""
    if not pairs:
        return px
    lab = to_lab(px[..., :3])
    old = to_lab(np.array([p[0] for p in pairs], np.float64))
    # (drawn, then, now), or (drawn, the colour to draw instead).
    new = np.array([anchor(o, to_lab(np.array(p[1], np.float64)), to_lab(np.array(p[2], np.float64))) if len(p) == 3
                    else to_lab(np.array(p[1], np.float64)) for o, p in zip(old, pairs)])
    shift = np.zeros_like(lab)
    total = np.zeros(lab.shape[:2])
    for o, n in zip(old, new):
        d = lab - o
        dist2 = (0.5 * d[..., 0]) ** 2 + d[..., 1] ** 2 + d[..., 2] ** 2
        w = np.exp(-dist2 / (reach * reach))
        contrast = min(max(n[0] / max(o[0], 1.0), 0.5), 2.0)
        dl = (lab[..., 0] - o[0]) * contrast + n[0] - lab[..., 0]
        delta = np.stack([dl, np.full(dl.shape, n[1] - o[1]), np.full(dl.shape, n[2] - o[2])], -1)
        shift += w[..., None] * delta
        total += w
    shift /= np.maximum(total, 1.0)[..., None]
    out = px.copy()
    out[..., :3] = np.clip(from_lab(lab + shift) + 0.5, 0, 255).astype(np.uint8)
    return out


def is_colour_texture(px):
    """Not a normal map, a mask or a flat fill."""
    c = px[..., :3].reshape(-1, 3).astype(np.int32)
    if len(c) > 65536:
        c = c[:: len(c) // 65536]
    mean = c.mean(0)
    if mean[2] > 180 and abs(mean[0] - 128) < 40 and abs(mean[1] - 128) < 40:
        return False   # a normal map
    if c.std(0).max() < 3:
        return False   # flat
    hi, lo = c.max(1), c.min(1)
    pure = ((hi >= 240) & (lo <= 80)) | (hi < 16)
    if pure.mean() > 0.9:
        return False   # a mask (pure channels)
    return True


def pairs_of(recipe):
    return [tuple(rgb(c) for c in p) for p in recipe]


# ---- the disc ---------------------------------------------------------------------------------

def _streams(area, start, end):
    """(offset, data) of the zlib streams in area[start:end], in order."""
    pos = start
    while pos < end:
        z = area.find(b"\x78\xda", pos, end)
        if z < 0:
            return
        d = zlib.decompressobj()
        try:
            data = d.decompress(area[z:z + 0x1000000])
        except zlib.error:
            pos = z + 2
            continue
        if not d.eof:
            pos = z + 2
            continue
        yield z, data
        pos = z + 0x1000000 - len(d.unused_data)


_NAME = rb"(?<![A-Za-z0-9_])"   # a name starts here (not inside another)


def _archive(disc, name):
    start, length = disc.archives[name]
    return disc.m[start:start + length]


def team_textures(disc, team):
    """The team's kit and helmet decal textures: [(kit or "HELMET", Texture)]."""
    area = _archive(disc, "qkl_fe2ig.ast")
    t = team.upper()
    found, seen = [], set()
    # A kit: from its sideline uniform's directory to the next package's header.
    for m in re.finditer(_NAME + rb"(" + re.escape(t).encode() + rb"[HA](?:_ALT\d)?)SIDELINETORSO_COL\0", area):
        kit = m.group(1).decode()
        if kit in seen:
            continue
        seen.add(kit)
        for z, data in _streams(area, m.end(), m.end() + 0x800000):
            if data.startswith(b"38\r\n"):
                break
            for tex in xpr.read_xpr2(data):
                found.append((kit, tex))
    m = re.search(_NAME + re.escape(t).encode() + rb"HELMET\0", area)
    if m:
        for z, data in _streams(area, m.end(), m.end() + 0x100):
            for tex in xpr.read_xpr2(data):
                found.append(("HELMET", tex))
            break
    return found


def ui_texture(disc, archive, offset, hash_):
    """The image of UI_TEXTURES' entry: (h, w, 4) uint8, or None."""
    area = _archive(disc, archive)
    data = zlib.decompressobj().decompress(area[offset:offset + 0x1000000])
    for tex in xpr.read_xpr2(data):
        if f"{tex.content_hash():016X}" == hash_:
            return tex.decode()
    return None


def helmet_material(disc, team):
    """The team's helmet material (the RSF after its helmet's textures): (offset in qkl_fe2ig.ast,
    its compressed length, its data), or None."""
    area = _archive(disc, "qkl_fe2ig.ast")
    m = re.search(_NAME + re.escape(team.upper()).encode() + rb"HELMET\0", area)
    if not m:
        return None
    for z, data in _streams(area, m.end(), m.end() + 0x80000):
        if data[:3] == b"RSF":
            d = zlib.decompressobj()
            d.decompress(area[z:z + 0x100000])
            return z, 0x100000 - len(d.unused_data), data
    return None


def material_colours(data, materials):
    """The offsets of these materials' baseColor (three big-endian floats, sRGB 0..1)."""
    out = []
    for mat in materials:
        at = data.find(mat + b"\0")
        if at < 0:
            continue
        b = data.find(b"baseColor\0", at)
        if b >= 0:
            out.append(b + len(b"baseColor\0"))
    return out


SHELL_MATERIALS = (b"helmetLeftShell_mat", b"helmetRightShell_mat")
FACEMASK_MATERIALS = (b"facemask_mat", b"Hz6a_JctThruIn_facemask_mat")


def patch_helmet(data, size, shell, facemask=None):
    """The material with the shells' (and facemasks') colour changed, compressed to at most `size`
    bytes (the game reads the stream where the original was): the colours' lowest float bits are
    nudged until it fits."""
    data = bytearray(data)
    targets = [(at, shell) for at in material_colours(data, SHELL_MATERIALS)]
    if facemask:
        targets += [(at, facemask) for at in material_colours(data, FACEMASK_MATERIALS)]
    # First in 256ths (floats ending in zero bytes, which compress best), then nudged.
    for attempt in range(-1, 4096):
        for at, colour in targets:
            for k, v in enumerate(colour):
                if attempt < 0:
                    bits = struct.unpack(">I", struct.pack(">f", round(v / 255.0 * 256) / 256))[0]
                else:
                    bits = struct.unpack(">I", struct.pack(">f", v / 255.0))[0] ^ (attempt >> (4 * k) & 0xF)
                struct.pack_into(">I", data, at + 4 * k, bits)
        for level in (9, 8, 7, 6):
            c = zlib.compressobj(level, zlib.DEFLATED, 15, 9)
            out = c.compress(bytes(data)) + c.flush()
            if len(out) <= size:
                return out
    return None


def write_helmets(disc, out_dir):
    """Writes the changed helmet materials (all of SHELLS) to hc27/disc as qkl_fe2ig.ast@<offset>.bin
    (see disc_patches.py), replacing those it wrote before."""
    os.makedirs(out_dir, exist_ok=True)
    listing = os.path.join(out_dir, "team_uniforms.files")
    if os.path.exists(listing):
        for name in open(listing).read().split():
            if os.path.exists(os.path.join(out_dir, name)):
                os.remove(os.path.join(out_dir, name))
    written = []
    for team, colours in sorted(SHELLS.items()):
        shell, facemask = (colours, None) if isinstance(colours, str) else colours
        found = helmet_material(disc, team)
        if not found:
            print(f"{team}: no helmet material")
            continue
        z, size, data = found
        packed = patch_helmet(data, size, rgb(shell), rgb(facemask) if facemask else None)
        if packed is None:
            print(f"{team}: the helmet material does not fit when changed")
            continue
        name = f"qkl_fe2ig.ast@{z:X}.bin"
        open(os.path.join(out_dir, name), "wb").write(packed)
        written.append(name)
    open(listing, "w").write("\n".join(written) + ("\n" if written else ""))
    return written


def main_colours(px, n=6):
    c = px[..., :3].reshape(-1, 3)
    if px.shape[-1] == 4:
        c = c[px[..., 3].reshape(-1) > 128]
    q = (c // 16).astype(np.int32)
    keys, counts = np.unique(q[:, 0] * 256 + q[:, 1] * 16 + q[:, 2], return_counts=True)
    order = np.argsort(-counts)[:n]
    out = []
    for k in keys[order]:
        sel = c[(q[:, 0] * 256 + q[:, 1] * 16 + q[:, 2]) == k]
        mean = sel.mean(0)
        out.append(("#%02X%02X%02X" % tuple(int(v) for v in mean), len(sel) / len(c)))
    return out


def update_team(disc, team, out, preview=None):
    """Writes the team's recoloured kit, helmet decal and UI textures; returns how many."""
    from PIL import Image
    recipe = RECOLOURS.get(team, {})
    textures = team_textures(disc, team) if recipe else []
    # A texture several kits share takes the recolouring of the one with its own (e.g. the home kit).
    kits_of = {}
    for kit, tex in textures:
        kits_of.setdefault(tex.content_hash(), set()).add(kit)
    done, written = set(), 0
    logo_textures = {h for textures_ in LOGO_TEXTURES.values() for h in textures_}

    def save(name, px):
        Image.fromarray(px, "RGBA").save(os.path.join(out, name))
        if preview:
            os.makedirs(preview, exist_ok=True)
            Image.fromarray(px, "RGBA").save(os.path.join(preview, f"{team}_{name}"))

    for kit, tex in textures:
        h = tex.content_hash()
        if h in done or f"{h:016X}" in logo_textures:   # a new logo's (team_logos.py)
            continue
        done.add(h)
        px = tex.decode()
        if px is None or not is_colour_texture(px):
            continue
        own = sorted(k for k in kits_of[h] if k in recipe)
        if kit == "HELMET":
            pairs = recipe.get("helmet", recipe.get("kits", []))
        elif own:
            pairs = recipe[own[0]]   # the kit with its own recolouring wins (the home kit over an alternate)
        else:
            pairs = recipe.get("kits", [])
        if not pairs:
            continue
        save(f"{h:016X}.png", recolour(px, pairs_of(pairs)))
        written += 1
    if team in UI_RECOLOURS:
        which, roles = UI_RECOLOURS[team]
        pairs = recipe.get(which, recipe.get("kits", []))
        for h, (archive, offset, role) in UI_TEXTURES.get(team, {}).items():
            if role not in roles:
                continue
            px = ui_texture(disc, archive, offset, h)
            if px is not None:
                save(f"{h}.png", recolour(px, pairs_of(pairs)))
                written += 1
    return written


def export(disc, folder, teams):
    """The original images of the teams' UI_TEXTURES, loading-screen backdrops and helmet decals, as <hash>.png in `folder`
    (team_logos.py --dump folder)."""
    from PIL import Image
    os.makedirs(folder, exist_ok=True)
    for team in teams:
        for h, (archive, offset, role) in UI_TEXTURES.get(team, {}).items():
            px = ui_texture(disc, archive, offset, h)
            if px is not None:
                Image.fromarray(px, "RGBA").save(os.path.join(folder, f"{h}.png"))
        for h, offset in LOGO_BACKDROPS.get(team, {}).items():
            px = ui_texture(disc, "qkl_boot.ast", offset, h)
            if px is not None:
                Image.fromarray(px, "RGBA").save(os.path.join(folder, f"{h}.png"))
        if team not in RECOLOURS and team not in UI_TEXTURES:
            continue
        for kit, tex in team_textures(disc, team):
            if kit == "HELMET":
                px = tex.decode()
                if px is not None:
                    Image.fromarray(px, "RGBA").save(os.path.join(folder, f"{tex.content_hash():016X}.png"))
                    print(f"{team}: helmet decal {tex.content_hash():016X}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", default=os.path.join(ROOT, "iso", "NFL Head Coach 09.iso"))
    parser.add_argument("--team", nargs="*", help="these teams' textures only (their files' names, e.g. seahawks)")
    parser.add_argument("--inspect", help="list a team's kit textures and their main colours")
    parser.add_argument("--preview", help="with --inspect or the update: also save the images to this folder")
    parser.add_argument("--roster", help="set TEAM_COLOURS in this roster save instead")
    parser.add_argument("--export", help="write the original logo images of the teams to this folder instead")
    parser.add_argument("--disc", default=os.path.join(ROOT, "hc27", "disc"), help="where to write the helmets' patches")
    parser.add_argument("--out", default=OUT, help="where to write the textures (default hc27/textures/hashed)")
    args = parser.parse_args()

    if args.roster:
        from tdb import SaveFile
        save = SaveFile(args.roster)
        set_team_colours(save.db.tables["TEAM"])
        save.fix_checksums()
        save.save(args.roster)
        print(f"team colours set in {args.roster}")
        return

    disc = xpr.Disc(args.iso)
    if args.export:
        export(disc, args.export, args.team or sorted(set(UI_TEXTURES) | set(LOGO_BACKDROPS)))
        return
    if args.inspect:
        from PIL import Image
        for kit, tex in team_textures(disc, args.inspect):
            px = tex.decode()
            f = tex.fetch
            kind = xpr.FORMATS[f.format][2]
            line = f"{kit:16s} {f.width}x{f.height} {kind:5s} {tex.content_hash():016X}"
            if px is not None:
                line += " colour " if is_colour_texture(px) else " data   "
                line += " ".join(f"{c} {share:.0%}" for c, share in main_colours(px))
                if args.preview:
                    os.makedirs(args.preview, exist_ok=True)
                    Image.fromarray(px, "RGBA").save(os.path.join(args.preview, f"{kit}_{tex.content_hash():016X}.png"))
            print(line)
        return

    os.makedirs(args.out, exist_ok=True)
    for team in args.team or sorted(set(RECOLOURS) | set(UI_RECOLOURS)):
        print(f"{team}: {update_team(disc, team, args.out, args.preview)} textures")
    for name in write_helmets(disc, args.disc):
        print(f"helmet: {name}")


if __name__ == "__main__":
    main()
