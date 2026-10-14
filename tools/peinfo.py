"""Print sections and .pdata function stats for a decrypted Xbox 360 PE image."""
import struct
import sys

d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec, = struct.unpack_from("<H", d, pe + 6)
optsz, = struct.unpack_from("<H", d, pe + 20)
base, = struct.unpack_from("<I", d, pe + 24 + 28)
print(f"image base {base:#x}")
sec_tab = pe + 24 + optsz
sections = []
for i in range(nsec):
    name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, sec_tab + i * 40)
    flags, = struct.unpack_from("<I", d, sec_tab + i * 40 + 36)
    name = name.rstrip(b"\0").decode()
    sections.append((name, va, vsize))
    print(f"  {name:8} va={base + va:#010x} size={vsize:#09x} flags={flags:#010x}")

for name, va, vsize in sections:
    if name == ".pdata":
        n = vsize // 8
        total = 0
        starts = []
        for i in range(n):
            begin, info = struct.unpack_from(">II", d, va + i * 8)
            if begin == 0:
                continue
            starts.append(begin)
            total += ((info >> 8) & 0x3FFFFF) * 4
        print(f".pdata: {len(starts)} functions, {total:#x} bytes of code, "
              f"range {min(starts):#x}-{max(starts):#x}")
