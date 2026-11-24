"""Builds the menus' fonts (fonts/menus/*.ttf) from the Barlow family (SIL Open Font License, fonts/OFL.txt).

The game's menus are set in six TrueType fonts packed in qkl_interface.ast (src/common/menu_fonts.cpp puts these in
their place). A font here must not be larger than the one it replaces (80 to 90 KB), so each is cut down to the
characters the game's text uses (Latin-1 and Windows-1252's punctuation): the glyphs those characters map to (and
their components), with their metrics, outlines, hinting and kerning (Barlow's GPOS pair kerning as a 'kern' table,
which the game's font engine reads). Layout tables (GSUB, GPOS, GDEF) are left out.

Usage: python tools/menu_fonts.py <folder with the Barlow .ttf files> [out folder, default fonts/menus]
The Barlow fonts: https://github.com/google/fonts/tree/main/ofl/barlow and .../ofl/barlowcondensed
(Barlow-Medium, Barlow-Bold, BarlowCondensed-Regular, -Bold, -ExtraBold, -Black).
No third-party Python packages.
"""
import os
import struct
import sys

# The game's font -> the Barlow style in its place, the most bytes it may take (the original's size), and the
# original's Windows ascent and descent. The game lays text out by its fonts' vertical metrics (the baseline an
# ascent below the top of a line), and its menus were made for those: the replacements take them (the game's fonts
# all have ascender 714 = their cap height, descender -286, line gap 200, per 1000 units), so Barlow's capitals (700
# high) sit where the game's did.
FONTS = [
    ("HCHelvN107EBC", "BarlowCondensed-Black.ttf", 79942, (996, 236)),
    ("HCHelvN65M", "Barlow-Medium.ttf", 80290, (952, 221)),
    ("NCAAHelvN57C", "BarlowCondensed-Regular.ttf", 89376, (932, 212)),
    ("NCAAHelvN75B", "Barlow-Bold.ttf", 88120, (975, 218)),
    ("NCAAHelvN77BC", "BarlowCondensed-Bold.ttf", 87348, (961, 224)),
    ("NCAAHelvN87HC", "BarlowCondensed-ExtraBold.ttf", 88488, (975, 230)),
]
ASCENDER, DESCENDER, LINE_GAP = 714, -286, 200

# Latin-1 and the printable characters Windows-1252 adds.
CHARS = sorted(set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | {
    0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x017D,
    0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x017E, 0x0178})


class Font:
    def __init__(self, data):
        self.data = data
        count = struct.unpack_from(">H", data, 4)[0]
        self.tables = {}
        for i in range(count):
            tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
            self.tables[tag.decode("latin-1")] = data[offset:offset + length]

    def u16(self, tag, at):
        return struct.unpack_from(">H", self.tables[tag], at)[0]


def read_cmap(font):
    """Character -> glyph, from the (3,1) or (0,x) subtable of format 4 or 12."""
    cmap = font.tables["cmap"]
    count = struct.unpack_from(">H", cmap, 2)[0]
    best = None
    for i in range(count):
        platform, encoding, offset = struct.unpack_from(">HHI", cmap, 4 + 8 * i)
        fmt = struct.unpack_from(">H", cmap, offset)[0]
        if (platform == 3 and encoding in (1, 10)) or platform == 0:
            if best is None or fmt == 12:
                best = (offset, fmt)
    offset, fmt = best
    out = {}
    if fmt == 4:
        segs = struct.unpack_from(">H", cmap, offset + 6)[0] // 2
        ends = struct.unpack_from(">%dH" % segs, cmap, offset + 14)
        starts = struct.unpack_from(">%dH" % segs, cmap, offset + 16 + 2 * segs)
        deltas = struct.unpack_from(">%dh" % segs, cmap, offset + 16 + 4 * segs)
        range_at = offset + 16 + 6 * segs
        ranges = struct.unpack_from(">%dH" % segs, cmap, range_at)
        for s in range(segs):
            for c in range(starts[s], ends[s] + 1):
                if c == 0xFFFF:
                    continue
                if ranges[s] == 0:
                    g = (c + deltas[s]) & 0xFFFF
                else:
                    at = range_at + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack_from(">H", cmap, at)[0]
                    if g:
                        g = (g + deltas[s]) & 0xFFFF
                if g:
                    out[c] = g
    elif fmt == 12:
        groups = struct.unpack_from(">I", cmap, offset + 12)[0]
        for k in range(groups):
            start, end, glyph = struct.unpack_from(">III", cmap, offset + 16 + 12 * k)
            for c in range(start, end + 1):
                out[c] = glyph + c - start
    return out


