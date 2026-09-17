#!/usr/bin/env python3
# tools/bancs/agg_gxm.py — aggregates one or more console logs (bp_<LAB>.txt) from the GXM path:
# average fps over 10s windows whose worst frame is >= f3600 (patrol only), and averages of the
# gxm: submit-us / gpu-wait-us / present-us counters over windows that actually draw.
# One line per file; the label is the filename without extension.
#   e.g.: python3 tools/bancs/agg_gxm.py build-vita/bancs/bp_A1.txt build-vita/bancs/bp_B1.txt
import re, sys, os
SEUIL = int(os.environ.get("SEUIL_IMAGE", "3600"))
def avg(a): return round(sum(a)/len(a)) if a else None
for path in sys.argv[1:]:
    lab = os.path.splitext(os.path.basename(path))[0]
    v, gpu, sub, pres = [], [], [], []
    for ln in open(path, errors='ignore'):
        m = re.search(r'frames: n=(\d+) fps=([\d.]+) pire=(\d+)ms@f(\d+)', ln)
        if m and int(m.group(4)) >= SEUIL: v.append(float(m.group(2)))
        g = re.search(r'gxm: images=(\d+).*soumission-us=(\d+) attente-gpu-us=(\d+)(?: presentation-us=(\d+))?', ln)
        if g and int(g.group(1)) > 0:
            sub.append(int(g.group(2))); gpu.append(int(g.group(3))); pres.append(int(g.group(4) or 0))
    fps = round(sum(v)/len(v), 2) if v else None
    print(f"{lab:14s} fps={fps} (n={len(v)}) | soumission={avg(sub)}us attente-gpu={avg(gpu)}us "
          f"presentation={avg(pres)}us (moy des {len(gpu)} fenetres)")
