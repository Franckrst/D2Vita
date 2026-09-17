#!/usr/bin/env bash
# tools/banc_phases.sh — qemu bench, real clock, for the per-phase breakdown
# (D2_PHASEPROF) and the D2_ONEDRAW knob. Sibling of banc_nocap.sh: same
# game-entry scenario, D2_REALCLOCK=1, D2_NOCAP_FROM=3200 (legs are identical
# up to frame 3200, then the cap lifts).
# Five legs, run IN PARALLEL (qemu time is relative; only legs launched under
# the same conditions are comparable):
#   NOPP : NOCAP=1                          (control, no instrumentation)
#   PP   : NOCAP=1 + D2_PHASEPROF=1         (sim/draw/flush/other breakdown)
#   OD1  : NOCAP=1 + PHASEPROF + D2_ONEDRAW=1  (one draw per step, short-circuit)
#   OD2  : NOCAP=1 + PHASEPROF + D2_ONEDRAW=2  (same + D2_ONEDRAW_US sleep)
#   CAP  : PHASEPROF alone, no NOCAP        (the game's own lock: draws/tick must be 1)
# MAXFRAMES=... TAG=... bash tools/banc_phases.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-9000}"
FROM="${FROM:-3200}"
TAG="${TAG:-}"
TMO="${TMO:-3600}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run(){ # $1 label, $2 MAXFRAMES, $3... extra env
  local LAB="$1$TAG" MF="$2"; shift 2
  local W=/tmp/phases_$LAB LOG=/tmp/phases_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=4000000 MAXFRAMES="$MF" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    D2_REALCLOCK=1 WX86_FRAMEPROF=1 D2_LOOPWATCH=1 \
    D2WRITE="$W" D2SCRIPT="$S" \
    env "$@" \
    timeout "$TMO" qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s  -> $LOG"
}
echo "== BANC PHASES (qemu, horloge REELLE) — MAXFRAMES=$MAXFRAMES FROM=$FROM =="
T0=$(date +%s)
run NOPP "$MAXFRAMES" D2_NOCAP=1 D2_NOCAP_FROM="$FROM" &
run PP   "$MAXFRAMES" D2_NOCAP=1 D2_NOCAP_FROM="$FROM" D2_PHASEPROF=1 &
run OD1  "$MAXFRAMES" D2_NOCAP=1 D2_NOCAP_FROM="$FROM" D2_PHASEPROF=1 D2_ONEDRAW=1 &
run OD2  "$MAXFRAMES" D2_NOCAP=1 D2_NOCAP_FROM="$FROM" D2_PHASEPROF=1 D2_ONEDRAW=2 &
run CAP  "$((FROM+2500))" D2_PHASEPROF=1 &
wait
echo "  duree totale: $(( $(date +%s) - T0 ))s"
for L in NOPP PP OD1 OD2 CAP; do
  echo "---- $L"
  grep -E "^nocap:|^phaseprof:|^\[frames:|^frames:|^rendu:|^phase" "/tmp/phases_$L$TAG.log" | sed 's/^/     /'
done