def glyph_data(font):
    long_loca = font.u16("head", 50) == 1
    loca = font.tables["loca"]
    glyphs = font.u16("maxp", 4)
    if long_loca:
        offsets = struct.unpack_from(">%dI" % (glyphs + 1), loca, 0)
    else:
        offsets = [2 * o for o in struct.unpack_from(">%dH" % (glyphs + 1), loca, 0)]
    glyf = font.tables["glyf"]
    return [glyf[offsets[i]:offsets[i + 1]] for i in range(glyphs)]


def components(glyph):
    """(byte offset of the glyph index, glyph index) of a composite glyph's components."""
    out = []
    if len(glyph) < 10 or struct.unpack_from(">h", glyph, 0)[0] >= 0:
        return out
    at = 10
    while True:
        flags, index = struct.unpack_from(">HH", glyph, at)
        out.append((at + 2, index))
        at += 4 + (4 if flags & 0x0001 else 2)
        at += 2 if flags & 0x0008 else 4 if flags & 0x0040 else 8 if flags & 0x0080 else 0
        if not flags & 0x0020:
            return out


def class_def(table, at):
    fmt = struct.unpack_from(">H", table, at)[0]
    out = {}
    if fmt == 1:
        start, count = struct.unpack_from(">HH", table, at + 2)
        for k, c in enumerate(struct.unpack_from(">%dH" % count, table, at + 6)):
            out[start + k] = c
    else:
        count = struct.unpack_from(">H", table, at + 2)[0]
        for k in range(count):
            start, end, c = struct.unpack_from(">HHH", table, at + 4 + 6 * k)
            for g in range(start, end + 1):
                out[g] = c
    return out


def coverage(table, at):
    fmt, count = struct.unpack_from(">HH", table, at)
    if fmt == 1:
        return list(struct.unpack_from(">%dH" % count, table, at + 4))
    out = []
    for k in range(count):
        start, end, _ = struct.unpack_from(">HHH", table, at + 4 + 6 * k)
        out.extend(range(start, end + 1))
    return out


def value_size(fmt):
    return 2 * bin(fmt).count("1")


def x_advance(table, at, fmt):
    """The XAdvance of a ValueRecord (0 when absent)."""
    if not fmt & 0x0004:
        return 0
    skip = 2 * bin(fmt & 0x0003).count("1")
    return struct.unpack_from(">h", table, at + skip)[0]


def pair_kerning(font, keep):
    """(left, right) -> x adjustment, from the GPOS pair adjustment lookups (kern feature), for glyphs in keep."""
    if "GPOS" not in font.tables:
        return {}
    gpos = font.tables["GPOS"]
    feature_list = struct.unpack_from(">H", gpos, 6)[0]
    lookup_list = struct.unpack_from(">H", gpos, 8)[0]
    lookups = set()
    count = struct.unpack_from(">H", gpos, feature_list)[0]
    for k in range(count):
        tag, offset = struct.unpack_from(">4sH", gpos, feature_list + 2 + 6 * k)
        if tag == b"kern":
            at = feature_list + offset
            n = struct.unpack_from(">H", gpos, at + 2)[0]
            lookups.update(struct.unpack_from(">%dH" % n, gpos, at + 4))
    kerning = {}
    for index in sorted(lookups):
        lookup = lookup_list + struct.unpack_from(">H", gpos, lookup_list + 2 + 2 * index)[0]
        kind, _, subtables = struct.unpack_from(">HHH", gpos, lookup)
        for s in range(subtables):
            sub = lookup + struct.unpack_from(">H", gpos, lookup + 6 + 2 * s)[0]
            k = kind
            if k == 9:   # extension
                k, offset = struct.unpack_from(">HI", gpos, sub + 2)
                sub += offset
            if k != 2:
                continue
            fmt, cov, vf1, vf2 = struct.unpack_from(">HHHH", gpos, sub)
            firsts = coverage(gpos, sub + cov)
            size = value_size(vf1) + value_size(vf2)
            if fmt == 1:
                sets = struct.unpack_from(">H", gpos, sub + 8)[0]
                for i, first in enumerate(firsts[:sets]):
                    if first not in keep:
                        continue
                    ps = sub + struct.unpack_from(">H", gpos, sub + 10 + 2 * i)[0]
                    pairs = struct.unpack_from(">H", gpos, ps)[0]
                    for p in range(pairs):
                        at = ps + 2 + p * (2 + size)
                        second = struct.unpack_from(">H", gpos, at)[0]
                        value = x_advance(gpos, at + 2, vf1)
                        if second in keep and value and (first, second) not in kerning:
                            kerning[(first, second)] = value
            elif fmt == 2:
                cd1, cd2, n1, n2 = struct.unpack_from(">HHHH", gpos, sub + 8)
                classes1 = class_def(gpos, sub + cd1)
                classes2 = class_def(gpos, sub + cd2)
                seconds = {}
                for g in keep:
                    seconds.setdefault(classes2.get(g, 0), []).append(g)
                for first in firsts:
                    if first not in keep:
                        continue
                    c1 = classes1.get(first, 0)
                    for c2, glyphs in seconds.items():
                        if c2 >= n2:
                            continue
                        at = sub + 16 + (c1 * n2 + c2) * size
                        value = x_advance(gpos, at, vf1)
                        if value:
                            for second in glyphs:
                                kerning.setdefault((first, second), value)
    return kerning


