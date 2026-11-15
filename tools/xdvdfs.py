"""Minimal read-only XDVDFS (Xbox/360 game partition) lister/extractor.
usage: xdvdfs.py list <iso>
       xdvdfs.py extract <iso> <outdir> [glob ...]
"""
import fnmatch, os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
OFFSETS = [0x0, 0xFD90000, 0x2080000, 0x18300000]  # raw, XGD2, XGD3, XGD1


def find_base(f):
    for base in OFFSETS:
        f.seek(base + 32 * SECTOR)
        hdr = f.read(28)
        if hdr[:20] == MAGIC:
            root_sec, root_size = struct.unpack("<II", hdr[20:28])
            return base, root_sec, root_size
    raise SystemExit("no XDVDFS volume descriptor found")


def walk(f, base, sec, size, prefix=""):
    if size == 0:
        return
    f.seek(base + sec * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        path = prefix + name
        if attr & 0x10:
            yield path + "/", start, fsize, True
            yield from walk(f, base, start, fsize, path + "/")
        else:
            yield path, start, fsize, False
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    cmd, iso = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base, rs, rz = find_base(f)
        print(f"# partition base 0x{base:X}, root sector {rs}, root size {rz}")
        entries = sorted(walk(f, base, rs, rz))
        if cmd == "list":
            total = 0
            for p, s, z, d in entries:
                if not d:
                    total += z
                    print(f"{z:>12}  {p}")
            print(f"# {sum(1 for e in entries if not e[3])} files, {total} bytes")
        elif cmd == "extract":
            out = sys.argv[3]
            globs = sys.argv[4:] or ["*"]
            for p, s, z, d in entries:
                if d or not any(fnmatch.fnmatch(p.lower(), g.lower()) for g in globs):
                    continue
                dst = os.path.join(out, *p.split("/"))
                if os.path.normpath(dst).startswith(os.path.normpath(out)) is False:
                    raise SystemExit(f"unsafe path {p}")
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                f.seek(base + s * SECTOR)
                left = z
                with open(dst, "wb") as o:
                    while left:
                        chunk = f.read(min(left, 8 << 20))
                        o.write(chunk)
                        left -= len(chunk)
                print(f"{z:>12}  {p}")


if __name__ == "__main__":
    main()
