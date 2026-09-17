#!/usr/bin/env bash
# tools/oracle_phaseprof.sh — PIXEL ORACLE for the per-phase instrumentation
# (D2_PHASEPROF) and the "one draw per step" knob (D2_ONEDRAW).
#
# What it proves: under the virtual clock (D2_VIRTCLOCK + D2_FAKEWALL) the
# bench is deterministic (A == A2). D2_PHASEPROF's entry/exit hooks (sim
# Game+0x12fd90, draw [0x7a0484], tick Game+0x4efa0, return trap) must NOT
# change a pixel (B1). D2_ONEDRAW without D2_NOCAP short-circuits NOTHING
# (the game's own lock [0x7a0704] already enforces one draw per step): B2
# (=1) and B3 (=2, with sleep) must therefore return the SAME fingerprint as
# A — this is the test that the knob has no side effect when it skips no
# draw, and that the faithful fallback of the three draw hooks is exact.
# What the oracle cannot prove: D2_ONEDRAW's fidelity WHEN it does skip draws
# (under D2_NOCAP) — fewer draws means fewer pump cycles, hence a different
# state at frame N under the virtual clock (same reason as for D2_NOCAP, see
# oracle_nocap.sh). That part is measured on the real-clock bench
# (banc_nocap.sh + EXTRAENV) via the sim= counter, which must stay at 25/s.
# The five passes run IN PARALLEL (deterministic bench: time plays no part in
# the fingerprint).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2... = extra environment variables
  local LAB="$1$TAG"; shift
  local W=/tmp/orcpp_$LAB LOG=/tmp/orcpp_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/orcpp_$1$TAG.log" | tail -1; }

echo "== ORACLE PHASEPROF/ONEDRAW — MAXFRAMES=$MAXFRAMES (layout console, 5 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run B1 D2_PHASEPROF=1 &
run B2 D2_ONEDRAW=1 &
run B3 D2_PHASEPROF=1 D2_ONEDRAW=2 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB1=$(H B1); HB2=$(H B2); HB3=$(H B3)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "/tmp/orcpp_A$TAG.log" | tail -1)
echo
echo "  A  = ${HA:-ABSENTE}   (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}  (controle de determinisme)"
echo "  B1 = ${HB1:-ABSENTE}  (D2_PHASEPROF=1)"
echo "  B2 = ${HB2:-ABSENTE}  (D2_ONEDRAW=1, sans NOCAP : aucun saut attendu)"
echo "  B3 = ${HB3:-ABSENTE}  (D2_PHASEPROF=1 D2_ONEDRAW=2)"
for L in B1 B3; do grep -E "^phase|^phaseprof" "/tmp/orcpp_$L$TAG.log" | tail -4 | sed "s/^/     [$L] /"; done
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB1:-y}" ] || { echo "FAIL: D2_PHASEPROF change des pixels"; fail=1; }
[ "${HA:-x}" = "${HB2:-y}" ] || { echo "FAIL: D2_ONEDRAW=1 change des pixels sans NOCAP"; fail=1; }
[ "${HA:-x}" = "${HB3:-y}" ] || { echo "FAIL: D2_PHASEPROF+D2_ONEDRAW=2 change des pixels sans NOCAP"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: instrumentation par phase et ONEDRAW pixel-neutres (sans saut)" || exit 1
