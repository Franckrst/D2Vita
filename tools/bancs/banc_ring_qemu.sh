#!/usr/bin/env bash
# tools/bancs/banc_ring_qemu.sh — ring-traversal bench under qemu-arm.
#
# One pass = 4000 frames of the Rogue camp with the patrol
# (d2script_camp_patrouille.txt, the console bench script), Glide path (-3dfx)
# + glide3x.dll ring + GXM sink (D2_GLIDEGXM=1: nothing is drawn under qemu,
# but the atlas, batches and RING TRAVERSAL still run in full). Virtual clock
# + frozen wall clock: the bench is deterministic, so it's comparable
# frame-for-frame via its two end-of-run fingerprints:
#   [ring-final] ... h=   what the host READ (record stream)
#   [ring-final] ... lh=  what it DID with it (vertices, indices, batches; D2_GXMLOTSHASH=1)
# and via its running total [ring-final] parcours-ring-us/img (the cost to cut).
# Durations are qemu's own: the SLOPE between two legs is meaningful, the
# absolute value is not (the verdict came from console, not qemu).
#
#   banc_ring_qemu.sh <ARM binary> <label> [VAR=val ...]
#   REFS=<1.14d folder containing glide3x.dll>  (default $HOME/d2-vita-refs/1.14d)
#   MAXFRAMES=<n> (default 4000)   OUT=<log directory> (default /tmp)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$1"; TAG="$2"; shift 2
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"
OUT="${OUT:-/tmp}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
[ -f "$REFS/glide3x.dll" ] || { echo "FAIL: $REFS/glide3x.dll absent (tools/build_glide_ring.sh puis copier build-glide/glide3x.dll)"; exit 1; }
W="$OUT/w_ring_$TAG"; rm -rf "$W"; mkdir -p "$W/Save"
cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt")
GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=400000 MAXFRAMES="${MAXFRAMES:-4000}" \
  D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
  FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
  D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
  D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMLOTSHASH=1 WX86_FRAMEPROF=1 \
  env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$REFS" > "$OUT/log_ring_$TAG.txt" 2>&1
rc=$?
echo "[$TAG] rc=$rc $(grep -o 'CLEAN EXIT.*' "$OUT/log_ring_$TAG.txt" | tail -1)"
grep '^\[ring-final\]' "$OUT/log_ring_$TAG.txt" | sed "s/^/  [$TAG] /"
