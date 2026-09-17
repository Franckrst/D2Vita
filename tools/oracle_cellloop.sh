#!/usr/bin/env bash
# tools/oracle_cellloop.sh — PIXEL ORACLE for the cell-loop port.
#
# The port of Game+0xf8b80 writes ONLY framebuffer pixels: FBHASH is therefore
# a TRUE oracle here, not a fallback. (The CpuUnicorn differential oracle no
# longer exists; cpu.h:4 and cpu_box86.cpp:501/810 still document it as if it
# were — stale comments.)
#
# Two qemu-arm passes STRICTLY identical except for NATIVECELLLOOP:
#   A = knob absent  -> old guest code
#   B = NATIVECELLLOOP=1
# VERDICT = identical FBHASH fingerprint AND a NON-EMPTY test (servis>0 on the
# B side, no [boucle] line on the A side). Without the non-empty check, a
# world that's never reached would return two equal fingerprints and a false
# GREEN.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/oracle_arm"
MAXFRAMES="${MAXFRAMES:-4000}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = NATIVECELLLOOP value ("" = absent)
  local LAB="$1" KNOB="$2" W=/tmp/oracle_$1 LOG=/tmp/oracle_$1.log
  rm -rf "$W"; mkdir -p "$W/Save"
  # ⚡ SEED THE CONSOLE REGISTRY. Without it the bench starts with no
  # persisted video settings, and the game does not render the same thing:
  # zero SCALE cells under qemu, versus many with it. The oracle would then
  # miss the code it's meant to cover.
  [ -z "${NOREG:-}" ] && cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export NATIVECELLLOOP="$KNOB"; else unset NATIVECELLLOOP; fi
  # DETERMINISM IS MANDATORY. Without D2_VIRTCLOCK + D2_FAKEWALL, two passes
  # of the SAME binary return DIFFERENT fingerprints and the oracle proves
  # nothing. This is the project's standard deterministic-bench rule.
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    timeout 2700 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset NATIVECELLLOOP
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[boucle\]|^\s*\[cellules\]|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}

echo "== ORACLE boucle de cellules — MAXFRAMES=$MAXFRAMES =="
echo "-- passe A  : knob ABSENT (ancien code) --"; run A ""
echo "-- passe A2 : knob ABSENT (CONTROLE de determinisme) --"; run A2 ""
HA0=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/oracle_A.log | tail -1)
HA2=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/oracle_A2.log | tail -1)
if [ "${HA0:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A  = ${HA0:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."
  echo "      Aucune comparaison n a de sens tant que ce n est pas resolu."
  exit 2
fi
echo "  controle de determinisme : A == A2 = $HA0  (l oracle est un oracle)"
echo "-- passe B  : NATIVECELLLOOP=${LVL:-1} --";   run B "${LVL:-1}"

HA=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/oracle_A.log | tail -1)
HB=$(grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' /tmp/oracle_B.log | tail -1)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' /tmp/oracle_A.log | tail -1)
SB=$(grep -oP '\[boucle\] servis=\K[0-9]+' /tmp/oracle_B.log | tail -1)
SA=$(grep -c '\[boucle\]' /tmp/oracle_A.log)
echo
echo "  empreinte A = ${HA:-ABSENTE}   (frames hachees=${NA:-0})"
echo "  empreinte B = ${HB:-ABSENTE}"
echo "  servis cote B = ${SB:-0}   |   lignes [boucle] cote A = $SA"
fail=0
[ -n "$HA" ] && [ -n "$HB" ] || { echo "FAIL: empreinte manquante"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: EMPREINTES DIFFERENTES — divergence pixel"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ "${SB:-0}" -gt 0 ] || { echo "FAIL: test NON VIDE — le portage n'a jamais servi cote B"; fail=1; }
[ "$SA" -eq 0 ] || { echo "FAIL: le portage a tourne cote A (temoin pollue)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: empreinte identique, portage actif, temoin propre" || exit 1
