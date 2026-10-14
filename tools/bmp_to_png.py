"""Convert the renderer's 32-bit top-down BMP screenshots to PNG (stdlib only).

Usage: bmp_to_png.py <in.bmp> <out.png> [scale_divisor]
"""
import struct
import sys
import zlib


def main(src, dst, divisor=1):
    data = open(src, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    top_down = height < 0
    height = abs(height)
    rows = []
    for y in range(0, height, divisor):
        row_index = y if top_down else height - 1 - y
        start = offset + row_index * width * 4
        pixels = data[start:start + width * 4]
        out = bytearray([0])  # filter type: none
        for x in range(0, width, divisor):
            b, g, r = pixels[x * 4], pixels[x * 4 + 1], pixels[x * 4 + 2]
            out += bytes((r, g, b))
        rows.append(bytes(out))
    out_w = (width + divisor - 1) // divisor
    out_h = len(rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 1)
