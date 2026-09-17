#!/usr/bin/env bash
# tools/oracle_cellpar.sh — pixel oracle for the cell-loop fork-join (D2_CELLPAR).
# Sibling of oracle_cellopt.sh, with one key difference: worker scheduling is
# not deterministic, so a race that only triggers rarely could pass a single
# run undetected. Each parallel leg is therefore repeated (REPEAT, default 3)
# and must match the control leg's fingerprint every time — one PASS proves
# nothing, ten in a row start to.
#
# Requires the console memory layout (D2LAYOUT=compact D2ARENA=12900000): the
# fork-join distributes host pointers (H(va) = va + membase) resolved by the
# calling thread, so a bench with membase=0 would not exercise the same
# address computation.
#
# PASS requires: A == A2 (bench is deterministic), every parallel leg matches
# A, no leg is empty (served>0 everywhere), and the safety counters are zero
# (lane overlaps on writes, out-of-view scale reads resolved by pass 1).
#
#   MASK=63                 D2_CELLOPT value on both sides (default: unset)
#   REPEAT=3                repetitions per parallel leg
#   WORKERS="1 2"           worker counts to test
#   CELLMODE=1              partition by cell instead of by band
#   MAXFRAMES=4000          bench length
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
MASK="${MASK:-}"
REPEAT="${REPEAT:-3}"
WORKERS="${WORKERS:-1 2}"
CELLMODE="${CELLMODE:-0}"
TAG="${TAG:-}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = worker count ("" = control leg)
  local LAB="$1$TAG" NW="$2" W=/tmp/cellpar_$1$TAG LOG=/tmp/cellpar_$1$TAG.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s) E=()
  [ -n "$MASK" ] && E+=("D2_CELLOPT=$MASK")
  [ -n "$NW" ]   && E+=("D2_CELLPAR=$NW")
  [ "$CELLMODE" = "1" ] && E+=("D2_CELLPARCELL=1")
  env GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    "${E[@]}" ${EXTRAENV:-} \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[boucle\]|cellpar:|cellovl:" "$LOG" | sed 's/^/     /'
}
emp() { grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/cellpar_$1$TAG.log" | tail -1; }
srv() { grep -oP '\[boucle\] servis=\K[0-9]+'          "/tmp/cellpar_$1$TAG.log" | tail -1; }
cnt() { grep -oP "cellpar: ouvriers=[0-9]+ appels=.*$2=\K[0-9]+" "/tmp/cellpar_$1$TAG.log" | tail -1; }

echo "== ORACLE D2_CELLPAR — ouvriers='$WORKERS' x$REPEAT — mode=$([ "$CELLMODE" = 1 ] && echo cellules || echo bandes) — D2_CELLOPT='${MASK:-absent}' — MAXFRAMES=$MAXFRAMES =="
echo "-- passe A  : D2_CELLPAR absent --";                         run A  ""
echo "-- passe A2 : D2_CELLPAR absent (CONTROLE determinisme) --"; run A2 ""
HA=$(emp A); HA2=$(emp A2)
if [ "${HA:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A  = ${HA:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."; exit 2
fi
echo "  controle de determinisme : A == A2 = $HA"
[ "$(srv A)" -gt 0 ] 2>/dev/null || { echo "FAIL: jambe temoin VIDE"; exit 1; }

fail=0
for nw in $WORKERS; do
  for r in $(seq 1 "$REPEAT"); do
    echo "-- passe B(ouvriers=$nw) essai $r/$REPEAT --"; run "B${nw}_$r" "$nw"
    H=$(emp "B${nw}_$r"); SB=$(srv "B${nw}_$r")
    CH=$(cnt "B${nw}_$r" chevauchements); EH=$(cnt "B${nw}_$r" "echelle-hors-vue")
    LIVE=$(grep -oP 'cellpar: ouvriers=\K[0-9]+' "/tmp/cellpar_B${nw}_$r$TAG.log" | tail -1)
    [ "${H:-x}" = "$HA" ] || { echo "FAIL: B(ouvriers=$nw,essai $r) empreinte ${H:-ABSENTE} != $HA"; fail=1; }
    [ "${SB:-0}" -gt 0 ]  || { echo "FAIL: B(ouvriers=$nw,essai $r) VIDE"; fail=1; }
    [ "${LIVE:-0}" = "$nw" ] || { echo "FAIL: $nw ouvriers demandes, ${LIVE:-0} demarres"; fail=1; }
    [ "${CH:-1}" = "0" ]  || { echo "FAIL: $CH ecriture(s) a cheval sur deux voies"; fail=1; }
    [ "${EH:-1}" = "0" ]  || { echo "FAIL: $EH lecture(s) echelle hors de la vue hote"; fail=1; }
  done
done
echo
echo "  empreinte temoin = $HA"
[ $fail -eq 0 ] && echo "PASS: empreinte identique sur toutes les repetitions, ouvriers actifs, compteurs de surete a zero" || exit 1
