#!/usr/bin/env bash
# tools/oracle_alloc_site.sh — HOST-SIDE ORACLE for the allocation-failure
# call-site formatter (src/runtime/alloc_site.cpp). No console, no game:
#   1. the unit compiles with NO warnings (-Wall -Wextra -Werror);
#   2. tools/tests/alloc_site_test.cpp passes under ASan+UBSan (the formatter
#      walks a caller-supplied buffer: bounds and the max cap are exercised).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/alloc_site_test.$$"
rc=0

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/src" -o "$BIN" \
       "$ROOT/tools/tests/alloc_site_test.cpp" "$ROOT/src/runtime/alloc_site.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ]; then "$BIN" || rc=1; else rc=1; fi
rm -f "$BIN"

[ $rc -eq 0 ] && echo "PASS: oracle alloc_site" || echo "FAIL: oracle alloc_site"
exit $rc
