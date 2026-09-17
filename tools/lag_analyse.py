#!/usr/bin/env python3
"""tools/lag_analyse.py — breaks down the freezes of a GAME session.

Reads a `boot_progress.txt` pulled from the console after a session played
with `D2_LAGWATCH`, and answers one question per freeze: **who is responsible?**

    python3 tools/lag_analyse.py boot_progress.txt [Game.exe]

Each `lente#NN` line carries two families of clues:

  * HOST causes, already counted by `WX86_FRAMEPROF`:
      jit=    translation of new blocks (entering a never-seen zone)
      sync=   instruction cache synchronization
      reads=  file reads (MPQ, memory card)
      scomp=  Storm decompressions
      sw=     guest thread switches
      look=   block table lookups
  * the GUEST cause: `ech=` EIP samples that landed in the frame, and the
    three dominant addresses.

`ech=0` is not silence, it's a result: either the frame was too short to be
sampled, or the freeze happened **inside a single dynablock** (an inner loop
never goes back through a prologue, so it's never sampled). Cross-referenced
with high `scomp`/`reads`, `ech=0` squarely points to a host cause.
"""
import re
import struct
import sys

RE_T = re.compile(r"^\[\s*([0-9.]+)s\]")
RE_MARK = re.compile(r"^\[\s*([0-9.]+)s\].*MARQUE #(\d+)")
# A SUSTAINED slowdown (25 -> 12 img/s over several seconds) produces NO frame
# above the threshold: it never shows up in any "lente#" line. The per-window
# frame rate is what carries that information instead, so we read it too.
RE_FPS = re.compile(r"^\[\s*([0-9.]+)s\]\s+frames: n=(\d+) fps=([0-9.]+)")
RE = re.compile(
    r"lente#(\d+) f(\d+) (\d+)ms \| io=(\d+)ms blocs=(\d+) jit=(\d+)ms sync=(\d+)ms "
    r"run=(\d+)ms reads=(\d+) scomp=(\d+) sw=(\d+) look=(\d+)"
    r"(?: \| invite ech=(\d+) ([0-9a-f]+)=(\d+) ([0-9a-f]+)=(\d+) ([0-9a-f]+)=(\d+))?")


def fn_entries(path):
    """Game.exe function entries: targets of `call rel32` + vtable."""
    f = open(path, 'rb').read()
    pe = struct.unpack_from('<I', f, 0x3c)[0]
    base = struct.unpack_from('<I', f, pe + 24 + 28)[0]
    txt = None
    for i in range(struct.unpack_from('<H', f, pe + 6)[0]):
        o = pe + 24 + struct.unpack_from('<H', f, pe + 20)[0] + i * 40
        if f[o:o + 8].rstrip(b'\0') == b'.text':
            vsz, va, rsz, raw = struct.unpack_from('<IIII', f, o + 8)
            txt = (raw, base + va, vsz)
    if not txt:
        return None, None
    off, tva, tsz = txt
    ent = set()
    for i in range(off, off + tsz - 5):
        if f[i] == 0xE8:
            t = (i + 5 - off + tva) + struct.unpack_from('<i', f, i + 1)[0]
            if tva <= t < tva + tsz:
                ent.add(t)
    for i in range(0, len(f) - 4, 4):
        v = struct.unpack_from('<I', f, i)[0]
        if tva <= v < tva + tsz and not (off <= i < off + tsz):
            ent.add(v)
    return sorted(ent), (tva, tsz)


def enclosing(ents, rng, va):
    if ents is None or not (rng[0] <= va < rng[0] + rng[1]):
        return None
    lo, hi, r = 0, len(ents) - 1, None
    while lo <= hi:
        m = (lo + hi) // 2
        if ents[m] <= va:
            r = ents[m]; lo = m + 1
        else:
            hi = m - 1
    return r


