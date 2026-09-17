#!/usr/bin/env python3
# tools/bancs/agg_natif.py — aggregates one or more console logs (bp_<LAB>.txt) captured with WX86_NATPROF=1:
# steady-state `alive:` windows (worst frame >= f3600), fps, then the top 9 native shims (`natif N:`)
# in ms/frame, excluding blocking calls (Sleep, WaitForSingleObject, PeekMessageA) which are not work.
# The label is the filename without extension.
#   e.g.: python3 tools/bancs/agg_natif.py build-vita/bancs/bp_X_NATPROF.txt
import re, sys, os, collections
SEUIL = int(os.environ.get("SEUIL_IMAGE", "3600"))
BLOQ = {'KERNEL32.dll!Sleep', 'KERNEL32.dll!WaitForSingleObject', 'USER32.dll!PeekMessageA'}
for path in sys.argv[1:]:
    lab = os.path.splitext(os.path.basename(path))[0]
    win, cur = [], None
    for ln in open(path, errors='ignore').read().splitlines():
        if 'alive: pump=' in ln:
            cur = {'natif': {}, 'n': None, 'fps': None, 'f': None, 'tot': None}; win.append(cur); continue
        if cur is None: continue
        m = re.search(r'frames: n=(\d+) fps=([\d.]+) pire=(\d+)ms@f(\d+)', ln)
        if m: cur['n'] = int(m.group(1)); cur['fps'] = float(m.group(2)); cur['f'] = int(m.group(4)); continue
        m = re.search(r'natif: total=(\d+)us', ln)
        if m: cur['tot'] = int(m.group(1)); continue
        m = re.search(r'natif \d+: (\d+)us [\d.]+% (.+)$', ln)
        if m: cur['natif'][m.group(2).strip()] = int(m.group(1))
    pat = [w for w in win if w['f'] and w['f'] >= SEUIL and w['n']]
    if not pat:
        print(f"{lab}: aucune fenetre en regime (>= f{SEUIL})"); continue
    frames = sum(w['n'] for w in pat); fps = sum(w['fps'] for w in pat)/len(pat)
    agg = collections.Counter()
    for w in pat:
        for k, v in w['natif'].items(): agg[k] += v
    print(f"{lab}: fenetres={len(pat)} images={frames} fps={fps:.2f} ({1000/fps:.1f} ms/img)")
    for k, v in agg.most_common(9):
        if k in BLOQ: continue
        print(f"  {v/frames/1000:6.2f} ms/img  {k}")
