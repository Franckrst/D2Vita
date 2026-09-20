#!/usr/bin/env bash
# tools/oracle_radial.sh — proves a change to the radial menu blitter draws the
# SAME pixels, and says how much work it dropped.
#
# Builds tools/radial_oracle.cpp twice: once against the committed
# src/platform/radial_menu.h (REFERENCE), once against the working tree
# (CANDIDATE), then compares the per-state checksums. Any difference is a
# rendering regression, full stop — no timing number is worth looking at until
# the two columns match.
#
# Both builds run on x86 AND on ARM under qemu: the console is ARM, and the
# conversion this blitter does per pixel (float -> int) is exactly the kind of
# thing that can agree on one architecture and differ on the other.
#
#   tools/oracle_radial.sh              # checksums only
#   tools/oracle_radial.sh 200          # + 200 timed draws per build
#
# The ARM timing is qemu, NOT the console: read it as "work removed", never as
# a frame rate. The verdict on frames is the on-screen FPS counter with the
# menu held open.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ITERS="${1:-0}"
REF_REV="${ORACLE_REF:-HEAD}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX_X86="${CXX_X86:-g++}"
CXX_ARM="${CXX_ARM:-arm-linux-gnueabihf-g++}"
# Same shape as the shipping VPK flags (tools/build_rt_boot_vpk.sh): -O2, no
# -ffast-math. Building the oracle with looser float flags than the console
# would make it answer a question nobody asked.
ARMFLAGS="-O2 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"

# REFERENCE tree: the committed header, next to the same generated assets.
mkdir -p "$OUT/ref/platform"
if ! git -C "$ROOT" show "$REF_REV:src/platform/radial_menu.h" > "$OUT/ref/platform/radial_menu.h" 2>/dev/null; then
    echo "oracle: impossible de lire $REF_REV:src/platform/radial_menu.h" >&2
    exit 2
fi
cp "$ROOT/src/platform/radial_menu_assets.h" "$OUT/ref/platform/"

if cmp -s "$OUT/ref/platform/radial_menu.h" "$ROOT/src/platform/radial_menu.h"; then
    echo "oracle: ATTENTION — l'arbre de travail est identique a $REF_REV, la comparaison ne prouve rien."
fi

build_and_run() {   # $1=cxx  $2=flags  $3=include-dir  $4=label  $5=outfile
    local bin="$OUT/$4"
    if ! "$1" $2 -std=gnu++17 -I"$3" -o "$bin" "$ROOT/tools/radial_oracle.cpp" -lm 2>"$OUT/$4.err"; then
        echo "oracle: compilation $4 KO" >&2; sed -n '1,20p' "$OUT/$4.err" >&2; return 1
    fi
    if [ "${1#arm-}" != "$1" ] || [ "${2#*-marm}" != "$2" ]; then
        qemu-arm -L /usr/arm-linux-gnueabihf "$bin" "$ITERS" > "$5" 2>&1
    else
        "$bin" "$ITERS" > "$5" 2>&1
    fi
}

status=0
for arch in x86 arm; do
    case "$arch" in
        x86) cxx="$CXX_X86"; flags="-O2" ;;
        arm) cxx="$CXX_ARM"; flags="$ARMFLAGS" ;;
    esac
    command -v "$cxx" >/dev/null 2>&1 || { echo "oracle[$arch]: $cxx absent — ignore"; continue; }
    [ "$arch" = arm ] && ! command -v qemu-arm >/dev/null 2>&1 && { echo "oracle[arm]: qemu-arm absent — ignore"; continue; }

    build_and_run "$cxx" "$flags" "$OUT/ref"   "ref_$arch"  "$OUT/ref_$arch.txt" || { status=1; continue; }
    build_and_run "$cxx" "$flags" "$ROOT/src"  "cand_$arch" "$OUT/cand_$arch.txt" || { status=1; continue; }

    if diff <(grep -v '^temps:' "$OUT/ref_$arch.txt") <(grep -v '^temps:' "$OUT/cand_$arch.txt") >"$OUT/d_$arch" 2>&1; then
        echo "oracle[$arch]: IDENTIQUE — $(grep -vc '^temps:' "$OUT/cand_$arch.txt") etats, meme somme de controle"
    else
        echo "oracle[$arch]: DIVERGENCE — le rendu a change :"
        sed -n '1,20p' "$OUT/d_$arch"
        status=1
    fi
    if [ "$ITERS" -gt 0 ] 2>/dev/null; then
        printf '  reference  %s\n' "$(grep '^temps:' "$OUT/ref_$arch.txt"  || echo '(pas de mesure)')"
        printf '  candidat   %s\n' "$(grep '^temps:' "$OUT/cand_$arch.txt" || echo '(pas de mesure)')"
    fi
done

exit $status
