#!/usr/bin/env bash
# tools/oracle_natdraw.sh — ORACLE for native draw serialization
# (D2_GLNATDRAW, d2vGlideDraw: src/glide_ring/glide3x_ring.c + gx_host.cpp).
#
# WHAT IS BEING PROVEN. With D2_GLNATDRAW=1 the DLL no longer copies draw
# vertices into the ring with translated x86: the host writes the record,
# served as a dynarec intrinsic. The claim is that the RING CONTENT is
# unchanged — same records, same bytes.
#
# TWO INDEPENDENT CHECKS:
#   (1) CROSS-ORACLE, bit-exact, per record: D2_GLNATDRAW=2 makes the
#       TRANSLATED DLL re-read every record the host wrote and compare it with
#       its own source bytes (header, mode/count/stride, every vertex byte it
#       would have copied). Counted in the guest ring header and published as
#       `verif=checked/bad` on [ring-final]. Required: checked > 0, bad = 0.
#   (2) GEOMETRY, whole run: the Glide bench is NOT bit-deterministic (the
#       texture-upload count varies between identical controls, which moves
#       atlas cells, u/v, and the ring hash — see oracle_flushfil.sh), but the
#       geometry counters are. Required: the two controls agree on them (bench
#       sanity), and the armed legs match the controls.
#       Past ~1200 frames (in game) even two controls diverge on geometry:
#       keep MAXFRAMES at 1200 for (2); a longer run still exercises (1).
# Also required: natdraw > 0 on the armed legs (a leg that silently kept the
# translated path would pass (2) while proving nothing), 0 on the controls,
# and perdus=0.
#
#   tools/oracle_natdraw.sh [refs-dir]    (BIN, MAXFRAMES, OUT overridable)
# Needs build-arm/oracle_arm (tools/build_oracle_arm.sh) and a fresh
# build-glide/glide3x.dll (tools/build_glide_ring.sh): the refs dir is
# mirrored with symlinks and that DLL substituted, so the refs stay untouched.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REFS="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-1200}"
OUT="${OUT:-/tmp}"
DLL="$ROOT/build-glide/glide3x.dll"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (tools/build_oracle_arm.sh)"; exit 1; }
[ -f "$DLL" ] || { echo "FAIL: $DLL absent (tools/build_glide_ring.sh)"; exit 1; }
DIR="$OUT/ornd_refs"; rm -rf "$DIR"; mkdir -p "$DIR"
for f in "$REFS"/*; do ln -s "$f" "$DIR/$(basename "$f")"; done
rm -f "$DIR/glide3x.dll"; cp "$DLL" "$DIR/glide3x.dll"

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
run() {  # $1 = label, $2... = env
  local LAB="$1"; shift
  local W="$OUT/ornd_$LAB" LOG="$OUT/ornd_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMLOTSHASH=1 \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
F(){ grep -oP "\\[ring-final\\].*? $2=\\K[0-9]+" "$OUT/ornd_$1.log" | tail -1; }
V(){ grep -oP "\\[ring-final\\].*? verif=\\K[0-9]+/[0-9]+" "$OUT/ornd_$1.log" | tail -1; }
G(){ grep -oP "recompositions=\\K[0-9]+" "$OUT/ornd_$1.log" | tail -1; }
GEO(){ echo "$(F $1 dessins)/$(F $1 sommets)/$(F $1 lots)/$(G $1)"; }
P(){ grep -oP "\\[ring-final\\].*" "$OUT/ornd_$1.log" | grep -c . ; }

echo "== ORACLE NATDRAW — MAXFRAMES=$MAXFRAMES (5 passes en parallele) =="
T0=$(date +%s)
run A  D2_GLNATDRAW=0 &
run A2 D2_GLNATDRAW=0 &
run B  D2_GLNATDRAW=1 &
run C  D2_GLNATDRAW=2 &
run D  D2_GLNATDRAW=3 &
wait
echo "== $(( $(date +%s) - T0 ))s =="
for L in A A2 B C D; do
  printf "  %-3s geometrie=%-34s natdraw=%-8s verif=%-12s texup=%s\n" \
    "$L" "$(GEO $L)" "$(F $L natdraw)" "$(V $L)" "$(F $L texup)"; done
rc=0
REF="$(GEO A)"
[ -n "$(F A dessins)" ] || { echo "FAIL: pas de ligne [ring-final] dans A — le chemin Glide/ring n'a pas tourne"; rc=1; }
[ "$(GEO A2)" = "$REF" ] || { echo "FAIL: banc NON deterministe sur la GEOMETRIE (A2 != A) — rien n'est concluable"; rc=1; }
for L in A A2; do [ "$(F $L natdraw)" = 0 ] || { echo "FAIL: temoin $L a servi des dessins natifs (knob ignore)"; rc=1; }; done
for L in B C; do
  [ "$(GEO $L)" = "$REF" ] || { echo "FAIL: $L geometrie differente du temoin — le flux du ring a change"; rc=1; }
  n=$(F $L natdraw); [ -n "$n" ] && [ "$n" -gt 0 ] || { echo "FAIL: $L natdraw=0 — le chemin natif n'a pas tourne (la passe ne prouve rien)"; rc=1; }
done
# C: host-written records; D: translated rep-movs copy (D2_GLNATDRAW=3).
for L in C D; do
  v=$(V $L); chk=${v%/*}; bad=${v#*/}
  [ -n "$v" ] && [ "$chk" -gt 0 ] || { echo "FAIL: $L n'a rien verifie (verif=$v)"; rc=1; }
  [ "${bad:-1}" = 0 ] || { echo "FAIL: $L verif=$v — des enregistrements different de la source"; rc=1; }
done
[ "$(GEO D)" = "$REF" ] || { echo "FAIL: D geometrie differente du temoin"; rc=1; }
[ "$(F D natdraw)" = 0 ] || { echo "FAIL: D a servi des dessins natifs (mode 3 = chemin traduit)"; rc=1; }
chk="$(V C) (hote) + $(V D) (copie traduite rep movs)"
for L in A A2 B C D; do grep -q "perdus=[1-9]" "$OUT/ornd_$L.log" && { echo "FAIL: $L a perdu des enregistrements"; rc=1; }; done
[ $rc = 0 ] && echo "PASS: verifies bit a bit $chk, 0 divergence ; geometrie identique aux temoins ; chemin natif arme"
exit $rc
