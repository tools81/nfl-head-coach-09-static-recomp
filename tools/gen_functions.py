"""Generate explicit function boundaries for code that .pdata does not describe.

Leaf functions have no .pdata entry. XenonAnalyse's linear sweep over those gaps merges
consecutive leaf functions, and treats a jump-table bctr as a tail call, truncating the
function. Both break the recompiled output: functions reached only through data (handler
tables, vtables) get no entry point, and switch targets fall outside their function.

For every gap between .pdata functions this script finds the function starts inside it:
  - bl targets,
  - code addresses stored in data sections that sit on a plausible boundary (after a blr,
    an unconditional branch or zero padding), excluding jump-table case labels,
  - the first instruction after zero padding that follows a blr or branch,
and emits each [start, next start) range (trailing padding trimmed) as an explicit function.

Code from the first import thunk onward (thunks, then the end-of-.text data block) is never
split: XenonRecomp names the thunks itself and the data block is skipped as invalid code.

Usage: gen_functions.py <pe> <switch_tables.toml> <out.toml> <imports.csv>
"""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xrefs import BASE, pdata_functions  # noqa: E402

BLR = 0x4E800020


def sections(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    nsec, = struct.unpack_from("<H", image, pe + 6)
    optsz, = struct.unpack_from("<H", image, pe + 20)
    for i in range(nsec):
        name, vsize, va = struct.unpack_from("<8sII", image, pe + 24 + optsz + i * 40)
        flags, = struct.unpack_from("<I", image, pe + 24 + optsz + i * 40 + 36)
        yield name.rstrip(b"\0").decode(), BASE + va, vsize, flags


def load_switches(path):
    text = open(path).read()
    for block in text.split("[[switch]]")[1:]:
        base = int(re.search(r"base = (0x[0-9A-Fa-f]+)", block).group(1), 16)
        labels = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", block.split("labels", 1)[1])]
        yield base, labels


def is_unconditional_branch(ins):
    return (ins >> 26 == 18 and not ins & 1) or ins == BLR or (ins & 0xFC00FFFE) == 0x4C000420  # b, blr, bctr


def main(pe_path, switch_path, out_path, imports_path):
    image = open(pe_path, "rb").read()
    limit = BASE + len(image)
    word = lambda a: struct.unpack_from(">I", image, a - BASE)[0] if BASE <= a < limit - 3 else 0
    pdata, _ = pdata_functions(image)
    first_thunk = min(int(line.split(",")[4], 16) for line in open(imports_path) if ",thunk," in line)
    code = [(va, min(va + size, first_thunk)) for name, va, size, flags in sections(image)
            if flags & 0x20 and not name.startswith(".embsec") and va < first_thunk]
    in_code = lambda a: any(lo <= a < hi for lo, hi in code)

    pd_starts = [b for b, _ in pdata]

    def inside_pdata(a):
        i = bisect.bisect_right(pd_starts, a) - 1
        return i >= 0 and pdata[i][0] <= a < pdata[i][0] + pdata[i][1]

    labels = set()
    for _, case_labels in load_switches(switch_path):
        labels.update(case_labels)

    def plausible_start(a):
        before = word(a - 4)
        return before == 0 or is_unconditional_branch(before)

    starts = set(pd_starts)
    for lo, hi in code:
        for a in range(lo, hi, 4):
            ins = word(a)
            if ins >> 26 == 18 and ins & 3 == 1:  # bl
                d = ins & 0x03FFFFFC
                d -= 0x04000000 if d & 0x02000000 else 0
                if in_code(a + d):
                    starts.add(a + d)
            # first instruction after padding that follows a return/branch
            if ins != 0 and word(a - 4) == 0 and not inside_pdata(a):
                p = a - 4
                while word(p) == 0 and p > lo:
                    p -= 4
                if is_unconditional_branch(word(p)):
                    starts.add(a)
    for name, va, size, flags in sections(image):
        if flags & 0x20 or name in (".pdata", ".reloc"):
            continue
        for a in range(va, min(va + size, limit) - 3, 4):
            v = word(a)
            if v & 3 == 0 and in_code(v) and not inside_pdata(v) and v not in labels and plausible_start(v):
                starts.add(v)
    # a .pdata function's end also begins whatever follows (after padding)
    for b, s in pdata:
        a = b + s
        while in_code(a) and word(a) == 0:
            a += 4
        starts.add(a)

    # Jump tables: the dispatch sequence starts at `base`; every case label is a forward
    # target of the function containing it.
    switch_reach = {}
    for base, case_labels in load_switches(switch_path):
        switch_reach[base] = max(case_labels + [base])

    # Gaps: code not covered by .pdata functions.
    gaps = []
    for lo, hi in code:
        a = lo
        for b, s in pdata:
            if b + s <= lo or b >= hi:
                continue
            if b > a:
                gaps.append((a, b))
            a = max(a, b + s)
        if a < hi:
            gaps.append((a, hi))

    # Split each gap by following control flow: a function ends at a terminator (blr, bctr
    # or unconditional b) once no forward branch or switch case reaches past it, or where a
    # known function start begins.
    functions = {}
    for g0, g1 in gaps:
        a = g0
        while a < g1 and word(a) == 0:
            a += 4
        while a < g1:
            start = a
            reach = a
            while a < g1:
                if a != start and a in starts and a > reach:
                    break
                ins = word(a)
                reach = max(reach, switch_reach.get(a, a))
                op = ins >> 26
                if op in (16, 18) and not ins & 2:  # relative bc / b
                    width = 0xFFFC if op == 16 else 0x03FFFFFC
                    sign = 0x8000 if op == 16 else 0x02000000
                    d = ins & width
                    d -= (width + 4) if d & sign else 0
                    if not ins & 1 and start <= a + d < g1:  # not a call
                        reach = max(reach, a + d)
                terminator = ins == BLR or ins == 0x4E800420 or (op == 18 and not ins & 1)
                a += 4
                if terminator and a > reach:
                    break
            functions[start] = a
            while a < g1 and word(a) == 0:
                a += 4

    problems = 0
    starts_sorted = sorted(functions)
    for base, reach in switch_reach.items():
        if inside_pdata(base):
            continue
        i = bisect.bisect_right(starts_sorted, base) - 1
        if i < 0 or functions[starts_sorted[i]] <= reach:
            problems += 1

    with open(out_path, "w") as out:
        out.write("# Generated by tools/gen_functions.py -- functions in .pdata gaps\n")
        out.write("functions = [\n")
        for s in sorted(functions):
            out.write(f"    {{ address = 0x{s:08X}, size = 0x{functions[s] - s:X} }},\n")
        out.write("]\n")
    print(f"{len(functions)} functions written, {problems} switch tables could not be placed")


if __name__ == "__main__":
    main(*sys.argv[1:5])
