"""Lists every file inside the disc's .ast archives (EA BGFA 1.05, nested, zlib) and copies out the
ones that mention a word (default: lion), to find a team's uniform data. Only reads the disc.

    python tools/roster_import/disc_scan.py "iso/NFL Head Coach 09.iso" [--word lion --word DET]

Words of 4+ letters match anywhere, ignoring case (lion matches Lions, LIONS_HOME); shorter ones
(DET) only as a word of their own, in capitals, so DETAIL does not count. Writes
build/disc_scan/listing.csv and build/disc_scan/<word>/*.bin.
"""
import argparse, csv, os, re, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import xdvdfs

MAX_LEAF = 16 << 20


def toc(head):
    count, toc_at = struct.unpack_from("<II", head, 12)
    widths = head[0x21:0x27]
    ntags = head[0x28]
    tags = [head[toc_at + 4 * i:toc_at + 4 * i + 4] for i in range(ntags)]
    pos = toc_at + 4 * ntags
    shift = widths[5]
    out = []
    for _ in range(count):
        flag = head[pos]
        kind = int.from_bytes(head[pos + 1:pos + 1 + widths[0]], "little")
        pos += 1 + widths[0]
        h = head[pos:pos + widths[1]].hex(); pos += widths[1]
        off = int.from_bytes(head[pos:pos + widths[2]], "little") << shift; pos += widths[2]
        size = int.from_bytes(head[pos:pos + widths[3]], "little"); pos += widths[3]
        pos += widths[4]
        tag = tags[kind] if kind < len(tags) else b"????"
        out.append((flag, tag, h, off, size))
    return out


def toc_size(f, base):
    f.seek(base)
    head = f.read(0x30)
    toc_at = struct.unpack_from("<I", head, 16)[0]
    tsize = struct.unpack_from("<Q", head, 24)[0]
    return toc_at + tsize


def walk(f, base, name, rows, hits, word, out_dir, depth=0):
    size = toc_size(f, base)
    f.seek(base)
    head = f.read(size)
    seen = set()
    for flag, tag, h, off, size in toc(head):
        if off < len(head) or (off, size) in seen:
            continue  # points into the table of contents (or at itself): not a file
        seen.add((off, size))
        f.seek(base + off)
        magic = f.read(8)
        path = f"{name}/{h}"
        tagtxt = re.sub(rb"[^A-Za-z0-9#.]", b"_", tag).decode()
        if magic == b"BGFA1.05" and not flag & 1 and depth < 8:
            rows.append((path, tagtxt, flag, size, "BGFA"))
            walk(f, base + off, path, rows, hits, word, out_dir, depth + 1)
            continue
        data = None
        if size <= MAX_LEAF:
            f.seek(base + off)
            data = f.read(size)
            if flag & 1:
                try:
                    data = zlib.decompress(data)
                except zlib.error:
                    data = None
        kind = (data[:4] if data else magic[:4])
        rows.append((path, tagtxt, flag, size, re.sub(rb"[^A-Za-z0-9#.]", b"_", kind).decode()))
        if data and data[:8] == b"BGFA1.05":
            continue  # compressed nested archive: listed, not opened
        if data:
            for w, rx in word:
                if rx.search(data):
                    hits.append((w, path))
                    fn = path.replace("/", "_") + "." + tagtxt + ".bin"
                    open(os.path.join(out_dir[w], fn), "wb").write(data)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("iso")
    p.add_argument("--word", action="append")
    a = p.parse_args()
    words = a.word or ["lion", "DET"]
    out = os.path.join(ROOT, "build", "disc_scan")
    word, hit_dir = [], {}
    for w in words:
        b = re.escape(w.encode())
        rx = re.compile(b, re.I) if len(w) >= 4 else re.compile(rb"(?<![A-Za-z0-9])" + b + rb"(?![A-Za-z0-9])")
        word.append((w, rx))
        hit_dir[w] = os.path.join(out, w)
        os.makedirs(hit_dir[w], exist_ok=True)
    base, ents = xdvdfs.entries(a.iso)
    rows, hits = [], []
    with open(a.iso, "rb") as f:
        for path, is_dir, start, length in ents:
            if is_dir or not path.lower().endswith(".ast"):
                continue
            print("scanning", path, flush=True)
            walk(f, base + start * xdvdfs.SECTOR, os.path.basename(path), rows, hits, word, hit_dir)
    with open(os.path.join(out, "listing.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["path", "type", "flag", "size", "starts_with"])
        w.writerows(rows)
    for w in words:
        print(f"{sum(1 for x, _ in hits if x == w)} files mention '{w}' -> {hit_dir[w]}")
    print(f"{len(rows)} files listed")


if __name__ == "__main__":
    main()
