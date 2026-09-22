#!/usr/bin/env bash
# tools/oracle_ringwalk.sh — FIDELITY ORACLE for D2_RINGFAST (the ring-walk
# rewrites in src/glide_ring/gx_host.cpp and src/glide_ring/glide_atlas.h).
#
# ---------------------------------------------------------------------------
# WHY THIS ORACLE IS NOT A RUN-TO-RUN HASH COMPARISON
# ---------------------------------------------------------------------------
# The obvious oracle would be: run the bench twice, once with the knob and
# once without, and require `lh=` (D2_GXMLOTSHASH: the FNV of every vertex,
# index and batch field that goes to the GPU) to be identical. It does not
# work here, and the reason is worth writing down rather than discovering
# again:
#
#   THE QEMU BENCH'S GUEST IS NOT DETERMINISTIC ACROSS PROCESSES. Measured on
#   this tree, two CONTROL passes of tools/bancs/banc_ring_qemu.sh (1200
#   frames, same binary, same script, same working directory name) give
#   rigorously identical draw counts — dessins=140296, sommets=561184,
#   lots=139544, recompositions=139544, evitees=752 — but a DIFFERENT number
#   of texture uploads: tele n=5550 vs 5584. Each extra upload shifts which
#   atlas cell a sprite lands in, which shifts every u,v that samples it, so
#   `lh=` and `lu=` differ between two runs of the SAME code. A FAIL read off
#   that comparison would be noise, and a PASS would be luck.
#
# So the proof is moved INSIDE one run, where the input is by construction the
# same for both implementations: D2_RINGVERIFY=1 recomputes, for every single
# draw, the REFERENCE result next to the fast one and compares them.
#   verif-sommets=<divergences>/<draws checked>
#       Builder::verifyRun — every vertex byte (x, y, u, v, argb, pal), every
#       index, plus the vertex counter and the drop counter. Covers bit0 (the
#       vertex loop, the 1/sNorm table, the 1/dim memo) AND bit4 (the frame
#       built straight into the GXM slot: verifyRun compares against wherever
#       the builder actually wrote).
#   verif-liaisons=<divergences>/<lookups>
#       every answer of the open-addressed binding table re-asked of the
#       std::unordered_map it replaces. Covers bit1.
#   verif-mots=<divergences>/<records>
#       bit2's one substantive claim — that the host pointer rp[i] and the
#       masked re-read RD(tail+4*i) name the same bytes.
#   verif-etats=<divergences>/<state records>
#       bit2's cheaper argument fill, compared with the reference fill.
#   verif-prep=<divergences>/<draws>
#       bit0's 1/sNorm table and 1/dim memo, compared field by field with the
#       divisions they replace.
# All five numerators must be 0 and all five denominators must be non-zero:
# a verifier that checked nothing is not a proof.
#
# The cross-run fingerprints are still printed for information, and the
# `gxm:` counters that must never move (reussites=, liaisons manquees=,
# ordre=, repli=, perimees=) are compared between the control and the treated
# leg — knowing that the upload drift can move them by a handful; the script
# says so instead of failing on it.
#
#   bash tools/oracle_ringwalk.sh
#   REFS=<1.14d folder containing glide3x.dll>   MAXFRAMES=<n>   OUT=<dir>
#   MASKS="1 2 4 16 31"    which D2_RINGFAST values to verify
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"
OUT="${OUT:-/tmp/ringwalk}"
MAXFRAMES="${MAXFRAMES:-1200}"
MASKS="${MASKS:-1 2 4 16 31}"
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (tools/build_oracle_arm.sh)"; exit 1; }
[ -f "$REFS/glide3x.dll" ] || { echo "FAIL: $REFS/glide3x.dll absent (tools/build_glide_ring.sh)"; exit 1; }

run() {   # $1 = label, $2... = extra env
  local LAB="$1"; shift
  OUT="$OUT" MAXFRAMES="$MAXFRAMES" REFS="$REFS" \
    bash "$ROOT/tools/bancs/banc_ring_qemu.sh" "$BIN" "$LAB" "$@" >"$OUT/run_$LAB.txt" 2>&1
  sed -n '1p' "$OUT/run_$LAB.txt"
}
fin()   { grep '^\[ring-final\]' "$OUT/log_ring_$1.txt" | tail -1; }
num()   { sed -n "s/.*[ |]$2=\([0-9]*\).*/\1/p" <<<"$(fin "$1")"; }
frac()  { sed -n "s#.* $2=\([0-9]*\)/\([0-9]*\).*#\1 \2#p" <<<"$(fin "$1")"; }
cnt()   { num "$1" "$2"; }

fail=0
echo "== ORACLE D2_RINGFAST — $MAXFRAMES images, patrouille du camp, qemu-arm =="
echo
echo "-- temoin (knob absent) --"
run A
echo "   $(fin A)"

