"""Imports the game's executable from the disc image, as HC09Setup.exe does on Windows.

    python3 tools/steamdeck/import_game.py <disc image (.iso)> <install folder>

Extracts default.xex to build/extracted/default.xex (which XenonRecomp recompiles), checks that the disc is NFL Head
Coach 09 (Xbox 360, title 45410841), and decrypts it to build/default.pe (which the game loads). Needs only Python's
standard library and the system's OpenSSL (see tools/xexdecrypt.py).
"""
import os
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import xdvdfs  # noqa: E402
import xexdecrypt  # noqa: E402

TITLE_ID = 0x45410841
EXECUTION_INFO = 0x00040006


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    iso, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    extracted = root / "build" / "extracted"
    try:
        xdvdfs.extract(str(iso), str(extracted), {"default.xex"})
    except (OSError, ValueError) as error:
        sys.exit(f"Reading {iso}: {error}")
    xex_path = extracted / "default.xex"
    if not xex_path.exists():
        sys.exit(f"{iso} has no default.xex: it is not an Xbox 360 game disc image")
    xex = xex_path.read_bytes()
    info = xexdecrypt.find_header(xex, EXECUTION_INFO) if xex[:4] == b"XEX2" else None
    if info is None or struct.unpack_from(">I", xex, info + 12)[0] != TITLE_ID:
        sys.exit("The disc image is not NFL Head Coach 09 (Xbox 360, title 45410841).")
    image = xexdecrypt.decrypt(xex)
    if image[:2] != b"MZ":
        sys.exit("Decrypting default.xex failed (no MZ header).")
    pe = root / "build" / "default.pe"
    pe.write_bytes(image)
    print(f"Extracted default.xex ({len(xex)} bytes) and decrypted it to build/default.pe ({len(image)} bytes)")


if __name__ == "__main__":
    main()
