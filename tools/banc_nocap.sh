#!/usr/bin/env bash
# tools/banc_nocap.sh — qemu bench for the frame limiter (D2_NOCAP), REAL CLOCK.
#
# The 25 fps cap only shows up in REAL TIME: under D2_VIRTCLOCK, GetTickCount
# advances one tick per call and the Fog loop's 40 ms credit is consumed
# within a few pump cycles — which is why the pixel oracle's 4000 frames show
# 40-55 fps without the limiter ever engaging. Here: D2_REALCLOCK=1, no
# D2_VIRTCLOCK.
#
# Two legs, same binary, same scenario (the one from oracle_cellopt.sh):
#   A = knob absent  -> "frames: fps=" windows must stay <= 25
#   B = D2_NOCAP=1   -> they must exceed it
# D2_LOOPWATCH=1 additionally publishes, per frame, the PRESENTATION THREAD wait.
#
# MAXFRAMES=... NOCAP=... TAG=... EXTRAENV="..." bash tools/banc_nocap.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
NOCAP="${NOCAP:-1}"
FROM="${FROM:-0}"   # D2_NOCAP_FROM: arming frame (0 = from load time)
TAG="${TAG:-}"
TMO="${TMO:-3600}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run(){ # $1 label  $2 D2_NOCAP value ("" = absent)
  local LAB="$1$TAG" KNOB="$2" W=/tmp/nocap_$1$TAG LOG=/tmp/nocap_$1$TAG.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export D2_NOCAP="$KNOB"; else unset D2_NOCAP; fi
  if [ -n "$KNOB" ] && [ "$FROM" != 0 ]; then export D2_NOCAP_FROM="$FROM"; else unset D2_NOCAP_FROM; fi
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=4000000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    D2_REALCLOCK=1 WX86_FRAMEPROF=1 D2_LOOPWATCH=1 \
    D2WRITE="$W" D2SCRIPT="$S" \
    env ${EXTRAENV:-} \
    timeout "$TMO" qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset D2_NOCAP D2_NOCAP_FROM
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s  -> $LOG"
  grep -E "^nocap:|^\s*\[frames:|^frames:|^rendu:|^\s*\[boucle\]" "$LOG" | sed 's/^/     /'
}

echo "== BANC LIMITEUR (qemu, horloge REELLE) — MAXFRAMES=$MAXFRAMES =="
[ -n "${ONLYB:-}" ] || { echo "-- jambe A : D2_NOCAP absent --"; run A ""; }
[ -n "${ONLYA:-}" ] || { echo "-- jambe B : D2_NOCAP=$NOCAP --";  run B "$NOCAP"; }
