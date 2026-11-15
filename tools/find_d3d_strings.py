"""Search the loaded image dump for XDK D3D strings (asserts, format names, function names).
    python -I tools/find_d3d_strings.py <image.bin> [pattern ...]
Prints the address where each string starts, plus the printable run around it.
"""
import re
import sys

BASE = 0x82000000
img = open(sys.argv[1], 'rb').read()

DEFAULT = [
    b'D3DDevice_', b'D3DDevice', b'DrawIndexedVertices', b'BeginVertices', b'EndVertices',
    b'DrawVertices', b'Resolve', b'VdSwap', b'D3D', b'xgraphics', b'MakeSpace',
]

for pat in ([p.encode() for p in sys.argv[2:]] or DEFAULT):
    print(f'=== {pat.decode(errors="replace")}')
    seen = 0
    for m in re.finditer(re.escape(pat), img):
        s = m.start()
        a = s
        while a > 0 and 0x20 <= img[a - 1] < 0x7f:
            a -= 1
        b = s
        while b < len(img) and 0x20 <= img[b] < 0x7f:
            b += 1
        print(f'{BASE + a:08X} {img[a:b].decode("latin1")[:150]!r}')
        seen += 1
        if seen >= 120:
            print('  ...')
            break
