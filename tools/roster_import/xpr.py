"""The disc's textures: finds packages in its archives, reads their XPR2 textures, decodes them and
hashes them as the game's texture replacements do (src/gpu/texture_cache.cpp TextureContentHash).

Needs numpy and xxhash (pip install numpy xxhash).
"""

import mmap
import os
import re
import struct
import sys
import zlib

import numpy as np
import xxhash

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import xdvdfs  # noqa: E402

# Format: (bytes per block, block side in texels, kind)
FORMATS = {
    0x06: (4, 1, "8888"),
    0x12: (8, 4, "dxt1"),
    0x13: (16, 4, "dxt3"),
    0x14: (16, 4, "dxt5"),
    0x31: (16, 4, "dxn"),
    0x3A: (8, 4, "dxt5a"),
    0x3B: (8, 4, "ctx1"),
    0x02: (1, 1, "8"),
    0x04: (2, 1, "565"),
}


class Fetch:
    def __init__(self, words):
        self.w = list(words)

    tiled = property(lambda s: bool(s.w[0] >> 31))
    pitch = property(lambda s: ((s.w[0] >> 22) & 0x1FF) * 32)
    format = property(lambda s: s.w[1] & 0x3F)
    endian = property(lambda s: (s.w[1] >> 6) & 3)
    base = property(lambda s: s.w[1] & 0xFFFFF000)
    width = property(lambda s: (s.w[2] & 0x1FFF) + 1)
    height = property(lambda s: ((s.w[2] >> 13) & 0x1FFF) + 1)
    mip_max = property(lambda s: (s.w[4] >> 6) & 0xF)
    dimension = property(lambda s: (s.w[5] >> 9) & 3)
    packed = property(lambda s: bool((s.w[5] >> 11) & 1))
    mip = property(lambda s: s.w[5] & 0xFFFFF000)
    stacked = property(lambda s: bool((s.w[1] >> 10) & 1))


