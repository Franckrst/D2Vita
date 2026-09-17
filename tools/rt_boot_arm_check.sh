#!/usr/bin/env bash
# tools/rt_boot_arm_check.sh — Phase A gate: the FULL orchestrator
# (tools/rt_boot.cpp + runtime + shims) cross-compiled for ARM, booting the
# real 1.14d MONOLITH Game.exe on the Box86-derived dynarec (CpuBox86) under
# qemu-arm, deterministically, to the title screen (~2 s of guest run).
#
# Determinism: D2_VIRTCLOCK=1 (virtual frame clock) + D2_FAKEWALL (frozen wall
# clock => stable RNG seed) + a dedicated fresh D2WRITE. Two consecutive runs
# must give the same verdict. Title screen only — small MAXFRAMES budget keeps
# the whole script (build included) well under 5 minutes.
#
# DEFAULT MODE = NATIVE: this port targets only the native scheduler, so this
# gate tests the SHIPPED configuration, not a bench configuration.
#
# Native is real-clock BY CONSTRUCTION and refuses D2_VIRTCLOCK (FATAL in
# make_scheduler): the native pass therefore has no virtual clock. It keeps
# D2_FAKEWALL (frozen wall clock => stable RNG seed). It is NOT deterministic
# (OS preemption; wakeups don't compare to coop switch counts) but the
# ASSERTIONS are the same: window created, >=5 title frames, CLEAN EXIT.
#
# D2SCHED=coop requests the DETERMINISTIC variant (virtual clock). Still
# useful because the tools/ oracles depend on it, but it's no longer the
# reference configuration — verdict labeled PASS(coop).
#
# Frame assertions use the run's own LOG ("[gdi] FRAME N DUMPED -> ...",
# fflush'd at dump time); the dumps themselves land in $WRITE (private per
# run), so concurrent host runs cannot race each other's files.
#
# Build: SAME cross-compile as tools/rt_gameplay_arm_check.sh (single source
# of truth for the ARM orchestrator build — keep the two in sync).
#
# Address-space note: the ARM binary is linked at 0x76000000 (-Ttext-segment)
# because the default 0x10000 base would collide with Game.exe's preferred
# base 0x400000 and the guest heap (identity memory: guest VA == host VA).
# qemu-arm runs with -B 0x10000 (guest_base shift): rt_boot maps the Windows
# TIB at guest linear 0, which the host kernel forbids at host address 0
# (mmap_min_addr); shifting the guest base makes guest page 0 mappable.
#
# Requires: g++-arm-linux-gnueabihf, qemu-arm, the genuine 1.14d refs in $DIR.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
WRITE=/tmp/d2vita_write_bootcheck
FRAMES="${MAXFRAMES:-12}"

[ -f "$DIR/Game.exe" ] || { echo "FAIL: refs not found in $DIR"; exit 1; }

# Default = native (shipped config). coop = the oracles' deterministic variant.
# An unknown D2SCHED is rejected HERE rather than letting rt_boot FATAL later:
# this gate's message names the mode; rt_boot's own message would arrive after
# the build and cost several minutes.
MODE="${D2SCHED:-native}"
case "$MODE" in
  native) MODE_TAG="(natif)"; CLOCK_ENV="" ;;
  coop)   MODE_TAG="(coop)";  CLOCK_ENV="D2_VIRTCLOCK=1" ;;
  *)      echo "FAIL: D2SCHED=$MODE inconnu (native|coop)"; exit 1 ;;
esac

# 0. dynarec library + ARM orchestrator (same build as rt_gameplay_arm_check.sh)
# The dynarec and the generic runtime layer live in the third_party/winx86
# submodule — see third_party/winx86/README.md.
WX86="$ROOT/third_party/winx86"
# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
bash "$WX86/build.sh" >/dev/null
arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
  -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DD2RT_CPU_BOX86 \
  -Wl,-Ttext-segment=0x76000000 \
  -I"$ROOT/src" -I"$WX86/src" \
  -I"$WX86/src/dynarec86" -I"$WX86/src/dynarec86/shim" \
  -I"$ROOT/third_party/pklib" \
  -I"$WX86/third_party/box86-dynarec/include" -I"$WX86/third_party/box86-dynarec" \
  "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/third_party/pklib/explode.c" \
  "$WX86/build-arm/libwinx86.a" -lpthread -lm \
  -o "$ROOT/build-arm/rt_boot_arm" 2>/tmp/rt_boot_arm_build.err || { echo "FAIL: compilation ARM (voir /tmp/rt_boot_arm_build.err)"; grep -E "error" /tmp/rt_boot_arm_build.err | head -10; exit 1; }

# 0.5 directory-index self-test (D2_PATHCACHETEST), run TWICE: index active,
# then D2_PATHCACHE=0. It doesn't boot the game (returns before that), runs in
# ~1 s, and covers what the boot can't show: the Realms.bin/.d2s scenario
# (create then re-read), the noisy fallback on a deliberately stale index, the
# write-before-data precedence under the safety net, and an unreadable folder.
# Both passes must return the SAME resolutions — proof that the fallback
# switch changes nothing observable. That equality is checked, not just
# claimed: the "obtenu=" lines from both logs are DIFFED below. (The counters
# themselves differ by construction and are reported as such in the self-test.)
for PC in 1 0; do
  PCW="/tmp/d2vita_write_pathcache_$PC"; rm -rf "$PCW"; mkdir -p "$PCW"
  if env D2_PATHCACHETEST=1 D2_PATHCACHE="$PC" D2WRITE="$PCW" \
       timeout 120 qemu-arm -B 0x10000 "$ROOT/build-arm/rt_boot_arm" "$DIR" \
       > "/tmp/rt_pathcache_$PC.log" 2>&1; then
    grep -E "^=== \[PATHCACHETEST\]" "/tmp/rt_pathcache_$PC.log" | tail -1
  else
    echo "FAIL: auto-test index de repertoire (D2_PATHCACHE=$PC) — voir /tmp/rt_pathcache_$PC.log"
    grep -E "ECHEC" "/tmp/rt_pathcache_$PC.log" | head -5; exit 1
  fi
