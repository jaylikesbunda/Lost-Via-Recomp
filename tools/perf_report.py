"""Frame-time report for a play/smoke session, with likely causes for each hitch.
    python -I tools/perf_report.py <session.log> [--csv <frames.csv>] [--top 15]
Reads the per-frame CSV (--lost_perf_csv; default: <log>-frames.csv next to the log) and the
runtime log. For each hitch (frame >= 50 ms) it counts what the log shows during that frame:
GPU pipeline creation, shader translation, texture loads/resolves (trace level), file opens,
Bink movie starts. "work" heavy = the game's CPU side; "swap" heavy = waiting in present
(vblank, GPU ring space, the command processor behind).
"""
import argparse
import bisect
import csv
import datetime as dt
import os
import re
import statistics
import sys

TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
CAUSES = [
    ('pipeline', re.compile(r'Creating graphics pipeline')),
    ('shader xlate', re.compile(r'(?i)translat(ed|ing) .*shader|shader .*translat')),
    ('texture load', re.compile(r'Loaded (tiled|linear) ')),
    ('resolve', re.compile(r'\] Resolve: ')),
    ('file open', re.compile(r'NtCreateFile\] path=|VFS resolved')),
    ('bink', re.compile(r'Bink worker start|videos\\')),
    ('alloc', re.compile(r'NtAllocateVirtualMemory\] base|MmAllocatePhysicalMemoryEx\] flags')),
]


def parse_ts(s):
    return dt.datetime.strptime(s, '%Y-%m-%d %H:%M:%S.%f')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--csv')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--hitch-ms', type=float, default=50.0)
    a = ap.parse_args()
    csv_path = a.csv or re.sub(r'\.log$', '-frames.csv', a.log)
    if not os.path.exists(csv_path):
        sys.exit(f'no frame CSV at {csv_path} (run with --lost_perf_csv)')

    frames = []
    with open(csv_path, newline='') as f:
        for row in csv.DictReader(f):
            try:
                frames.append((int(row['frame']), parse_ts(row['wall']), int(row['frame_us']),
                               int(row['work_us']), int(row['swap_us'])))
            except (ValueError, KeyError, TypeError):
                pass  # e.g. the last row, cut off when the process was stopped
    if not frames:
        sys.exit('frame CSV is empty')

    # Log events by type, as sorted timestamp lists.
    events = {name: [] for name, _ in CAUSES}
    with open(a.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            for name, pat in CAUSES:
                if pat.search(line):
                    events[name].append(parse_ts(m.group(1)))
                    break
    for v in events.values():
        v.sort()

    ft = sorted(fr[2] for fr in frames)
    n = len(ft)
    total_s = sum(ft) / 1e6
    print(f'{n} frames over {total_s:.1f} s: {n / total_s:.1f} fps average')
    print(f'  median {statistics.median(ft) / 1000:.2f} ms | 1% low {1e6 / ft[min(n - 1, n * 99 // 100)]:.1f} fps'
          f' | 0.1% low {1e6 / ft[min(n - 1, n * 999 // 1000)]:.1f} fps | max {ft[-1] / 1000:.1f} ms')
    buckets = [(0, 17), (17, 34), (34, 50), (50, 100), (100, 250), (250, 1e9)]
    hist = ' '.join(
        f'{lo}-{"inf" if hi > 1e8 else int(hi)}ms:{sum(1 for t in ft if lo * 1000 <= t < hi * 1000)}'
        for lo, hi in buckets)
    print(f'  frame time histogram: {hist}')
    work = [fr[3] for fr in frames]
    swap = [fr[4] for fr in frames]
    print(f'  work avg {statistics.mean(work) / 1000:.2f} ms, swap avg {statistics.mean(swap) / 1000:.2f} ms')

    hitches = [fr for fr in frames if fr[2] >= a.hitch_ms * 1000]
    print(f'\n{len(hitches)} hitches >= {a.hitch_ms:.0f} ms '
          f'({sum(h[2] for h in hitches) / 1e6:.1f} s lost in them)')
    if not hitches:
        return
    totals = {name: 0 for name, _ in CAUSES}
    rows = []
    for no, end, fus, wus, sus in hitches:
        start = end - dt.timedelta(microseconds=fus)
        counts = {}
        for name, _ in CAUSES:
            ts = events[name]
            c = bisect.bisect_right(ts, end) - bisect.bisect_left(ts, start)
            counts[name] = c
            totals[name] += c > 0
        rows.append((fus, no, end, wus, sus, counts))
    print('hitches with each kind of event inside them: ' +
          ', '.join(f'{k} {v}' for k, v in totals.items() if v))
    work_heavy = sum(1 for r in rows if r[3] > r[4])
    print(f'work-dominated {work_heavy}, swap-dominated {len(rows) - work_heavy}')
    print(f'\nworst {min(a.top, len(rows))}:')
    for fus, no, end, wus, sus, counts in sorted(rows, reverse=True)[:a.top]:
        what = ', '.join(f'{k} x{v}' for k, v in counts.items() if v) or 'nothing logged'
        print(f'  frame {no:>6} @ {end.time()}  {fus / 1000:7.1f} ms '
              f'(work {wus / 1000:6.1f}, swap {sus / 1000:6.1f})  {what}')


if __name__ == '__main__':
    main()
