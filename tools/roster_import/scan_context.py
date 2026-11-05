"""Summarises the files disc_scan.py copied out: for each, its size, what it starts with, and the
readable text around each mention of the word. Writes build/disc_scan/<word>_context.txt.
    python tools/roster_import/scan_context.py lion DET
"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCAN = os.path.join(ROOT, "build", "disc_scan")


def printable(b):
    return re.sub(rb"[^\x20-\x7e]", b".", b).decode()


for word in sys.argv[1:] or ["lion", "DET"]:
    folder = os.path.join(SCAN, word)
    w = word.encode()
    rx = re.compile(re.escape(w), re.I) if len(word) >= 4 else re.compile(rb"(?<![A-Za-z0-9])" + re.escape(w) + rb"(?![A-Za-z0-9])")
    with open(os.path.join(SCAN, f"{word}_context.txt"), "w", encoding="utf-8") as out:
        for name in sorted(os.listdir(folder)):
            data = open(os.path.join(folder, name), "rb").read()
            hits = [m.start() for m in rx.finditer(data)]
            out.write(f"== {name} {len(data)} bytes, starts {printable(data[:16])!r}, {len(hits)} mentions\n")
            # the distinct readable strings containing the word (up to 25)
            strings = []
            for s in re.finditer(rb"[\x20-\x7e]{3,}", data):
                if rx.search(s.group()) and s.group() not in strings:
                    strings.append(s.group())
                    if len(strings) >= 25:
                        break
            for s in strings:
                out.write("   " + s.decode()[:160] + "\n")
    print("wrote", os.path.join(SCAN, f"{word}_context.txt"))
