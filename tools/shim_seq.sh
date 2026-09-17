#!/usr/bin/env bash
# tools/shim_seq.sh — compares the current tree's shim table against another
# revision's. THE control that protects a shim move (Win32 hooks AND native
# hooks installed via set_alternate()+shim_trap() — same table).
#
#   tools/shim_seq.sh                # compare against HEAD (pure regression)
#   tools/shim_seq.sh <git-ref>      # compare against this revision
#   ALLOW_REORDER=1 tools/shim_seq.sh <ref>   # tolerates a block permutation
#                                             # PROVIDED the effective table
#                                             # is identical
#   ALLOW_BODY=<file> ...            # excuses changed bodies, but ONLY the
#                                    # exact transition written there (key,
#                                    # old fingerprint, new) — any further
#                                    # drift becomes fatal again.
#
# Three verdicts, from most to least severe:
#   ENSEMBLE  a key appears/disappears, or its registration count changes.
#             Always fatal: the table doesn't have the same content.
#   EFFECTIF  same key set, but for at least one key a DIFFERENT body wins.
#             Always fatal: register_shim overwrites the key, so this is
#             exactly the bug a block move can create, and tools/torture
#             cannot see it.
#   ORDRE     same effective table, but the sequence differs (block
#             permutation). Fatal by default; ALLOW_REORDER=1 accepts it,
#             which is legitimate for a refactor that moves whole blocks.
#
# Nonzero exit as soon as one verdict isn't green, with the OFFENDING KEY
# named — never a bare "something changed".
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="${1:-HEAD}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

python3 "$ROOT/tools/shim_seq.py"            > "$TMP/cur.txt" 2>"$TMP/cur.err" || {
    echo "FAIL: reconstruction de l'arbre courant impossible"; cat "$TMP/cur.err"; exit 2; }
python3 "$ROOT/tools/shim_seq.py" --ref "$REF" > "$TMP/ref.txt" 2>"$TMP/ref.err" || {
    echo "FAIL: reconstruction de $REF impossible"; cat "$TMP/ref.err"; exit 2; }

ncur=$(wc -l < "$TMP/cur.txt"); nref=$(wc -l < "$TMP/ref.txt")
echo "== shim_seq : $REF ($nref inscriptions) -> arbre courant ($ncur) =="
# An empty report would pass every test that follows: reject it upfront.
if [ "$ncur" -lt 100 ] || [ "$nref" -lt 100 ]; then
    echo "FAIL: sequence invraisemblablement courte — le parseur n'a rien vu"; exit 2
fi

rc=0
# --- ENSEMBLE ---
cut -f1 "$TMP/ref.txt" | LC_ALL=C sort > "$TMP/ref.keys"
cut -f1 "$TMP/cur.txt" | LC_ALL=C sort > "$TMP/cur.keys"
if ! diff -q "$TMP/ref.keys" "$TMP/cur.keys" >/dev/null; then
    echo "ENSEMBLE: DIVERGENT"
    diff "$TMP/ref.keys" "$TMP/cur.keys" | grep -E '^[<>]' | sed 's/^</  disparue: /;s/^>/  apparue : /' | head -40
    rc=2
else
    echo "ENSEMBLE: identique ($ncur inscriptions, cles et multiplicites)"
fi

# --- EFFECTIF: for each key, the body of the LAST registration ---
awk -F'\t' '{ w[$1]=$2 } END { for (k in w) print k"\t"w[k] }' "$TMP/ref.txt" | LC_ALL=C sort > "$TMP/ref.win"
awk -F'\t' '{ w[$1]=$2 } END { for (k in w) print k"\t"w[k] }' "$TMP/cur.txt" | LC_ALL=C sort > "$TMP/cur.win"
# Explicitly documented body transitions: key + BEFORE fingerprint + AFTER
# fingerprint. Nothing else is excused.
# Sentinel: an EMPTY file as awk's first argument would make NR==FNR true for
# the whole second file too — every divergence would then be silently
# "allowed". That's exactly the check that passes without measuring anything;
# the sentinel forbids it.
printf '\t\t\n' > "$TMP/allow"
if [ -n "${ALLOW_BODY:-}" ]; then
    [ -f "$ALLOW_BODY" ] || { echo "FAIL: ALLOW_BODY=$ALLOW_BODY introuvable"; exit 2; }
    grep -vE '^\s*(#|$)' "$ALLOW_BODY" | awk '{print $1"\t"$2"\t"$3}' >> "$TMP/allow"
