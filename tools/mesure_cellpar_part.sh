#!/usr/bin/env bash
# tools/mesure_cellpar_part.sh — SWEEP of D2_CELLPARSHARE under qemu-arm.
#
# What this script measures, and what it does NOT measure.
#
#   IT MEASURES COUNTERS: the share actually received by workers (span bytes
#   and cells), the calling thread's wait at the join, and the worker's wait
#   BEFORE its first band, broken down into "prologue" (the calling thread is
#   still inside the call: pass 1, band table, pre-resolution) and "outside
#   the call". Everything is in POLL-LOOP TICKS.
#
#   IT DOES NOT MEASURE A GAIN. Under qemu, scheduling has nothing to do with
#   the Vita's (RUN-TO-BLOCK verdict against an eight-core host): ticks don't
#   convert into console microseconds. What this reads is a work BREAKDOWN,
#   not a speed.
#
# TWO BOUNDARY LEGS give the serial/draw ratio in a SINGLE unit (worker-loop
# ticks), which is the only honest way to add them together:
#   share=99: the worker draws almost everything -> wait-prologue ~= S
#   share=1:  the worker draws almost nothing     -> wait-prologue ~= S + D
# hence D ~= prologue(1) - prologue(99) and the ratio S/D.
#
#   SHARES="egale 1 10 30 50 70 90 99"   legs to run ("egale" = knob absent)
#   MAXFRAMES=4000   WORKERS=1   MASK=63
#
# LEGS RUN ONE AT A TIME: two parallel runs oversubscribe the host and worker
# sleep would skew every wait measurement.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
MASK="${MASK:-63}"
WORKERS="${WORKERS:-1}"
SHARES="${SHARES:-egale 1 10 30 50 70 90 99}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

echo "== BALAYAGE D2_CELLPARSHARE — ouvriers=$WORKERS D2_CELLOPT=$MASK MAXFRAMES=$MAXFRAMES =="
printf '%-8s %-20s %8s %8s %10s %10s %10s %8s\n' \
  part empreinte oct% cell% att-appel att-prol att-gap vols
for sh in $SHARES; do
  W=/tmp/cellpart_$sh; LOG=/tmp/cellpart_$sh.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  E=("D2_CELLOPT=$MASK" "D2_CELLPAR=$WORKERS")
  [ "$sh" != egale ] && E+=("D2_CELLPARSHARE=$sh")
  env GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    "${E[@]}" ${EXTRAENV:-} \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  H=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$LOG" | tail -1)
  O=$(grep -oP 'obtenue-octets=\K[0-9]+'   "$LOG" | tail -1)
  C=$(grep -oP 'obtenue-cellules=\K[0-9]+' "$LOG" | tail -1)
  A=$(grep -oP 'attente-appelant=\K[0-9]+' "$LOG" | tail -1)
  P=$(grep -oP 'attente-ouvrier-prologue=\K[0-9]+' "$LOG" | tail -1)
  G=$(grep -oP 'attente-ouvrier-hors-appel=\K[0-9]+' "$LOG" | tail -1)
  V=$(grep -oP 'cellpar: ouvriers=[0-9]+ appels=.*vols=\K[0-9]+' "$LOG" | tail -1)
  printf '%-8s %-20s %8s %8s %10s %10s %10s %8s\n' \
    "$sh" "${H:-ABSENTE}" "${O:--}" "${C:--}" "${A:--}" "${P:--}" "${G:--}" "${V:--}"
done