# The bench's own determinism, stated rather than assumed: whatever is equal
# between two controls can be compared later, whatever is not cannot.
if [ -z "${SKIPA2:-}" ]; then
  echo "-- temoin 2 (mesure de la derive du banc) --"
  run A2
  echo "   $(fin A2)"
  for f in dessins sommets lots; do
    x=$(num A "$f"); y=$(num A2 "$f")
    [ "$x" = "$y" ] || { echo "   NOTE: $f derive entre deux temoins ($x vs $y)"; }
  done
  if [ "$(num A texup)" != "$(num A2 texup)" ]; then
    echo "   DERIVE CONNUE: texup=$(num A texup) vs $(num A2 texup) -> lh=/lu= NE SONT PAS"
    echo "                  comparables entre deux passes. La preuve passe par D2_RINGVERIFY."
  fi
fi

echo
for M in $MASKS; do
  L="V$M"
  echo "-- palier D2_RINGFAST=$M + D2_RINGVERIFY=1 --"
  run "$L" D2_RINGFAST="$M" D2_RINGVERIFY=1
  F=$(fin "$L"); echo "   $F"
  # --- armement : le palier demande a-t-il vraiment tourne ? ---
  SEQ=$(num "$L" sequences); NA=$(num "$L" nonalignes)
  ZC=$(sed -n 's#.* sans-recopie=\([0-9]*\)/\([0-9]*\).*#\1#p' <<<"$F")
  [ "${NA:-1}" = "0" ] || { echo "   FAIL: nonalignes=$NA — des dessins sont repasses par l ancienne boucle"; fail=1; }
  if [ $((M & 1)) -ne 0 ] && [ "${SEQ:-0}" = "0" ]; then
    echo "   FAIL: bit0 demande, sequences=0 — TEST VIDE"; fail=1; fi
  if [ $((M & 1)) -eq 0 ] && [ "${SEQ:-0}" != "0" ]; then
    echo "   FAIL: bit0 NON demande mais sequences=$SEQ — le bouton ne coupe pas"; fail=1; fi
  if [ $((M & 16)) -ne 0 ] && [ "${ZC:-0}" = "0" ]; then
    echo "   FAIL: bit4 demande, sans-recopie=0 — TEST VIDE"; fail=1; fi
  if [ $((M & 16)) -eq 0 ] && [ "${ZC:-0}" != "0" ]; then
    echo "   FAIL: bit4 NON demande mais sans-recopie=$ZC — le bouton ne coupe pas"; fail=1; fi
  # --- verificateurs ---
  for V in verif-sommets verif-liaisons verif-mots verif-etats verif-prep; do
    read -r BAD TOT <<<"$(frac "$L" "$V")"
    case "$V:$M" in
      verif-sommets:*)  need=$((M & 1))  ;;   # le verificateur vit DANS le chemin bit0 ;
                                              # bit4 seul est couvert par le palier 31,
                                              # ou verifyRun compare contre le tampon GXM
                                              # lui-meme (pv_), pas contre les vecteurs.
      verif-liaisons:*) need=$((M & 2))  ;;
      verif-mots:*)     need=1           ;;   # toujours verifiable
      verif-etats:*)    need=$((M & 4))  ;;
      verif-prep:*)     need=$((M & 1))  ;;   # la table 1/sNorm contre la division
    esac
    if [ "${BAD:-x}" != "0" ]; then
      echo "   FAIL: $V=${BAD:-?}/${TOT:-?} — DIVERGENCE"; fail=1
    elif [ "$need" -ne 0 ] && [ "${TOT:-0}" = "0" ]; then
      echo "   FAIL: $V=0/0 — le verificateur n a RIEN verifie"; fail=1
    else
      echo "   $V=${BAD}/${TOT} OK"
    fi
  done
  # --- compteurs gxm: qui ne doivent pas bouger ---
  # La CLASSIFICATION des liaisons ordonnees doit etre identique au compte
  # pres (le code rejoue deja les compteurs sur le chemin court pour ca).
  # Elle depend du nombre de televersements, qui derive d une passe a l autre :
  # un ecart est signale, jamais transforme en FAIL silencieux.
  for C in ordre repli perimees hors-atlas recompositions evitees; do
    x=$(cnt A "$C"); y=$(cnt "$L" "$C")
    if [ "$x" = "$y" ]; then echo "   $C=$y identique au temoin"
    else echo "   NOTE: $C=$x (temoin) vs $y (palier) — cf. derive texup du banc"; fi
  done
done

echo
if [ $fail -eq 0 ]; then
  echo "PASS: sur chaque palier arme, les cinq verificateurs rendent 0 divergence"
  echo "      sur un echantillon non vide — le chemin rapide produit les MEMES octets"
  echo "      que le chemin de reference, dessin par dessin."
else
  echo "ECHEC: voir ci-dessus"; exit 1
fi
