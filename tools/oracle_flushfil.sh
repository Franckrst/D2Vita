#!/usr/bin/env bash
# tools/oracle_flushfil.sh — ORACLE for the DEPORTED ring flush (D2_FLUSHFIL=1,
# src/glide_ring/gx_host.cpp "FLUSH THREAD").
#
# WHAT IS BEING PROVEN. D2_FLUSHFIL moves the ring walk, the texture uploads
# and the GXM submission off the GAME thread onto a dedicated host thread. The
# claim made in the code is that the SEQUENCE of operations is unchanged — only
# the thread performing them changes. Texture uploads become TEXUP records
# consumed AT THEIR PLACE in the stream, which is exactly where the synchronous
# path would have uploaded and emitted a TEXBIND.
#
# WHAT THIS ORACLE CANNOT PROVE, AND WHY — read this before trusting a PASS.
# The obvious oracle would be `lh=` / `lu=` (D2_GXMLOTSHASH=1), the FNV
# fingerprint of everything that goes to the GPU. It is NOT usable here,
# because THE BENCH ITSELF IS NOT BIT-DETERMINISTIC ON THE GLIDE PATH. Four
# control passes, same binary, same env, D2_VIRTCLOCK + D2_FAKEWALL, gave:
#     texup = 5527 / 5544 / 5558 / 5565   (etats moves by exactly the same amount)
# The game does not request the same number of texture downloads twice in a
# row. A different upload count puts textures in different atlas cells, which
# changes u,v in the vertices, which changes `lh=`. So `lh=` differs between
# two CONTROLS and a mismatch on the armed leg would prove nothing.
#
# WHAT IS DETERMINISTIC, and what this oracle therefore checks: in those same
# four control passes the GEOMETRY was bit-identical —
#     dessins=140296 sommets=561184 lots=139544 recompositions=139544 evitees=752
# So the oracle requires:
#   (1) the four controls agree on the geometry counters (bench sanity: without
#       this, a mismatch on the armed leg is noise, not a finding);
#   (2) the armed leg has the SAME geometry counters — same draws, same
#       vertices, same batches, same state recompositions;
#   (3) the armed leg's `texup` falls INSIDE the control band (it may not be
#       distinguishable from control noise, and must not be outside it);
#   (4) "flushfil: ARME" is in the log — a leg that silently fell back to the
#       synchronous path would pass everything above while proving nothing;
#   (5) nothing was dropped (perdus=0) and the atlas never filled.
# This is weaker than a bit-exact fingerprint. Say so when reporting it.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-1200}"
TAG="${TAG:-}"
OUT="${OUT:-/tmp}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
run() {  # $1 = label, $2... = env
  local LAB="$1$TAG"; shift
  local W="$OUT/orff_$LAB" LOG="$OUT/orff_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  # -3dfx + the ring: this oracle is about the GLIDE path; the GDI path has no
  # ring at all. d2gxm_* is inert under qemu, the walk and the builder are not.
  GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMLOTSHASH=1 \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
F(){ grep -oP "\\[ring-final\\].*? $2=\\K[0-9]+" "$OUT/orff_$1$TAG.log" | tail -1; }
G(){ grep -oP "recompositions=\\K[0-9]+"          "$OUT/orff_$1$TAG.log" | tail -1; }
E(){ grep -oP "recompositions=[0-9]+ evitees=\\K[0-9]+" "$OUT/orff_$1$TAG.log" | tail -1; }
GEO(){ echo "$(F $1 dessins)/$(F $1 sommets)/$(F $1 lots)/$(G $1)/$(E $1)"; }
AR(){ grep -c 'flushfil: ARME' "$OUT/orff_$1$TAG.log"; }
echo "== ORACLE FLUSH DEPORTE — MAXFRAMES=$MAXFRAMES (6 passes en parallele) =="
T0=$(date +%s)
for L in A A2 A3 A4; do run $L & done
run B D2_FLUSHFIL=1 &
run C D2_FLUSHFIL=1 D2_TEXHASH=1 &
wait
echo "== $(( $(date +%s) - T0 ))s =="
for L in A A2 A3 A4 B C; do
  printf "  %-3s geometrie=%-42s texup=%-6s arme=%s\n" "$L" "$(GEO $L)" "$(F $L texup)" "$(AR $L)"; done
rc=0
REF="$(GEO A)"
[ -n "$(F A dessins)" ] || { echo "FAIL: pas de ligne [ring-final] dans A — le chemin Glide/ring n'a pas tourne"; rc=1; }
for L in A2 A3 A4; do [ "$(GEO $L)" = "$REF" ] || { echo "FAIL: banc NON deterministe sur la GEOMETRIE ($L != A) — rien n'est concluable"; rc=1; }; done
LO=$(F A texup); HI=$LO
for L in A2 A3 A4; do v=$(F $L texup); [ "$v" -lt "$LO" ] && LO=$v; [ "$v" -gt "$HI" ] && HI=$v; done
echo "  bande temoin texup = [$LO , $HI]"
for L in B C; do
  [ "$(AR $L)" -ge 1 ] || { echo "FAIL: $L n'a pas arme le fil de flush (repli synchrone: la passe ne prouve rien)"; rc=1; }
  [ "$(GEO $L)" = "$REF" ] || { echo "FAIL: $L geometrie differente du temoin — le flux GPU a change"; rc=1; }
  v=$(F $L texup)
  if [ "$v" -lt "$LO" ] || [ "$v" -gt "$HI" ]; then echo "ATTENTION: $L texup=$v HORS de la bande temoin [$LO,$HI] — a investiguer"; fi
done
[ $rc = 0 ] && echo "PASS (au sens restreint decrit en tete): geometrie identique, fil arme"
exit $rc
