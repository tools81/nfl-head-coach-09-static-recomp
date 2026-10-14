"""Disassemble a range of the flat image. Usage: disasm.py <pe> <addr_hex> <size_hex>"""
import sys

import capstone

BASE = 0x82000000
image = open(sys.argv[1], "rb").read()
addr, size = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
off = addr - BASE
for a in range(addr, addr + size, 4):
    word = image[a - BASE: a - BASE + 4]
    ins = next(md.disasm(word, a), None)
    text = f"{ins.mnemonic} {ins.op_str}" if ins else ".long"
    print(f"{a:08X}: {word.hex()}  {text}")
