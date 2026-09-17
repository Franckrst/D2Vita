#!/usr/bin/env bash
# tools/crashtest_arm_check.sh — qemu-arm verification of D2_CRASHTEST=
# native|halt (runtime side src/crashreport/cr_crashtest.h +
# tools/rt_boot.cpp's d2crashtest_tick, compiled only with -DD2V_CRASHTEST).
#
# A third leg, hang (parking the native-scheduler thread forever to trip
# automatic hang detection), has no counterpart any more: automatic hang
# detection itself was removed (src/crashreport/cr_evidence.cpp) once
# real-hardware testing showed the general watchdog already covers a genuine
# freeze. This script no longer covers it at all.
#
# Same qemu-arm recipe as tools/rt_boot_arm_check.sh (title screen boot, same
# D2ARGS/env), plus D2V_CRASHTEST=1 and, per kind, D2_CRASHTEST=<kind>. Builds
# to a SEPARATE binary (rt_boot_arm_crashtest) so the normal gate's own binary
# is never touched.
#
# What this can and cannot prove under qemu-arm (no sceIo/sceNet/sceKernel,
# user-mode ARM emulation only — read this before trusting a PASS blindly):
#   native  - PROVES a real ARM fault happens (qemu reports the real SIGSEGV,
#             the process dies with signal 11). Does NOT and CANNOT prove
#             Kind::HostFault: that needs a real psp2core dump, which only a
#             real Vita OS ever produces; already covered on synthetic and
#             (when available) real dumps by run_crashreport_tests.sh leg 3.
#             Under qemu specifically the fault is caught by cpu_box86.cpp's
#             diag_segv (#ifndef __vita__ only) and the run classifies as
#             Kind::None (NOEVIDENCE) — verified below, reported honestly,
#             not papered over.
#   halt    - PROVES the whole pipeline end to end: the game's OWN real Halt
#             reporter thunk (Game.exe VA 0x408a60) runs, writes a real
#             Crash.txt, and a real d2cr::build_evidence() call classifies it
#             Kind::Halt with the exact synthetic code. This is the one kind
#             fully, genuinely verifiable under qemu (Crash.txt goes through
#             cross-platform KERNEL32 file shims, not the Vita-only progress
#             log). ALSO proves the process actually terminates afterward
#             (exit=3, kD2CrashTestHaltExitCode in tools/rt_boot.cpp) — same
#             standard as native's exit=139: a real exit code, not a log
#             line. Unlike native, this is not a crash at the OS level
#             (no signal, no fault) — it is this codebase's own deliberate
#             _exit() once the real reporter above has genuinely finished,
#             because the organic Halt-thunk's own tail-jmp chain does not
#             reliably stop the process on its own (see d2crashtest_tick's
#             comment in tools/rt_boot.cpp for the disassembly that traced
#             why, and why that path was not reused directly).
#
# Usage: bash tools/crashtest_arm_check.sh [DIR-with-1.14d-refs]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
[ -f "$DIR/Game.exe" ] || { echo "FAIL: refs introuvables dans $DIR"; exit 1; }

WX86="$ROOT/third_party/winx86"
# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
mkdir -p "$ROOT/build-arm"
BIN="$ROOT/build-arm/rt_boot_arm_crashtest"

echo "== 0. build (D2V_CRASHTEST=1, ARM/qemu, same recipe as rt_boot_arm_check.sh) =="
bash "$WX86/build.sh" >/dev/null
arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
  -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DD2RT_CPU_BOX86 -DD2V_CRASHTEST \
  -Wl,-Ttext-segment=0x76000000 \
  -I"$ROOT/src" -I"$WX86/src" \
  -I"$WX86/src/dynarec86" -I"$WX86/src/dynarec86/shim" \
  -I"$ROOT/third_party/pklib" \
  -I"$WX86/third_party/box86-dynarec/include" -I"$WX86/third_party/box86-dynarec" \
  "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/third_party/pklib/explode.c" \
  "$WX86/build-arm/libwinx86.a" -lpthread -lm \
  -o "$BIN" 2>/tmp/rt_boot_arm_crashtest_build.err \
  || { echo "FAIL: compilation ARM (voir /tmp/rt_boot_arm_crashtest_build.err)"; /usr/bin/grep -E error /tmp/rt_boot_arm_crashtest_build.err | head -10; exit 1; }
echo "  OK: $BIN"

