"""Cross-references in the loaded image (runtime dump from --lost_dump_image).
    python -I tools/xref.py <image.bin> strings <regex>       strings matching regex, with addresses
    python -I tools/xref.py <image.bin> refs <hex addr> [...]  code that builds the address with
                                                              lis+addi/ori/lwz/lfs.. and data words equal to it
The image is mapped at 0x82000000. Code is scanned 0x82150000-0x8288C86C (REX_CODE_BASE/SIZE).
"""
import re
import struct
import sys

BASE = 0x82000000
CODE_LO, CODE_HI = 0x82150000, 0x8288C86C

img = open(sys.argv[1], 'rb').read()
cmd = sys.argv[2]


def word(a):
    return struct.unpack_from('>I', img, a - BASE)[0]


def containing_function(addr):
    """Walk back to the nearest mflr r12 / blr boundary: a rough function start."""
    a = addr
    while a > CODE_LO:
        w = word(a - 4)
        if w == 0x4E800020:  # blr
            return a
        a -= 4
    return None


if cmd == 'strings':
    pat = re.compile(sys.argv[3], re.I)
    for m in re.finditer(rb'[\x20-\x7e]{4,120}', img):
        s = m.group().decode()
        if pat.search(s):
            print(f'0x{BASE + m.start():08X}  {s}')
elif cmd == 'refs':
    targets = [int(t, 16) for t in sys.argv[3:]]
    for t in targets:
        hi = (t + 0x8000) >> 16 & 0xFFFF   # lis value paired with a signed low half
        lo = t & 0xFFFF
        hi_ori = t >> 16 & 0xFFFF
        print(f'== 0x{t:08X}')
        # data words
        for off in range(0, len(img) - 3, 4):
            if struct.unpack_from('>I', img, off)[0] == t:
                print(f'  data  0x{BASE + off:08X}')
        # lis rX,hi ; then within 24 instructions an op with rA=rX and imm == lo
        for a in range(CODE_LO, CODE_HI, 4):
            w = word(a)
            if w >> 26 != 15 or (w >> 16 & 31) != 0:  # addis rD,0,imm == lis
                continue
            imm = w & 0xFFFF
            rd = w >> 21 & 31
            if imm not in (hi, hi_ori):
                continue
            for b in range(a + 4, min(a + 4 + 24 * 4, CODE_HI), 4):
                v = word(b)
                op = v >> 26
                ra = v >> 16 & 31
                vimm = v & 0xFFFF
                if ra != rd:
                    continue
                ok = (imm == hi and vimm == lo and op in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54)) or \
                     (imm == hi_ori and vimm == lo and op == 24)
                if ok:
                    fs = containing_function(a)
                    print(f'  code  lis@0x{a:08X} use@0x{b:08X}  (function ~0x{fs:08X})' if fs else
                          f'  code  lis@0x{a:08X} use@0x{b:08X}')
                    break
