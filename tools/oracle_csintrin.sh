#!/usr/bin/env bash
# tools/oracle_csintrin.sh — PIXEL ORACLE for D2_CSINTRIN (lever B6).
#
# D2_CSINTRIN=1 serves EnterCriticalSection / LeaveCriticalSection /
# TryEnterCriticalSection as INTRINSICS in the dynarec's trap dispatch: no
# more crossing Bridge::trap_handler (slot bound check, WX86_WATCH check,
# note_shim_enter/exit, std::function indirection, take_redirect), and a
# SINGLE R_ESP resolution instead of two (trap_retaddr() then arg(0)). State
# stays host-side (KCrit), no guest byte is written, no code is emitted into
# the guest.
#
# ⚠️ THIS ORACLE IS NOT ENOUGH ON ITS OWN. A critical section is a
# SYNCHRONIZATION object and the game's measured contention is 0.0003%: a
# BROKEN exclusion path would still return the same fingerprint. Proof of
# synchronization is a SEPARATE, mandatory bench:
#     D2_CSTEST=1 [D2_CSINTRIN=1] [D2SCHED=native] ... rt_boot_arm
# (mutual exclusion + recursion, three guest threads contending).
# And the boot gate:
#     [D2SCHED=native] D2_CSINTRIN=1 bash tools/rt_boot_arm_check.sh
#
# Three qemu-arm passes STRICTLY identical except for the knob:
#   A  = knob absent   (legacy shim path)
#   A2 = knob absent   (bench DETERMINISM CONTROL)
#   B  = D2_CSINTRIN=1
# VERDICT = A == A2, A == B, and a NON-EMPTY test: the "[csintrin]" line must
# be ABSENT in A and carry nonzero enter/leave in B.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${ORACLE_BIN:-$ROOT/build-arm/rt_boot_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = D2_CSINTRIN value ("" = absent)
  local LAB="$1" KNOB="$2" W=/tmp/ocs_$1 LOG=/tmp/ocs_$1.log
  rm -rf "$W"; mkdir -p "$W/Save"          # FRESH WRITE FOLDER per leg
  # SEED THE CONSOLE REGISTRY: without it the bench starts with no persisted
  # video settings and does NOT cover the same code (zero SCALE cells on the
  # qemu side).
  [ -z "${NOREG:-}" ] && cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export D2_CSINTRIN="$KNOB"; else unset D2_CSINTRIN; fi
  # DETERMINISM IS MANDATORY: without D2_VIRTCLOCK + D2_FAKEWALL, two passes
  # of the SAME binary return different fingerprints, and a FAIL can no
  # longer distinguish "the code diverges" from "the bench diverges".
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" NATIVECELLLOOP=1 \
    D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    timeout 2700 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset D2_CSINTRIN
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\s*\[csintrin\]|^csintrin:|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/ocs_$1.log" | tail -1; }
E(){ grep -oP '\[csintrin\] enter=\K[0-9]+' "/tmp/ocs_$1.log" | tail -1; }
V(){ grep -oP '\[csintrin\].* leave=\K[0-9]+' "/tmp/ocs_$1.log" | tail -1; }

echo "== ORACLE D2_CSINTRIN (B6) — MAXFRAMES=$MAXFRAMES =="
echo "-- passe A  : knob ABSENT --";                         run A  ""
echo "-- passe A2 : knob ABSENT (CONTROLE determinisme) --"; run A2 ""
HA=$(H A); HA2=$(H A2)
if [ "${HA:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A = ${HA:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."
  echo "      Aucune comparaison n a de sens tant que ce n est pas resolu."; exit 2
fi
echo "  controle de determinisme : A == A2 = $HA"
echo "-- passe B  : D2_CSINTRIN=1 --"; run B "1"
HB=$(H B); NA=$(E A); NB=$(E B); LB=$(V B)
echo
echo "  A  empreinte = ${HA:-ABSENTE}   [csintrin] enter = ${NA:-<absente>}"
echo "  B  empreinte = ${HB:-ABSENTE}   [csintrin] enter = ${NB:-<absente>} leave = ${LB:-<absente>}"
fail=0
[ -n "${HA:-}" ] && [ -n "${HB:-}" ] || { echo "FAIL: empreinte manquante"; fail=1; }
[ -z "${NA:-}" ] || { echo "FAIL: la jambe A a servi des intrinseques — les jambes ne different pas"; fail=1; }
[ -n "${NB:-}" ] && [ "${NB:-0}" -gt 0 ] || { echo "FAIL: TEST VIDE — la jambe B n a servi AUCUN Enter"; fail=1; }
[ -n "${LB:-}" ] && [ "${LB:-0}" -gt 0 ] || { echo "FAIL: TEST VIDE — la jambe B n a servi AUCUN Leave"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: EMPREINTES DIFFERENTES — l'intrinseque change l'image"; fail=1; }
[ $fail -eq 0 ] && echo "PASS : empreinte IDENTIQUE et jambe B non vide (enter=$NB leave=$LB)" || exit 1
