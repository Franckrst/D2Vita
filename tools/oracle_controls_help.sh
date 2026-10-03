#!/usr/bin/env bash
# tools/oracle_controls_help.sh — HOST-SIDE ORACLE for the title-screen Controls
# panel (src/platform/controls_editor.h model + controls_help.h drawing).
# No console, no game. Same test as the CMake target controls_editor_test.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/controls_help_test.$$"
rc=0

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/src" -I "$ROOT/third_party/winx86/src" \
       -o "$BIN" "$ROOT/tests/pad/controls_editor_test.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ] && "$BIN"; then echo "PASS: panneau controles"; else echo "FAIL: panneau controles"; rc=1; fi
rm -f "$BIN"

exit $rc
