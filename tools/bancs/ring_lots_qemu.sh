#!/usr/bin/env bash
# tools/bancs/ring_lots_qemu.sh — BATCH DUMP under qemu-arm: plays the camp
# patrol in Glide ring + GXM sink and dumps the batches of frame LOTS
# (D2_GXMLOTS) to $D2VITA_WORK/lots_<TAG>.log; counts gxlot: lines,
# OUT-OF-CELL vertices (uv outside the atlas cell) and filtered (bilinear)
# batches.
# Variables: REFS=<1.14d folder with glide3x.dll> D2VITA_WORK= LOTS=4000 MAXFRAMES=LOTS+200 [VAR=val ...]
#   e.g. REFS=/tmp/refs_ring LOTS=4000 bash tools/bancs/ring_lots_qemu.sh build-arm/rt_boot_arm pal D2_GXMPAL=1
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -ge 2 ] || { echo "usage: ring_lots_qemu.sh <binaire ARM> <etiquette> [VAR=val ...]"; exit 2; }
BIN="$1"; TAG="$2"; shift 2
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"
W="${D2VITA_WORK:-$ROOT/build-arm/bancs}"; mkdir -p "$W"
LOTS="${LOTS:-4000}"; MAXFRAMES="${MAXFRAMES:-$((LOTS+200))}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
[ -f "$REFS/glide3x.dll" ] || { echo "FAIL: $REFS/glide3x.dll absent (tools/build_glide_ring.sh puis copier build-glide/glide3x.dll dans un dossier de liens vers 1.14d)"; exit 1; }
WD="$W/w_lots_$TAG"; rm -rf "$WD"; mkdir -p "$WD/Save"
cp "$ROOT/tools/registry_console.txt" "$WD/registry.txt"
LOG="$W/lots_$TAG.log"
env GAMEEXE=1 D2ARGS="game.exe -3dfx" D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    D2SCRIPT="$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt")" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    MAXFRAMES="$MAXFRAMES" D2WRITE="$WD" D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMLOTS="$LOTS" "$@" \
    timeout 1800 qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1
rc=$?
echo "[$TAG] rc=$rc gxlot=$(grep -c '^gxlot:' "$LOG") HORS-CELLULE=$(grep -c 'HORS-CELLULE' "$LOG") filtre=$(grep -c 'flags=0x[2367aAbBeEfF]' "$LOG") -> $LOG"
grep "^\[gxm: lots=" "$LOG" | tail -3 | sed "s/^/  [$TAG] /"
