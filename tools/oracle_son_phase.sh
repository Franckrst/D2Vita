#!/usr/bin/env bash
# tools/oracle_son_phase.sh — proves WHY sound shifts frames.
#
# tools/oracle_son.sh shows that the D2_SON=null leg does not return the same
# image fingerprint as the muted leg. The only question that matters: does the
# mixer corrupt the RENDER, or does the game's extra work simply advance its
# clock differently?
#
# This script settles it on the images themselves:
#   1. dumps 460 frames from BOTH legs (D2_FBWIN);
#   2. computes the global bounding box of the differences;
#   3. within that box, searches each sampled frame for a SHIFT k such that
#      sound_leg[n] == muted_leg[n+k].
#
# A pure PHASE shift (the muted leg matches the sound leg at some k != 0
# within the diff box) means the mixer isn't corrupting the render — the game
# clock is just ticking on a different cadence. Under D2_VIRTCLOCK the guest
# clock advances PER CALL to timeGetTime; the sound engine's 20 Hz service
# thread and its volume fades issue extra calls.
#
# Cost: ~450 MiB of dumps in /tmp, ~3 min. This is not a build gate.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
N="${MAXFRAMES:-460}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

run(){ local LAB=$1; shift
  local W=/tmp/sonph_$LAB; rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" MAXSW=400000 MAXFRAMES="$N" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 FBDUMP=1 D2_FBWIN="1:$N" D2_ROOMGUARD=1 \
    D2WRITE="$W" D2SCRIPT="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 1800 qemu-arm -B 0x10000 "$BIN" "$DIR" > "/tmp/sonph_$LAB.log" 2>&1
  echo "  [$LAB] rc=$? dumps=$(ls /tmp/sonph_$LAB/*.raw 2>/dev/null | wc -l)"
}
echo "== ORACLE SON / PHASE — $N images, 2 jambes en parallele =="
run A & run B D2_SON=null & wait

python3 - "$N" <<'PY'
import hashlib, os, sys
W = 800
N = int(sys.argv[1])
def p(l, n): return f"/tmp/sonph_{l}/d2_frame_{n}_800x600x8.raw"
common = [n for n in range(1, N+1) if os.path.exists(p('A',n)) and os.path.exists(p('B',n))]
if not common: print("FAIL: aucun dump commun"); sys.exit(1)
md5 = lambda q: hashlib.md5(open(q,'rb').read()).hexdigest()
diff = [n for n in common if md5(p('A',n)) != md5(p('B',n))]
print(f"  images comparees={len(common)}  identiques={len(common)-len(diff)}  differentes={len(diff)}"
      f"  premiere divergence={diff[0] if diff else '-'}")
if not diff:
    print("PASS: le son ne deplace AUCUNE image sur cette fenetre"); sys.exit(0)
x0=y0=10**9; x1=y1=-1; tot=0
for n in diff:
    a=open(p('A',n),'rb').read(); b=open(p('B',n),'rb').read()
    for i in range(W*600):
        if a[i]!=b[i]:
            tot+=1; x,y=i%W,i//W
            x0=min(x0,x); x1=max(x1,x); y0=min(y0,y); y1=max(y1,y)
print(f"  boite GLOBALE des differences: ({x0},{y0})-({x1},{y1}) = {x1-x0+1}x{y1-y0+1}"
      f"  ({100.0*(x1-x0+1)*(y1-y0+1)/(W*600):.1f} % de l'ecran), {tot} pixels cumules")
def box(q):
    d=open(q,'rb').read()
    return b''.join(d[y*W+x0:y*W+x1+1] for y in range(y0,y1+1))
ech=[n for n in diff if 60 <= n <= N-30][::max(1,len(diff)//10)][:10]
exact=0
for n in ech:
    bb=box(p('B',n)); best=None
    for k in range(-25,26):
        if n+k not in common: continue
        aa=box(p('A',n+k))
        d=sum(1 for u,v in zip(aa,bb) if u!=v)
        if best is None or d<best[1]: best=(k,d)
    if best[1]==0: exact+=1
    print(f"   image {n}: meilleur decalage k={best[0]:+d} -> {best[1]} px differents sur {len(bb)}")
print(f"  images dont la boite est OCTET-IDENTIQUE a un decalage: {exact}/{len(ech)}")
if exact*2 >= len(ech):
    print("PASS: glissement de PHASE d'une animation, pas un changement de rendu")
else:
    print("FAIL: les differences ne s'expliquent PAS par un decalage temporel — a instruire"); sys.exit(1)
PY
