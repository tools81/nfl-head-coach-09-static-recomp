"""Locate compiler ABI helper functions (save/restore GPR/FPR/VMX) in a flat Xbox 360 PE image."""
import sys

BASE = 0x82000000
PATTERNS = {
    "restgprlr_14_address": "e9c1ff68",
    "savegprlr_14_address": "f9c1ff68",
    "restfpr_14_address": "c9ccff70",
    "savefpr_14_address": "d9ccff70",
    "restvmx_14_address": "3960fee07dcb60ce",
    "savevmx_14_address": "3960fee07dcb61ce",
    "restvmx_64_address": "3960fc00100b60cb",
    "savevmx_64_address": "3960fc00100b61cb",
}

image = open(sys.argv[1], "rb").read()
for name, pat in PATTERNS.items():
    needle = bytes.fromhex(pat)
    hits, i = [], image.find(needle)
    while i != -1:
        if i % 4 == 0:
            hits.append(BASE + i)
        i = image.find(needle, i + 1)
    print(f"{name} = {', '.join(f'0x{h:08X}' for h in hits) or 'NOT FOUND'}")
