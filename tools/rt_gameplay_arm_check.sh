#!/usr/bin/env bash
# tools/rt_gameplay_arm_check.sh — ROGUE ENCAMPMENT on the ARM/dynarec path.
#
# Boot Game.exe (1.14d monolith), navigate the menu with the frame-scheduled
# input script (Single Player -> Barbarian -> name "VITA" -> Enter -> Enter),
# load into the game world.
#
# PASS = WORLD REACHED + CLEAN EXIT, asserted from the run's own log
# (shared /tmp frame files are unreliable under concurrent runs):
#   - monolith mode entered + window created + the Enter keydown injected;
#   - a fresh .d2s save appears in $WRITE (character created past the name
#     form — the exact step the false XFAIL claimed was blocked);
#   - "[automap] ... ENOENT (D2 regenerates)" — D2 opens the automap only at
#     act load; with a fresh save dir this line = world load underway;
#   - the LAST dumped frame is > ENTER_F+1000 — the game loop keeps rendering
#     long after the load (menu frames end <=~2600; 2800+ are in-world);
#   - "CLEAN EXIT" in $WRITE/crash.log.
# The final frame is converted to a PNG next to the run's own output ($WRITE).
#
# Clock caveat: this run is REAL-CLOCK (no D2_VIRTCLOCK — the menu needs real
# timings, and D2SCHED=native refuses a virtual clock by design). Frame
# numbers of the input script may need shifting if scheduling divergence makes
# the menu unready at click time: SHIFT_F env (default 0) shifts every
# scripted frame; document any non-zero value used.
#
# RETRY POLICY — classified by failure signature, never blind:
#   CRASH class (Fog Crash.txt, nonzero-code ExitProcess/TerminateProcess,
#   nonzero guest main-exit, no CLEAN EXIT, qemu timeout = hang/deadlock, or
#   frame-loop death AFTER a successful load) = hard FAIL, NEVER retried:
#   an intermittent crash here is the signal this gate exists to catch (the
#   ROOMGUARD finding came from investigating one; the sprite bet of spec D8
#   is itself an intermittent crash; a missed-wake deadlock (D4 class) shows
#   as frozen frames + timeout — retrying would mask and mislabel it).
#   TIMING class (Enter never injected / no .d2s / no automap, WITH a CLEAN
#   EXIT and zero crash evidence) = the frame-scheduled menu navigation
#   missed its window; a TRUE timing miss always ends cleanly (the failed
#   navigation idles to MAXFRAMES, main returns, CLEAN EXIT). Retried ONCE,
#   run 1 log kept at /tmp/rt_gameplay_arm.log.run1, verdict says "run 2/2".
#
# D2_ROOMGUARD=1 is set to MATCH THE SHIPPED VITA CONFIG (baked ON in
# src/platform/vita_present.cpp; env.txt D2_ROOMGUARD=0 disables it on
# console): the Halt-316 family is a LATENT D2 bug (room2 node migration
# leaves un-rebased nodes depending on async level-stream completion ORDER).
# The deterministic coop schedule happens to stream in a benign order, which
# masks the divergence; any real-clock schedule (D2SCHED=native, hardware)
# can land the bad order and Halt at act load. Running the gate guard-OFF
# tests a config that no target ships.
#
# Uses a dedicated D2WRITE so concurrent host runs don't share save state;
# it is recreated fresh before the run (character creation must start fresh —
# the automap ENOENT marker depends on it). Note: the monolith's Save\ paths
# map flat into $WRITE (VITA.d2s lands at $WRITE/VITA.d2s).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
WRITE=/tmp/d2vita_write_arm
SHIFT_F="${SHIFT_F:-0}"
# Frame budget: the verdict is decided by ~ENTER_F+1200 (world entry happens
# right after the Enters, and PASS only needs frames past Enter+1000).
MAXFRAMES="${MAXFRAMES:-6000}"
# Mode tag: D2SCHED=native runs are tagged in the verdict (real OS preemption,
# non-deterministic interleavings — wall-clock and switch counts not
# comparable to coop).
MODE_TAG=""
[ "${D2SCHED:-}" = "native" ] && MODE_TAG="(native)"

[ -f "$DIR/Game.exe" ] || { echo "FAIL: refs not found in $DIR"; exit 1; }

# 0. dynarec library + ARM orchestrator.
# Delegates to tools/build_oracle_arm.sh: a SINGLE compile line for the whole
# qemu bench, instead of a local copy that could drift out of sync with it.
OUTBIN="$ROOT/build-arm/rt_boot_arm" bash "$ROOT/tools/build_oracle_arm.sh"

