"""Explain native renderer flashes from --lost_native_glitch_catch recordings.

    python -I tools/flash_report.py <prefix> [--min-frame N] [--mark]

<prefix> is the glitch catch prefix (e.g. C:\\lost\\gc\\).

Default: every <prefix>flash-<label>.rec (written by the live flash detector).
--mark:  every <prefix>mark-<label>.rec (written when F12 is pressed: the last ~3.7 s). The
         recording is searched offline for short flashes, more finely than the live detector:
         pixels that move away from the frame before for 1-4 frames and come back, in a region
         that was still just before.

For each flash: <prefix><kind>-<label>-f<frame>-{before,flash,after}.png crops, then the draw
comparison from <prefix>draws-<label>.txt (the draw flight recorder). Each draw is compared as a
whole signature (shaders, pipeline key, every texture's fetch dwords, descriptor index and sign),
so draws that only change order do not show. Listed:
  * NEW during the flash: a signature in a flash frame that neither neighbouring frame has;
  * GONE during the flash: a signature both neighbours have that the flash frame lacks.
"""
import glob
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict

import numpy as np


def read_rec(path):
    data = open(path, 'rb').read()
    frames = []
    pos = 0
    while pos + 32 <= len(data):
        magic, w, h, _res, frame, _t = struct.unpack_from('<4sIIIQQ', data, pos)
        if magic != b'PGRF':
            break
        pos += 32
        size = w * h * 3
        img = np.frombuffer(data, np.uint8, size, pos).reshape(h, w, 3)
        frames.append((frame, img))
        pos += size
    return frames