done
# each pass has its OWN write folder (otherwise the second would inherit the
# first's arena): this prefix is neutralized below since it isn't the point.
pcres(){ grep '^    obtenu=' "/tmp/rt_pathcache_$1.log" | sed "s#/tmp/d2vita_write_pathcache_$1/#<W>/#g"; }
if ! diff <(pcres 1) <(pcres 0) >/dev/null; then
  echo "FAIL: index ACTIF et D2_PATHCACHE=0 ne rendent pas les memes resolutions"
  diff <(pcres 1) <(pcres 0) | head -10; exit 1
fi
echo "  resolutions identiques index ACTIF / D2_PATHCACHE=0 ($(pcres 1 | wc -l) comparees)"

# 1. dedicated fresh write root (crash.log + frame dumps live here)
rm -rf "$WRITE"; mkdir -p "$WRITE"

echo "== qemu-arm (CpuBox86): 1.14d monolith boot to title screen ($FRAMES frames)${MODE_TAG:+ $MODE_TAG} =="
T0=$(date +%s.%N)
env GAMEEXE=1 D2ARGS="game.exe -w" $CLOCK_ENV D2_FAKEWALL=1756000000 \
  MAXFRAMES="$FRAMES" FBDUMP=1 D2WRITE="$WRITE" \
  timeout 240 qemu-arm -B 0x10000 "$ROOT/build-arm/rt_boot_arm" "$DIR" \
  > /tmp/rt_boot_arm.log 2>&1 || true
T1=$(date +%s.%N)
ARM_S=$(echo "$T1 $T0" | awk '{printf "%.1f", $1-$2}')

OUT="$(cat /tmp/rt_boot_arm.log)"
fail=0
# ⚠️ `echo "$OUT" | grep -q ...` IS A TRAP under `set -o pipefail` (line 1):
# `grep -q` exits on the FIRST match and closes the pipe; if `echo` hasn't
# finished writing, it takes a SIGPIPE and returns 141; pipefail then fails
# the ENTIRE pipeline, and the `||` branch reports "FAIL: monolith mode not
# entered" on a log that actually contains the line. This is a race — it
# depends on machine load and where stdout goes, i.e. a bench that lies
# intermittently. Grep the FILE instead: no pipe, no race. (The `grep -c` /
# `grep -o | tail | sed` below all read their entire input and never close
# early: those are safe.)
grep -q "=== 1.14 monolith: single Game.exe ===" /tmp/rt_boot_arm.log \
  || { echo "FAIL: monolith mode not entered (see /tmp/rt_boot_arm.log)"; fail=1; }
grep -q "\[win\] CreateWindowExA .* hwnd=0x" /tmp/rt_boot_arm.log \
  || { echo "FAIL: window not created"; fail=1; }
NFRAMES=$(grep -c "FRAME [0-9]* DUMPED" /tmp/rt_boot_arm.log || true)
[ "$NFRAMES" -ge 5 ] || { echo "FAIL: only $NFRAMES frames dumped (want >=5)"; fail=1; }
# the title screen is the 800x600x8 DIB — assert the LAST dumped frame's format
FRAME_SPEC=$(grep -o "frame_[0-9]*_[0-9x]*\.raw" /tmp/rt_boot_arm.log | tail -1 | sed 's/.*frame_[0-9]*_//;s/\.raw//' || true)
[ "$FRAME_SPEC" = "800x600x8" ] || { echo "FAIL: last frame format '$FRAME_SPEC' (want 800x600x8)"; fail=1; }
grep -q "CLEAN EXIT" "$WRITE/crash.log" 2>/dev/null \
  || { echo "FAIL: no CLEAN EXIT in $WRITE/crash.log"; fail=1; }
if [ "$MODE" = native ]; then
  grep -q "sched: backend=NATIVE" /tmp/rt_boot_arm.log \
    || { echo "FAIL: mode natif attendu mais stampe backend=NATIVE absente"; fail=1; }
fi

echo "$OUT" | grep -E "1.14 monolith|CreateWindowExA|FRAME [0-9]+ DUMPED|scheduler stopped" | head -12 || true
echo "   crash.log: $(tail -1 "$WRITE/crash.log" 2>/dev/null || echo '(missing)')"
echo "== wall clock: ARM/qemu(Box86 dynarec)=${ARM_S}s to $NFRAMES dumped frames =="
if [ "$MODE" = native ]; then VNOTE="ordonnanceur natif, horloge reelle — run non deterministe, memes assertions"; else VNOTE="env deterministe (oracles)"; fi
# Native is real-clock and non-deterministic: it does not publish a
# switch-count counter, and its numbers don't compare to anything. Said HERE
# so nobody compares a native run to an old coop reading and concludes a
# regression that doesn't exist.
if [ "$MODE_TAG" != "(coop)" ]; then
  echo "NOTE: run NON DETERMINISTE (ordonnanceur natif, horloge reelle) — pas de compteur de bascules."
  echo "      Pour un releve COMPARABLE d'un run a l'autre : D2SCHED=coop $0"
fi
[ $fail -eq 0 ] && echo "PASS${MODE_TAG} : 1.14d monolith boots to the title screen on ARM/dynarec ($VNOTE)" \
                || { echo "FAIL${MODE_TAG}"; exit 1; }
