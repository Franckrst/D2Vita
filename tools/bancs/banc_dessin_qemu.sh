#!/usr/bin/env bash
# tools/bancs/banc_dessin_qemu.sh — qemu bench, real clock, for the Glide draw
# traversal (console path: -3dfx + glide3x.dll ring + GXM sink).
#
# Sibling of tools/banc_phases.sh, which measures the GDI path (`-w`). Here
# drawing goes through the ring DLL, matching what the console does — this is
# the split we care about (35-39 ms of "draw" per frame in a dense scene).
# Nothing is presented under qemu: the atlas, batches and ring traversal still
# run in full, as does the D2Client walk.
#
#   TAG=x bash tools/bancs/banc_dessin_qemu.sh <LAB> [VAR=val ...]
#     LAB          leg label (log /tmp/dessin_<LAB>.log)
#     MAXFRAMES=   default 6200; FROM= frame where the cap lifts (default 3200)
#     REFS=        1.14d folder CONTAINING glide3x.dll (never the shared folder)
#     BIN=         ARM binary (default build-arm/oracle_arm)
#     OUT=         log directory (default /tmp)
#
# Durations are qemu's own: only the SLOPE between two legs run under the same
# conditions is meaningful (parallel bench).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAB="$1"; shift
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
REFS="${REFS:?REFS= (dossier 1.14d avec glide3x.dll) obligatoire}"
OUT="${OUT:-/tmp}"
MAXFRAMES="${MAXFRAMES:-6200}"
FROM="${FROM:-3200}"
TMO="${TMO:-5400}"
# DETERMINISTE=1: virtual clock + frozen wall clock (the tools/oracle_*.sh
# bench). The scene becomes IDENTICAL leg to leg — same light count, same
# cells — so `phase: dessin=` and `phase/x:` compare without the scene noise a
# real-clock run has. The stopwatch itself stays real (rt_now_us =
# CLOCK_MONOTONIC under qemu): this measures CODE SPEED at constant guest
# workload.
if [ "${DETERMINISTE:-0}" = 1 ]; then
  CLOCK=(D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 FBHASH=1)
else
  CLOCK=(D2_REALCLOCK=1 D2_NOCAP=1 D2_NOCAP_FROM="$FROM")
fi
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
[ -f "$REFS/glide3x.dll" ] || { echo "FAIL: $REFS/glide3x.dll absent (tools/build_glide_ring.sh)"; exit 1; }
W="$OUT/w_dessin_$LAB"; LOG="$OUT/dessin_$LAB.log"
rm -rf "$W"; mkdir -p "$W/Save"
cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt")
T0=$(date +%s)
GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=4000000 MAXFRAMES="$MAXFRAMES" \
  D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
  WX86_FRAMEPROF=1 D2_LOOPWATCH=1 D2_PHASEPROF=1 \
  D2_GLIDERING=1 D2_GLIDEGXM=1 \
  D2WRITE="$W" D2SCRIPT="$S" \
  env "${CLOCK[@]}" "$@" timeout "$TMO" qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1
echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s  -> $LOG"
