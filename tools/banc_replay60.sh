#!/usr/bin/env bash
# tools/banc_replay60.sh — qemu bench (real clock) for the host-side 60 Hz
# replay (D2_REPLAY60) on the Glide ring path (-3dfx, D2_GLIDERING=1,
# D2_GLIDEGXM=1: under qemu the batch builder runs, GPU submission is a no-op).
#
# Scenario: enter the game at the Rogue camp (patrol = moving units), then
# three player-movement clicks (camera moving). The game draws ONCE per step
# (D2_NOCAP=1 D2_ONEDRAW=2), the replay thread submits at 60 Hz.
#
# Legs (parallel, same binary):
#   TEM  : ONEDRAW=2 alone (control: ring untagged, direct flush)
#   INV  : + D2_RINGTAG + D2_RINGPHASE_X (per-function draw/vertex inventory)
#          + D2_REPLAY60_DUMP (unit/camera dumps) — used to locate the UI boundary
#   R60I : + D2_REPLAY60=1 (interpolation)
#   R60E : + D2_REPLAY60=1 D2_REPLAY60_MODE=extrap
# The ring DLL must be in the game folder: the bench builds a folder of
# symlinks ($REFS60) and drops build-glide/glide3x.dll there, to avoid
# touching the shared reference folder.
#
# Expected (doc section 9): in menu/loading windows
#   `replay60: ... rejeux=0 ... etat=veille images-hors-jeu=<n>` (no replay
# submissions outside the game); in-game `rejeux=596-603` per 10 s window
# (60/s), `rejeux-hors-jeu=0`, `borne3: ... max-rejeux/img=3`.
#
# MAXFRAMES=... TAG=... ONLY="INV R60I" EXTRA="D2_REPLAY60_DUMP=600" bash tools/banc_replay60.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-6000}"
FROM="${FROM:-3200}"   # D2_NOCAP_FROM: menus stay capped, otherwise the frame-indexed script clicks too early
TAG="${TAG:-}"
TMO="${TMO:-5400}"
OUTD="${OUTD:-/tmp}"
REFS60="${REFS60:-/tmp/refs60_$USER}"
ONLY="${ONLY:-TEM INV R60I R60E}"
UI="${UI:-}"      # UI boundary RVA (D2_REPLAY60_UI); empty = binary default
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
[ -f "$ROOT/build-glide/glide3x.dll" ] || bash "$ROOT/tools/build_glide_ring.sh" >/dev/null || { echo "FAIL: DLL ring"; exit 1; }
mkdir -p "$REFS60"; for f in "$DIR"/*; do ln -sf "$f" "$REFS60/"; done
rm -f "$REFS60/glide3x.dll"; cp "$ROOT/build-glide/glide3x.dll" "$REFS60/glide3x.dll"

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
# player movement (camera) once in-game
S="$S,3600:move:500:300,3650:ldown:500:300,3660:lup:500:300"
S="$S,4200:move:150:250,4250:ldown:150:250,4260:lup:150:250"
S="$S,4800:move:320:400,4850:ldown:320:400,4860:lup:320:400"
S="$S,5400:move:600:200,5450:ldown:600:200,5460:lup:600:200"

run(){ # $1 label, $2... extra env
  local LAB="$1$TAG"; shift
  local W=$OUTD/r60_$LAB LOG=$OUTD/r60_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=4000000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    D2_REALCLOCK=1 WX86_FRAMEPROF=1 D2_LOOPWATCH=1 \
    D2_GLIDERING=1 D2_GLIDEGXM=1 D2_NOCAP_FROM="$FROM" \
    D2WRITE="$W" D2SCRIPT="$S" \
    env "$@" \
    timeout "$TMO" qemu-arm -B 0x10000 "$BIN" "$REFS60" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s  -> $LOG"
}
UIENV=(); [ -n "$UI" ] && UIENV=(D2_REPLAY60_UI="$UI")
INVX="${INVX:-73f50,76bc0,56ee0,f98e0,684c0,77980,f6190}"   # 5b440 (camera) and dc7b0 (unit) already have their hook
echo "== BANC REPLAY60 (qemu, horloge REELLE) — MAXFRAMES=$MAXFRAMES jambes: $ONLY =="
T0=$(date +%s)
for J in $ONLY; do case "$J" in
  TEM)  run TEM  D2_NOCAP=1 D2_ONEDRAW=2 D2_PHASEPROF=1 & ;;
  INV)  run INV  D2_NOCAP=1 D2_ONEDRAW=2 D2_PHASEPROF=1 D2_RINGTAG=1 D2_RINGPHASE_X="$INVX" D2_REPLAY60=1 D2_REPLAY60_DUMP=400 "${UIENV[@]}" & ;;
  R60I) run R60I D2_NOCAP=1 D2_ONEDRAW=2 D2_PHASEPROF=1 D2_REPLAY60=1 "${UIENV[@]}" ${EXTRA:-} & ;;
  R60E) run R60E D2_NOCAP=1 D2_ONEDRAW=2 D2_PHASEPROF=1 D2_REPLAY60=1 D2_REPLAY60_MODE=extrap "${UIENV[@]}" ${EXTRA:-} & ;;
esac; done
wait
echo "  duree totale: $(( $(date +%s) - T0 ))s"
for J in $ONLY; do
  echo "---- $J"
  grep -E "^\[?ringtag:|^\[?replay60|^\[frames:|^frames:|^phase/tick|^\[ring:|^\[gxm: lots" "$OUTD/r60_$J$TAG.log" | tail -12 | sed 's/^/     /'
done
