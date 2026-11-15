"""Frame-time stats for a time window of a per-frame CSV (--lost_perf_csv).
    python -I tools/perf_window.py <frames.csv> [start_s=185] [end_s=218] [<more.csv> ...]
Seconds are counted from about 3 s before the first presented frame (process start). The
defaults are the jungle walk of tools/autopilot/speed.txt (gameplay from ~175 s)."""
import csv
import datetime as dt
import sys

args = sys.argv[1:]
paths = [a for a in args if a.endswith('.csv')]
nums = [float(a) for a in args if not a.endswith('.csv')]
lo, hi = (nums + [185.0, 218.0])[:2]
for path in paths:
    rows = [r for r in csv.reader(open(path)) if len(r) >= 5 and r[0] != 'frame' and r[1].count(':') == 2]
    t = [dt.datetime.strptime(r[1], '%Y-%m-%d %H:%M:%S.%f').timestamp() for r in rows]
    if not t:
        print(f'{path}: no frames')
        continue
    t0 = t[0] - 3
    g = [r for r, ti in zip(rows, t) if lo <= ti - t0 <= hi]
    if not g:
        print(f'{path}: no frames in {lo:.0f}-{hi:.0f} s (run lasted {t[-1] - t0:.0f} s)')
        continue
    ft = sorted(int(r[2]) / 1000 for r in g)
    wk = sorted(int(r[3]) / 1000 for r in g)
    n = len(g)
    print(f'{path}: {n / (hi - lo):6.1f} fps | median {ft[n // 2]:.2f} ms, 1% worst '
          f'{ft[int(n * .99)]:.1f}, max {ft[-1]:.1f} | game work median {wk[n // 2]:.2f} ms')
