"""Generate [[switch]] entries for jump-table dispatches XenonAnalyse missed.

Handles the absolute-address pattern without a bounds check:
    lis rT, hi ; addi rT, rT, lo ; rlwinm/slwi r0, rIdx, 2 ; lwzx r0, rT, r0 ; mtctr r0 ; bctr
The table is read while entries stay inside .text and near the dispatch. Other dispatch
shapes are reported for manual review. Appends to the switch table file.

Usage: add_missing_switches.py <pe> <switch_tables.toml> <bctr address>...
"""
import struct
import sys

BASE = 0x82000000


def word(image, address):
    return struct.unpack_from(">I", image, address - BASE)[0]


def main():
    image = open(sys.argv[1], "rb").read()
    out_path = sys.argv[2]
    entries = []
    for text in sys.argv[3:]:
        bctr = int(text, 16)
        seq = [word(image, bctr - 4 * k) for k in range(5, 0, -1)]  # lis, addi, rlwinm, lwzx, mtctr
        lis, addi, shift, lwzx, mtctr = seq
        ok = (lis >> 26 == 15 and addi >> 26 == 14 and shift >> 26 == 21 and lwzx >> 26 == 31 and
              ((lwzx >> 1) & 0x3FF) == 23 and mtctr == 0x7C0903A6)
        if not ok:
            print(f"0x{bctr:08X}: unrecognized dispatch shape, review manually")
            continue
        reg = (lis >> 21) & 31
        hi = lis & 0xFFFF
        lo = addi & 0xFFFF
        if lo & 0x8000:
            lo -= 0x10000
        table = ((hi << 16) + lo) & 0xFFFFFFFF
        index_reg = (shift >> 21) & 31
        labels = []
        for i in range(512):
            target = word(image, table + 4 * i)
            if not (0x82000000 <= target < 0x83000000) or abs(target - bctr) > 0x10000:
                break
            labels.append(target)
        if not labels:
            print(f"0x{bctr:08X}: empty table at 0x{table:08X}")
            continue
        base = bctr - 4 * 5
        # No bounds check: the last label serves as the (unused) default.
        entries.append((base, index_reg, labels[-1], labels))
        print(f"0x{bctr:08X}: base 0x{base:08X} r{index_reg} table 0x{table:08X}, {len(labels)} labels (lis r{reg})")
    with open(out_path, "a") as f:
        for base, index_reg, default, labels in entries:
            f.write(f"\n[[switch]]\nbase = 0x{base:08X}\nr = {index_reg}\ndefault = 0x{default:08X}\nlabels = [\n")
            for label in labels:
                f.write(f"    0x{label:08X},\n")
            f.write("]\n")


if __name__ == "__main__":
    main()
