"""Minimal Xbox 360 XDVDFS (XGD) reader: list and extract files from a game-partition ISO."""
import os
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Known game-partition offsets: raw partition, XGD1, XGD2, XGD3
CANDIDATE_OFFSETS = [0, 0x18300000, 0xFD90000, 0x2080000]


def find_partition(f):
    for base in CANDIDATE_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise ValueError("XDVDFS volume descriptor not found")


def walk(f, base, sector, size, prefix=""):
    """Yield (path, is_dir, start_sector, size) for a directory table (binary tree)."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding
            continue
        name = table[off + 14: off + 14 + nlen].decode("latin-1")
        path = f"{prefix}/{name}" if prefix else name
        is_dir = bool(attr & 0x10)
        yield path, is_dir, start, length
        if is_dir:
            yield from walk(f, base, start, length, path)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def entries(iso):
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        return base, list(walk(f, base, root_sector, root_size))


def extract(iso, out_dir, only=None):
    base, ents = entries(iso)
    with open(iso, "rb") as f:
        for path, is_dir, start, length in ents:
            if is_dir or (only and path.lower() not in only):
                continue
            dst = os.path.join(out_dir, *path.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(base + start * SECTOR)
            remaining = length
            with open(dst, "wb") as o:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 20))
                    o.write(chunk)
                    remaining -= len(chunk)


if __name__ == "__main__":
    cmd, iso = sys.argv[1], sys.argv[2]
    if cmd == "list":
        base, ents = entries(iso)
        for path, is_dir, start, length in sorted(ents):
            print(f"{'D' if is_dir else 'F'} {length:>12} {path}")
    elif cmd == "extract":
        only = {p.lower() for p in sys.argv[4:]} or None
        extract(iso, sys.argv[3], only)
