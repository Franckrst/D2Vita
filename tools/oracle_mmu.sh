#!/usr/bin/env bash
# tools/oracle_mmu.sh — PIXEL ORACLE for the two fastmmu levers
# (D2_MMUFOLD and D2_MMUSTACK, contract in
#  third_party/box86-dynarec/dynarec/dynarec_arm_mmu.h).
#
# Five qemu-arm passes STRICTLY identical except for the switches:
#   A  = no knob (old code)
#   A2 = no knob (DETERMINISM CONTROL: without A==A2 the bench isn't an
#        oracle and nothing that follows means anything)
#   F  = D2_MMUFOLD=1
#   P  = D2_MMUSTACK=1  (POP  only: shortens the dependent CHAIN)
#   U  = D2_MMUSTACK=2  (PUSH only: removes ONLY instructions)
#   FS = D2_MMUFOLD=1 D2_MMUSTACK=3 (everything; the sites are disjoint, and
#        this PROVES it)
#
# VERDICT = A==A2, then A==F==S==FS AND non-empty counters on the armed side.
# An armed leg with no folding and no link is an EMPTY PASS.
#
# ⚡ REQUIRES THE CONSOLE MEMORY MODEL. D2LAYOUT=compact D2ARENA=12900000 is
# what vita_present.cpp bakes in for the console. WITHOUT AN ARENA,
# dyn86_membase is 0: MMU_ADDR emits nothing, BOTH levers are inert by
# construction, and the oracle would pass green without having tested anything.
#
# ⚡ DETERMINISM IS MANDATORY: D2_VIRTCLOCK + D2_FAKEWALL + a FRESH write
# folder per leg + a seeded registry_console.txt (without it, zero SCALE
# cells on the qemu side versus many on the console).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/oracle_arm"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-mmu}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = D2_MMUFOLD ("" = absent), $3 = D2_MMUSTACK
  local LAB="$1" KF="$2" KS="$3" W=/tmp/oracle_${TAG}_$1 LOG=/tmp/oracle_${TAG}_$1.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KF" ]; then export D2_MMUFOLD="$KF"; else unset D2_MMUFOLD; fi
  if [ -n "$KS" ]; then export D2_MMUSTACK="$KS"; else unset D2_MMUSTACK; fi
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 NATIVECELLLOOP=1 \
    D2LAYOUT=compact D2ARENA=12900000 \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset D2_MMUFOLD D2_MMUSTACK
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\s*\[fbhash\]|^\[emis\]|^absolus:|^pile:|^pile refus:" "$LOG" | sed 's/^/     /'
}

