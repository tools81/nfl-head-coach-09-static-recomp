"""Goes through every database on the disc (or in files you extracted) and dumps every table, to
find everything that has to change with the salary cap.

    python tools/roster_import/cap_audit.py --iso "D:/Games/NFL Head Coach 09.iso"
    python tools/roster_import/cap_audit.py --files path/to/extracted     (a folder or files: .ast, .db, saves)

Writes to tools/roster_import/data/cap_audit/:

    index.csv     every table of every database: where it is (file, database offset), its name,
                  records, fields, whether HC27 already patches it (hc27/disc/*.files) and why
                  it is flagged
    suspects.txt  the flagged tables, worst first, with their records: those holding dollar
                  amounts, the 2008 cap ($116M, or 11600 in $10,000s), years from 2007 on (the
                  cap's years, dates), or known salary fields (SCAD, SMAD, CSAL, RFA*, PC**, PSA*...)
    tables/       each distinct table as CSV (<name>_<hash>.csv); a table stored identically in
                  several databases is written once, and index.csv lists every copy

Nothing is changed: this only reads.
"""

import argparse
import csv
import hashlib
import mmap
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from tdb import Database  # noqa: E402

SIGNATURE = b"DB\x00\x08"
CAP_2008 = 116_000_000
MONEY_NAMES = re.compile(r"^(SCAD|SMAD|SMID|SIDO|SBID|SFRV|CSAL|CSBL|RFA\d|PC\d\d|PS[AB]\d|PSBO|PVSB|PCSA|PTSA|PVTS|TMSA)$")
OUT = os.path.join(HERE, "data", "cap_audit")
PATCHES = os.path.join(ROOT, "hc27", "disc")


def scan(buf, start, end):
    """Databases in buf[start:end] as (offset, Database)."""
    at = buf.find(SIGNATURE, start, end)
    while at >= 0:
        db = None
        try:
            size = struct.unpack_from(">I", buf, at + 8)[0]
            if 0 < size <= end - at:
                db = Database(bytearray(buf[at:at + size]), 0)
        except (ValueError, struct.error, IndexError):
            db = None
        if db is not None:
            yield at, db
        at = buf.find(SIGNATURE, at + 4, end)


