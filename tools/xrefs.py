"""Find `bl`/`b` branches to a target address and report the containing .pdata function.

Usage: xrefs.py <decrypted.pe> <target_hex> [...]
"""
import bisect
import struct
import sys

BASE = 0x82000000


def pdata_functions(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    nsec, = struct.unpack_from("<H", image, pe + 6)
    optsz, = struct.unpack_from("<H", image, pe + 20)
    funcs, text = [], None
    for i in range(nsec):
        name, vsize, va = struct.unpack_from("<8sII", image, pe + 24 + optsz + i * 40)
        if name.startswith(b".pdata"):
            for j in range(vsize // 8):
                begin, info = struct.unpack_from(">II", image, va + j * 8)
                if begin:
                    funcs.append((begin, ((info >> 8) & 0x3FFFFF) * 4))
        if name.startswith(b".text"):
            text = (va, vsize)
    funcs.sort()
    return funcs, text


def branches_to(image, text, target):
    va, size = text
    for off in range(va, va + size, 4):
        ins, = struct.unpack_from(">I", image, off)
        if ins >> 26 != 18 or ins & 2:  # I-form, relative only
            continue
        disp = ins & 0x03FFFFFC
        if disp & 0x02000000:
            disp -= 0x04000000
        if BASE + off + disp == target:
            yield BASE + off, bool(ins & 1)


if __name__ == "__main__":
    image = open(sys.argv[1], "rb").read()
    funcs, text = pdata_functions(image)
    starts = [f[0] for f in funcs]
    for t in sys.argv[2:]:
        target = int(t, 16)
        print(f"xrefs to 0x{target:08X}:")
        for addr, link in branches_to(image, text, target):
            i = bisect.bisect_right(starts, addr) - 1
            fn = f"0x{funcs[i][0]:08X}+0x{addr - funcs[i][0]:X} (size 0x{funcs[i][1]:X})" if i >= 0 else "?"
            print(f"  {'bl' if link else 'b '} at 0x{addr:08X} in {fn}")
