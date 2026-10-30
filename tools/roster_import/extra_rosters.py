"""Puts HC27's roster into the disc's other copies of the default roster, which HC27 does not
replace (it replaces the one at qkl_boot.ast 0x72CD90, see src/common/edition.cpp).

    python tools/roster_import/extra_rosters.py --iso "D:/Games/NFL Head Coach 09.iso"

A career started in the off-season loads its own roster (qkl_boot.ast 0x5D1870): 2008's, a
year earlier (2156 players, before that year's moves). With HC27's portraits drawn by portrait id
(PSXP), it showed the new players' pictures over 2008's names and ratings (Jared Goff's picture
on Jon Kitna, whose record Goff took over). A third copy (0x80D0B0, the default roster without
injuries) is filled too.

These rosters have the same tables and fields as hc27/ROSTER, some with fewer records and one
(DRPP) with less room. Each table gets hc27/ROSTER's records and record count (and its empty
slots, as far as there is room), and its header and table checksums are recomputed. Each database is written
whole as a patch to hc27/disc (<archive>@<hex offset>.bin, see disc_patches.py), which HC27 lays
over the disc at startup. Rerun after rebuilding hc27/ROSTER.
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from tdb import Database, SaveFile, Table, table_crc  # noqa: E402
import xdvdfs  # noqa: E402

TOOL = "extra_rosters"
# (archive, offset of the database in it, what it is)
TARGETS = [
    ("qkl_boot.ast", 0x5D1870, "off-season career roster"),
    ("qkl_boot.ast", 0x80D0B0, "default roster copy without injuries"),
]
OUT = os.path.join(ROOT, "hc27", "disc")
COUNT_AT = 18  # u16 record count in a table header
HEADER_CRC_AT = Table.HEADER.size - 4


def layout(table):
    return [(f.kind, f.offset, f.name, f.bits) for f in table.fields]


def fill(target, roster):
    """Copies roster's records into target, table by table. Returns [(table, old count, new
    count)]; raises ValueError if a table does not fit."""
    changes = []
    data = target.data
    for name, t in target.tables.items():
        src = roster.tables.get(name)
        if src is None:
            raise ValueError(f"{name}: not in the roster")
        if layout(src) != layout(t) or src.record_bytes != t.record_bytes:
            raise ValueError(f"{name}: fields differ from the roster's")
        if src.count > t.capacity:
            raise ValueError(f"{name}: {src.count} records do not fit its {t.capacity}")
        # The records, then the roster's empty slots as they are, as far as both have room.
        size = min(src.capacity, t.capacity) * t.record_bytes
        data[t.data_start:t.data_start + size] = roster.data[src.data_start:src.data_start + size]
        data[t.data_start + size:t.data_end] = bytes(t.data_end - t.data_start - size)
        changes.append((name, t.count, src.count))
        struct.pack_into(">H", data, t.start + COUNT_AT, src.count)
        t.count = src.count
        struct.pack_into(">I", data, t.start + HEADER_CRC_AT,
                         table_crc(bytes(data[t.start:t.start + HEADER_CRC_AT])))
    target.fix_table_crcs()
    return changes


def read_database(iso, archive, offset):
    base, entries = xdvdfs.entries(iso)
    match = [e for e in entries if not e[1] and os.path.basename(e[0]).lower() == archive.lower()]
    if not match:
        raise SystemExit(f"{archive} is not on {iso}")
    _, _, start, length = match[0]
    with open(iso, "rb") as f:
        f.seek(base + start * xdvdfs.SECTOR + offset)
        head = f.read(24)
        if head[:4] != b"DB\x00\x08":
            raise SystemExit(f"no database at {archive} 0x{offset:X}: a different version of the disc?")
        size = struct.unpack_from(">I", head, 8)[0]
        if offset + size > length:
            raise SystemExit(f"{archive} 0x{offset:X}: database runs past the archive")
        return Database(bytearray(head + f.read(size - 24)), 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--roster", default=os.path.join(ROOT, "hc27", "ROSTER"))
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args()

    roster = SaveFile(args.roster).db
    os.makedirs(args.out, exist_ok=True)
    listing = os.path.join(args.out, f"{TOOL}.files")
    if os.path.exists(listing):
        for name in open(listing).read().split():
            if os.path.exists(os.path.join(args.out, name)):
                os.remove(os.path.join(args.out, name))
    written = []
    for archive, offset, what in TARGETS:
        db = read_database(args.iso, archive, offset)
        try:
            changes = fill(db, roster)
        except ValueError as e:
            raise SystemExit(f"{archive} 0x{offset:X} ({what}): {e}")
        if not db.table_crcs_valid():
            raise SystemExit(f"{archive} 0x{offset:X}: checksums did not come out right")
        name = f"{archive}@{offset:X}.bin"
        open(os.path.join(args.out, name), "wb").write(bytes(db.data))
        written.append(name)
        counts = ", ".join(f"{n} {a}->{b}" for n, a, b in changes if a != b) or "same counts"
        print(f"{name}: {what}, {len(changes)} tables ({counts})")
    open(listing, "w").write("\n".join(written) + "\n")
    print(f"{len(written)} rosters written to {args.out}")


if __name__ == "__main__":
    main()
