#!/usr/bin/env bash
# tools/bancs/patrouille_acte5.sh — one console pass of the Act V patrol
# (Harrogath), from the player's own save, measured over frames 2800..5700.
#
#   tools/bancs/patrouille_acte5.sh <LAB> [VAR=val ...]
#       e.g.  patrouille_acte5.sh natdraw1
#             patrouille_acte5.sh natdraw0 D2_GLNATDRAW=0
#
# What it does, in order:
#   1. stops the game, RESTORES ux0:data/d2vita/save_banc/ from the local
#      reference copy (every pass starts from the same save: position,
#      mercenary, automap), writes env.txt = base measurement env + D2WRITE +
#      the patrol D2SCRIPT (gen_patrouille_acte5.py) + the knobs given;
#   2. launches, waits for frame 5850 (or a stall / CLEAN EXIT);
#   3. CHECKS the pass landed in Act V at the spawn: the frame-2499 capture
#      must match the Harrogath reference (mean grey difference); a pass that
#      landed elsewhere (menu click missed, wrong difficulty) is relaunched
#      once under identical conditions, then reported as FAILED — never
#      measured;
#      The frame-5760 capture (end of route, after the window) must match
#      $W/ref_fin_5760.png: a pass whose route diverged (an NPC in the way, a
#      click that opened a panel) played a different scene and is not
#      measured either. The first valid pass creates that reference.
#   4. prints fps over the window (alive: beats), c0, guest run/frame, slow
#      frames, and keeps the log in $W/bp_<LAB>.txt.
#
# Needs (all outside git — player data):
#   $W/save_banc_ref/   copy of the player's save dir (jujd.d2s, .key, .ma*,
#                       .map, default.key, registry.txt, locale)
#   $W/ref_apparition_2499.png   reference capture of the spawn
# Validation level: console. Two passes per flavour; report the dispersion.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IP="${VITA_IP:-10.113.1.159}"
FTP="ftp://$IP:1337/ux0:"
W="${D2VITA_WORK:-$ROOT/build-vita/bancs/acte5}"
REF="$W/save_banc_ref"; REFIMG="$W/ref_apparition_2499.png"; REFFIN="$W/ref_fin_5760.png"; REFFIN_B="$W/ref_fin_5760_B.png"
END=5850
[ $# -ge 1 ] || { echo "usage: patrouille_acte5.sh <LAB> [VAR=val ...]"; exit 2; }
LAB="$1"; shift
[ -f "$REF/jujd.d2s" ] || { echo "FAIL: $REF/ absent (copie de la sauvegarde du joueur)"; exit 1; }
[ -f "$REFIMG" ] || { echo "FAIL: $REFIMG absent"; exit 1; }
vc(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" 1338 >/dev/null 2>&1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
BP="$W/bp_$LAB.txt"

# Base measurement env: the player's defaults (baked into the build) plus
# diagnostics that cost the same in every flavour. NOT D2_INTRIN: the five
# native ports of the audit are off by default and, measured on this bench,
# COST ~4.3 ms of guest run per frame (docs/audi_perf.md §12.8).
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
D2WRITE=ux0:data/d2vita/save_banc
EOF
  echo "D2SCRIPT=$(python3 "$ROOT/tools/bancs/gen_patrouille_acte5.py")"
  for k in "$@"; do echo "$k"; done
} > "$TMP/env.txt"

one_pass() {
  vc destroy; sleep 8
  for f in "$REF"/*; do
    timeout 30 curl -sf --ftp-create-dirs -T "$f" "$FTP/data/d2vita/save_banc/$(basename "$f")" \
      || { echo "$LAB: FAIL restauration $(basename "$f")"; return 2; }
  done
  timeout 20 curl -sf -T "$TMP/env.txt" "$FTP/data/d2vita/env.txt" || { echo "$LAB: FAIL depot env.txt"; return 2; }
  vc "nosleep on"; vc "launch DTWO00001"
  echo "$LAB lance $(date +%H:%M:%S) [$*]"
  local prev=-1 stall=0 n=0
  for i in $(seq 1 80); do sleep 10
    timeout 30 curl -sf "$FTP/data/d2vita/boot_progress.txt" -o "$BP" 2>/dev/null
    n=$(grep -oE 'frames=[0-9]+' "$BP" 2>/dev/null | tail -1 | cut -d= -f2); n=${n:-0}
    [ "$n" -ge "$END" ] && break
    grep -q "CLEAN EXIT" "$BP" 2>/dev/null && break
    if [ "$n" = "$prev" ]; then stall=$((stall+1)); else stall=0; fi; prev=$n
    [ "$stall" -ge 6 ] && { echo "$LAB: compteur d'images fige a $n"; break; }
  done
  vc destroy; sleep 6
  # Act V check: the frame-2499 capture against the spawn reference.
  local shot; shot=$(grep "gxm: capture" "$BP" | grep -oE "ux0:data/d2vita/shot_(249[0-9]|250[0-9])\.bmp" | head -1)
  [ -n "$shot" ] || { echo "$LAB: pas de capture a 2499 (pas en jeu ?)"; return 1; }
  timeout 60 curl -sf "$FTP/${shot#ux0:}" -o "$TMP/spawn.bmp" || { echo "$LAB: capture introuvable"; return 1; }
  python3 - "$TMP/spawn.bmp" "$REFIMG" <<'EOF' || return 1
import sys
from PIL import Image
import numpy as np
g=lambda p: np.asarray(Image.open(p).convert('L').resize((240,136)),dtype=float)[5:110]
d=float(np.abs(g(sys.argv[1])-g(sys.argv[2])).mean())
print(f"  controle acte V : ecart a l'apparition d'Harrogath = {d:.1f} (seuil 6)")
sys.exit(0 if d < 6 else 1)
EOF
  [ "$n" -ge "$END" ] || { echo "$LAB: fenetre incomplete (frames=$n)"; return 1; }
  # Route check: the end-of-route capture against the reference pass.
  local fin; fin=$(grep "gxm: capture" "$BP" | grep -oE "ux0:data/d2vita/shot_(575[0-9]|576[0-9])\.bmp" | tail -1)
  [ -n "$fin" ] || { echo "$LAB: pas de capture de fin de parcours"; return 1; }
  timeout 60 curl -sf "$FTP/${fin#ux0:}" -o "$TMP/fin.bmp" || { echo "$LAB: capture de fin introuvable"; return 1; }
  cp "$TMP/fin.bmp" "$W/fin_$LAB.bmp"
  if [ ! -f "$REFFIN" ]; then
    python3 -c "from PIL import Image; Image.open('$TMP/fin.bmp').convert('RGB').save('$REFFIN')"
    echo "  controle parcours : reference de fin CREEE depuis cette passe ($REFFIN) — a verifier a l'oeil"
    return 0
  fi
  # TWO routes. The end-of-route gap is bimodal (~1 or ~16.7 on 30 passes,
  # 27-28/09/2026): the patrol forks at ONE click (an NPC or the mercenary
  # in the way) and each branch is itself reproducible (route B passes are
  # within 1.6 of each other). A pass that matches route A or route B is
  # measured and LABELLED with its route; A/B comparisons are made within
  # one route only. Only an end matching neither is rejected.
  local route
  route=$(python3 - "$TMP/fin.bmp" "$REFFIN" "$REFFIN_B" <<'PYEOF'
import sys, os
from PIL import Image
import numpy as np
g=lambda p: np.asarray(Image.open(p).convert('L').resize((240,136)),dtype=float)[5:110]
f=g(sys.argv[1])
dA=float(np.abs(f-g(sys.argv[2])).mean())
dB=float(np.abs(f-g(sys.argv[3])).mean()) if os.path.exists(sys.argv[3]) else 99.0
r='A' if dA<6 else ('B' if dB<6 else '-')
print(f"{r} {dA:.1f} {dB:.1f}")
PYEOF
)
  echo "  controle parcours : route ${route%% *} (ecart a A=$(echo $route | cut -d' ' -f2), a B=$(echo $route | cut -d' ' -f3), seuil 6)"
  case "${route%% *}" in
    A|B) echo "[banc] route=${route%% *}" >> "$BP"; return 0 ;;
    *)   echo "$LAB: parcours DIVERGENT (fin differente des routes A et B)"; return 1 ;;
  esac
}

# The player's env.txt: saved before the first pass (unless it is itself a
# bench env, left behind by an interrupted run), put back when the script
# exits whatever happens. The bench used to leave its own env on the console:
# the next time the player launched the game, it replayed the patrol on the
# bench save.
ENVJ="$W/env_joueur_console.txt"
if timeout 20 curl -sf "$FTP/data/d2vita/env.txt" -o "$TMP/env_avant.txt" && ! grep -q '^D2SCRIPT=' "$TMP/env_avant.txt"; then
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
if [ $rc = 1 ]; then echo "$LAB: passe invalide, relance a l'identique"; mv -f "$BP" "$W/bp_${LAB}_rejetee1.txt" 2>/dev/null; one_pass "$@"; rc=$?; fi
[ $rc = 0 ] || mv -f "$BP" "$W/bp_${LAB}_rejetee.txt" 2>/dev/null
[ $rc = 0 ] || { echo "$LAB: ECHEC (non mesure)"; exit 1; }

python3 - "$BP" <<'EOF'
import re,sys,statistics as st
L=open(sys.argv[1],errors='replace').read().splitlines()
beats=[];cpu=[];runs=[];slow=0
for l in L:
    m=re.search(r'\[\s*(\d+)\.\d+s\] alive: pump=\d+ frames=(\d+)',l)
    if m: beats.append((int(m.group(1)),int(m.group(2))))
    m=re.search(r'\[\s*(\d+)\.\d+s\] cpu\(10s\): occupes c0=(\d+)%',l)
    if m: cpu.append((int(m.group(1)),int(m.group(2))))
    m=re.search(r'\[\s*(\d+)\.\d+s\] frames: .*run=(\d+)\.(\d)ms/img',l)
    if m: runs.append((int(m.group(1)),int(m.group(2))+int(m.group(3))/10))
w=[b for b in beats if 2800<=b[1]<=5700]
if len(w)<2: print("fenetre vide"); sys.exit(1)
t0,t1=w[0][0],w[-1][0]
fps=(w[-1][1]-w[0][1])/(t1-t0)
c0=[c for t,c in cpu if t0<=t<=t1]; rr=[r for t,r in runs if t0<=t<=t1]
route=next((l.split('route=')[1].strip() for l in L if l.startswith('[banc] route=')),'?')
print(f"  route {route} | fenetre 2800-5700 : {t1-t0} s, fps={fps:.2f}"
      f" | c0 med={st.median(c0) if c0 else float('nan'):.0f}%"
      f" | run/img med={st.median(rr) if rr else float('nan'):.1f} ms")
EOF
echo "$LAB journal -> $BP"
