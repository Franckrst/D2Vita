#!/usr/bin/env bash
# tools/oracle_item_assist.sh — HOST-SIDE ORACLE for the item-assist
# (src/platform/item_assist.h). No console, no game.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/item_assist_test.$$"
rc=0

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/src" -I "$ROOT/third_party/winx86/src" \
       -o "$BIN" "$ROOT/tools/tests/item_assist_test.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ]; then "$BIN" || rc=1; else rc=1; fi
rm -f "$BIN"

exit $rc
