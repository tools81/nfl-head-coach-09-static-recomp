"""Lays recoloured uniform textures (numbers, names, ...) over every copy of the original on the
disc. Each texture is a pair in tools/roster_import/data/uniforms: <NAME>.orig (the file as it is
compressed in the disc's archives, from uniform_extract.py) and <NAME>.new (the recoloured one, compressed, padded to the
same size). Every place an .orig appears in an archive gets a patch in hc27/disc
(<archive>@<hex offset>.bin), which HC27 lays over the disc at startup. The pairs and the
patches are derived from the game's files, so they stay out of git (data/ is ignored, and the
patches are added to .git/info/exclude).

    python tools/roster_import/uniform_patches.py "iso/NFL Head Coach 09.iso"
"""
import mmap, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import xdvdfs

TOOL = "uniform_patches"
SRC = os.path.join(HERE, "data", "uniforms")
OUT = os.path.join(ROOT, "hc27", "disc")


def main():
    iso = sys.argv[1]
    pairs = []
    for f in sorted(os.listdir(SRC)):
        if f.endswith(".orig"):
            orig = open(os.path.join(SRC, f), "rb").read()
            new = open(os.path.join(SRC, f[:-5] + ".new"), "rb").read()
            assert len(new) == len(orig), f
            pairs.append((f[:-5], orig, new))
    os.makedirs(OUT, exist_ok=True)
    listing = os.path.join(OUT, f"{TOOL}.files")
    if os.path.exists(listing):
        for n in open(listing).read().split():
            if os.path.exists(os.path.join(OUT, n)):
                os.remove(os.path.join(OUT, n))
    base, ents = xdvdfs.entries(iso)
    written = []
    with open(iso, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for path, is_dir, start, length in ents:
            if is_dir or not path.lower().endswith(".ast"):
                continue
            a0 = base + start * xdvdfs.SECTOR
            arch = os.path.basename(path)
            for name, orig, new in pairs:
                at = m.find(orig[:4096], a0, a0 + length)
                count = 0
                while at >= 0:
                    if m[at:at + len(orig)] == orig:
                        fn = f"{arch}@{at - a0:X}.bin"
                        open(os.path.join(OUT, fn), "wb").write(new)
                        written.append(fn)
                        count += 1
                    at = m.find(orig[:4096], at + 1, a0 + length)
                if count:
                    print(f"{name}: {count} copies in {arch}")
    open(listing, "w").write("\n".join(written) + "\n")
    exclude = os.path.join(ROOT, ".git", "info", "exclude")
    if os.path.isdir(os.path.dirname(exclude)):
        text = open(exclude).read() if os.path.exists(exclude) else ""
        lines = [f"hc27/disc/{TOOL}.files"] + [f"hc27/disc/{x}" for x in written]
        missing = [x for x in lines if x not in text.split("\n")]
        if missing:
            open(exclude, "a").write(("" if text.endswith("\n") or not text else "\n") + "\n".join(missing) + "\n")
    print(f"{len(written)} patches written to {OUT}")


if __name__ == "__main__":
    main()