class Texture:
    def __init__(self, name, fetch, data):
        self.name, self.fetch, self.data = name, fetch, data   # data: the package's data area

    def layout(self):
        f = self.fetch
        unit, block, _ = FORMATS[f.format]
        bw, bh = (f.width + block - 1) // block, (f.height + block - 1) // block
        pitch = max(f.pitch // block, bw)
        rows = (bh + 31) & ~31 if f.tiled else bh
        spitch = (pitch + 31) & ~31 if f.tiled else pitch
        return unit, block, bw, bh, spitch, rows

    def content_hash(self):
        """TextureContentHash: XXH3-64 of the base level's bytes, xor the mips' times 0x9E37..."""
        f = self.fetch
        unit, block, bw, bh, spitch, rows = self.layout()
        source = spitch * rows * unit
        levels = 1
        if f.mip != 0 and f.mip_max > 0:
            size_max = 0
            while (max(f.width, f.height) >> (size_max + 1)) != 0:
                size_max += 1
            levels = min(f.mip_max, size_max) + 1
        log2w = (f.width - 1).bit_length()
        log2h = (f.height - 1).bit_length()
        packed_base = 32 if not f.packed else (min(log2w, log2h) - 4 if min(log2w, log2h) > 4 else 0)
        mip_bytes = 0
        level = 1
        while level < levels and level <= packed_base:
            p = (max((1 << log2w) >> level, 1) + block - 1) // block
            r = (max((1 << log2h) >> level, 1) + block - 1) // block
            if f.tiled:
                p, r = (p + 31) & ~31, (r + 31) & ~31
            else:
                p = ((p * unit + 255) & ~255) // unit
            mip_bytes += (p * r * unit + 4095) & ~4095
            level += 1
        h = xxhash.xxh3_64_intdigest(bytes(self.data[f.base:f.base + source]))
        if mip_bytes:
            m = xxhash.xxh3_64_intdigest(bytes(self.data[f.mip:f.mip + mip_bytes]))
            h ^= (m * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        return h

    def blocks(self):
        """The base level's blocks, untiled and in little-endian order: (bh, bw, unit) uint8."""
        f = self.fetch
        unit, block, bw, bh, spitch, rows = self.layout()
        raw = np.frombuffer(bytes(self.data[f.base:f.base + spitch * rows * unit]), np.uint8)
        if f.endian == 1:   # 8in16
            raw = raw.reshape(-1, 2)[:, ::-1].reshape(-1)
        elif f.endian == 2:   # 8in32
            raw = raw.reshape(-1, 4)[:, ::-1].reshape(-1)
        elif f.endian == 3:   # 16in32
            raw = raw.reshape(-1, 2, 2)[:, ::-1].reshape(-1)
        ys, xs = np.mgrid[0:bh, 0:bw]
        if f.tiled:
            off = tiled_offset(xs, ys, spitch, unit.bit_length() - 1)
        else:
            off = (ys * spitch + xs) * unit
        idx = off[..., None] + np.arange(unit)
        return raw[idx]

    def decode(self):
        """The base level as RGBA uint8 (h, w, 4), or None for a format not decoded."""
        f = self.fetch
        kind = FORMATS[f.format][2]
        b = self.blocks()
        if kind == "8888":
            # Bytes in memory after the swap: A R G B? The renderer's as drawn: R G B A.
            px = b[..., [1, 2, 3, 0]]
            return px[: f.height, : f.width]
        if kind in ("dxt1", "dxt3", "dxt5"):
            return decode_dxt(b, kind)[: f.height, : f.width]
        return None


def tiled_offset(x, y, pitch, log2_bpp):
    """Xenos 2D tiling (Xenia's XGAddress2DTiledOffset), x and y in blocks: the byte offset."""
    aligned = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def _565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_dxt(b, kind):
    bh, bw = b.shape[:2]
    colour = b[..., 8:16] if kind != "dxt1" else b[..., 0:8]
    c0 = colour[..., 0].astype(np.int32) | (colour[..., 1].astype(np.int32) << 8)
    c1 = colour[..., 2].astype(np.int32) | (colour[..., 3].astype(np.int32) << 8)
    bits = (colour[..., 4].astype(np.uint32) | (colour[..., 5].astype(np.uint32) << 8) |
            (colour[..., 6].astype(np.uint32) << 16) | (colour[..., 7].astype(np.uint32) << 24))
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | (kind != "dxt1")
    p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 2)   # bh, bw, 4, 3
    alpha_pal = np.full((bh, bw, 4), 255, np.int32)
    if kind == "dxt1":
        alpha_pal[..., 3] = np.where(four, 255, 0)
    out = np.zeros((bh, 4, bw, 4, 4), np.uint8)
    for i in range(16):
        sel = (bits >> (2 * i)) & 3
        rgb = np.take_along_axis(pal, sel[..., None, None].astype(np.int64).repeat(3, -1), 2)[:, :, 0]
        a = np.take_along_axis(alpha_pal, sel[..., None].astype(np.int64), 2)[..., 0]
        out[:, i // 4, :, i % 4, :3] = rgb
        out[:, i // 4, :, i % 4, 3] = a
    if kind == "dxt3":
        ab = b[..., 0:8]
        for i in range(16):
            v = (ab[..., i // 2] >> (4 * (i % 2))) & 15
            out[:, i // 4, :, i % 4, 3] = v * 17
    elif kind == "dxt5":
        ab = b[..., 0:8].astype(np.int32)
        a0, a1 = ab[..., 0], ab[..., 1]
        abits = np.zeros(a0.shape, np.uint64)
        for k in range(6):
            abits |= ab[..., 2 + k].astype(np.uint64) << np.uint64(8 * k)
        apal = [a0, a1]
        for k in range(1, 7):
            apal.append(np.where(a0 > a1, ((7 - k) * a0 + k * a1) // 7,
                                 np.where(k < 5, ((5 - k) * a0 + k * a1) // 5, 0 if k == 5 else 255)))
        apal = np.stack(apal, -1)
        for i in range(16):
            sel = ((abits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64)
            out[:, i // 4, :, i % 4, 3] = np.take_along_axis(apal, sel[..., None], 2)[..., 0]
    return out.reshape(bh * 4, bw * 4, 4)


def read_xpr2(blob):
    """The textures of an XPR2 package: [Texture]."""
    if blob[:4] != b"XPR2":
        return []
    header, data_size, count = struct.unpack_from(">III", blob, 4)
    data = memoryview(blob)[12 + header:12 + header + data_size]
    out = []
    for i in range(count):
        kind, off, size, name_off = struct.unpack_from(">4sIII", blob, 16 + 16 * i)
        if kind != b"TX2D":
            continue
        end = blob.index(b"\0", 12 + name_off)
        name = blob[12 + name_off:end].decode("latin-1")
        fetch = Fetch(struct.unpack_from(">6I", blob, 12 + off + 0x1C))
        if fetch.format in FORMATS and fetch.dimension == 1:
            out.append(Texture(name, fetch, data))
    return out


class Disc:
    """The disc's archives, mapped."""

    def __init__(self, iso):
        base, entries = xdvdfs.entries(iso)
        self.archives = {p: (base + s * xdvdfs.SECTOR, l) for p, d, s, l in entries if not d}
        self.file = open(iso, "rb")
        self.m = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)

    def packages(self, archive, pattern):
        """(name, XPR2 bytes) for each package of `archive` whose name matches `pattern` (a regex
        on the upper-case name): its data is the first zlib stream after the name."""
        start, length = self.archives[archive]
        area = self.m[start:start + length]
        for m in re.finditer(rb"([A-Z0-9_]{3,40})\0", area):
            name = m.group(1).decode()
            if not re.fullmatch(pattern, name):
                continue
            z = area.find(b"\x78\xda", m.end(), m.end() + 0x100)
            if z < 0:
                continue
            d = zlib.decompressobj()
            try:
                blob = d.decompress(area[z:z + 0x2000000])
            except zlib.error:
                continue
            if blob[:4] == b"XPR2":
                yield name, blob
