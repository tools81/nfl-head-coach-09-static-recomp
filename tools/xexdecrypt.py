"""Decrypt and decompress a retail XEX2 (basic compression) into a flat PE image.

Usage: xexdecrypt.py <in.xex> <out.pe>

The retail key is not shipped with this project; it is read from the XenonRecomp checkout
(thirdparty/XenonRecomp/XenonUtils/xex.h), which must be cloned first.
"""
import pathlib
import re
import struct
import sys

from Crypto.Cipher import AES

XEX_H = pathlib.Path(__file__).resolve().parent.parent / "thirdparty/XenonRecomp/XenonUtils/xex.h"
DEVKIT_KEY = bytes(16)


def load_retail_key():
    try:
        text = XEX_H.read_text()
    except OSError:
        sys.exit(f"{XEX_H} not found: clone XenonRecomp into thirdparty/XenonRecomp first")
    m = re.search(r"Xex2RetailKey\[16\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        sys.exit(f"Xex2RetailKey not found in {XEX_H}")
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def find_header(d, key):
    count = struct.unpack_from(">I", d, 0x14)[0]
    for i in range(count):
        k, v = struct.unpack_from(">II", d, 0x18 + i * 8)
        if k == key:
            return v
    return None


def decrypt(d):
    retail_key = load_retail_key()
    pe_off, sec_off = struct.unpack_from(">I", d, 8)[0], struct.unpack_from(">I", d, 0x10)[0]
    ffi = find_header(d, 0x000003FF)
    info_size, enc, comp = struct.unpack_from(">IHH", d, ffi)
    payload = d[pe_off:]
    if enc:
        wrapped = d[sec_off + 0x150: sec_off + 0x160]
        for master in (retail_key, DEVKIT_KEY):
            session = AES.new(master, AES.MODE_ECB).decrypt(wrapped)
            cipher = AES.new(session, AES.MODE_CBC, iv=bytes(16))
            usable = len(payload) // 16 * 16
            plain = cipher.decrypt(payload[:usable]) + payload[usable:]
            if comp != 1 or plain[:2] == b"MZ":
                break
        payload = plain
    if comp == 0:
        return payload
    if comp != 1:
        raise NotImplementedError("only basic (raw block) compression is supported")
    out, src = bytearray(), 0
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack_from(">II", d, ffi + 8 + i * 8)
        out += payload[src: src + data_size]
        out += bytes(zero_size)
        src += data_size
    return bytes(out)


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    image = decrypt(data)
    if image[:2] != b"MZ":
        sys.exit("decryption failed: no MZ header")
    open(sys.argv[2], "wb").write(image)
    print(f"wrote {len(image):#x} bytes")
