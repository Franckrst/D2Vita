#!/usr/bin/env bash
# tools/bancs/orbe_givre.sh — one console pass of the Frozen Orb bench: a
# level-99 Sorceress takes the Harrogath waypoint and casts Frozen Orb
# non-stop at the destination; fps measured over the casting window.
#
#   tools/bancs/orbe_givre.sh <LAB> [VAR=val ...]
#   EXPLORE=1 tools/bancs/orbe_givre.sh explo1     calibration pass: captures
#       at every step, all pulled back into $W/shots_<LAB>/, nothing measured
#
# Same structure as patrouille_acte5.sh (read it first):
#   1. stops the game, restores ux0:data/d2vita/save_orbe/ from the local
#      reference ($W/save_orbe_ref/, made by gen_save_orbe.py from the Act V
#      bench save if absent), removes the automap the previous pass left,
#      writes env.txt = the same measurement env as the Act V patrol + D2WRITE
#      + the D2SCRIPT of gen_orbe_givre.py + the knobs given;
#   2. launches, waits until the right button is released (or a stall /
#      CLEAN EXIT);
#   3. CHECKS the spawn capture (frame 2499) and the arrival capture against
#      $W/ref_apparition.png / $W/ref_arrivee.png — the first valid pass
#      creates them (look at them!); a pass that landed elsewhere (menu click
#      missed, waypoint panel not opened, wrong row) is relaunched once, then
#      reported FAILED — never measured;
#   4. prints fps over the window, c0, guest run/frame, keeps the log in
#      $W/bp_<LAB>.txt.
# There is no end-of-route check: monsters come and die at their own pace,
# the scene is the same zone and the same spell, not the same pixels.
# Validation level: console. Two passes per flavour, interleaved; report the
# dispersion.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IP="${VITA_IP:-10.113.1.159}"
FTP="ftp://$IP:1337/ux0:"
W="${D2VITA_WORK:-$ROOT/build-vita/bancs/orbe}"
A5="$ROOT/build-vita/bancs/acte5/save_banc_ref"
REF="$W/save_orbe_ref"; REFSPAWN="$W/ref_apparition.png"; REFARR="$W/ref_arrivee.png"
GEN="$ROOT/tools/bancs/gen_orbe_givre.py"
read -r WIN0 WIN1 END ARR <<<"$(python3 "$GEN" window)"
END=$((END+100))
EXPLORE="${EXPLORE:-0}"
[ $# -ge 1 ] || { echo "usage: orbe_givre.sh <LAB> [VAR=val ...]"; exit 2; }
LAB="$1"; shift
mkdir -p "$W"
if [ ! -f "$REF/Givre.d2s" ]; then
  [ -f "$A5/jujd.d2s" ] || { echo "FAIL: ni $REF/Givre.d2s ni $A5/jujd.d2s"; exit 1; }
  python3 "$ROOT/tools/bancs/gen_save_orbe.py" "$A5/jujd.d2s" "$REF" Givre || exit 1
  cp "$A5/jujd.key" "$REF/Givre.key"; cp "$A5/default.key" "$A5/registry.txt" "$A5/locale" "$REF/"
fi
vc(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" 1338 >/dev/null 2>&1; }
TMP="$(mktemp -d)"
BP="$W/bp_$LAB.txt"

{ cat <<'EOF'
D2NET=1
WX86_FRAMEPROF=1
D2_LAGWATCH=60
D2_LAGWATCH_NOSAMP=1
D2_WAITPROF=1
D2_LOOPWATCH=1
D2_NATPROF=1
D2_FILSTAT=1
D2_IOSTAT=1
D2_SONCPU=2
D2_PERSPECTIVE=1
D2WRITE=ux0:data/d2vita/save_orbe
EOF
  if [ "$EXPLORE" = 1 ]; then echo "D2SCRIPT=$(python3 "$GEN" explore)"; else echo "D2SCRIPT=$(python3 "$GEN")"; fi
  for k in "$@"; do echo "$k"; done
} > "$TMP/env.txt"

# Pull the capture written for a scripted snap near frame $1 (named
# shot_<frame-1> or shot_<frame>: read it from THIS pass's log).
pull_shot() {
  local f=$1 out=$2 shot
  shot=$(grep "gxm: capture" "$BP" | grep -oE "ux0:data/d2vita/shot_[0-9]+\.bmp" \
         | awk -F'[_.]' -v f="$f" '{n=$(NF-1)+0; if (n>=f-12 && n<=f+12) print}' | head -1)
  [ -n "$shot" ] && timeout 60 curl -sf "$FTP/${shot#ux0:}" -o "$out"
}

# Compare a capture with a reference (mean grey difference on the world
# area); creates the reference from the first pass.
check_img() {
  local img=$1 ref=$2 what=$3
  if [ ! -f "$ref" ]; then
    python3 -c "from PIL import Image; Image.open('$img').convert('RGB').save('$ref')"
    echo "  controle $what : reference CREEE depuis cette passe ($ref) — a verifier a l'oeil"
    return 0
  fi
  python3 - "$img" "$ref" "$what" <<'EOF'
import sys
from PIL import Image
import numpy as np
g=lambda p: np.asarray(Image.open(p).convert('L').resize((240,136)),dtype=float)[5:110]
d=float(np.abs(g(sys.argv[1])-g(sys.argv[2])).mean())
print(f"  controle {sys.argv[3]} : ecart a la reference = {d:.1f} (seuil 8)")
sys.exit(0 if d < 8 else 1)
EOF
}

one_pass() {
  vc destroy; sleep 8
  for f in "$REF"/*; do
    timeout 30 curl -sf --ftp-create-dirs -T "$f" "$FTP/data/d2vita/save_orbe/$(basename "$f")" \
      || { echo "$LAB: FAIL restauration $(basename "$f")"; return 2; }
  done
  # Automap left by the previous pass: every pass starts from the same save.
  for e in ma0 ma1 ma2 ma3 map; do
    timeout 15 curl -s -o /dev/null "$FTP/data/d2vita/save_orbe/" -Q "-DELE ux0:/data/d2vita/save_orbe/Givre.$e" >/dev/null 2>&1
  done
  timeout 20 curl -sf -T "$TMP/env.txt" "$FTP/data/d2vita/env.txt" || { echo "$LAB: FAIL depot env.txt"; return 2; }
  vc "nosleep on"; vc "launch DTWO00001"
  echo "$LAB lance $(date +%H:%M:%S) [$*]"
  local prev=-1 stall=0 n=0
  for i in $(seq 1 90); do sleep 10
    timeout 30 curl -sf "$FTP/data/d2vita/boot_progress.txt" -o "$BP" 2>/dev/null
    n=$(grep -oE 'frames=[0-9]+' "$BP" 2>/dev/null | tail -1 | cut -d= -f2); n=${n:-0}
    [ "$n" -ge "$END" ] && break
    grep -q "CLEAN EXIT" "$BP" 2>/dev/null && break
    if [ "$n" = "$prev" ]; then stall=$((stall+1)); else stall=0; fi; prev=$n
    [ "$stall" -ge 6 ] && { echo "$LAB: compteur d'images fige a $n"; break; }
  done
  vc destroy; sleep 6
  if [ "$EXPLORE" = 1 ]; then
    mkdir -p "$W/shots_$LAB"
    for s in $(grep "gxm: capture" "$BP" | grep -oE "ux0:data/d2vita/shot_[0-9]+\.bmp" | sort -u); do
      timeout 60 curl -sf "$FTP/${s#ux0:}" -o "$W/shots_$LAB/$(basename "$s")" && echo "  capture $(basename "$s")"
    done
    echo "$LAB: passe d'exploration (frames=$n) -> $W/shots_$LAB/"
    return 3
  fi
  pull_shot 2499 "$TMP/spawn.bmp" || { echo "$LAB: pas de capture a 2499 (pas en jeu ?)"; return 1; }
  check_img "$TMP/spawn.bmp" "$REFSPAWN" "apparition Harrogath" || return 1
  pull_shot "$ARR" "$TMP/arr.bmp" || { echo "$LAB: pas de capture d'arrivee ($ARR)"; return 1; }
  cp "$TMP/arr.bmp" "$W/arrivee_$LAB.bmp"
  check_img "$TMP/arr.bmp" "$REFARR" "arrivee waypoint" || return 1
  [ "$n" -ge "$END" ] || { echo "$LAB: fenetre incomplete (frames=$n)"; return 1; }
  return 0
}

ENVJ="$W/env_joueur_console.txt"
# Not the player's if it carries any bench mark: a D2SCRIPT, the route
# recorder, or a bench save dir (05/10/2026: the recording env, which has no
# D2SCRIPT, was saved as the player's and put back on the console).
if timeout 20 curl -sf "$FTP/data/d2vita/env.txt" -o "$TMP/env_avant.txt" \
   && ! grep -qE '^(D2SCRIPT|D2_RECORD)=|save_(orbe|banc)' "$TMP/env_avant.txt"; then
  cp "$TMP/env_avant.txt" "$ENVJ"
fi
restore_env(){
  [ -f "$ENVJ" ] || { echo "ATTENTION: aucune copie de l'env.txt du joueur ($ENVJ) — env.txt du banc laisse sur la console"; return; }
  vc destroy; sleep 4
  timeout 20 curl -sf -T "$ENVJ" "$FTP/data/d2vita/env.txt" && echo "env.txt du joueur restaure" \
    || echo "ATTENTION: restauration de env.txt ECHOUEE — la console garde l'env du banc"
}
trap 'restore_env; rm -rf "$TMP"' EXIT

one_pass "$@"; rc=$?
[ $rc = 3 ] && exit 0
if [ $rc = 1 ]; then echo "$LAB: passe invalide, relance a l'identique"; mv -f "$BP" "$W/bp_${LAB}_rejetee1.txt" 2>/dev/null; one_pass "$@"; rc=$?; fi
[ $rc = 0 ] || mv -f "$BP" "$W/bp_${LAB}_rejetee.txt" 2>/dev/null
[ $rc = 0 ] || { echo "$LAB: ECHEC (non mesure)"; exit 1; }

python3 - "$BP" "$WIN0" "$WIN1" <<'EOF'
import re,sys,statistics as st
L=open(sys.argv[1],errors='replace').read().splitlines()
w0,w1=int(sys.argv[2]),int(sys.argv[3])
beats=[];cpu=[];runs=[]
for l in L:
    m=re.search(r'\[\s*(\d+)\.\d+s\] alive: pump=\d+ frames=(\d+)',l)
    if m: beats.append((int(m.group(1)),int(m.group(2))))
    m=re.search(r'\[\s*(\d+)\.\d+s\] cpu\(10s\): occupes c0=(\d+)%',l)
    if m: cpu.append((int(m.group(1)),int(m.group(2))))
    m=re.search(r'\[\s*(\d+)\.\d+s\] frames: .*run=(\d+)\.(\d)ms/img',l)
    if m: runs.append((int(m.group(1)),int(m.group(2))+int(m.group(3))/10))
w=[b for b in beats if w0<=b[1]<=w1]
if len(w)<2: print("fenetre vide"); sys.exit(1)
t0,t1=w[0][0],w[-1][0]
fps=(w[-1][1]-w[0][1])/(t1-t0)
# per-beat fps inside the window: the dips are where the orbs pile up
seg=[(b[1]-a[1])/(b[0]-a[0]) for a,b in zip(w,w[1:]) if b[0]>a[0]]
c0=[c for t,c in cpu if t0<=t<=t1]; rr=[r for t,r in runs if t0<=t<=t1]
print(f"  fenetre {w0}-{w1} : {t1-t0} s, fps={fps:.2f} (min/battement {min(seg):.1f}, max {max(seg):.1f})"
      f" | c0 med={st.median(c0) if c0 else float('nan'):.0f}%"
      f" | run/img med={st.median(rr) if rr else float('nan'):.1f} ms")
EOF
echo "$LAB journal -> $BP"
