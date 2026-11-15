"""Summarise a generated recompiled function: entry arguments, device-field accesses, calls.
    python -I tools/fnsum.py <addr> [addr ...]
Prints, per function, the r3..r10 uses near entry (argument order), every `NNN(rX)` device
offset load/store, and the functions it calls, in order.
"""
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'port', 'generated', 'default')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
CALL = re.compile(r'\bbl 0x([0-9A-F]{8})')
OFF = re.compile(r'(?:REX_LOAD|REX_STORE)_[US](?:8|16|32|64)\(ctx\.(r\d+)\.u32 \+ (-?\d+)')
OFFV = re.compile(r'REX_RAW_ADDR\(ea\)')


def body(addr):
    want = addr.upper().replace('0X', '')
    for path in sorted(glob.glob(os.path.join(GEN, 'lost_recomp.*.cpp'))):
        cur, buf = None, []
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if cur == want:
                    return buf
                cur, buf = m.group(1), [line]
            elif cur is not None:
                buf.append(line)
        if cur == want:
            return buf
    return None


def main():
    for addr in sys.argv[1:]:
        lines = body(addr)
        if lines is None:
            print(f'sub_{addr}: NOT FOUND')
            continue
        print(f'===== sub_{addr.upper()}   ({len(lines)} lines)')
        # Entry register moves in the first 40 lines.
        ent = []
        for line in lines[:60]:
            m = re.match(r'\s*//\s*(mr|li)\s+(r\d+),(r\d+)', line)
            if m and m.group(2) not in [e[0] for e in ent]:
                ent.append((m.group(2), m.group(3)))
        print('  entry moves: ' + ', '.join(f'{a}<-{b}' for a, b in ent[:12]))
        offs, calls = [], []
        for line in lines:
            for m in OFF.finditer(line):
                offs.append((m.group(1), int(m.group(2))))
            m = CALL.search(line)
            if m:
                calls.append(m.group(1))
        # Collapse offsets per base register.
        agg = {}
        for reg, off in offs:
            agg.setdefault(reg, []).append(off)
        print('  device offsets:')
        for reg in sorted(agg, key=lambda r: len(agg[r]), reverse=True)[:4]:
            vals = sorted(set(agg[reg]))
            print(f'    {reg}: {vals[:40]}')
        print('  calls: ' + ' '.join(calls[:40]))


main()
