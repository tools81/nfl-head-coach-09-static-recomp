"""Report bctr sites inside functions that XenonAnalyse did not turn into a switch table.

A bctr preceded within a few instructions by an indexed load (lwzx/lhzx/lbzx) or a
table-address computation is very likely a jump table rather than a virtual call.
Usage: check_switches.py <pe> <switch_tables.toml>
"""
import re
import struct
import sys

BASE = 0x82000000
image = open(sys.argv[1], "rb").read()
known = {int(m, 16) for m in re.findall(r"base = (0x[0-9A-Fa-f]+)", open(sys.argv[2]).read())}

pe = struct.unpack_from("<I", image, 0x3C)[0]
nsec, = struct.unpack_from("<H", image, pe + 6)
optsz, = struct.unpack_from("<H", image, pe + 20)
text = []
for i in range(nsec):
    name, vsize, va = struct.unpack_from("<8sII", image, pe + 24 + optsz + i * 40)
    if name.startswith(b".text"):
        text.append((va, vsize))

INDEXED_LOADS = {23: "lwzx", 279: "lhzx", 87: "lbzx"}
total, missing = 0, []
for va, size in text:
    for off in range(va, va + size, 4):
        ins, = struct.unpack_from(">I", image, off)
        if ins != 0x4E800420:  # bctr
            continue
        # mtctr fed directly by `lwz rX, d(rY)` is a vtable/function-pointer call, not a switch
        mtctr, prev = struct.unpack_from(">II", image, off - 4)[0], struct.unpack_from(">I", image, off - 8)[0]
        if mtctr >> 26 == 31 and prev >> 26 == 32 and (prev >> 21) & 31 == (mtctr >> 21) & 31:
            continue
        window = [struct.unpack_from(">I", image, off - 4 * k)[0] for k in range(1, 9)]
        if any(w >> 26 == 31 and ((w >> 1) & 0x3FF) in INDEXED_LOADS for w in window):
            total += 1
            # XenonAnalyse keys tables by the start of the dispatch sequence, a few
            # instructions before the bctr
            if not any(BASE + off - 4 * k in known for k in range(0, 16)):
                missing.append(BASE + off)
print(f"likely jump-table bctr sites: {total}, detected: {len(known)}, uncovered: {len(missing)}")
for a in missing[:20]:
    print(f"  0x{a:08X}")