fi
LC_ALL=C join -t $'\t' -j1 "$TMP/ref.win" "$TMP/cur.win" > "$TMP/pairs" 2>/dev/null
awk -F'\t' 'NR==FNR{a[$1"\t"$2"\t"$3]=1;next} $2!=$3 && !($1"\t"$2"\t"$3 in a){print}' \
    "$TMP/allow" "$TMP/pairs" > "$TMP/badbody"
awk -F'\t' 'NR==FNR{a[$1"\t"$2"\t"$3]=1;next} $2!=$3 && ($1"\t"$2"\t"$3 in a){print}' \
    "$TMP/allow" "$TMP/pairs" > "$TMP/okbody"
comm -3 <(cut -f1 "$TMP/ref.win" | LC_ALL=C sort) <(cut -f1 "$TMP/cur.win" | LC_ALL=C sort) > "$TMP/onlyone"
if [ -s "$TMP/badbody" ] || [ -s "$TMP/onlyone" ]; then
    echo "EFFECTIF: DIVERGENT — le corps gagnant a change pour :"
    awk -F'\t' '{print "  "$1"  ("$2" -> "$3")"}' "$TMP/badbody" | head -40
    sed 's/^/  cle absente d un cote: /' "$TMP/onlyone" | head -10
    rc=2
else
    n=$(wc -l < "$TMP/cur.win"); k=$(wc -l < "$TMP/okbody")
    if [ "$k" -gt 0 ]; then
        echo "EFFECTIF: identique sur $((n-k)) cles ; $k corps modifies, tous documentes dans $ALLOW_BODY :"
        awk -F'\t' '{print "  "$1}' "$TMP/okbody"
    else
        echo "EFFECTIF: identique (meme corps gagnant pour les $n cles)"
    fi
fi

# --- ORDRE ---
# Compares the SEQUENCE OF KEYS, not key+fingerprint lines: the body is
# EFFECTIF's question, not ORDRE's. Comparing fingerprints here would report
# "ORDRE: DIVERGENT (0 zones), premiere cle deplacee :" — zero zones and no
# named key — every time a DOCUMENTED body transition passed EFFECTIF. An
# alert that names nothing and counts zero is an alert people learn to
# ignore; that's how a safety net stops protecting anything.
cut -f1 "$TMP/ref.txt" > "$TMP/ref.keys"; cut -f1 "$TMP/cur.txt" > "$TMP/cur.keys"
if diff -q "$TMP/ref.keys" "$TMP/cur.keys" >/dev/null; then
    echo "ORDRE: identique"
else
    first=$(diff <(cut -f1 "$TMP/ref.txt") <(cut -f1 "$TMP/cur.txt") | grep -E '^[<>]' | head -1 | sed 's/^[<>] //')
    nblk=$(diff <(cut -f1 "$TMP/ref.txt") <(cut -f1 "$TMP/cur.txt") | grep -cE '^[0-9]')   # zones de diff, pas blocs source
    if [ "${ALLOW_REORDER:-0}" = "1" ] && [ "$rc" -eq 0 ]; then
        echo "ORDRE: $nblk zones deplacees, table effective inchangee (ALLOW_REORDER=1)"
        echo "       premiere cle deplacee : $first"
    else
        echo "ORDRE: DIVERGENT ($nblk zones), premiere cle deplacee : $first"
        rc=1
    fi
fi
[ "$rc" -eq 0 ] && echo "OK" || echo "ECHEC (rc=$rc)"
exit $rc
