"""Patches of the game's databases on the disc, for HC27 to lay over the disc's files.

The databases (TDB, see tdb.py) are stored uncompressed in the disc's archives, each more than
once. A tool edits tables of them and writes each changed table (its records and the checksum that
ends it) to hc27/disc as <archive>@<hex offset>.bin, which HC27 lays over the disc at startup.
Each tool lists the files it wrote in hc27/disc/<tool>.files and removes those it wrote before,
so the tools can share the folder.
"""

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

OUT = os.path.join(ROOT, "hc27", "disc")


def databases(iso_path, tables):
    """(archive path, offset in the archive, Database) for each database on the disc that has a
    table named in `tables` with records."""
    base, entries = xdvdfs.entries(iso_path)
    archives = [(base + start * xdvdfs.SECTOR, length, path) for path, is_dir, start, length in entries
                if not is_dir and path.lower().endswith(".ast")]
    found = []
    with open(iso_path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for start, length, path in archives:
            at = m.find(b"DB\x00\x08", start, start + length)
            while at >= 0:
                try:
                    size = struct.unpack_from(">I", m, at + 8)[0]
                    db = Database(bytearray(m[at:at + size]), 0) if 0 < size <= start + length - at else None
                except (ValueError, struct.error):
                    db = None
                if db and any(name in db.tables and db.tables[name].count for name in tables):
                    found.append((path, at - start, db))
                at = m.find(b"DB\x00\x08", at + 4, start + length)
    return found


def write_patches(tool, patches, out=OUT):
    """Writes the changed tables: patches is [(archive path, database offset, Database, changed
    table names)]. Returns the number of files written."""
    os.makedirs(out, exist_ok=True)
    listing = os.path.join(out, f"{tool}.files")
    if os.path.exists(listing):
        for name in open(listing).read().split():
            if os.path.exists(os.path.join(out, name)):
                os.remove(os.path.join(out, name))
    written = []
    for path, offset, db, changed in patches:
        db.fix_table_crcs()
        for table, end in db.table_extents():
            if table.name in changed:
                at = table.data_start
                name = f"{os.path.basename(path)}@{offset + at:X}.bin"
                open(os.path.join(out, name), "wb").write(bytes(db.data[at:end]))
                written.append(name)
    open(listing, "w").write("\n".join(written) + "\n")
    return len(written)
