#!/usr/bin/env bash
# tools/oracle_cellopt.sh — PIXEL ORACLE for the cell-loop OPTIMIZATIONS
# (D2_CELLOPT). Sibling of oracle_cellloop.sh, with TWO differences:
#
#   1. REQUIRES THE CONSOLE MEMORY LAYOUT (D2LAYOUT=compact
#      D2ARENA=12900000). The optimizations resolve HOST pointers
#      (H(va) = va + membase): a bench with membase=0 would not exercise the
#      same address computation as the console.
#   2. the treated leg keeps NATIVECELLLOOP=1 and adds D2_CELLOPT=<mask>. The
#      port itself is therefore armed on BOTH sides: what's compared is no
#      longer "port vs guest code" but "slow port vs fast port", which is the
#      actual question being asked.
#
# VERDICT = A == A2 (determinism) AND A == B (no pixel changed) AND legs
# non-empty (servis>0 on both sides).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
MASK="${MASK:-255}"
TAG="${TAG:-}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = D2_CELLOPT value ("" = absent)
  local LAB="$1$TAG" KNOB="$2" W=/tmp/cellopt_$1$TAG LOG=/tmp/cellopt_$1$TAG.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export D2_CELLOPT="$KNOB"; else unset D2_CELLOPT; fi
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env ${EXTRAENV:-} \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset D2_CELLOPT
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[boucle\]|^\s*\[cellules\]|^\s*\[cellstat\]|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}

echo "== ORACLE D2_CELLOPT=$MASK — MAXFRAMES=$MAXFRAMES (layout console) =="
if [ -z "${SKIPA:-}" ]; then
  echo "-- passe A  : D2_CELLOPT absent --";                          run A ""
  echo "-- passe A2 : D2_CELLOPT absent (CONTROLE determinisme) --";  run A2 ""
fi
HA0=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/cellopt_A$TAG.log | tail -1)
HA2=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/cellopt_A2$TAG.log | tail -1)
if [ "${HA0:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A  = ${HA0:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."
  exit 2
fi
echo "  controle de determinisme : A == A2 = $HA0"
echo "-- passe B  : D2_CELLOPT=$MASK --"; run B "$MASK"

HB=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/cellopt_B$TAG.log | tail -1)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' /tmp/cellopt_A$TAG.log | tail -1)
SA=$(grep -oP '\[boucle\] servis=\K[0-9]+' /tmp/cellopt_A$TAG.log | tail -1)
SB=$(grep -oP '\[boucle\] servis=\K[0-9]+' /tmp/cellopt_B$TAG.log | tail -1)
echo
echo "  empreinte A = ${HA0:-ABSENTE}   (frames hachees=${NA:-0})"
echo "  empreinte B = ${HB:-ABSENTE}"
echo "  servis A = ${SA:-0}   servis B = ${SB:-0}"
fail=0
[ -n "$HA0" ] && [ -n "$HB" ] || { echo "FAIL: empreinte manquante"; fail=1; }
[ "${HA0:-x}" = "${HB:-y}" ] || { echo "FAIL: EMPREINTES DIFFERENTES — divergence pixel"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ "${SB:-0}" -gt 0 ] || { echo "FAIL: test NON VIDE cote B"; fail=1; }
[ "${SA:-0}" -gt 0 ] || { echo "FAIL: test NON VIDE cote A"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: empreinte identique, portage actif des deux cotes" || exit 1