def write_png(path, img):
    h, w, _ = img.shape
    raw = b''.join(b'\x00' + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def find_flashes(frames, away=60, back=14, max_len=4, min_px=6, max_frac=0.05):
    """(start index, end index exclusive, mask) of short local flashes in a frame list."""
    imgs = [f[1].astype(np.int16) for f in frames]
    nums = [f[0] for f in frames]
    out = []
    consumed = 0
    for n in range(3, len(imgs)):
        for i in range(max(2, n - max_len), n):
            if i < consumed or nums[n] - nums[i - 2] != n - (i - 2):
                continue
            base, pre = imgs[i - 1], imgs[i - 2]
            still = np.abs(base - pre).max(axis=2) <= back
            ret = np.abs(imgs[n] - base).max(axis=2) <= back
            moved = np.ones(still.shape, bool)
            for k in range(i, n):
                moved &= np.abs(imgs[k] - base).max(axis=2) >= away
            mask = still & ret & moved
            count = int(mask.sum())
            if count >= min_px and count <= max_frac * mask.size:
                out.append((i, n, mask))
                consumed = n + 1
                break
    return out


def save_crops(prefix, kind, label, frames, i, n, mask):
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    pad = 16
    h, w = mask.shape
    y0, x0 = max(0, y0 - pad), max(0, x0 - pad)
    y1, x1 = min(h - 1, y1 + pad), min(w - 1, x1 + pad)
    stem = f'{prefix}{kind}-{label}-f{frames[i][0]}'
    for name, k in (('before', i - 1), ('flash', i), ('after', n)):
        write_png(f'{stem}-{name}.png', np.ascontiguousarray(frames[k][1][y0:y1 + 1, x0:x1 + 1]))
    flash_px = frames[i][1][mask].astype(int)
    before_px = frames[i - 1][1][mask].astype(int)
    return (int(x0), int(y0), int(x1), int(y1)), before_px.mean(axis=0), flash_px.mean(axis=0)


def read_draws(path):
    frames = defaultdict(list)
    for line in open(path):
        if line.startswith('#') or ' missing ' in line:
            continue
        head, _, tex = line.partition('|')
        f = head.split()
        if len(f) < 8:
            continue
        t = tex.split()
        textures = tuple(tuple(t[k:k + 7]) for k in range(0, len(t), 7))
        frames[int(f[0])].append(dict(idx=int(f[1]), prim=f[2], count=f[3], gen=f[4], key=f[5],
                                      vs=f[6], ps=f[7], tex=textures))
    return frames


def signature(d):
    return (d['vs'], d['ps'], d['key'], d['gen'], d['prim'], d['count'], d['tex'])


def tex_text(t):
    stage, f0, f1, f2, desc, sign, signed = t
    return (f'{stage} addr={int(f1, 16) & 0xFFFFF000:08X} f0={f0} f1={f1} f2={f2} desc={desc} '
            f'sign={sign} sv={signed}')


def compare_draws(draws, flash_frames):
    before, after = flash_frames[0] - 1, flash_frames[-1] + 1
    if before not in draws or after not in draws:
        print(f'    draw record lacks frame {before} or {after}')
        return
    sb = Counter(signature(d) for d in draws[before])
    sa = Counter(signature(d) for d in draws[after])
    print(f'    draws: before {len(draws[before])}, after {len(draws[after])}; ' +
          ', '.join(f'{f}: {len(draws.get(f, []))}' for f in flash_frames))
    for f in flash_frames:
        sf = Counter(signature(d) for d in draws.get(f, []))
        new = [d for d in draws.get(f, []) if signature(d) not in sb and signature(d) not in sa]
        gone = [sig for sig in sb if sig in sa and sf[sig] < min(sb[sig], sa[sig])]
        for d in new[:20]:
            # The closest neighbour draw with the same shaders and key: what changed.
            same = [x for x in draws[before] + draws[after]
                    if (x['vs'], x['ps'], x['key']) == (d['vs'], d['ps'], d['key'])]
            note = ''
            if same:
                ref = same[0]
                diffs = []
                if ref['gen'] != d['gen']:
                    diffs.append(f'generic {ref["gen"]}->{d["gen"]}')
                if (ref['prim'], ref['count']) != (d['prim'], d['count']):
                    diffs.append(f'prim/count {ref["prim"]}/{ref["count"]}->{d["prim"]}/{d["count"]}')
                for k in range(max(len(ref['tex']), len(d['tex']))):
                    a = ref['tex'][k] if k < len(ref['tex']) else None
                    b = d['tex'][k] if k < len(d['tex']) else None
                    if a != b:
                        diffs.append(f'tex{k} {tex_text(a) if a else "-"} => {tex_text(b) if b else "-"}')
                note = ' | vs a neighbour draw: ' + ('; '.join(diffs) or 'no field differs')
            else:
                note = ' | no neighbour draw with these shaders and key'
            print(f'    NEW frame {f} draw {d["idx"]} vs={d["vs"]} ps={d["ps"]} gen={d["gen"]}'
                  f' textures={len(d["tex"])}{note}')
        if len(new) > 20:
            print(f'    ... {len(new) - 20} more new draws in frame {f}')
        for sig in gone[:10]:
            print(f'    GONE frame {f}: vs={sig[0]} ps={sig[1]} key={sig[2]} textures={len(sig[6])}')


def main():
    args = sys.argv[1:]
    prefix = args[0]
    mark = '--mark' in args
    min_frame = int(args[args.index('--min-frame') + 1]) if '--min-frame' in args else 0
    kind = 'mark' if mark else 'flash'
    for rec in sorted(glob.glob(prefix + kind + '-*.rec')):
        label = int(re.search(kind + r'-(\d+)\.rec$', rec).group(1))
        if label < min_frame:
            continue
        frames = read_rec(rec)
        print(f'== {kind} {label}: {len(frames)} frames ({frames[0][0]}-{frames[-1][0]})' if frames
              else f'== {kind} {label}: empty')
        if not frames:
            continue
        dpath = f'{prefix}draws-{label}.txt'
        draws = read_draws(dpath) if os.path.exists(dpath) else None
        if draws is None:
            print(f'  no draw record {dpath}')
        flashes = find_flashes(frames)
        if not flashes:
            print('  no short local flash found in the recording')
        for i, n, mask in flashes:
            box, before_rgb, flash_rgb = save_crops(prefix, kind, label, frames, i, n, mask)
            flash_frames = [frames[k][0] for k in range(i, n)]
            print(f'  flash frames {flash_frames}: {int(mask.sum())} px in {box} (half size), '
                  f'mean rgb {before_rgb.round().astype(int).tolist()} -> '
                  f'{flash_rgb.round().astype(int).tolist()}')
            if draws is not None:
                compare_draws(draws, flash_frames)


if __name__ == '__main__':
    main()
