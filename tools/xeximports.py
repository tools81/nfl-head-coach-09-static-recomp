"""List XEX2 imports (library, ordinal, name, kind, address) using Xenia's export tables.

Usage: xeximports.py <default.xex> <decrypted.pe> [--csv out.csv]
"""
import os
import re
import struct
import sys

BASE = 0x82000000
HERE = os.path.dirname(os.path.abspath(__file__))


def load_names():
    names = {}
    for lib, fn in (("xboxkrnl.exe", "xboxkrnl_table.inc"), ("xam.xex", "xam_table.inc")):
        for m in re.finditer(r"XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+)",
                             open(os.path.join(HERE, "data", fn)).read()):
            names[(lib, int(m.group(1), 16))] = m.group(2)
    return names


def imports(xex, image):
    count = struct.unpack_from(">I", xex, 0x14)[0]
    off = next(v for k, v in (struct.unpack_from(">II", xex, 0x18 + i * 8) for i in range(count))
               if k == 0x000103FF)
    total, str_size, str_count = struct.unpack_from(">III", xex, off)
    libnames = [s.decode() for s in xex[off + 12: off + 12 + str_size].split(b"\0") if s]
    p, end = off + 12 + str_size, off + total
    while p < end:
        size, = struct.unpack_from(">I", xex, p)
        name_idx, rec_count = struct.unpack_from(">HH", xex, p + 0x24)
        lib = libnames[name_idx]
        for i in range(rec_count):
            addr, = struct.unpack_from(">I", xex, p + 0x28 + i * 4)
            value, = struct.unpack_from(">I", image, addr - BASE)
            yield lib, value & 0xFFFF, "thunk" if value >> 24 == 1 else "variable", addr
        p += size


if __name__ == "__main__":
    xex, image = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
    names = load_names()
    rows = [(lib, o, names.get((lib, o), f"ord_{o:03X}"), kind, a) for lib, o, kind, a in imports(xex, image)]
    out = open(sys.argv[4], "w") if "--csv" in sys.argv else sys.stdout
    out.write("library,ordinal,name,kind,address\n")
    for r in rows:
        out.write(f"{r[0]},0x{r[1]:03X},{r[2]},{r[3]},0x{r[4]:08X}\n")