h()   { grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$1" | tail -1; }
nf()  { grep -oP '\[fbhash\] frames hachees=\K[0-9]+'  "$1" | tail -1; }
arm() { grep -oP '^\[emis\].* arm=\K[0-9]+'            "$1" | tail -1; }
fold(){ grep -oP '^absolus: sites=[0-9]+ pliables=[0-9]+ plies=\K[0-9]+' "$1" | tail -1; }
able(){ grep -oP '^absolus: sites=[0-9]+ pliables=\K[0-9]+' "$1" | tail -1; }
req() { grep -oP '^absolus: sites=\K[0-9]+'            "$1" | tail -1; }
lnk() { grep -oP '^pile: tetes=[0-9]+ maillons=\K[0-9]+' "$1" | tail -1; }
lpo() { grep -oP '^pile: .*\(POP=\K[0-9]+'             "$1" | tail -1; }
lpu() { grep -oP '^pile: .*PUSH=\K[0-9]+'               "$1" | tail -1; }
hd()  { grep -oP '^pile: tetes=\K[0-9]+'               "$1" | tail -1; }

echo "== ORACLE fastmmu — MAXFRAMES=$MAXFRAMES (modele memoire console) =="
echo "-- passe A  : aucun knob --";                             run A  "" ""
echo "-- passe A2 : aucun knob (controle de determinisme) --";  run A2 "" ""
LA=/tmp/oracle_${TAG}_A.log; LA2=/tmp/oracle_${TAG}_A2.log
HA=$(h "$LA"); HA2=$(h "$LA2")
if [ "${HA:-x}" != "${HA2:-y}" ]; then
  echo; echo "  A  = ${HA:-ABSENTE}"; echo "  A2 = ${HA2:-ABSENTE}"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."
  exit 2
fi
echo "  controle de determinisme : A == A2 = $HA  (l oracle est un oracle)"
echo "-- passe F  : D2_MMUFOLD=1 --";                        run F  1 ""
echo "-- passe P  : D2_MMUSTACK=1 (POP seul) --";             run P  "" 1
echo "-- passe U  : D2_MMUSTACK=2 (PUSH seul) --";            run U  "" 2
echo "-- passe FS : D2_MMUFOLD=1 D2_MMUSTACK=3 --";           run FS 1 3
LF=/tmp/oracle_${TAG}_F.log; LP=/tmp/oracle_${TAG}_P.log
LU=/tmp/oracle_${TAG}_U.log; LFS=/tmp/oracle_${TAG}_FS.log

echo
printf '  %-3s %-20s %12s %8s %8s %8s %8s\n' jambe empreinte "arm(o)" plies maillons POP PUSH
for J in A A2 F P U FS; do
  L=/tmp/oracle_${TAG}_$J.log
  printf '  %-3s %-20s %12s %8s %8s %8s %8s\n' "$J" "$(h "$L")" "$(arm "$L")" \
         "$(fold "$L")" "$(lnk "$L")" "$(lpo "$L")" "$(lpu "$L")"
done
AA=$(arm "$LA")
for J in F P U FS; do
  L=/tmp/oracle_${TAG}_$J.log; AX=$(arm "$L")
  if [ -n "$AA" ] && [ -n "$AX" ] && [ "$AA" -gt 0 ]; then
    echo "  $J : code emis $AX o contre $AA o = $((AX-AA)) o, soit $(( (AA-AX)*1000/AA ))/1000 de moins"
  fi
done
echo "  sites d'adressage vus / PLIABLES (verdict pur, publie dans TOUTES les jambes"
echo "  = le reconnaisseur tourne des deux cotes) : A=$(req "$LA")/$(able "$LA")  F=$(req "$LF")/$(able "$LF")"
echo "  tetes de chaine : A=$(hd "$LA") P=$(hd "$LP") U=$(hd "$LU") FS=$(hd "$LFS")"
echo "  (tetes + maillons doit valoir le nombre de sites PUSH/POP r32 traduits dans TOUTES les jambes)"

fail=0
for J in A2 F P U FS; do
  HX=$(h /tmp/oracle_${TAG}_$J.log)
  [ "${HA:-x}" = "${HX:-y}" ] || { echo "FAIL: $J empreinte $HX != A $HA — divergence pixel"; fail=1; }
done
[ "$(nf "$LA")" -ge 360 ] 2>/dev/null || { echo "FAIL: trop peu de frames hachees"; fail=1; }
[ "$(fold "$LF")" -gt 0 ] 2>/dev/null   || { echo "FAIL: TEST VIDE — aucun absolu plie cote F"; fail=1; }
[ "$(lpo "$LP")" -gt 0 ] 2>/dev/null   || { echo "FAIL: TEST VIDE — aucun maillon POP cote P"; fail=1; }
[ "$(lpu "$LP")" = 0 ] 2>/dev/null     || { echo "FAIL: la jambe POP a chaine des PUSH"; fail=1; }
[ "$(lpu "$LU")" -gt 0 ] 2>/dev/null   || { echo "FAIL: TEST VIDE — aucun maillon PUSH cote U"; fail=1; }
[ "$(lpo "$LU")" = 0 ] 2>/dev/null     || { echo "FAIL: la jambe PUSH a chaine des POP"; fail=1; }
[ "$(( $(hd "$LA") ))" = "$(( $(hd "$LFS") + $(lnk "$LFS") ))" ] 2>/dev/null || \
  { echo "FAIL: tetes(A) != tetes+maillons(FS) — le recensement ne boucle pas"; fail=1; }
[ "$(fold "$LA")" = 0 ] 2>/dev/null      || { echo "FAIL: le temoin a plie (jambe A polluee)"; fail=1; }
[ "$(lnk "$LA")" = 0 ] 2>/dev/null      || { echo "FAIL: le temoin a chaine (jambe A polluee)"; fail=1; }
[ "$(able "$LA")" -gt 0 ] 2>/dev/null   || { echo "FAIL: le temoin ne publie pas le verdict pur (pliables=0)"; fail=1; }
[ "$(able "$LA")" = "$(able "$LF")" ] 2>/dev/null || { echo "FAIL: verdict pur different entre temoin et jambe armee"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: empreinte identique dans les 5 jambes, leviers actifs, temoin propre" || exit 1
