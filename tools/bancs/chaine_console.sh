#!/usr/bin/env bash
# tools/bancs/chaine_console.sh — standard console chain: waits for the
# console, deploys the eboot (read back and md5-checked), the .gxp shaders and
# the ring DLL, plays "LAB:knobs" legs (passe_console.sh + pulls back
# shot_*.bmp captures), stops if the console disappears, then restores the
# game env (ENV_RESTAURE).
# Variables: EBOOT= DLL= SHADERS=1 DLL_RETIRER=0 ENV_RESTAURE=tools/bancs/env_jeu_glide.txt SUITE= D2VITA_WORK= VITA_*
#   e.g. EBOOT=build-vita/eboot.bin DLL=build-glide/glide3x.dll SUITE=c1 bash tools/bancs/chaine_console.sh "A:D2_GXMASYNC=2" "B:D2_GXMASYNC=2 D2_GXMCLEAR=plat"
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IP="${VITA_IP:-10.113.1.159}"; FTPP="${VITA_FTP_PORT:-1337}"; VCP="${VITA_VC_PORT:-1338}"
FTP="ftp://$IP:$FTPP/ux0:"; TITLE="${VITA_TITLE:-DTWO00001}"
W="${D2VITA_WORK:-$ROOT/build-vita/bancs}"; mkdir -p "$W/shots"
SUITE="${SUITE:-chaine}"; L="$W/$SUITE.log"
EBOOT="${EBOOT:-}"; DLL="${DLL:-}"; SHADERS="${SHADERS:-1}"; SHADERS_DIR="${SHADERS_DIR:-$ROOT/shaders}"
DLL_RETIRER="${DLL_RETIRER:-0}"          # 1 = remove 1.14d/glide3x.dll at the end of the chain (GDI game env)
ENV_RESTAURE="${ENV_RESTAURE:-$ROOT/tools/bancs/env_jeu_glide.txt}"
ATTENTE_CONSOLE="${ATTENTE_CONSOLE:-14400}"   # s: max wait for the console to come back
[ $# -ge 1 ] || { echo "usage: [EBOOT=..] [DLL=..] chaine_console.sh 'LAB:knobs' ..."; exit 2; }
say(){ echo "$(date +%H:%M:%S) $*" | tee -a "$L"; }
vcmd(){ printf '%s\n' "$1" | timeout 8 nc -q1 "$IP" "$VCP" >/dev/null 2>&1; }
alive(){ timeout 15 curl -s -f "$FTP/data/d2vita/" -o /dev/null; }
fetch_shots(){ local lab="$1" n=0 f
  for f in $(timeout 30 curl -s "$FTP/data/d2vita/" | awk '{print $NF}' | grep -E '^shot_[0-9]+\.bmp$'); do
    if timeout 120 curl -s -f -o "$W/shots/${lab}_$f" "$FTP/data/d2vita/$f"; then n=$((n+1))
      command -v convert >/dev/null && convert "$W/shots/${lab}_$f" "$W/shots/${lab}_${f%.bmp}.png" 2>/dev/null && rm -f "$W/shots/${lab}_$f"
    fi
    timeout 30 curl -s -o /dev/null "$FTP/data/d2vita/" -Q "-DELE ux0:/data/d2vita/$f" >/dev/null 2>&1
  done; say "$lab: $n capture(s) rapatriee(s) -> $W/shots"; }

say "== CHAINE $SUITE : eboot=${EBOOT:-aucun} dll=${DLL:-aucune} jambes: $*"
T0=$(date +%s)
until alive; do sleep 20; [ $(( $(date +%s) - T0 )) -gt "$ATTENTE_CONSOLE" ] && { say "CONSOLE ABSENTE — ABANDON"; exit 1; }; done
say "console vivante"; vcmd destroy; sleep 6

if [ -n "$EBOOT" ]; then
  [ -f "$EBOOT" ] || { say "EBOOT $EBOOT absent — ABANDON"; exit 1; }
  timeout 180 curl -s -f -T "$EBOOT" "$FTP/app/$TITLE/eboot.bin" || { say "DEPLOIEMENT ECHOUE"; exit 1; }
  timeout 180 curl -s -f -o "$W/eboot_relu.bin" "$FTP/app/$TITLE/eboot.bin"
  if [ "$(md5sum < "$W/eboot_relu.bin")" = "$(md5sum < "$EBOOT")" ]; then say "EBOOT A BORD md5=$(md5sum < "$EBOOT" | cut -c1-32)"
  else say "EMPREINTE DIFFERENTE — ABANDON"; exit 1; fi
  rm -f "$W/eboot_relu.bin"
  if [ "$SHADERS" = 1 ]; then for f in "$SHADERS_DIR"/*.gxp; do
    timeout 60 curl -s -f -T "$f" "$FTP/data/d2vita/shaders/$(basename "$f")" || { say "DEPOT SHADER ECHOUE ($f)"; exit 1; }
  done; say "shaders deposes ($(ls "$SHADERS_DIR"/*.gxp | wc -l))"; fi
fi
if [ -n "$DLL" ]; then
  timeout 60 curl -s -f -T "$DLL" "$FTP/data/d2vita/1.14d/glide3x.dll" && say "DLL RING DEPOSEE" || { say "DEPOT DLL ECHOUE"; exit 1; }
fi
fetch_shots PREALABLE
rc=0
for spec in "$@"; do
  lab="${spec%%:*}"; knobs="${spec#*:}"; [ "$knobs" = "$lab" ] && knobs=""
  bash "$ROOT/tools/bancs/passe_console.sh" "$lab" $knobs >> "$L" 2>&1
  fetch_shots "$lab"
  alive || { say "CONSOLE PERDUE apres $lab — arret des jambes"; rc=2; break; }
done
[ "$DLL_RETIRER" = 1 ] && timeout 30 curl -s -o /dev/null "$FTP/data/d2vita/1.14d/" -Q "-DELE ux0:/data/d2vita/1.14d/glide3x.dll" >/dev/null 2>&1 && say "DLL RING RETIREE"
timeout 40 curl -s -f -T "$ENV_RESTAURE" "$FTP/data/d2vita/env.txt" && say "ENV JEU RESTAURE ($ENV_RESTAURE)" || say "ECHEC RESTAURATION ENV — a refaire a la main"
say "CHAINE $SUITE TERMINEE rc=$rc"; exit $rc
