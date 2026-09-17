#!/usr/bin/env bash
# tools/bancs/banc_memintrin_qemu.sh — A/B/A of the D2_MEMINTRIN knob under
# qemu, on the DETERMINISTIC bench (D2_VIRTCLOCK + D2_FAKEWALL: same scene,
# same scheduling, same image fingerprint leg to leg — the stopwatch itself
# stays real). Legs run ONE AT A TIME: parallel legs would contend for the
# host CPU and the measured time would mean nothing.
#
# What gets read:
#   [blocs] executes=   GUEST work (translated blocks executed over the whole
#                       run). The honest portability metric: it drops if and
#                       only if translated code disappeared. Says nothing
#                       about TIME.
#   duree               host time for the full run, same scene: a proxy for
#                       TOTAL host work (translated + native). Under qemu the
#                       emulation models neither cache nor Cortex-A9 dual
#                       issue: the SIGN carries over, not the magnitude. The
#                       console has the final verdict.
#   phase: dessin=      host microseconds of the draw traversal (D2_PHASEPROF).
# Legs: A (control) B (D2_MEMINTRIN=1) A2 (control again) — bracketing is
# mandatory, the machine drifts.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
OUT="${OUT:-/tmp}"
REPEAT="${REPEAT:-1}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
if [ "${PATROUILLE:-0}" = 1 ]; then S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt"); else
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
fi
run() {  # $1 = label, $2... = env
  local LAB="$1"; shift
  local W="$OUT/bmi_$LAB" LOG="$OUT/bmi_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s.%N)
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2_PHASEPROF=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local T1=$(date +%s.%N)
  printf "  %-4s duree=%ss  blocs=%-12s empreinte=%s  %s\n" "$LAB" \
    "$(echo "$T1-$T0"|bc)" \
    "$(grep -oP '\[blocs\] executes=\K[0-9]+' "$LOG"|tail -1)" \
    "$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$LOG"|tail -1)" \
    "$(grep -oP '^phase: .*' "$LOG"|tail -1)"
}
echo "== BANC MEMINTRIN (deterministe, sequentiel) MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} =="
# A = control, P = D2_MEMINTRIN=2 (prologue emitted + native call WITHOUT
#     serving: the PURE cost of instrumentation), B = D2_MEMINTRIN=1 (cost +
#     benefit), M = D2_MEMINTRIN=1 D2_MEMINTRIN_MAX=100: only serves n <= 256
#     bytes, i.e. EXACTLY what the guest does NOT vectorize (beyond that it
#     falls back to its SSE2 branches, which box86 translates to NEON),
# C = control again (bracketing is mandatory, the machine drifts).
for i in $(seq 1 "$REPEAT"); do
  run "A$i"
  run "P$i" D2_MEMINTRIN=2
  run "B$i" D2_MEMINTRIN=1
  run "M$i" D2_MEMINTRIN=1 D2_MEMINTRIN_MAX=100
  run "C$i"
done
grep -h "\[memintrin\] memcpy:" "$OUT/bmi_B1.log" | sed 's/^/  /'
