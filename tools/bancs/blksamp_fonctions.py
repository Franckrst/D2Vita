#!/usr/bin/env python3
"""tools/bancs/blksamp_fonctions.py — time per guest FUNCTION from a
D2_BLKSAMP pass.

    blksamp_fonctions.py <bp_log.txt> [Game.exe] [--top N] [--fn RVA ...]

Input: the "blk:" lines of a pass run with the measurement flavour
(D2VPK_BLKSAMP=1) and D2_TIMESAMP=<us>: every sample names the translated
block that was running (rva=n) or the shim body (S<name>=n). Each sample is
the same slice of wall time, so shares are TIME shares — unlike
D2_TIMEPROF/D2_TIMESAMP on the plain build (docs/audi_perf.md §4).

Blocks are folded into the enclosing function: function starts are the
targets of every direct `call rel32` found by a linear sweep of Game.exe's
.text (capstone), plus any --fn given. A block belongs to the greatest start
<= its address. This is SELF time of a function (its own blocks), not
inclusive time: callees are counted in their own rows.

Needs: pip install capstone pefile (a venv is fine).
"""
import re, sys, bisect, argparse, os

def lire_blocs(path):
    blocs, shims, entete = {}, {}, ""
    for l in open(path, errors="replace"):
        if "blksamp: fenetre" in l:
            entete = l.split("] ", 1)[-1].strip()
        if "] blk:" not in l:
            continue
        for tok in l.split("blk:", 1)[1].split():
            k, _, v = tok.rpartition("=")
            if not k:
                continue
            if k.startswith("S"):
                shims[k[1:]] = shims.get(k[1:], 0) + int(v)
            elif k.startswith("E") and k[1:].isdigit():   # engine stage of a trap round trip
                nm = {"1": "sortie JIT -> C++", "2": "verrou pris", "3": "entree Bridge",
                      "4": "retour: fin de portee (liberation du verrou)", "5": "queue du Bridge",
                      "6": "haut de boucle -> DynaRun", "7": "recherche de bloc (DBGetBlock)",
                      "8": "prologue -> premier bloc"}.get(k[1:], k[1:])
                shims["(moteur) " + nm] = shims.get("(moteur) " + nm, 0) + int(v)
            elif k.startswith("P"):             # phase marker inside a native port (native_f3_114.cpp PH)
                shims["(phase) " + k[1:]] = shims.get("(phase) " + k[1:], 0) + int(v)
            elif k.startswith("T"):             # block AT a trap slot: dispatch / inline intrinsic
                shims["(trap) " + k[1:]] = shims.get("(trap) " + k[1:], 0) + int(v)
            else:
                blocs[int(k, 16)] = blocs.get(int(k, 16), 0) + int(v)
    return entete, blocs, shims

def debuts_fonctions(exe):
    import pefile, capstone
    pe = pefile.PE(exe)
    starts = set()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):     # executable
            continue
        data = s.get_data(); va = s.VirtualAddress
        lo, hi = va, va + len(data)
        for ins in md.disasm(data, va):
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 16)
                if lo <= t < hi:
                    starts.add(t)
    return sorted(starts), pe.OPTIONAL_HEADER.SizeOfImage

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("exe", nargs="?", default=os.path.expanduser("~/d2-vita-refs/1.14d/Game.exe"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--fn", nargs="*", default=[], help="extra function starts (hex RVA)")
    a = ap.parse_args()
    entete, blocs, shims = lire_blocs(a.log)
    tot = sum(blocs.values()) + sum(shims.values())
    if not tot:
        sys.exit("aucune ligne blk: (passe sans D2VPK_BLKSAMP=1 / D2_TIMESAMP ?)")
    starts, taille = debuts_fonctions(a.exe)
    starts = sorted(set(starts) | {int(x, 16) for x in a.fn})
    fonc = {}; hors = {}
    for rva, n in list(blocs.items()):
        if rva >= taille:          # not Game.exe: another translated module (ring DLL, CRT...)
            hors[rva >> 16] = hors.get(rva >> 16, 0) + n
            continue
        i = bisect.bisect_right(starts, rva) - 1
        f = starts[i] if i >= 0 else 0
        e = fonc.setdefault(f, [0, {}])
        e[0] += n; e[1][rva] = n
    guest = sum(blocs.values()); sh = sum(shims.values())
    print(entete)
    print(f"echantillons={tot}  code invite={guest} ({100*guest/tot:.1f}%)  shims={sh} ({100*sh/tot:.1f}%)"
          f"  fonctions={len(fonc)}  debuts connus={len(starts)}")
    print(f"\n{'fonction':>10} {'%temps':>7} {'cumul':>6}  blocs  bloc le plus chaud")
    cum = 0
    for f, (n, bl) in sorted(fonc.items(), key=lambda kv: -kv[1][0])[:a.top]:
        cum += n
        hb = max(bl.items(), key=lambda kv: kv[1])
        print(f"  {f:#08x} {100*n/tot:6.2f}% {100*cum/tot:5.1f}%  {len(bl):5d}  {hb[0]:#08x} ({100*hb[1]/tot:.2f}%)")
    if hors:
        print(f"\nhors Game.exe (SizeOfImage={taille:#x}), par tranche de 64 Kio :")
        for k, n in sorted(hors.items(), key=lambda kv: -kv[1])[:10]:
            print(f"  {100*n/tot:6.2f}%  rva {k<<16:#x}..")
    print("\nshims (temps passe DANS le corps natif, tous fils invites) :")
    for k, n in sorted(shims.items(), key=lambda kv: -kv[1])[:24]:
        print(f"  {100*n/tot:6.2f}%  {k}")

if __name__ == "__main__":
    main()
