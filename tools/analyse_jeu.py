#!/usr/bin/env python3
# analyse_jeu.py <log...> — steady-state fps from alive: lines in an in-game run.
# In-game logs have no fps= field (only the menu benchmark does), so fps is
# derived as delta(frames)/delta(t) between consecutive alive: lines.
# Steady-state window starts at T >= 200s: jeu.sh/netwatch.sh scripts the run
# to the second, with the game created around T+145s.
import sys, re, statistics
REGIME_T0 = 200.0
print(f"{'run':<12} {'n':>3} {'mediane':>8} {'moy':>7} {'min':>6} {'max':>6} {'disp%':>6} | "
      f"{'blocs_thr1':>12} {'jit':>6} {'reads':>6} {'n_dyn':>6} {'acq':>10}")
res = {}
for path in sys.argv[1:]:
    pts = []
    last = None
    for line in open(path, errors='replace'):
        m = re.match(r'\[\s*([\d.]+)s\]\s+alive:.*?frames=(\d+)', line)
        if not m:
            continue
        t, f = float(m.group(1)), int(m.group(2))
        if last is not None and t > last[0]:
            dt, df = t - last[0], f - last[1]
            if t >= REGIME_T0 and dt > 0 and df >= 0:
                pts.append(df / dt)
        last = (t, f)
        alive = line
    if not pts:
        print(f"{path.split('/')[-1]:<12} aucune fenetre de regime")
        continue
    med = statistics.median(pts)
    g = lambda p: (re.search(p, alive).group(1) if re.search(p, alive) else '-')
    name = path.split('/')[-1].replace('JEU_', '').replace('.txt', '')
    res[name] = med
    print(f"{name:<12} {len(pts):>3} {med:>8.2f} {statistics.mean(pts):>7.2f} "
          f"{min(pts):>6.2f} {max(pts):>6.2f} {100*(max(pts)-min(pts))/med:>6.1f} | "
          f"{g(r'run=1:[A-Z]:(\d+)'):>12} {g(r'jit=(\d+)ms'):>6} {g(r'reads=(\d+)'):>6} "
          f"{g(r' n=(\d+)'):>6} {g(r'acq=(\d+)'):>10}")
if len(res) > 1:
    v = sorted(res.values())
    print(f"\npopulation : n={len(v)}  mediane={statistics.median(v):.2f}  "
          f"min={v[0]:.2f}  max={v[-1]:.2f}  etendue={100*(v[-1]-v[0])/statistics.median(v):.1f} %")