# fresh save state per attempt (also guarantees the automap ENOENT world
# marker) happens inside do_run.

# input script (frames shiftable by SHIFT_F for scheduling divergence)
S() { echo $(( $1 + SHIFT_F )); }
SCRIPT="$(S 300):activate,$(S 400):move:400:308,$(S 450):ldown:400:308,$(S 460):lup:400:308"
SCRIPT="$SCRIPT,$(S 1500):move:400:300,$(S 1550):ldown:400:300,$(S 1560):lup:400:300"
SCRIPT="$SCRIPT,$(S 2400):chr:86,$(S 2410):chr:73,$(S 2420):chr:84,$(S 2430):chr:65"
SCRIPT="$SCRIPT,$(S 2600):keydown:13,$(S 2620):keyup:13"
# first Enter validates the name form (character is created and saved; the
# 1.14d monolith then loads straight into the world); the second Enter is kept
# as a char-select fallback and is harmless in-world
SCRIPT="$SCRIPT,$(S 2800):keydown:13,$(S 2820):keyup:13"
ENTER_F=$(S 2820)

# one full run + assertions; sets RUN_CLASS=pass|timing|crash (see header)
do_run() {
  rm -rf "$WRITE"; mkdir -p "$WRITE/Save"
  echo "== qemu-arm (CpuBox86): Game.exe 1.14d -> menu -> char create -> game world${MODE_TAG:+ $MODE_TAG} =="
  local T0 T1 RC=0
  T0=$(date +%s.%N)
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" FBDUMP=200 \
    D2_ROOMGUARD=1 D2WRITE="$WRITE" D2SCRIPT="$SCRIPT" \
    timeout 2700 qemu-arm -B 0x10000 "$ROOT/build-arm/rt_boot_arm" "$DIR" \
    > /tmp/rt_gameplay_arm.log 2>&1 || RC=$?
  T1=$(date +%s.%N)
  ARM_S=$(echo "$T1 $T0" | awk '{printf "%.0f", $1-$2}')

  OUT="$(cat /tmp/rt_gameplay_arm.log)"
  echo "$OUT" | grep -E "1.14 monolith|sched: backend|CreateWindowExA|automap|scheduler stopped" | head -6 || true
  LAST_FRAME_LINE="$(echo "$OUT" | grep -o "FRAME [0-9]* DUMPED -> [^ ]*" | tail -1 || true)"
  LAST_N="$(echo "$LAST_FRAME_LINE" | sed 's/FRAME \([0-9]*\).*/\1/')"
  LAST_FILE="$(echo "$LAST_FRAME_LINE" | sed 's/.*-> //')"
  echo "last dump: frame ${LAST_N:-none} after ${ARM_S}s"

  local fail=0 nav=0 crash=0 d2s_ok=1 automap_ok=1 frames_ok=1 clean_ok=1
  echo "$OUT" | grep -q "=== 1.14 monolith: single Game.exe ===" \
    || { echo "FAIL: monolith mode not entered (1.13c-era refs? see /tmp/rt_gameplay_arm.log)"; fail=1; }
  echo "$OUT" | grep -q "\[win\] CreateWindowExA .* hwnd=0x" \
    || { echo "FAIL: window not created"; fail=1; }
  echo "$OUT" | grep -q "\[inj\] frame $(S 2600): keydown 13" \
    || { echo "FAIL: Enter keydown never injected (run too short? MAXFRAMES=$MAXFRAMES)"; fail=1; nav=1; }
  [ -n "$LAST_N" ] || { echo "FAIL: no frames dumped"; fail=1; }
  # --- world-entry markers (see header) ---
  D2S="$(find "$WRITE" -name '*.d2s' 2>/dev/null | head -1)"
  [ -n "$D2S" ] || { echo "FAIL: no .d2s save created (name entry/char creation blocked)"; fail=1; nav=1; d2s_ok=0; }
  echo "$OUT" | grep -q "\[automap\] .*ENOENT (D2 regenerates)" \
    || { echo "FAIL: no automap open (act load never started)"; fail=1; nav=1; automap_ok=0; }
  [ "${LAST_N:-0}" -gt $(( ENTER_F + 1000 )) ] \
    || { echo "FAIL: last frame ${LAST_N:-0} not past Enter+1000 ($((ENTER_F+1000))) — game loop died during/after load"; fail=1; frames_ok=0; }
  grep -q "CLEAN EXIT" "$WRITE/crash.log" 2>/dev/null \
    || { echo "FAIL: no CLEAN EXIT in $WRITE/crash.log ($(tail -1 "$WRITE/crash.log" 2>/dev/null || echo missing))"; fail=1; clean_ok=0; }
  # --- crash evidence (CLEAN EXIT alone does not clear a guest crash: the
  #     Halt-316 run had CLEAN EXIT *and* ExitProcess(-1)). Two layers:
  #     1. the stdout line "[ExitProcess code=%u chain: ...]" (also
  #        TerminateProcess; rt_boot.cpp exit_chain, %u so -1 prints
  #        4294967295) — fires whichever THREAD kills the process;
  #     2. crash.log "main-exit=0x..." (game path writes it too,
  #        rt_boot.cpp:6460 coop / :6471 native) — kept because it covers a
  #        main that dies without an Exit/TerminateProcess call, while layer
  #        1 covers a WORKER TerminateProcess that leaves main-exit=0. ---
  local pexit; pexit="$(echo "$OUT" | grep -oE "\[(ExitProcess|TerminateProcess) code=[0-9]+" | grep -v " code=0$" | tail -1 || true)"
  [ -n "$pexit" ] && { echo "FAIL: guest killed the process (${pexit#[} ...)"; fail=1; crash=1; } || true
  local mexit; mexit="$(grep -o "main-exit=0x[0-9a-f]*" "$WRITE/crash.log" 2>/dev/null | tail -1 || true)"
  if [ -n "$mexit" ] && [ "$mexit" != "main-exit=0x00000000" ]; then
      echo "FAIL: guest main exited nonzero ($mexit)"; fail=1; crash=1; fi
  [ -f "$WRITE/Crash.txt" ] && { echo "FAIL: Fog error handler ran ($WRITE/Crash.txt)"; fail=1; crash=1; } || true
  [ $RC -eq 124 ] && { echo "FAIL: qemu timeout — hang/deadlock, class CRASH (a true timing miss ends cleanly)"; fail=1; crash=1; } || true
  [ $d2s_ok -eq 1 ] && [ $automap_ok -eq 1 ] && [ $frames_ok -eq 0 ] && crash=1 || true   # died AFTER a successful load
  [ -n "$D2S" ] && echo "   save file: $D2S" || true

  # milestone PNG from the run's own last frame file (may lose a shared-/tmp race)
  if [ $fail -eq 0 ] && [ -f "$LAST_FILE" ]; then
      python3 "$ROOT/tools/frame2png.py" "$LAST_FILE" "$WRITE/rogue_encampment_arm.png" \
        && echo "   milestone PNG: $WRITE/rogue_encampment_arm.png"
  fi
  [ "$SHIFT_F" != "0" ] && echo "   (SHIFT_F=$SHIFT_F was applied to the input script)" || true
  echo "== wall clock: ARM/qemu(Box86 dynarec)=${ARM_S}s to frame ${LAST_N:-0} =="

  # --- classify (see header: CRASH is the signal, only TIMING may retry) ---
  if [ $fail -eq 0 ]; then RUN_CLASS=pass
  elif [ $crash -eq 1 ]; then RUN_CLASS=crash
  elif [ $nav -eq 1 ] && [ $clean_ok -eq 1 ]; then RUN_CLASS=timing
  else RUN_CLASS=crash; fi
}

RETRIED=0
do_run
if [ "$RUN_CLASS" = "timing" ]; then
    RETRIED=1
    cp /tmp/rt_gameplay_arm.log /tmp/rt_gameplay_arm.log.run1
    echo "== run 1 = TIMING miss (navigation missed its frame schedule; no crash evidence) — retrying once; run 1 log kept: /tmp/rt_gameplay_arm.log.run1 =="
    do_run
fi
case "$RUN_CLASS" in
  pass)
    if [ $RETRIED -eq 1 ]; then
        echo "PASS${MODE_TAG} (run 2/2 — run 1 timing miss, log kept) : ROGUE ENCAMPMENT reached on the ARM/dynarec path"
    else
        echo "PASS${MODE_TAG} : ROGUE ENCAMPMENT reached on the ARM/dynarec path"
    fi ;;
  crash)
    echo "REGRESSION (class CRASH — the signal, never retried: investigate /tmp/rt_gameplay_arm.log)"; exit 1 ;;
  *)
    echo "REGRESSION (class TIMING twice — input schedule needs SHIFT_F, see header)"; exit 1 ;;
esac
