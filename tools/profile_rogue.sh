#!/usr/bin/env bash
# tools/profile_rogue.sh — the Rogue Encampment profiling run. Builds the
# PROF_COUNTERS variant into its own binary (never overwrites the validated
# rt_boot_arm), then drives the
# 6-phase deterministic scenario under the Vita memory model (compact+arena):
#   P0 boot | P1 menu | P2 create/nav | P3 level load | P4 idle | P5 movement
# Counters are exact under qemu (deterministic); wall-clock ratios are
# indicative only — confirm timing on Vita3K/hardware (3-level rule).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.13c}"
OUT="$ROOT/build-arm/rt_boot_arm_prof"
LOG="${LOG:-/tmp/profile_rogue.log}"

# The dynarec and the generic runtime layer live in the third_party/winx86
# submodule — see third_party/winx86/README.md. PROF_COUNTERS touches code
# that now lives INSIDE libwinx86 (prof.cpp, sched_*.cpp, bridge.cpp), hence
# the dedicated LIBTAG=_prof flavor, same technique as build_rt_boot_vpk.sh
# (D2VPK_PROF=1).
WX86="$ROOT/third_party/winx86"
# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
LIBTAG=_prof EXTRA="-DPROF_COUNTERS" bash "$WX86/build.sh" >/dev/null

echo "== build PROF_COUNTERS variant =="
arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
  -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DD2RT_CPU_BOX86 -DPROF_COUNTERS \
  -Wl,-Ttext-segment=0x76000000 \
  -I"$ROOT/src" -I"$WX86/src" -I"$WX86/src/dynarec86" -I"$WX86/src/dynarec86/shim" \
  -I"$WX86/third_party/box86-dynarec/include" -I"$WX86/third_party/box86-dynarec" \
  -I"$ROOT/third_party/pklib" \
  "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/third_party/pklib/explode.c" \
  "$WX86/build-arm/libwinx86_prof.a" -lpthread -lm \
  -o "$OUT" 2>&1 | grep -i "error:" && exit 1 || true
[ -x "$OUT" ] || { echo "FAIL: build"; exit 1; }

WRITE=/tmp/prof_rogue_save; rm -rf "$WRITE"; mkdir -p "$WRITE/Save"

# 6-phase input script. P0-P3 = the validated gameplay path; P4 = idle in the
# world; P5 = movement clicks (640x480 world coords) alternating with idle.
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
# P5 movement: click-to-move around the camp
S="$S,4600:move:500:300,4650:ldown:500:300,4660:lup:500:300"
S="$S,5000:move:150:250,5050:ldown:150:250,5060:lup:150:250"
S="$S,5400:move:320:400,5450:ldown:320:400,5460:lup:320:400"

echo "== profiling run (compact+arena, 6000 frames) -> $LOG =="
env GAMEEXE=1 D2ARGS="game.exe -w" D2LAYOUT=compact D2ARENA=12900000 \
    WLOG=1 MAXSW=800000 MAXFRAMES=6000 FBDUMP=2000 \
    PROF_PHASES="1500,2820,3200,4500,6000" \
    D2WRITE="$WRITE" D2SCRIPT="$S" \
    timeout 1200 qemu-arm -B 0x10000 "$OUT" "$DIR" > "$LOG" 2>&1 || true

echo "== result =="
grep -c "=== PROFILE" "$LOG" | xargs echo "profile dumps:"
grep -o "FRAME [0-9]* DUMPED" "$LOG" | tail -1
grep -o "640x480x8" "$LOG" | tail -1
grep -E "scheduler stopped" "$LOG" | tail -1
echo "full log: $LOG"
