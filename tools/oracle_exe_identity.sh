#!/usr/bin/env bash
# tools/oracle_exe_identity.sh — HOST-SIDE ORACLE for the Game.exe version
# guard (src/runtime/exe_identity.cpp). Two legs, no console, no game:
#   1. the unit compiles with NO warnings (-Wall -Wextra -Werror);
#   2. tools/tests/exe_identity_test.cpp passes under ASan+UBSan (the unit
#      parses an untrusted file's headers: every bound is exercised).
# When the maintainer's reference binaries are reachable (D2V_REFS or
# ~/d2-vita-refs) the test also runs on the real 1.14d / 1.14b / 1.13c files;
# elsewhere that leg prints SKIPPED — Blizzard binaries never enter the repo.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/exe_identity_test.$$"
rc=0

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/src" -o "$BIN" \
       "$ROOT/tools/tests/exe_identity_test.cpp" "$ROOT/src/runtime/exe_identity.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ]; then "$BIN" || rc=1; else rc=1; fi
rm -f "$BIN"

[ $rc -eq 0 ] && echo "PASS: oracle exe_identity" || echo "FAIL: oracle exe_identity"
exit $rc
