#!/usr/bin/env bash
# tools/bancs/porte_branches.sh — checks whether validated work has actually
# landed in main.
#
# No other bench, doc, or gate looks at the full set of branches, so the
# question "what isn't merged into main yet?" can go unasked even though one
# command answers it in a second. This gate asks it every run, and
# distinguishes two cases a plain branch listing conflates:
#
#   ORPHANED WORK     the branch carries commits whose CONTENT is not in main
#                     (compared by patch fingerprint, not sha: a rebase or
#                     cherry-pick changes the sha, not the content). The
#                     serious case.
#   STALE POINTER     all its commits are already in main by content. Costs
#                     nothing but clutters the list and makes the serious
#                     case harder to spot. Should be deleted.
#
# Verdict: informational below $SEUIL_JOURS, FATAL beyond it. The threshold is
# deliberately generous — a branch still being worked on should not fail the
# gate on other branches' behalf.
#
# A branch deliberately kept out of main is recorded in
# tools/bancs/branches.allow with its REASON. Keep this file short: if it
# grows, that signals a branch-hygiene problem, not a reason to expand it.
#
#   bash tools/bancs/porte_branches.sh              # default threshold (3 days)
#   SEUIL_JOURS=2 bash tools/bancs/porte_branches.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
# 3 days, not 7: a longer threshold would name a stale branch without ever
# turning fatal, so it would never force a decision. Most working branches on
# this repo merge into main the same day; three days leaves room to work
# while still catching real cases. The CURRENT branch is never counted.
SEUIL_JOURS="${SEUIL_JOURS:-3}"
REF="${BRANCHE_REF:-main}"
MAINTENANT=$(date +%s)
ACTUELLE=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
ALLOW="${BRANCHE_ALLOW:-$ROOT/tools/bancs/branches.allow}"

git rev-parse --verify -q "$REF" >/dev/null || { echo "FAIL: pas de branche $REF"; exit 2; }
# The REFERENCE is whichever is further ahead: the local branch or its
# upstream. Across many worktrees nobody updates the local `main` — each one
# pushes `HEAD:main` to origin from its own branch. Comparing against local
# main would flag already-pushed work as orphaned — a gate that wrongly
# accuses is worse than no gate at all.
if git rev-parse --verify -q "origin/$REF" >/dev/null \
   && git merge-base --is-ancestor "$REF" "origin/$REF" 2>/dev/null \
   && [ "$(git rev-parse "$REF")" != "$(git rev-parse "origin/$REF")" ]; then
  echo "  (reference : origin/$REF, en avance sur le $REF local)"
  REF="origin/$REF"
fi

fail=0
nb=0
while read -r b; do
  [ -n "$b" ] || continue
  [ "$b" = "$REF" ] && continue
  # The CURRENT branch is work in progress by construction: named, but not
  # counted against the gate.
  courante=""
  [ "$b" = "$ACTUELLE" ] && courante=" (branche courante)"
  n=$(git rev-list --count "$REF..$b" 2>/dev/null || echo 0)
  [ "${n:-0}" -gt 0 ] || continue
  nb=$((nb+1))
  ts=$(git log -1 --format=%ct "$b" 2>/dev/null || echo "$MAINTENANT")
  jours=$(( (MAINTENANT - ts) / 86400 ))
  # Two checks, because either alone can be wrong.
  # 1) `git cherry` marks "+" a commit whose patch fingerprint is absent from
  #    the reference. It catches cherry-picks and rebases — but it
  #    over-flags: a branch whose work was reimplemented in main keeps a
  #    different patch fingerprint (the patch starts from different content)
  #    and would be wrongly declared orphaned.
  # 2) So the tiebreaker is CONTENT: of the files the branch touched since its
  #    branch point, how many still differ from the reference? Zero means the
  #    work IS in main, whatever the sha.
  orphelins=$(git cherry "$REF" "$b" 2>/dev/null | grep -c '^+' || true)
  # An annotation, not a verdict: of the files the branch touched since its
  # branch point, how many still differ from the reference? Zero is a strong
  # hint the work is already there under another sha — but only a hint, since
  # the reference may have touched those same files for unrelated reasons.
  # The final call stays human; this gate names, it doesn't classify.
  base=$(git merge-base "$REF" "$b" 2>/dev/null)
  touches=$(git diff --name-only "$base" "$b" 2>/dev/null)
  restants=0
  [ -n "$touches" ] && restants=$(git diff --name-only "$REF" "$b" -- $touches 2>/dev/null | wc -l)
  if [ "${restants:-0}" -eq 0 ]; then
    note="aucun de ses fichiers ne differe de $REF — probablement un pointeur PERIME"
  else
    note="$restants fichier(s) different(s) encore de $REF"
  fi
  if [ "${orphelins:-0}" -eq 0 ]; then
    printf '  [PERIME]   %-26s %2d commit(s), %2d j — tous deja dans %s%s\n' \
           "$b" "$n" "$jours" "$REF" "$courante"
  elif [ -f "$ALLOW" ] && grep -qE "^${b}([[:space:]]|$)" "$ALLOW"; then
    printf '  [tolere]   %-26s %2d commit(s) hors de %s, %2d j — %s\n' \
           "$b" "$orphelins" "$REF" "$jours" "$(grep -E "^${b}[[:space:]]" "$ALLOW" | head -1 | cut -c$((${#b}+2))- | cut -c1-90)"
  elif [ "$jours" -ge "$SEUIL_JOURS" ] && [ -z "$courante" ]; then
    printf '  [FAIL]     %-26s %2d commit(s) hors de %s, %2d j (seuil %d) — %s\n' \
           "$b" "$orphelins" "$REF" "$jours" "$SEUIL_JOURS" "$note"
    fail=1
  else
    printf '  [en cours] %-26s %2d commit(s) hors de %s, %2d j%s — %s\n' \
           "$b" "$orphelins" "$REF" "$jours" "$courante" "$note"
  fi
done < <(git for-each-ref --format='%(refname:short)' refs/heads)

[ "$nb" -eq 0 ] && echo "  (aucune branche en avance sur $REF)"
echo "  triage : git log --oneline $REF..<branche> | git diff --stat $REF...<branche>"
[ $fail -eq 0 ] && echo "PASS : aucun travail orphelin au-dela du seuil" \
                || echo "FAIL : du travail valide vit hors de $REF depuis plus de $SEUIL_JOURS jours"
exit $fail