def name_table(font, family, full_name):
    """The name table: Barlow's copyright, licence and designer records kept; the names the game asks for."""
    old = font.tables["name"]
    _, count, strings = struct.unpack_from(">HHH", old, 0)
    keep = {}
    for i in range(count):
        platform, encoding, language, nid, length, offset = struct.unpack_from(">HHHHHH", old, 6 + 12 * i)
        if platform == 3 and encoding == 1 and language == 0x409:
            keep[nid] = old[strings + offset:strings + offset + length].decode("utf-16-be")
    keep[1], keep[2], keep[4], keep[6] = family, "Regular", full_name, family
    keep[3] = "hc09 menus: " + family + " (" + keep.get(4, "") + ")"
    keep[5] = keep.get(5, "") + "; subset for the hc09 menus"
    for nid in (16, 17):
        keep.pop(nid, None)
    records = []
    blob = b""
    for nid in sorted(keep):
        text = keep[nid].encode("utf-16-be")
        records.append(struct.pack(">HHHHHH", 3, 1, 0x409, nid, len(text), len(blob)))
        blob += text
    return struct.pack(">HHH", 0, len(records), 6 + 12 * len(records)) + b"".join(records) + blob


def cmap_table(mapping):
    """A (3,1) format 4 subtable."""
    chars = sorted(c for c in mapping if c < 0xFFFF)
    segments = []
    for c in chars:
        g = mapping[c]
        if segments and segments[-1][1] == c - 1 and segments[-1][2] + (c - segments[-1][0]) == g:
            segments[-1][1] = c
        else:
            segments.append([c, c, g])
    segments.append([0xFFFF, 0xFFFF, 0])
    n = len(segments)
    ends = [s[1] for s in segments]
    starts = [s[0] for s in segments]
    deltas = [((s[2] - s[0]) & 0xFFFF) if s[0] != 0xFFFF else 1 for s in segments]
    search = 2 * (1 << (n.bit_length() - 1))
    body = struct.pack(">HHHH", 2 * n, search, (search // 2).bit_length() - 1, 2 * n - search)
    body += struct.pack(">%dH" % n, *ends) + b"\0\0" + struct.pack(">%dH" % n, *starts)
    body += struct.pack(">%dH" % n, *deltas) + struct.pack(">%dH" % n, *([0] * n))
    sub = struct.pack(">HHH", 4, 6 + len(body), 0) + body
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def subset(source, family, windows):
    font = Font(source)
    cmap = read_cmap(font)
    glyphs = glyph_data(font)
    wanted = {0}
    mapping = {}
    for c in CHARS:
        if c in cmap:
            mapping[c] = cmap[c]
            wanted.add(cmap[c])
    todo = list(wanted)
    while todo:
        g = todo.pop()
        for _, component in components(glyphs[g]):
            if component not in wanted:
                wanted.add(component)
                todo.append(component)
    order = sorted(wanted)
    new_index = {old: new for new, old in enumerate(order)}

    # glyf and loca (long offsets), components renumbered.
    glyf = b""
    loca = []
    for old in order:
        g = bytearray(glyphs[old])
        for at, component in components(bytes(g)):
            struct.pack_into(">H", g, at, new_index[component])
        g += b"\0" * (-len(g) % 4)
        loca.append(len(glyf))
        glyf += bytes(g)
    loca.append(len(glyf))

    # hmtx: every glyph a long metric.
    hmtx_old = font.tables["hmtx"]
    long_metrics = font.u16("hhea", 34)
    def metric(g):
        if g < long_metrics:
            return struct.unpack_from(">Hh", hmtx_old, 4 * g)
        advance = struct.unpack_from(">H", hmtx_old, 4 * (long_metrics - 1))[0]
        lsb = struct.unpack_from(">h", hmtx_old, 4 * long_metrics + 2 * (g - long_metrics))[0]
        return advance, lsb
    hmtx = b"".join(struct.pack(">Hh", *metric(g)) for g in order)

    tables = {}
    head = bytearray(font.tables["head"])
    struct.pack_into(">I", head, 8, 0)
    struct.pack_into(">h", head, 50, 1)
    tables["head"] = bytes(head)
    hhea = bytearray(font.tables["hhea"])
    struct.pack_into(">H", hhea, 34, len(order))
    struct.pack_into(">hhh", hhea, 4, ASCENDER, DESCENDER, LINE_GAP)
    tables["hhea"] = bytes(hhea)
    maxp = bytearray(font.tables["maxp"])
    struct.pack_into(">H", maxp, 4, len(order))
    tables["maxp"] = bytes(maxp)
    os2 = bytearray(font.tables["OS/2"])
    struct.pack_into(">HH", os2, 64, min(mapping), min(max(mapping), 0xFFFF))
    struct.pack_into(">hhhHH", os2, 68, ASCENDER, DESCENDER, LINE_GAP, *windows)
    tables["OS/2"] = bytes(os2)
    tables["hmtx"] = hmtx
    tables["loca"] = struct.pack(">%dI" % len(loca), *loca)
    tables["glyf"] = glyf
    tables["cmap"] = cmap_table({c: new_index[g] for c, g in mapping.items()})
    tables["post"] = struct.pack(">IIhhIIIII", 0x00030000, *struct.unpack_from(">Ihh", font.tables["post"], 4), *struct.unpack_from(">IIIII", font.tables["post"], 12))
    tables["name"] = name_table(font, family, family)
    for tag in ("cvt ", "fpgm", "prep", "gasp"):
        if tag in font.tables:
            tables[tag] = font.tables[tag]
    kerning = pair_kerning(font, wanted)
    if kerning:
        pairs = sorted((new_index[a], new_index[b], v) for (a, b), v in kerning.items())
        n = len(pairs)
        search = 6 * (1 << (n.bit_length() - 1))
        sub = struct.pack(">HHH", n, search, (search // 6).bit_length() - 1) + struct.pack(">H", 6 * n - search)
        sub += b"".join(struct.pack(">HHh", a, b, v) for a, b, v in pairs)
        tables["kern"] = struct.pack(">HH", 0, 1) + struct.pack(">HHH", 0, 6 + len(sub), 0x0001) + sub

    # The file: the table directory, then the tables, each 4-byte aligned.
    tags = sorted(tables)
    n = len(tags)
    search = 16 * (1 << (n.bit_length() - 1))
    out = struct.pack(">IHHHH", 0x00010000, n, search, (search // 16).bit_length() - 1, 16 * n - search)
    offset = 12 + 16 * n
    directory = b""
    body = b""
    for tag in tags:
        data = tables[tag]
        directory += struct.pack(">4sIII", tag.encode("latin-1"), checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font_bytes = bytearray(out + directory + body)
    head_at = font_bytes.index(b"head", 12)
    head_offset = struct.unpack_from(">I", font_bytes, head_at + 8)[0]
    struct.pack_into(">I", font_bytes, head_offset + 8, (0xB1B0AFBA - checksum(bytes(font_bytes))) & 0xFFFFFFFF)
    return bytes(font_bytes), len(order), len(kerning)


def main():
    source = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "fonts", "menus")
    os.makedirs(out, exist_ok=True)
    for family, barlow, limit, windows in FONTS:
        data, glyphs, pairs = subset(open(os.path.join(source, barlow), "rb").read(), family, windows)
        status = "ok" if len(data) <= limit else "TOO LARGE"
        print(f"{family:14} <- {barlow:30} {len(data):6} bytes (at most {limit}), {glyphs} glyphs, {pairs} kerning pairs: {status}")
        if len(data) <= limit:
            open(os.path.join(out, family + ".ttf"), "wb").write(data)


main()
