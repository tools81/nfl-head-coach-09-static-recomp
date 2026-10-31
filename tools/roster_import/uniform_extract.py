"""Copies every team's home and away uniform files off the disc (as they are stored, compressed):
jersey, sleeves, socks, pants, numbers, names, the helmet's settings and decals, and the far-away
versions. Writes build/uniforms_raw/<NAME>.orig and build/uniforms_raw/index.csv (name, archive,
offset, size, copies). Only reads the disc.

    python tools/roster_import/uniform_extract.py "iso/NFL Head Coach 09.iso"
"""
import csv, hashlib, mmap, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import xdvdfs

OUT = os.path.join(ROOT, "build", "uniforms_raw")
WANT = re.compile(r"^(?:[A-Z0-9]+[HA](?:TORSOBAKED|SLEEVEBAKED|RSLEEVEBAKED|SOCKS|PANTSBAKED|NUM|NAME|"
                  r"HELMETSOCKSSHOES|PANTSSLEEVES|SIDELINETORSO)_COL|[A-Z0-9]+[HA]UNIFORM|[A-Z0-9]+HELMET|"
                  r"HELMET_[A-Z0-9]+_DECAL|HELMET_[A-Z0-9]+STRIPE_DECAL|TEAMCOLORTABLE)$")


def packages(m, lo, hi):
    at = m.find(b"BGFA1.05", lo, hi)
    while at >= 0:
        try:
            count, toc_at = struct.unpack_from("<II", m, at + 12)
            w = m[at + 0x21:at + 0x27]
            ntags = m[at + 0x28]
            namelen = struct.unpack_from("<I", m, at + 0x2C)[0]
            if 0 < namelen <= 64 and 0 < count < 600:
                pos = at + toc_at + 4 * ntags
                ents = []
                for _ in range(count):
                    flag = m[pos]; p = pos + 1 + w[0] + w[1]
                    off = int.from_bytes(m[p:p + w[2]], "little") << w[5]; p += w[2]
                    size = int.from_bytes(m[p:p + w[3]], "little"); p += w[3] + w[4]
                    name = bytes(m[p:p + namelen]).split(b"\0")[0].decode("latin1"); p += namelen
                    ents.append((name, flag, at + off, size)); pos = p
                yield ents
        except (struct.error, IndexError, UnicodeDecodeError):
            pass
        at = m.find(b"BGFA1.05", at + 8, hi)


def main():
    iso = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    base, entries = xdvdfs.entries(iso)
    seen = {}
    with open(iso, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for path, is_dir, start, length in entries:
            if is_dir or not path.lower().endswith(".ast"):
                continue
            a0 = base + start * xdvdfs.SECTOR
            arch = os.path.basename(path)
            print("reading", arch, flush=True)
            for ents in packages(m, a0, a0 + length):
                for name, flag, off, size in ents:
                    if "_ALT" in name or not WANT.match(name) or not 0 < size < 8 << 20:
                        continue
                    data = bytes(m[off:off + size])
                    digest = hashlib.sha1(data).hexdigest()
                    if name not in seen:
                        open(os.path.join(OUT, name + ".orig"), "wb").write(data)
                        seen[name] = [arch, off - a0, size, flag, digest, 1, 0]
                    elif seen[name][4] == digest:
                        seen[name][5] += 1
                    else:
                        seen[name][6] += 1
    with open(os.path.join(OUT, "index.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "archive", "offset", "size", "flag", "sha1", "same_copies", "different_copies"])
        for name in sorted(seen):
            w.writerow([name] + seen[name])
    total = sum(v[2] for v in seen.values())
    print(f"{len(seen)} files ({total / 1e6:.0f} MB) written to {OUT}")


if __name__ == "__main__":
    main()
