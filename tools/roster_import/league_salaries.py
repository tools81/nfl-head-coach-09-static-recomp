"""Brings HC27's league finances to the present: the salary cap, minimum salaries, restricted free
agent tenders and the salary scales the game pays and asks for in free agency, re-signing and
coach hiring.

    python tools/roster_import/league_salaries.py --iso "iso/NFL Head Coach 09.iso"

They are tables of the game's databases, stored uncompressed in the disc's archives (each database
more than once):

    SLRI  the cap (SCAD, $116M in 2008), the four restricted free agent tenders (RFA1-4) and other
          league amounts (SMAD, SFRV)
    SAYP  the minimum salary by years in the league
    SADP  salary ranges (most, signing bonus, least, ideal) by position and rating tier
    SAPC  salary percentiles by position (in $10,000s)
    SACP  coach salary ranges (most, least, ideal) by role

The cap, minimums and tenders are set to the season's (CAP, MINIMUMS, TENDERS); the scales grow
with the cap. The changed tables, with their checksums fixed, are written as patches to
hc27/disc (one file per table: <archive>@<offset>.bin), which HC27 lays over the disc's files at
startup. Rerun after changing the values; the folder is rebuilt each time.
"""

import argparse
import glob
import mmap
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from tdb import Database  # noqa: E402
import xdvdfs  # noqa: E402

SEASON = 2026
CAP = 301_200_000
# Minimum salary by credited seasons (the last applies to every later season too).
MINIMUMS = [885_000, 1_005_000, 1_075_000, 1_145_000, 1_215_000, 1_215_000, 1_215_000, 1_300_000]
# Restricted free agent tenders, lowest first: right of first refusal, original round, second
# round, first round.
TENDERS = [3_546_000, 3_702_000, 5_811_000, 8_107_000]
TABLES = ("SLRI", "SAYP", "SADP", "SAPC", "SACP")


def minimum_salary(years):
    return MINIMUMS[min(years, len(MINIMUMS) - 1)]


def round_to(value, unit=10_000):
    return int(round(value / unit) * unit)


def update(db, scale):
    """Edits the database's salary tables; returns the names of those it changed."""
    changed = []
    t = db.tables.get("SLRI")
    if t and t.count:
        t.set(0, "SCAD", CAP)
        for k, tender in enumerate(TENDERS, 1):
            t.set(0, f"RFA{k}", tender)
        for field in ("SMAD", "SFRV"):
            t.set(0, field, round_to(t.get(0, field) * scale))
        changed.append("SLRI")
    t = db.tables.get("SAYP")
    if t and t.count:
        for i in range(t.count):
            t.set(i, "SMID", minimum_salary(t.get(i, "PYRP")))
        changed.append("SAYP")
    t = db.tables.get("SADP")
    if t and t.count:
        for i in range(t.count):
            for field in ("SMAD", "SBID", "SMID", "SIDO"):
                value = round_to(t.get(i, field) * scale)
                t.set(i, field, value if field == "SBID" else max(value, MINIMUMS[0]))
        changed.append("SADP")
    t = db.tables.get("SAPC")
    if t and t.count:
        for i in range(t.count):
            for field in ["PC00", "PC10"] + [f"PC0{k}" for k in range(1, 10)]:
                value = t.get(i, field)
                if value:
                    t.set(i, field, max(round(value * scale), MINIMUMS[0] // 10_000))
        changed.append("SAPC")
    t = db.tables.get("SACP")
    if t and t.count:
        for i in range(t.count):
            for field in ("SMAD", "SMID", "SIDO"):
                t.set(i, field, round_to(t.get(i, field) * scale))
        changed.append("SACP")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--out", default=os.path.join(ROOT, "hc27", "disc"))
    args = parser.parse_args()

    base, entries = xdvdfs.entries(args.iso)
    archives = [(base + start * xdvdfs.SECTOR, length, path) for path, is_dir, start, length in entries
                if not is_dir and path.lower().endswith(".ast")]
    os.makedirs(args.out, exist_ok=True)
    for old in glob.glob(os.path.join(args.out, "*.bin")):
        os.remove(old)

    patches, cap_2008 = [], None
    with open(args.iso, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for start, length, path in archives:
            at = m.find(b"DB\x00\x08", start, start + length)
            while at >= 0:
                try:
                    size = struct.unpack_from(">I", m, at + 8)[0]
                    db = Database(bytearray(m[at:at + size]), 0) if 0 < size <= start + length - at else None
                except (ValueError, struct.error):
                    db = None
                if db and any(name in db.tables and db.tables[name].count for name in TABLES):
                    slri = db.tables.get("SLRI")
                    if slri and slri.count:
                        cap_2008 = slri.get(0, "SCAD")
                    patches.append((path, at - start, db))
                at = m.find(b"DB\x00\x08", at + 4, start + length)
    if not patches or not cap_2008:
        raise SystemExit(f"no salary tables found in {args.iso}")

    scale = CAP / cap_2008
    written = 0
    for path, offset, db in patches:
        changed = update(db, scale)
        db.fix_table_crcs()
        for table, end in db.table_extents():
            if table.name in changed:
                # The table's records and the checksum that ends it.
                at = table.data_start
                name = f"{os.path.basename(path)}@{offset + at:X}.bin"
                open(os.path.join(args.out, name), "wb").write(bytes(db.data[at:end]))
                written += 1
    print(f"{SEASON} cap ${CAP:,} (2008 ${cap_2008:,}, scales x{scale:.2f}); "
          f"{written} tables in {len(patches)} databases patched in {args.out}")


if __name__ == "__main__":
    main()
