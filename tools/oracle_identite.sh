#!/usr/bin/env bash
# tools/oracle_identite.sh — PIXEL ORACLE for the IDENTITY memory MODEL
# (D2LAYOUT=haut)
#
# QUESTION. On Vita, guest memory isn't identity-mapped: the kernel only hands
# out blocks >= 0x80000000, while the game lives low, hence the single arena
# and `membase = host - guest`. Every x86 memory access then becomes "ADD
# address+membase" THEN "LDR/STR", and 61.5% of those ADDs cannot be removed
# by any ARM addressing trick. The ONLY way to
# remove all of them is membase = 0: the IDENTITY model.
#
# This bench plays, under qemu, the TRANSLATED compact plan (D2LAYOUT=haut,
# D2HI=base) with membase = 0 (no D2ARENA), and compares it to the console's
# plan (D2LAYOUT=compact D2ARENA=12900000, membase != 0) on the SAME scenario.
#
# ⚡ BOTH LEGS ARE THE SAME BINARY: the layout is read at runtime.
# ⚡ DETERMINISM IS MANDATORY: D2_VIRTCLOCK + D2_FAKEWALL + a FRESH write
#    folder per leg + a seeded registry_console.txt (without it, zero SCALE
#    cells on the qemu side versus many on the console).
# ⚡ UNDER QEMU THE HIGH PLAN MUST AVOID THE ARM BINARY (linked at
#    0x76000000): the free window above 2 GiB starts at 0x81000000 (D2HI
#    default), and the only free window BELOW that's large enough for
#    0x11900000 bytes is 0x01000000. This is why the "bit 31" control runs
#    between D2HI=01000000 and D2HI=81000000: these two layouts differ by
#    EXACTLY ONE BIT on every guest address.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/oracle_arm"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-ident}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (bash tools/build_oracle_arm.sh)"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2.. = layout environment variables
  local LAB="$1"; shift
  local W=/tmp/oracle_${TAG}_$LAB LOG=/tmp/oracle_${TAG}_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  env GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 NATIVECELLLOOP=1 \
    "$@" timeout 7200 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\[emis\]|^signe:|CLEAN EXIT|host signal" "$LOG" | sed 's/^/     /'
}

h()   { grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$1" | tail -1; }
nf()  { grep -oP '\[fbhash\] frames hachees=\K[0-9]+'  "$1" | tail -1; }
arm() { grep -oP '^\[emis\].* arm=\K[0-9]+'            "$1" | tail -1; }
x86() { grep -oP '^\[emis\].* x86=\K[0-9]+'            "$1" | tail -1; }
cln() { grep -c "CLEAN EXIT" "$1"; }

echo "== ORACLE modele a l'identite — MAXFRAMES=$MAXFRAMES =="
echo "-- C  : plan console (compact + arene, membase != 0) --"
run C  D2LAYOUT=compact D2ARENA=12900000
echo "-- C2 : idem (CONTROLE de determinisme) --"
run C2 D2LAYOUT=compact D2ARENA=12900000
echo "-- I  : plan haut sous 2 Gio (identite, membase = 0) --"
run I  D2LAYOUT=haut D2HI=01000000
echo "-- IS : idem + D2_SIGNTAG=1 (discriminant bit 30) --"
run IS D2LAYOUT=haut D2HI=01000000 D2_SIGNTAG=1
echo "-- H  : plan haut AU-DESSUS de 2 Gio (le modele Vita) --"
run H  D2LAYOUT=haut D2HI=81000000
echo "-- HS : idem + D2_SIGNTAG=1 --"
run HS D2LAYOUT=haut D2HI=81000000 D2_SIGNTAG=1

LC=/tmp/oracle_${TAG}_C.log; LC2=/tmp/oracle_${TAG}_C2.log
LI=/tmp/oracle_${TAG}_I.log;  LIS=/tmp/oracle_${TAG}_IS.log
LH=/tmp/oracle_${TAG}_H.log;  LHS=/tmp/oracle_${TAG}_HS.log
echo
printf '  %-3s %-20s %10s %12s %12s %8s\n' jambe empreinte images "x86(o)" "arm(o)" sortie
for J in C C2 I IS H HS; do
  L=/tmp/oracle_${TAG}_$J.log
  printf '  %-3s %-20s %10s %12s %12s %8s\n' "$J" "$(h "$L")" "$(nf "$L")" \
         "$(x86 "$L")" "$(arm "$L")" "$([ "$(cln "$L")" -gt 0 ] && echo PROPRE || echo CRASH)"
done

HC=$(h "$LC"); HC2=$(h "$LC2"); HI=$(h "$LI")
fail=0
[ "${HC:-x}" = "${HC2:-y}" ] || { echo "FAIL: LE BANC N EST PAS DETERMINISTE (C != C2)"; exit 2; }
echo "  controle de determinisme : C == C2 = $HC"
[ "${HC:-x}" = "${HI:-y}" ] || { echo "FAIL: le plan a l'identite diverge du plan console ($HI != $HC)"; fail=1; }
[ "$(h "$LIS")" = "${HC:-y}" ] || { echo "FAIL: D2_SIGNTAG change l'image (jambe IS)"; fail=1; }
grep -q "^signe: knob=1" "$LIS" || { echo "FAIL: TEST VIDE — D2_SIGNTAG non arme cote IS"; fail=1; }
grep -oP '^signe:.*reecrits_bit30=\K[0-9]+' "$LIS" | tail -1 | grep -qvE '^0$' \
  || { echo "FAIL: TEST VIDE — aucun motif reecrit cote IS"; fail=1; }
grep -oP '^signe:.*reecrits_bit30=\K[0-9]+' "$LI" | tail -1 | grep -qE '^0$' \
  || { echo "FAIL: le temoin I a reecrit (jambe polluee)"; fail=1; }
# Cost of the memory model: code emitted with and without membase, same scenario.
AC=$(arm "$LC"); AI=$(arm "$LI")
if [ -n "$AC" ] && [ -n "$AI" ]; then
  echo "  [emis] console $AC o  vs  identite $AI o  =  $((AI-AC)) o, soit $(( (AC-AI)*1000/AC ))/1000 de moins"
fi
# H and HS are EXPECTED to fail: that's the result, not a bug in the bench.
for J in H HS; do
  L=/tmp/oracle_${TAG}_$J.log
  if [ "$(cln "$L")" -gt 0 ]; then
     echo "  ⚠ INATTENDU : la jambe $J (au-dessus de 2 Gio) est allee au bout — a instruire"
  else
     echo "  attendu : jambe $J morte — $(grep -oP 'x86-insn=\K0x[0-9a-f]+' "$L" | tail -1) (idiome du pointeur complemente)"
  fi
done
[ $fail -eq 0 ] && echo "PASS: le modele a l'identite est PIXEL-FIDELE sous 2 Gio ; bit 31 = mortel" || exit 1
