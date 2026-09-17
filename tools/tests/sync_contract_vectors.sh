#!/usr/bin/env bash
# tools/tests/sync_contract_vectors.sh — keep tests/crashreport/vectors/ identical
# to the API v1 contract vectors of the D2Vita-website repository.
#
#   tools/tests/sync_contract_vectors.sh [--check] [CONTRACT_DIR]
#
# CONTRACT_DIR defaults to $D2V_CONTRACT, then ~/repos/D2Vita-website/contract.
# --check copies nothing and exits 1 when any vector file differs or is missing.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
check=0
if [ "${1:-}" = "--check" ]; then check=1; shift; fi
contract="${1:-${D2V_CONTRACT:-$HOME/repos/D2Vita-website/contract}}"
src="$contract/vectors"
dst="$ROOT/tests/crashreport/vectors"
[ -d "$src" ] || { echo "contract vectors not found: $src" >&2; exit 2; }
status=0
for f in "$src"/*.json; do
  name="$(basename "$f")"
  if ! cmp -s "$f" "$dst/$name"; then
    if [ "$check" = 1 ]; then echo "STALE: $name" >&2; status=1
    else cp "$f" "$dst/$name"; echo "copied: $name"; fi
  fi
done
for f in "$dst"/*.json; do
  [ -e "$src/$(basename "$f")" ] || { echo "EXTRA (not in contract): $(basename "$f")" >&2; status=1; }
done
[ "$status" = 0 ] && [ "$check" = 1 ] && echo "OK: vectors match $src"
exit "$status"