echo "== 0.5 host evidence-collector tool (crashreport_boot_e2e, same library the console links) =="
source "$ROOT/tools/crashreport_srcs.sh"
E2E_DIR="$(mktemp -d)"
cc -O1 -c "${CRASHREPORT_C_SRCS[0]}" -o "$E2E_DIR/mono.o"
cc -O1 -I "$ROOT/third_party/monocypher" -c "${CRASHREPORT_C_SRCS[1]}" -o "$E2E_DIR/mono_ed.o"
g++ -std=c++17 -O1 -I "$ROOT/src" -I "$ROOT/third_party/monocypher" \
  "${CRASHREPORT_SRCS[@]}" "${CRASHREPORT_HOST_SRCS[@]}" "$ROOT/tools/tests/crashreport_boot_e2e.cpp" \
  "$E2E_DIR/mono.o" "$E2E_DIR/mono_ed.o" -lz -o "$E2E_DIR/e2e"
E2E="$E2E_DIR/e2e"
echo "  OK: $E2E"

fail=0
run_kind() { # $1 = native|halt ; $2 = timeout seconds
  local kind="$1" tmo="$2" W="/tmp/d2vita_write_crashtest_$1"
  rm -rf "$W"; mkdir -p "$W"
  set +e
  env GAMEEXE=1 D2ARGS="game.exe -w" D2_FAKEWALL=1756000000 D2_CRASHTEST="$kind" D2_HALTWATCH=1 \
    MAXFRAMES=200 FBDUMP=1 D2WRITE="$W" \
    timeout "$tmo" qemu-arm -B 0x10000 "$BIN" "$DIR" >"/tmp/rt_crashtest_$kind.log" 2>&1
  local ex=$?
  set -e
  echo "$W|$ex"
}

echo "== 1. native: real ARM fault =="
res="$(run_kind native 60)"; W="${res%|*}"; ex="${res#*|}"
if [ "$ex" = 139 ] || /usr/bin/grep -q "uncaught target signal 11" "/tmp/rt_crashtest_native.log"; then
  echo "  OK: real SIGSEGV (exit=$ex, qemu reports signal 11) — genuine fault, not a log message"
else
  echo "  FAIL: no real fault observed (exit=$ex)"; fail=1
fi
"$E2E" init-session --root "$W" --session-id ct-native --started-unix 1700000000 >/dev/null
out="$("$E2E" collect --root "$W" --now 1900000000)"
echo "  evidence pass: $out"
echo "  (expected under qemu: NOEVIDENCE — Kind::HostFault needs a real Vita psp2core dump, hardware-only; see notes above)"

echo "== 2. halt: the game's REAL Halt reporter =="
res="$(run_kind halt 60)"; W="${res%|*}"; ex="${res#*|}"
if [ -f "$W/Crash.txt" ] && /usr/bin/grep -qE '^\[Halt\] \(.*\) failed at \(99999999\)' "$W/Crash.txt"; then
  echo "  OK: real Crash.txt, real synthetic code in the summary line"
else
  echo "  FAIL: no Crash.txt, or summary line missing the synthetic code"; fail=1
fi
# The process must actually stop — a triggered Halt behaving like a Halt, not
# a log line while the game keeps running (the hardware bug this fixes).
# exit=3 = kD2CrashTestHaltExitCode (tools/rt_boot.cpp) via d2cr_shutdown()+
# _exit(): a real, deliberate, non-zero exit code, same standard as native's
# exit=139 below (a real exit code, not just a log line) — deliberately a
# DIFFERENT number from native's, since this is not an OS-level fault.
if [ "$ex" = 3 ]; then
  echo "  OK: process terminated (exit=$ex) — Halt actually stopped the game, not just logged it"
else
  echo "  FAIL: process did not terminate as expected (exit=$ex, wanted 3)"; fail=1
fi
"$E2E" init-session --root "$W" --session-id ct-halt --started-unix 1700000000 >/dev/null
out="$("$E2E" collect --root "$W" --now 1900000000)"
echo "  evidence pass: $out"
case "$out" in
  *"kind=halt"*) echo "  OK: classified Kind::Halt" ;;
  *) echo "  FAIL: expected kind=halt"; fail=1 ;;
esac
/usr/bin/grep -q '"code":99999999' "$W/reports/outbox"/*/evidence.txt 2>/dev/null \
  && echo "  OK: features.code carries the synthetic value end to end" \
  || { echo "  FAIL: synthetic code missing from the collected claim"; fail=1; }

# A third leg used to run here (hang: park the native-scheduler thread
# forever, proving the park was real and that a synthetic boot_progress.txt
# in the real "alive:" shape classified Kind::Hang). D2_CRASHTEST=hang and
# automatic hang detection itself were removed (src/crashreport/cr_evidence.cpp)
# once real-hardware testing showed the general watchdog already covers a
# genuine freeze. Nothing replaces it: a hung session is no longer classified
# or reported by this system at all.

[ "$fail" = 0 ] && echo "PASS: crashtest_arm_check (native fault real, halt fully verified end to end — see notes above for what qemu cannot show)" \
                || { echo "FAIL: crashtest_arm_check"; exit 1; }
