#!/usr/bin/env bash
# tools/build_gxm_probe.sh — builds the bare sceGxm APP (src/platform/probe).
#
# WHY. A binary that bundles a dynarec, a guest scheduler, a network stack and
# three host threads gives no way to localize "it hangs" to any one of them.
# This VPK contains ONLY the suspect GXM async mechanism, with the same sceGxm
# parameters, the same CDRAM, the same carousel, the same display queue and
# THE SAME SHADERS — so a hang here isolates the GXM path itself.
#
# TITLE_ID DTWO00009 (9 characters — the hardware rejects 10-character IDs)
# with its own log: it COEXISTS with the game, it replaces nothing.
#
# Output: build-probe/d2vita_gxmprobe.vpk  (eboot: build-probe/eboot.bin)
# Console log: ux0:data/d2vita/gxmprobe_progress.txt
# Console knobs: ux0:data/d2vita/gxmprobe.txt (KEY=VALUE per line)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${D2PROBE_OUT:-$ROOT/build-probe}"
TITLE="${TITLE:-DTWO00009}"
APPNAME="${APPNAME:-D2Vita GXM Probe}"
VPKOUT="${VPKOUT:-d2vita_gxmprobe.vpk}"

[ ${#TITLE} -eq 9 ] || { echo "FATAL: TITLE_ID doit faire exactement 9 caracteres (recu '$TITLE')"; exit 1; }

export PATH="${VITASDK:-/usr/local/vitasdk}/bin:$PATH"
command -v arm-vita-eabi-g++ >/dev/null || { echo "FATAL: VitaSDK absent du PATH (/usr/local/vitasdk/bin)"; exit 1; }

mkdir -p "$OUT"
# DESTROY before building: otherwise a failed build would leave the previous
# VPK in place and the bench would run on stale code while reporting a green
# build.
rm -f "$OUT/gxm_probe.o" "$OUT/gxm_probe.elf" "$OUT/gxm_probe.velf" "$OUT/eboot.bin" "$OUT/$VPKOUT"

echo "== compilation =="
arm-vita-eabi-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I"$ROOT/src" -c "$ROOT/src/platform/probe/gxm_probe.cpp" -o "$OUT/gxm_probe.o"

echo "== edition de liens =="
arm-vita-eabi-g++ -Wl,-q "$OUT/gxm_probe.o" \
  -lSceDisplay_stub -lSceGxm_stub -lSceCtrl_stub -lSceSysmem_stub -lSceLibKernel_stub \
  -lSceIofilemgr_stub -lSceProcessmgr_stub -lSceKernelThreadMgr_stub \
  -lm -o "$OUT/gxm_probe.elf"

vita-elf-create "$OUT/gxm_probe.elf" "$OUT/gxm_probe.velf" >/dev/null
# self NOT signed (no -s), like the game's VPK: same privilege level, so the
# same allocation behavior. Requires "unsigned homebrew" enabled.
vita-make-fself "$OUT/gxm_probe.velf" "$OUT/eboot.bin" >/dev/null
vita-mksfoex -s TITLE_ID="$TITLE" "$APPNAME" "$OUT/param.sfo" >/dev/null

# Shaders travel INSIDE the VPK, same as the game: the bare app must be able
# to run on a console where nothing else has been deployed.
SHADER_ARGS=()
for sh in d2_ring_v d2_ring_f; do
  [ -f "$ROOT/shaders/$sh.gxp" ] && SHADER_ARGS+=(-a "$ROOT/shaders/$sh.gxp=shaders/$sh.gxp")
done
[ ${#SHADER_ARGS[@]} -eq 4 ] || { echo "FATAL: shaders/d2_ring_{v,f}.gxp manquants"; exit 1; }

vita-pack-vpk -s "$OUT/param.sfo" -b "$OUT/eboot.bin" "${SHADER_ARGS[@]}" "$OUT/$VPKOUT" >/dev/null
echo "== built $OUT/$VPKOUT (TITLE_ID=$TITLE) =="
echo "   eboot        : $OUT/eboot.bin"
echo "   journal      : ux0:data/d2vita/gxmprobe_progress.txt"
echo "   knobs        : ux0:data/d2vita/gxmprobe.txt (D2_GXMASYNC/D2_GXMRING/D2_GXMVSYNC/D2_GXMCDRAM/D2_GXMSECONDS)"
