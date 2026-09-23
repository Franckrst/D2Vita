#!/usr/bin/env bash
# tools/qemu_jitfail_repro.sh — reproduce the JIT-pool-exhaustion guest-thread
# death under qemu-arm, using AllocDynarecMap's own fault-injection knobs
# (D2_JITFAILAFTER/D2_JITCHUNK_KB — see custommem.c's header comment) instead
# of the real Vita pool ceiling (mman_vita.c, TARGET=vita-only, not linked
# into the qemu-arm build at all).
# Usage: [FAILAFTER=1] [CHUNKKB=64] [FRAMES=6000] [BIN=build-arm/rt_boot_arm] [SKIPBUILD=1] bash $0
# Output: $LOG (default /tmp/qemu_jitfail.log). Look for "[jit] ... MORT d'un
# fil invite" and read the arenefail=/metafail= fields Task 1 added.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

# Rebuild libwinx86.a and relink rt_boot_arm before every run: this script
# exists specifically to test dynarec/eviction code changes, and BIN defaults
# to a binary this script does not otherwise touch — a stale rt_boot_arm from
# before the latest edit would silently reproduce the OLD behavior. Bitten by
# this once already (2026-09-23, Task 5): a re-run looked like a clean no-op
# regression check only because nobody had rebuilt the consumer binary.
# Same recipe as tools/rt_boot_arm_check.sh, minus that script's own
# pathcache/title-screen self-checks (irrelevant here). SKIPBUILD=1 to skip
# when you've just rebuilt yourself and want a faster iteration loop.
if [ "${SKIPBUILD:-0}" != 1 ]; then
    bash "$ROOT/third_party/winx86/build.sh" >/dev/null \
        || { echo "FAIL: winx86 build.sh"; exit 1; }
    source "$ROOT/tools/rt_boot_srcs.sh"
    arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
      -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
      -DD2RT_CPU_BOX86 \
      -Wl,-Ttext-segment=0x76000000 \
      -I"$ROOT/src" -I"$ROOT/third_party/winx86/src" \
      -I"$ROOT/third_party/winx86/src/dynarec86" -I"$ROOT/third_party/winx86/src/dynarec86/shim" \
      -I"$ROOT/third_party/pklib" \
      -I"$ROOT/third_party/winx86/third_party/box86-dynarec/include" -I"$ROOT/third_party/winx86/third_party/box86-dynarec" \
      "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/third_party/pklib/explode.c" \
      "$ROOT/third_party/winx86/build-arm/libwinx86.a" -lpthread -lm \
      -o "$ROOT/build-arm/rt_boot_arm" 2>/tmp/rt_boot_arm_build.err \
        || { echo "FAIL: rt_boot_arm relink (see /tmp/rt_boot_arm_build.err)"; grep -E "error" /tmp/rt_boot_arm_build.err | head -10; exit 1; }
fi

TAG="${TAG:-jitfail}"; FRAMES="${FRAMES:-6000}"
LOG="${LOG:-/tmp/qemu_$TAG.log}"; W=/tmp/d2vita_write_$TAG; rm -rf "$W"; mkdir -p "$W"
env GAMEEXE=1 D2ARGS="game.exe -w" D2LAYOUT=compact D2ARENA=12900000 WLOG=1 \
    MAXSW=60000000 MAXFRAMES="$FRAMES" \
    D2_JITFAILAFTER="${FAILAFTER:-1}" D2_JITCHUNK_KB="${CHUNKKB:-64}" \
    D2WRITE="$W" timeout 600 qemu-arm -B 0x10000 "${BIN:-build-arm/rt_boot_arm}" "$HOME/d2-vita-refs/1.14d" > "$LOG" 2>&1
echo "QEMU EXIT=$?" >> "$LOG"
echo "--- injection lines ---"; grep -E "INJECTION|JIT eviction|MORT d'un fil" "$LOG"
echo "--- log at $LOG ---"
