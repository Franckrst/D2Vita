#!/usr/bin/env bash
# tools/build_oracle_arm.sh — builds build-arm/oracle_arm, the binary that
# every pixel oracle (oracle_cellloop.sh, oracle_lazyseek.sh, oracle_pinemu.sh,
# oracle_csintrin.sh) requires. The compile line mirrors rt_boot_arm_check.sh,
# of which this script is the sibling — keep the two in sync.
#
#   EXTRA_DEFS="-DD2_TLSCOUNT"   MEASUREMENT build (emutls census). NEVER use
#                                for a speed A/B: it adds an increment per
#                                call to E().
#   OUTBIN=<path>                alternate output name (default build-arm/oracle_arm)
#   LIBTAG=_x EXTRA="-Dxxx"      libwinx86 FLAVOR (separate objects + archive,
#                                third_party/winx86/build.sh). Required for a
#                                measurement build like -DD2_EMITPROF: without
#                                the tag, the instrumented .o files overwrite
#                                the normal build's and the next "control"
#                                binary picks them up silently. EXTRA_DEFS must
#                                carry the SAME define for rt_boot (it reads
#                                the counters).
#
# The dynarec and the generic runtime layer (Cpu/Bridge/schedulers/PE32) live
# in the separate winx86 repo, consumed here as the third_party/winx86
# submodule — see third_party/winx86/README.md for the extension contract.
# Only what stays D2-specific (rt_boot.cpp, native_hooks_*, dcc_native,
# scomp_*, glide_ring, vita_net/vita_present/vita_gxm) is compiled directly
# here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WX86="$ROOT/third_party/winx86"
# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
OUTBIN="${OUTBIN:-$ROOT/build-arm/oracle_arm}"
EXTRA_DEFS="${EXTRA_DEFS:-}"
LIBTAG="${LIBTAG:-}" EXTRA="${EXTRA:-}" bash "$WX86/build.sh" >/dev/null
LIBA="$WX86/build-arm/libwinx86${LIBTAG:-}.a"
mkdir -p "$(dirname "$OUTBIN")"
# shellcheck disable=SC2086
arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
  -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DD2RT_CPU_BOX86 $EXTRA_DEFS \
  -Wl,-Ttext-segment=0x76000000 \
  -I"$ROOT/src" -I"$WX86/src" \
  -I"$WX86/src/dynarec86" -I"$WX86/src/dynarec86/shim" \
  -I"$ROOT/third_party/pklib" \
  -I"$WX86/third_party/box86-dynarec/include" -I"$WX86/third_party/box86-dynarec" \
  "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" "$ROOT/third_party/pklib/explode.c" \
  "$LIBA" -lpthread -lm \
  -o "$OUTBIN"
echo "== $OUTBIN ${EXTRA_DEFS:+[$EXTRA_DEFS]} =="
