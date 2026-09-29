#!/usr/bin/env python3
"""tools/bancs/a_coups.py — stutter metrics of Act V patrol passes.

    a_coups.py bp_A1.txt bp_B1.txt ...

At ~24 img/s the mean frame rate sits at D2's own 25 Hz ceiling and barely
moves; what the player sees are the slow frames. Per pass, over the patrol
window (frames 2800-5700):
  fps       frames / seconds from the alive: beats (same as patrouille_acte5.sh)
  run       median guest run per frame (frames: lines, D2_LAGWATCH)
  >60ms     frames over the lagwatch threshold (chrono/lente lines need
            D2_LAGWATCH=60, which the bench always sets)
  sauts     of those, D2's own frame skip (timeline shows t[2,..]: the draw
            was skipped because the accumulated lateness reached 40 ms —
            needs D2_PPTRACE=1 D2_PHASEPROF=1; '-' without it)
  autres    the rest (long draw: streaming, uploads...)
  pire      worst frame in the window (ms)
  route     A or B: the patrol forks at one click into two reproducible
            routes (patrouille_acte5.sh); compare passes of the SAME route.
"""
import re, sys, statistics as st

def une(path):
    L = open(path, errors="replace").read().splitlines()
    beats, runs = [], []
    lents = {}          # frame -> ms (natif-image or chrono lines, whichever exists)
    sauts = set(); chrono = False; route = '?'
    for l in L:
        if l.startswith('[banc] route='): route = l.split('=', 1)[1].strip()
        m = re.search(r'\[\s*(\d+)\.\d+s\] alive: pump=\d+ frames=(\d+)', l)
        if m: beats.append((int(m.group(1)), int(m.group(2))))
        m = re.search(r'\[\s*(\d+)\.\d+s\] frames: .*run=(\d+)\.(\d)ms/img', l)
        if m: runs.append((int(m.group(1)), int(m.group(2)) + int(m.group(3)) / 10))
        m = re.search(r'chrono f(\d+) \((\d+)ms\):', l)
        if m:
            chrono = True
            f = int(m.group(1)); lents[f] = int(m.group(2))
            if re.search(r't\+\d+\[2,', l): sauts.add(f)
        m = re.search(r'natif-image f(\d+) (\d+)ms', l)
        if m: lents.setdefault(int(m.group(1)), int(m.group(2)))
    w = [b for b in beats if 2800 <= b[1] <= 5700]
    if len(w) < 2: return None
    t0, t1 = w[0][0], w[-1][0]
    fps = (w[-1][1] - w[0][1]) / (t1 - t0)
    rr = [r for t, r in runs if t0 <= t <= t1]
    fen = {f: ms for f, ms in lents.items() if 2800 <= f <= 5700}
    s = sum(1 for f in fen if f in sauts)
    return dict(fps=fps, run=st.median(rr) if rr else float('nan'), n=len(fen),
                sauts=s if chrono else None, autres=(len(fen) - s) if chrono else None,
                pire=max(fen.values()) if fen else 0, secs=t1 - t0, route=route)

print(f"{'passe':28} {'route':>5} {'fps':>6} {'run':>5} {'>60ms':>6} {'sauts':>6} {'autres':>6} {'pire':>5}")
for p in sys.argv[1:]:
    r = une(p)
    nm = p.rsplit('/', 1)[-1]
    if not r: print(f"{nm:28} fenetre vide"); continue
    f = lambda v: '-' if v is None else str(v)
    print(f"{nm:28} {r['route']:>5} {r['fps']:6.2f} {r['run']:5.1f} {r['n']:6d} {f(r['sauts']):>6} {f(r['autres']):>6} {r['pire']:5d}")
