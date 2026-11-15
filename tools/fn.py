"""Print one generated recompiled function (or list functions matching an address prefix).
    python -I tools/fn.py <addr hex> [ndisasm-style filter]
    python -I tools/fn.py --list 82391   all functions whose address starts with
Reads port/generated/default/*.cpp and *.h.
"""
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'port', 'generated', 'default')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
CALL = re.compile(r'\bsub_([0-9A-F]{8})\(')


def files():
    return sorted(glob.glob(os.path.join(GEN, 'lost_recomp.*.cpp')))


def main():
    arg = sys.argv[1]
    if arg == '--list':
        prefix = sys.argv[2].upper()
        hits = []
        for path in files():
            for line in open(path, encoding='utf-8', errors='replace'):
                m = FUNC.match(line)
                if m and m.group(1).startswith(prefix):
                    hits.append((m.group(1), path))
        for a, p in hits:
            print(f'{a}  {os.path.basename(p)}')
        print(f'{len(hits)} functions', file=sys.stderr)
        return
    want = arg.upper().replace('0X', '')
    for path in files():
        cur, buf = None, []
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if cur == want:
                    sys.stdout.write(''.join(buf))
                    return
                cur, buf = m.group(1), [line]
            elif cur is not None:
                buf.append(line)
        if cur == want:
            sys.stdout.write(''.join(buf))
            return
    print(f'sub_{want} not found', file=sys.stderr)
    sys.exit(1)


main()
