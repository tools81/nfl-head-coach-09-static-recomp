"""Dump XEX2 optional headers (no decryption)."""
import struct
import sys

NAMES = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO", 0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS", 0x000103FF: "IMPORT_LIBRARIES", 0x00018002: "CHECKSUM_TIMESTAMP",
    0x000183FF: "ORIGINAL_PE_NAME", 0x000200FF: "STATIC_LIBRARIES", 0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE", 0x00030000: "SYSTEM_FLAGS", 0x00040006: "EXECUTION_INFO",
    0x00040310: "GAME_RATINGS", 0x00040404: "LAN_KEY", 0x000405FF: "XBOX360_LOGO",
    0x00020301: "DEFAULT_HEAP_SIZE", 0x00040201: "TITLE_WORKSPACE_SIZE",
}

d = open(sys.argv[1], "rb").read()
magic, flags, pe_off, _, sec_off, count = struct.unpack_from(">4sIIIII", d, 0)
print(f"magic={magic} flags={flags:#x} pe_data_offset={pe_off:#x} headers={count}")
for i in range(count):
    key, val = struct.unpack_from(">II", d, 0x18 + i * 8)
    name = NAMES.get(key, f"{key:#010x}")
    print(f"  {name:22} {val:#x}")
    if key == 0x00010100:
        print(f"    entry point = {val:#010x}")
    elif key == 0x000003FF:
        size, enc, comp = struct.unpack_from(">IHH", d, val)
        print(f"    encryption={['none','normal'][enc]} compression={['none','basic','normal(LZX)','delta'][comp]}")
    elif key == 0x000183FF:
        n = struct.unpack_from(">I", d, val)[0]
        print("    ", d[val + 4: val + 4 + n].split(b"\0")[0].decode())
    elif key == 0x00040006:
        media, ver, bver, tid, plat, etype, disc, ndisc, sid = struct.unpack_from(">IIIIBBBBI", d, val)
        print(f"    title_id={tid:08X} version={ver:#x} disc={disc}/{ndisc}")
    elif key == 0x000200FF:
        size = struct.unpack_from(">I", d, val)[0]
        for o in range(val + 4, val + size, 16):
            name_b = d[o:o + 8].split(b"\0")[0].decode()
            ma, mi, b, q = struct.unpack_from(">HHHH", d, o + 8)
            print(f"    lib {name_b:10} {ma}.{mi}.{b}.{q & 0x7fff}")
    elif key == 0x000103FF:
        size, slen, scount = struct.unpack_from(">III", d, val)
        names = d[val + 12: val + 12 + slen].split(b"\0")
        print("    imports:", [n.decode() for n in names if n])

sec = struct.unpack_from(">I", d, sec_off + 0x110)[0]
img_size = struct.unpack_from(">I", d, sec_off + 4)[0]
print(f"image size={img_size:#x}")
