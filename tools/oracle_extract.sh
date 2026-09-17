#!/usr/bin/env bash
# tools/oracle_extract.sh — locates and extracts the DCC for a sprite named by
# its 5 concatenated tokens (the <sprite> in save2/oracle_<sprite>.out), from
# the reference MPQs.
#
#   oracle_extract.sh <sprite> [output_dir]
#   e.g. oracle_extract.sh FASHbucWLhth        -> <scratch>/FASHbucWLhth.dcc
#        oracle_extract.sh NELGlitTNhth /tmp/x -> /tmp/x/NELGlitTNhth.dcc
#
# <sprite> = token(2) + component(2) + armor class(3) + mode(2) + weapon(3)
#   FASHbucWLhth -> token FA, component SH -> data\global\monsters\FA\SH\FASHbucWLhth.dcc
#   NELGlitTNhth -> token NE (necromancer, PLAYER) -> data\global\chars\NE\LG\...
# Whether a token is a monster, a player or an object isn't known in advance:
# folders are tried in order, and MPQs in the game's own priority order
# (Patch_D2 > d2exp > d2data > d2char). Extraction is BY NAME (MPQ hash,
# -n -N = WITHOUT a listfile, otherwise smpq compares names case-sensitively
# and misses data/global/CHARS/...) — 1.14d's d2data.mpq lists no .dcc at all.
#
# Variables: D2REFS (default ~/d2-vita-refs/1.14d), SMPQ (default smpq).
# Output: path to the .dcc on stdout (last line), details on stderr. rc=1 if not found.
set -uo pipefail
SPRITE="${1:?usage: oracle_extract.sh <sprite> [dossier_sortie]}"
OUTDIR="${2:-${ORACLE_DIR:-/tmp/d2vita_oracle}}"
D2REFS="${D2REFS:-$HOME/d2-vita-refs/1.14d}"
SMPQ="${SMPQ:-smpq}"
command -v "$SMPQ" >/dev/null || { echo "FAIL: $SMPQ introuvable (apt install smpq)" >&2; exit 2; }
[ ${#SPRITE} -ge 4 ] || { echo "FAIL: sprite trop court « $SPRITE »" >&2; exit 2; }
TOKEN="${SPRITE:0:2}"; COMP="${SPRITE:2:2}"
mkdir -p "$OUTDIR"

# Game's own priority order (the first MPQ that has the file wins).
MPQS=()
for n in Patch_D2.mpq patch_d2.mpq d2exp.mpq D2EXP.MPQ d2data.mpq D2DATA.MPQ d2char.mpq D2CHAR.MPQ; do
  [ -f "$D2REFS/$n" ] && MPQS+=("$D2REFS/$n")
done
[ ${#MPQS[@]} -gt 0 ] || { echo "FAIL: aucun MPQ dans $D2REFS" >&2; exit 2; }

# smpq extracts into the CURRENT directory, recreating the folder tree: this
# works in a temp directory then flattens the result into $OUTDIR/<sprite>.dcc.
TMP="$(mktemp -d "$OUTDIR/.x.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
FOUND=""
for mpq in "${MPQS[@]}"; do
  for folder in monsters chars objects missiles overlays; do
    p="data/global/$folder/$TOKEN/$COMP/$SPRITE.dcc"
    if (cd "$TMP" && "$SMPQ" -q -n -N -e "$mpq" "$p" >/dev/null 2>&1) && [ -s "$TMP/$p" ]; then
      FOUND="$mpq|$p"; break 2
    fi
    # paths are case-insensitive on the MPQ side but smpq names the output
    # file exactly as requested: a single attempt is enough.
  done
done
[ -n "$FOUND" ] || { echo "FAIL: $SPRITE.dcc introuvable (token $TOKEN, composant $COMP) dans : ${MPQS[*]}" >&2; exit 1; }
mpq="${FOUND%%|*}"; p="${FOUND#*|}"
OUT="$OUTDIR/$SPRITE.dcc"
mv -f "$TMP/$p" "$OUT"
SZ=$(stat -c %s "$OUT")
SIG=$(head -c1 "$OUT" | od -An -tx1 | tr -d ' ')
[ "$SIG" = "74" ] || echo "ATTENTION: signature $SIG != 74 (pas un DCC ?)" >&2
echo "OK: $SPRITE <- $(basename "$mpq") : $p ($SZ o, signature 0x$SIG)" >&2
echo "$OUT"
