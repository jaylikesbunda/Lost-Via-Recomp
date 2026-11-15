"""Disassemble the loaded image (runtime dump from --lost_dump_image) with string annotations.
    python -I tools/dis.py <image.bin> <hex start> [count=64]
    python -I tools/dis.py <image.bin> func <hex addr>     whole function containing addr (to blr)
    python -I tools/dis.py <image.bin> callers <hex addr>  every bl to addr
Annotates lis+addi pairs that land on printable strings, and bl targets.
"""
import re
import struct
import sys

import capstone

BASE = 0x82000000
CODE_LO, CODE_HI = 0x82150000, 0x8288C86C
img = open(sys.argv[1], 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)


def word(a):
    return struct.unpack_from('>I', img, a - BASE)[0]


def cstr(a):
    if not (BASE <= a < BASE + len(img)):
        return None
    m = re.match(rb'[\x20-\x7e]{3,80}', img[a - BASE:a - BASE + 80])
    return m.group().decode() if m else None


def dis(start, count):
    hi = {}
    a = start
    for _ in range(count):
        w = word(a)
        insns = list(md.disasm(struct.pack('>I', w), a))
        text = f'{insns[0].mnemonic} {insns[0].op_str}' if insns else f'.long 0x{w:08X}'
        note = ''
        op = w >> 26
        rd, ra, imm = w >> 21 & 31, w >> 16 & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
        elif op in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54) and ra in hi:
            v = (hi[ra] + simm) & 0xFFFFFFFF
            s = cstr(v)
            note = f'  ; 0x{v:08X}' + (f' "{s}"' if s else '')
        if op == 18 and (w & 1):
            t = (a + ((w & 0x3FFFFFC) - (0x4000000 if w & 0x2000000 else 0))) & 0xFFFFFFFF
            note = f'  ; -> sub_{t:08X}'
        print(f'{a:08X}  {w:08X}  {text}{note}')
        if w == 0x4E800020 and count > 4096:
            break
        a += 4


def func_start(addr):
    a = addr
    while a > CODE_LO and word(a - 4) != 0x4E800020:
        a -= 4
    return a


cmd = sys.argv[2]
if cmd == 'func':
    s = func_start(int(sys.argv[3], 16))
    dis(s, 1 << 20)
elif cmd == 'callers':
    t = int(sys.argv[3], 16)
    for a in range(CODE_LO, CODE_HI, 4):
        w = word(a)
        if w >> 26 == 18 and (w & 3) == 1:
            d = (w & 0x3FFFFFC) - (0x4000000 if w & 0x2000000 else 0)
            if (a + d) & 0xFFFFFFFF == t:
                print(f'{a:08X}  (in ~{func_start(a):08X})')
else:
    dis(int(cmd, 16), int(sys.argv[3]) if len(sys.argv) > 3 else 64)
