#!/usr/bin/env bash
# tools/oracle_nocap.sh — PIXEL ORACLE for the frame-limiter TOOLING.
#
# WHAT IT PROVES, AND WHAT IT CANNOT PROVE.
#
# D2_NOCAP=1 changes the NUMBER of frames rendered for the same play time.
# Under the VIRTUAL clock (D2_VIRTCLOCK) the game clock advances one tick per
# GetTickCount read: fewer pump cycles per frame means fewer ticks per frame,
# hence a DIFFERENT game state at frame N. An identical framebuffer
# fingerprint between "knob absent" and "D2_NOCAP=1" is therefore IMPOSSIBLE
# by construction, and requiring it would be a false test.
#
# What the oracle checks instead, the real fidelity question for THIS change:
#   B1  D2_LOOPWATCH=1            : the instrument (alternate hook at the
#                                   entry of Game+0x12fd90, faithful fallback)
#                                   does NOT change A SINGLE PIXEL.
#   B2  D2_NOCAP=40 (+LOOPWATCH)  : the immediate-patching mechanism, armed
#                                   with the ORIGINAL value (0x28), does NOT
#                                   change A SINGLE PIXEL either.
# Proof that D2_NOCAP=1 doesn't affect GAME SPEED lives elsewhere: the "sim="
# counter of the real-clock bench (tools/banc_nocap.sh), which counts
# simulation steps per real second.
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
  local W=/tmp/orclim_$LAB LOG=/tmp/orclim_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[boucle\]|^nocap:|^loopwatch:|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/orclim_$1$TAG.log" | tail -1; }

echo "== ORACLE OUTILLAGE LIMITEUR — MAXFRAMES=$MAXFRAMES (layout console) =="
echo "-- passe A  : temoin --";                                    run A
echo "-- passe A2 : temoin (CONTROLE determinisme) --";            run A2
HA=$(H A); HA2=$(H A2)
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "  A=${HA:-ABSENTE} A2=${HA2:-ABSENTE}"; echo "FAIL: BANC NON DETERMINISTE"; exit 2; }
echo "  controle de determinisme : A == A2 = $HA"
echo "-- passe B1 : D2_LOOPWATCH=1 --";                            run B1 D2_LOOPWATCH=1
echo "-- passe B2 : D2_LOOPWATCH=1 D2_NOCAP=40 (valeur d origine) --"; run B2 D2_LOOPWATCH=1 D2_NOCAP=40
HB1=$(H B1); HB2=$(H B2)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "/tmp/orclim_A$TAG.log" | tail -1)
echo
echo "  A  = ${HA:-ABSENTE}   (frames hachees=${NA:-0})"
echo "  B1 = ${HB1:-ABSENTE}  (instrument seul)"
echo "  B2 = ${HB2:-ABSENTE}  (instrument + ecriture de l immediat a sa valeur d origine)"
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HB1:-y}" ] || { echo "FAIL: L INSTRUMENT change des pixels"; fail=1; }
[ "${HA:-x}" = "${HB2:-y}" ] || { echo "FAIL: L ECRITURE DE L IMMEDIAT change des pixels"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: instrument et mecanisme d ecriture pixel-neutres" || exit 1
