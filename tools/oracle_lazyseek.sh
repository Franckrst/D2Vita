#!/usr/bin/env bash
# tools/oracle_lazyseek.sh — PIXEL ORACLE for LAZY seeking (D2_LAZYSEEK).
#
# Question: does keeping the file position in memory and only doing the real
# fseek when it's actually needed change a SINGLE byte the game reads? An
# offset drift doesn't show up right away — it surfaces as a corrupted sprite
# minutes later. Only a pixel oracle on a loaded game can settle it.
#
#   A  = D2_LAZYSEEK absent (immediate fseek+ftell, legacy behavior)
#   A2 = determinism control (A replayed)
#   B  = D2_LAZYSEEK=1
# VERDICT = fingerprint A == A2 (the oracle is an oracle) AND A == B.
# Scenario, binary and environment from the deterministic bench (see oracle_cellloop.sh).
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

run() {  # $1 = label, $2 = D2_LAZYSEEK value ("" = absent)
  local LAB="$1" KNOB="$2" W=/tmp/olz_$1 LOG=/tmp/olz_$1.log
  rm -rf "$W"; mkdir -p "$W/Save"
  # Seed the console registry: without it the bench starts with no persisted
  # video settings and does not cover the same code.
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export D2_LAZYSEEK="$KNOB"; else unset D2_LAZYSEEK; fi
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" NATIVECELLLOOP=1 \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset D2_LAZYSEEK
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[lazyseek|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/olz_$1.log" | tail -1; }

echo "== ORACLE du positionnement paresseux — MAXFRAMES=$MAXFRAMES =="
echo "-- passe A  : D2_LAZYSEEK ABSENT --";                       run A  ""
echo "-- passe A2 : D2_LAZYSEEK ABSENT (CONTROLE de determinisme) --"; run A2 ""
HA=$(H A); HA2=$(H A2)
if [ "${HA:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A = ${HA:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."; exit 2
fi
echo "  controle de determinisme : A == A2 = $HA  (l oracle est un oracle)"
echo "-- passe B  : D2_LAZYSEEK=1 --"; run B "1"
HB=$(H B)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' /tmp/olz_A.log | tail -1)
EV=$(grep -oP 'lazyseek\(final\): evites=\K[0-9]+' /tmp/olz_B.log | tail -1)
LZ=$(grep -oP 'paresseux=\K[0-9]+' /tmp/olz_B.log | tail -1)
SY=$(grep -oP 'vrais-fseek=\K[0-9]+' /tmp/olz_B.log | tail -1)
OA=$(grep -c 'lazyseek' /tmp/olz_A.log)
echo
echo "  empreinte A = ${HA:-ABSENTE}   (frames hachees=${NA:-0})"
echo "  empreinte B = ${HB:-ABSENTE}"
echo "  cote B : fseek EVITES=${EV:-0}  (paresseux=${LZ:-0} vrais-fseek=${SY:-0})"
echo "  lignes lazyseek cote A = $OA (doit valoir 0 : temoin propre)"
fail=0
[ -n "$HA" ] && [ -n "$HB" ] || { echo "FAIL: empreinte manquante"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: EMPREINTES DIFFERENTES — le paresseux corrompt les lectures"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ "${EV:-0}" -gt 0 ] || { echo "FAIL: test NON VIDE — aucun fseek evite cote B"; fail=1; }
[ "$OA" -eq 0 ] || { echo "FAIL: le paresseux a tourne cote A (temoin pollue)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: empreinte identique, paresseux actif, temoin propre" || exit 1