def cause(d):
    """Attribution in order of confidence. I/O TIME comes first: it's the only
    cost that can explain an entire frame with no visible work."""
    ms = d['ms']
    if d['io'] * 2 > ms:                       # l'E/S explique la moitié ou plus
        return f"ATTENTE E/S — carte mémoire ({d['io']}ms sur {ms}ms)"
    if d['scomp'] > 200 and d['ech'] == 0:
        return "DÉCOMPRESSION (Storm)"
    if d['reads'] > 200 and d['ech'] == 0:
        return "LECTURES FICHIER / carte mémoire"
    if d['jit'] * 3 > ms:
        return "TRADUCTION JIT (zone jamais visitée)"
    if d['sync'] * 3 > ms:
        return "SYNCHRO I-CACHE"
    if d['ech'] > 0:
        return "CODE INVITÉ"
    if d['blocs'] > 200 or d['look'] > 20000:
        return "traduction + consultations (chargement)"
    return "INDÉTERMINÉ (aucun échantillon, aucun poste hôte net)"


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    ents = rng = None
    if len(sys.argv) > 2:
        ents, rng = fn_entries(sys.argv[2])

    seen, rows, marks, fps = set(), [], [], []
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        mf = RE_FPS.match(line)
        if mf:
            fps.append((float(mf.group(1)), float(mf.group(3))))
        mm = RE_MARK.match(line)
        if mm:
            marks.append((float(mm.group(1)), int(mm.group(2))))
        m = RE.search(line)
        if not m:
            continue
        mt = RE_T.match(line)
        t = float(mt.group(1)) if mt else -1.0
        g = m.groups()
        d = dict(t=t, frame=int(g[1]), ms=int(g[2]), io=int(g[3]), blocs=int(g[4]),
                 jit=int(g[5]), sync=int(g[6]), run=int(g[7]), reads=int(g[8]),
                 scomp=int(g[9]), sw=int(g[10]), look=int(g[11]),
                 ech=int(g[12]) if g[12] else 0,
                 ips=[(int(g[13], 16), int(g[14])), (int(g[15], 16), int(g[16])),
                      (int(g[17], 16), int(g[18]))] if g[12] else [])
        key = (d['frame'], d['ms'])
        if key in seen:            # lines get republished on every window
            continue
        seen.add(key)
        rows.append(d)

    if not rows:
        print("Aucune ligne « lente#NN » : WX86_FRAMEPROF et D2_LAGWATCH étaient-ils armés ?")
        return 1

    rows.sort(key=lambda d: -d['ms'])
    if fps:
        print("\n=== cadence par fenêtre de 10 s ===")
        med = sorted(f[1] for f in fps)[len(fps)//2]
        line = "   "
        for t, v in fps:
            tok = f"{t:.0f}s:{v:.0f}" + ("!" if v < 0.7 * med else "") + "  "
            if len(line) + len(tok) > 96:
                print(line); line = "   "
            line += tok
        if line.strip():
            print(line)
        print(f"   (médiane {med:.0f} img/s ; « ! » = sous 70 % de la médiane)")
    if marks:
        print(f"\n=== {len(marks)} MARQUE(S) DU JOUEUR (L + flèche du haut) ===")
        print("Le gel s'est produit AVANT la marque. Images lentes dans les 15 s précédentes :\n")
        for t, n in marks:
            cands = [d for d in rows if 0 <= t - d['t'] <= 15]
            cands.sort(key=lambda d: -d['ms'])
            near = [f for f in fps if abs(f[0] - t) <= 12]
            ctx = " · ".join(f"{f[1]:.0f}" for f in near)
            med = sorted(f[1] for f in fps)[len(fps)//2] if fps else 0
            print(f"  MARQUE #{n} à {t:.0f}s   cadence autour : {ctx} img/s "
                  f"(médiane de la session {med:.0f})")
            if near and med and min(f[1] for f in near) < 0.7 * med:
                print("     >>> RALENTISSEMENT SOUTENU : la cadence est bien tombée,"
                      " sans qu'aucune image ne dépasse le seuil")
            if not cands:
                print("     aucune image lente dans les 15 s — donc pas un à-coup :"
                      " soit soutenu (voir la cadence ci-dessus), soit sous le seuil")
            for d in cands[:4]:
                print(f"     {t - d['t']:4.0f}s avant · f{d['frame']} {d['ms']}ms · {cause(d)}")
            print()
    print(f"{len(rows)} gels distincts relevés")
    if any(d['t'] >= 0 for d in rows):
        ch = sorted((d for d in rows if d['t'] >= 0), key=lambda d: d['t'])
        print("\nChronologie (pour recouper avec ce que tu faisais) :")
        line = "   "
        for d in ch:
            tok = f"{d['t']:.0f}s:{d['ms']}ms  "
            if len(line) + len(tok) > 96:
                print(line); line = "   "
            line += tok
        if line.strip():
            print(line)
    print()
    print(f"{'t(s)':>8} {'image':>8} {'durée':>8}  cause")
    print("-" * 82)
    for d in rows:
        ts = f"{d['t']:8.0f}" if d['t'] >= 0 else "       ?"
        print(f"{ts} {d['frame']:8d} {d['ms']:6d}ms  {cause(d)}")
        det = (f"           io={d['io']}ms jit={d['jit']}ms sync={d['sync']}ms reads={d['reads']} "
               f"scomp={d['scomp']} blocs={d['blocs']} sw={d['sw']} look={d['look']}")
        print(det)
        if d['ech']:
            parts = []
            for rva, n in d['ips']:
                if not rva:
                    continue
                e = enclosing(ents, rng, rva + 0x400000) if ents else None
                nom = f"Game+0x{rva:06x}"
                if e is not None and (e - 0x400000) != rva:
                    nom += f" (dans Game+0x{e-0x400000:06x})"
                parts.append(f"{nom} ×{n}")
            print(f"           invité ech={d['ech']} : " + " · ".join(parts))
        print()

    # Summary: which guest functions recur across freezes?
    agg = {}
    for d in rows:
        for rva, n in d['ips']:
            if not rva:
                continue
            e = enclosing(ents, rng, rva + 0x400000) if ents else None
            k = (e - 0x400000) if e is not None else rva
            a = agg.setdefault(k, [0, 0])
            a[0] += n; a[1] += 1
    if agg:
        print("Fonctions invitées revenant dans les gels (échantillons, nb de gels) :")
        for k, (n, g) in sorted(agg.items(), key=lambda kv: -kv[1][0])[:12]:
            print(f"   Game+0x{k:06x}   {n:5d} éch.   dans {g} gel(s)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
