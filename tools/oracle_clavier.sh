#!/usr/bin/env bash
# tools/oracle_clavier.sh — HOST-SIDE ORACLE for the virtual keyboard.
# Three legs, no console, no game:
#   1. the module compiles with NO warnings (-Wall -Wextra -Werror);
#   2. tools/tests/kb_test.cpp passes under ASan+UBSan (covers all 95
#      printable ASCII characters, drawing clipped to the panel, touch ==
#      draw, 3-state SHIFT, masked mode, echo bounds, inconsistent state);
#   3. SECRET GUARD: no log call ever sees the typed text.
# The rest (the keyboard actually draws on top of the image, on both
# presentation paths) can only be proven on console.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/kb_test.$$"
rc=0
# The virtual keyboard lives in the engine (winx86/src/platform/vita_kb.h),
# not under $ROOT/src/platform/. A grep on a missing file finds nothing, so
# leg 3 would silently report "OK" — a FALSE PASS. This resolves the real
# path and requires the target to exist.
KBH="$ROOT/third_party/winx86/src/platform/vita_kb.h"
[ -f "$KBH" ] || { echo "ECHEC: vita_kb.h introuvable ($KBH) — sous-module winx86 non peuple ?"; exit 1; }

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/third_party/winx86/src" -o "$BIN" "$ROOT/tools/tests/kb_test.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ]; then "$BIN" || rc=1; else rc=1; fi
rm -f "$BIN"

echo "== 3. garde de secret (le texte saisi ne va dans aucun journal) =="
# a) the keyboard header calls NO output function whatsoever.
if grep -nE '\b(printf|fprintf|sprintf|snprintf|puts|fputs|fwrite|d2vita_progress|jpline)\b' \
        "$KBH"; then
  echo "   ECHEC: vita_kb.h contient un appel de sortie"; rc=1
else echo "   OK: vita_kb.h sans appel de sortie"; fi
# b) no line anywhere mixes a log call with the echoed/typed character.
# Diablo's boot config lives in its own file (d2_boot_config.cpp), separate
# from vita_present.cpp. An oracle that scans only one file stops covering
# the other SILENTLY the moment a block moves — so this scans every unit
# involved, and checks that each one still exists: a path that no longer
# resolves to anything would match nothing and produce a false PASS. Add any
# future unit born from the same kind of split here.
KBSRC=("$ROOT/src/platform/vita_present.cpp"
       "$ROOT/src/platform/d2_boot_config.cpp")
for f in "${KBSRC[@]}"; do
  [ -f "$f" ] || { echo "   ECHEC: unite scrutee introuvable: $f"; rc=1; }
done
if grep -nE '(printf|puts|fputs|fwrite|d2vita_progress|jpline)[^;]*\b(echo|g_kb\.echo)\b' \
        "${KBSRC[@]}"; then
  echo "   ECHEC: l'echo du clavier atteint un journal"; rc=1
else echo "   OK: aucun journal ne voit l'echo"; fi

[ $rc -eq 0 ] && echo "PASS: oracle clavier" || echo "FAIL: oracle clavier"
exit $rc
