#!/usr/bin/env python3
"""tools/bancs/fps_patrouille.py — extracts THE number from one console pass.

WHY THIS IS NOT A WINDOW AVERAGE

`boot_progress.txt` publishes one `frames:` line per 10s window of WALL time, but
`D2SCRIPT` is paced in GAME FRAMES. A run that clears the menus faster reaches a
given patrol position at a different wall-clock instant, so 10s windows don't
land on the same scene across runs — reading fps per window is noisy enough to
swallow real optimizations.

RULE: wall time taken to go from game frame F1 to game frame F2. Both bounds are
game frames, so every run covers the same patrol segment regardless of time
spent in menus. Wall time at a given frame is linearly interpolated between two
windows (each window gives a cumulative frame count and its timestamp).

Default bounds: F1=3700 (the patrol starts at frame 3600 in
d2script_camp_patrouille_v2.txt; +100 lets the first move begin), F2=6000
(before MAXFRAMES=6200, whose tail is truncated by the stop). Override with
--f1 / --f2.
"""
import argparse
import re
import sys

RE_W = re.compile(r"^\[\s*([0-9.]+)s\]\s+frames: n=(\d+) fps=([0-9.]+)")


def cumul(path):
    """[(window_end_time, cumulative_frames)], starting from (0,0)."""
    pts = [(0.0, 0)]
    n = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_W.match(line)
            if m:
                n += int(m.group(2))
                pts.append((float(m.group(1)), n))
    return pts


def temps_a(pts, frame):
    """Wall time at which the frame counter reaches `frame` (interpolated)."""
    for i in range(1, len(pts)):
        t0, n0 = pts[i - 1]
        t1, n1 = pts[i]
        if n1 >= frame:
            if n1 == n0:
                return None
            return t0 + (t1 - t0) * (frame - n0) / (n1 - n0)
    return None


def mesure(path, f1, f2):
    pts = cumul(path)
    if len(pts) < 3:
        return None, "journal trop court (la passe n'a pas atteint le jeu)"
    total = pts[-1][1]
    if total < f2:
        return None, f"la passe s'arrete a l'image {total}, borne F2={f2} jamais atteinte"
    t1, t2 = temps_a(pts, f1), temps_a(pts, f2)
    if t1 is None or t2 is None or t2 <= t1:
        return None, "bornes non interpolables"
    return ((f2 - f1) / (t2 - t1), t2 - t1, total), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("journaux", nargs="+")
    ap.add_argument("--f1", type=int, default=3700)
    ap.add_argument("--f2", type=int, default=6000)
    a = ap.parse_args()
    vals = []
    for path in a.journaux:
        lab = path.split("bp_")[-1].replace(".txt", "")
        res, err = mesure(path, a.f1, a.f2)
        if err:
            print(f"{lab:12s} ECHEC : {err}")
            continue
        fps, duree, total = res
        vals.append((lab, fps))
        print(f"{lab:12s} fps={fps:6.2f}  ({a.f1}->{a.f2} en {duree:5.1f}s, passe={total} images)")
    if len(vals) > 1:
        xs = [v for _, v in vals]
        lo, hi, moy = min(xs), max(xs), sum(xs) / len(xs)
        print(f"\ndispersion : min={lo:.2f} max={hi:.2f} moy={moy:.2f} "
              f"etendue={(hi - lo) / moy * 100:.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
