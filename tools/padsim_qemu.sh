#!/usr/bin/env bash
# tools/padsim_qemu.sh — DIAGNOSTIC: short presses at low fps, under qemu-arm.
# Builds the oracle WITH tools/padsim_qemu.cpp (a simulated Vita pad driver,
# see that file) into its own binary, reaches the Rogue Encampment with the
# rt_gameplay_arm_check.sh menu script, then presses Triangle-as-inventory on
# a wall-clock timeline and reports presses sent / injected / inventory
# toggles Game.exe actually made.
#
# Usage: tools/padsim_qemu.sh <old|new> <hold_ms> [nap_ms] [gap_ms] [count]
# Output: $PADSIM_WORK (default build-arm/padsim), one log per pass.
# Validation level: qemu-arm. A qemu frame rate is NOT a console frame rate;
# what transfers is the mechanism (ticks per frame vs press length).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:?old|new}"; HOLD="${2:?hold_ms}"; NAP="${3:-0}"; GAP="${4:-1500}"; COUNT="${5:-12}"
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"
W="${PADSIM_WORK:-$ROOT/build-arm/padsim}"; mkdir -p "$W"
BIN="$W/rt_boot_arm_padsim"
WX86="$ROOT/third_party/winx86"
# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
if [ ! -x "$BIN" ] || [ -n "${PADSIM_REBUILD:-}" ]; then
  bash "$WX86/build.sh" >/dev/null || { echo "FAIL: moteur"; exit 1; }
  arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
    -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -DD2RT_CPU_BOX86 \
    -Wl,-Ttext-segment=0x76000000 \
    -I"$ROOT/src" -I"$WX86/src" -I"$WX86/src/dynarec86" -I"$WX86/src/dynarec86/shim" \
    -I"$ROOT/third_party/pklib" \
    -I"$WX86/third_party/box86-dynarec/include" -I"$WX86/third_party/box86-dynarec" \
    "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/tools/padsim_qemu.cpp" \
    "$ROOT/third_party/pklib/explode.c" \
    "$WX86/build-arm/libwinx86.a" -lpthread -lm -o "$BIN" 2>"$W/build.err" \
    || { echo "FAIL: build (voir $W/build.err)"; grep error "$W/build.err" | head; exit 1; }
fi
SCRIPT="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
SCRIPT="$SCRIPT,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
SCRIPT="$SCRIPT,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
SCRIPT="$SCRIPT,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
TAG="${MODE}_h${HOLD}_n${NAP}"
WD="$W/w_$TAG"; rm -rf "$WD"; mkdir -p "$WD/Save"
LOG="$W/padsim_$TAG.log"
# The run ends by itself once the result line is out (MAXFRAMES is a guard).
GAMEEXE=1 D2ARGS="game.exe -w" MAXSW=400000 MAXFRAMES="${MAXFRAMES:-9000}" \
  D2_ROOMGUARD=1 D2WRITE="$WD" D2SCRIPT="$SCRIPT" \
  PADSIM="$HOLD:$GAP:$COUNT" PADSIM_MODE="$MODE" PADSIM_NAP_MS="$NAP" \
  timeout "${PADSIM_TIMEOUT:-3000}" qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1 &
PID=$!
while kill -0 $PID 2>/dev/null; do
  if grep -q "\[padsim\] RESULTAT" "$LOG" 2>/dev/null; then sleep 2; kill $PID 2>/dev/null; break; fi
  sleep 5
done
wait $PID 2>/dev/null
grep "\[padsim\]" "$LOG" | sed "s/^/[$TAG] /"
grep -q "\[padsim\] RESULTAT" "$LOG" || { echo "[$TAG] PAS DE RESULTAT (voir $LOG)"; exit 1; }
