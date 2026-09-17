#!/usr/bin/env bash
# tools/bancs/porte_secrets.sh — secrets hygiene gate.
#
# Checks one thing: can a CD key (or a value derived from it) end up in a file
# that gets shared? Targets the files that actually travel: what git tracks,
# what tools/vita_drive.sh pulls back (registry.txt, env.txt,
# boot_progress*.txt, netlog.txt, crash.log), and tools/bncs_local.log, which
# gets pasted into reports.
#
# Never prints what it finds — only where it found it. A pattern is a string,
# never a real value: the repo holds no key, and this file must not introduce
# one either.
#
#   bash tools/bancs/porte_secrets.sh            # repo + known write roots
#   SCAN_DIRS="/tmp/x /tmp/y" bash tools/bancs/porte_secrets.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
fail=0
say(){ printf '  [%s] %s\n' "$1" "$2"; }

# 1) git: no tracked file may be named like a secrets store, or contain the
#    registry value name.
if git ls-files | grep -iE '(^|/)(keystore\.bin|keys\.txt)$|d2vita_secret/' >/dev/null 2>&1; then
  say FAIL "git suit un fichier de magasin de secrets"; fail=1
else say PASS "git ne suit aucun keystore.bin / keys.txt / d2vita_secret/"; fi

# 2) .gitignore covers all three names.
miss=""
for pat in 'keystore.bin' 'keys.txt' 'd2vita_secret/'; do
  grep -qF "$pat" .gitignore || miss="$miss $pat"
done
if [ -n "$miss" ]; then say FAIL ".gitignore ne couvre pas :$miss"; fail=1
else say PASS ".gitignore couvre keystore.bin, keys.txt, d2vita_secret/"; fi

# 3) git-tracked files: no blizzardkey registry VALUE.
#    (the bare name in a comment or in code is fine — it's the value line
#     "...|blizzardkey|...|3|<hex>" that must never appear.)
if git grep -InE '\|blizzardkey\|[^|]*\|[0-9]+\|[0-9a-fA-F]{8,}' -- . >/dev/null 2>&1; then
  say FAIL "une LIGNE de valeur blizzardkey est commitee"; fail=1
else say PASS "aucune ligne de valeur blizzardkey dans les fichiers suivis"; fi

# 4) registry_console.txt (committed, seeds many benches): must hold nothing secret.
if [ -f tools/registry_console.txt ] && grep -qiE 'blizzardkey|keystore' tools/registry_console.txt; then
  say FAIL "tools/registry_console.txt contient une valeur de cle"; fail=1
else say PASS "tools/registry_console.txt sans valeur de cle"; fi

# 5) write roots + local logs: registry.txt, env.txt, boot_progress*.txt,
#    netlog.txt and bncs_local.log must all be clean.
DIRS="${SCAN_DIRS:-/tmp/d2vita_write_bootcheck $ROOT/build-vita $ROOT/tools /tmp}"
hits=0; checked=0
for d in $DIRS; do
  [ -d "$d" ] || continue
  while IFS= read -r f; do
    checked=$((checked+1))
    grep -qi 'blizzardkey' "$f" 2>/dev/null && { say FAIL "valeur de cle dans $f"; hits=$((hits+1)); }
  done < <(find "$d" -maxdepth 3 -type f \( -name 'registry.txt' -o -name 'env*.txt' \
             -o -name 'boot_progress*.txt' -o -name 'netlog*.txt' -o -name 'bncs_local.log' \) 2>/dev/null)
done
if [ $hits -eq 0 ]; then say PASS "aucun blizzardkey dans $checked fichier(s) rapatriables inspecte(s)"
else fail=1; fi

# 6) the private server must not dump packet 0x51 as hex.
if grep -q 'corps MASQUE (cles CD)' tools/bncs_local.py; then
  say PASS "tools/bncs_local.py masque le corps de SID_AUTH_CHECK"
else say FAIL "tools/bncs_local.py vide encore SID_AUTH_CHECK en hexa"; fail=1; fi

# 7) console env.txt: secrets must be REJECTED, not just masked.
#    (searches the pattern across all of src/platform rather than one named
#    file — a check pinned to one file goes stale silently if the reader code
#    moves.)
if grep -rq 'secret interdit dans env.txt' src/platform/; then
  say PASS "le lecteur d'env.txt REFUSE les variables KEY/SECRET/PASS"
else say FAIL "env.txt reemet encore toute variable verbatim dans boot_progress"; fail=1; fi

# 8) pre-existing private-server logs: for every online session they hold the
#    key's public value and its digest in clear text. This gate flags them but
#    does not erase them on its own (user data); `--purge` truncates them.
OLD=""
# Checks every worktree, not just the current one: this log lives next to the
# script, so a fresh worktree has none while an older one might.
WTS=$(git worktree list --porcelain 2>/dev/null | sed -n 's/^worktree //p')
for f in $(printf '%s\n' $WTS | sed 's|$|/tools/bncs_local.log|') /tmp/bncs.log; do
  [ -f "$f" ] && grep -q 'SID_AUTH_CHECK' "$f" 2>/dev/null && OLD="$OLD $f"
done
if [ -n "$OLD" ]; then
  if [ "${1:-}" = "--purge" ]; then
    for f in $OLD; do : > "$f"; done
    say PASS "journaux ANCIENS tronques :$OLD"
  else
    say WARN "journaux ANCIENS a purger (valeur publique + condensat en clair) :$OLD"
    echo "         -> bash tools/bancs/porte_secrets.sh --purge"
  fi
else say PASS "aucun journal ancien porteur de SID_AUTH_CHECK"; fi

[ $fail -eq 0 ] && echo "PASS : hygiene des secrets" || echo "FAIL : hygiene des secrets"
exit $fail