def iso_sources(iso):
    import xdvdfs
    base, entries = xdvdfs.entries(iso)
    f = open(iso, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    for path, is_dir, start, length in sorted(entries):
        if not is_dir:
            begin = base + start * xdvdfs.SECTOR
            yield path, m, begin, begin + length


def file_sources(paths):
    for p in paths:
        files = [os.path.join(d, n) for d, _, ns in os.walk(p) for n in ns] if os.path.isdir(p) else [p]
        for path in sorted(files):
            if os.path.getsize(path) < 32:
                continue
            f = open(path, "rb")
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            yield os.path.relpath(path, p) if os.path.isdir(p) else os.path.basename(path), m, 0, len(m)


def patched():
    """(archive name lower, offset) of each table HC27 patches, with the tool that writes it."""
    done = {}
    if os.path.isdir(PATCHES):
        for listing in os.listdir(PATCHES):
            if listing.endswith(".files"):
                for name in open(os.path.join(PATCHES, listing)).read().split():
                    archive, _, offset = name[:-4].rpartition("@")
                    done[(archive.lower(), int(offset, 16))] = listing[:-6]
    return done


def clean(value):
    return value.replace("\0", "") if isinstance(value, str) else value


def rows(table):
    out = []
    for i in range(table.count):
        try:
            out.append({k: clean(v) for k, v in table.row(i).items()})
        except (IndexError, struct.error):
            break
    return out


def flags(table, records):
    """Why a table looks like it matters for the cap (empty: it does not)."""
    why, score = [], 0
    names = [f.name for f in table.fields if MONEY_NAMES.match(f.name)]
    if names:
        why.append("salary-like fields " + " ".join(names))
        score += 1
    numbers = [(k, v) for r in records for k, v in r.items() if isinstance(v, (int, float)) and not isinstance(v, bool)]
    if any(v in (CAP_2008, CAP_2008 // 10_000) for _, v in numbers):
        why.append("holds the 2008 cap")
        score += 10
    money = sorted({k for k, v in numbers if 100_000 <= v < 2_000_000_000})
    if money:
        why.append("dollar amounts in " + " ".join(money))
        score += 5
    years = sorted({k for k, v in numbers if 2007 <= v <= 2040 and k not in names})
    if years:
        why.append("years in " + " ".join(years))
        score += 3
    return score, "; ".join(why)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--iso", help="the game's disc image")
    source.add_argument("--files", nargs="+", help="extracted files or folders (.ast, .db, saves)")
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args()

    os.makedirs(os.path.join(args.out, "tables"), exist_ok=True)
    done = patched()
    seen = {}       # content hash -> (file name, score, why, table name, records)
    index = []
    files = dbs = 0
    for name, buf, start, end in (iso_sources(args.iso) if args.iso else file_sources(args.files)):
        found = False
        for at, db in scan(buf, start, end):
            found, dbs = True, dbs + 1
            offset = at - start
            for table, table_end in db.table_extents():
                table.name = re.sub(r"[^0-9A-Za-z]", "_", table.name)
                for field in table.fields:
                    field.name = re.sub(r"[^0-9A-Za-z]", "_", field.name)
                content = bytes(db.data[table.start:table_end])
                digest = hashlib.sha1(content).hexdigest()[:8]
                if digest not in seen:
                    records = rows(table)
                    score, why = flags(table, records)
                    csv_name = f"{table.name}_{digest}.csv"
                    with open(os.path.join(args.out, "tables", csv_name), "w", newline="", encoding="utf-8") as f:
                        w = csv.DictWriter(f, fieldnames=[x.name for x in table.fields], extrasaction="ignore")
                        w.writeheader()
                        w.writerows(records)
                    seen[digest] = (csv_name, score, why, table.name, records)
                csv_name, score, why, _, _ = seen[digest]
                tool = done.get((os.path.basename(name).lower(), offset + table.data_start), "")
                index.append([name, f"0x{offset:X}", table.name, table.count, table.capacity,
                              " ".join(f.name for f in table.fields), tool, score, why, csv_name])
        files += found
        if found:
            print(f"{name}: databases found", flush=True)

    with open(os.path.join(args.out, "index.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["file", "database", "table", "records", "capacity", "fields", "patched_by", "score", "why", "csv"])
        w.writerows(index)

    copies = {}
    for row in index:
        copies.setdefault(row[-1], []).append(row)
    flagged = sorted((v for v in seen.values() if v[1] > 0), key=lambda v: (-v[1], v[3]))
    with open(os.path.join(args.out, "suspects.txt"), "w", encoding="utf-8") as f:
        for csv_name, score, why, table_name, records in flagged:
            where = copies[csv_name]
            patched_by = sorted({r[6] for r in where if r[6]})
            f.write(f"=== {table_name} ({csv_name}) score {score}: {why}\n")
            f.write(f"    {len(where)} copies; patched by: {', '.join(patched_by) or 'NOTHING'}\n")
            for r in where[:8]:
                f.write(f"    {r[0]} db {r[1]}{'  [patched]' if r[6] else ''}\n")
            if len(where) > 8:
                f.write(f"    ... {len(where) - 8} more\n")
            for rec in records[:40]:
                f.write("    " + "  ".join(f"{k}={v}" for k, v in rec.items()) + "\n")
            if len(records) > 40:
                f.write(f"    ... {len(records) - 40} more records in tables/{csv_name}\n")
            f.write("\n")

    print(f"{files} files with {dbs} databases, {len(index)} tables ({len(seen)} distinct, "
          f"{len(flagged)} flagged) -> {args.out}")


if __name__ == "__main__":
    main()
