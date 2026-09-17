#!/usr/bin/env bash
# tools/bancs/suite_console.sh — runs console bench passes (passe_console.sh)
# in sequence, one per "LAB:knob=val knob=val" argument, logging everything to
# $D2VITA_WORK/$SUITE.log (SUITE=<name>).
# Pass options (NOCAP, GLIDE, MAXFRAMES, VITA_*) are inherited
# from the environment.
#   e.g. SUITE=s_clear NOCAP=1 GLIDE=1 bash tools/bancs/suite_console.sh \
#          "A1:D2_GXMASYNC=2" "B1:D2_GXMASYNC=2 D2_GXMCLEAR=plat" "A2:D2_GXMASYNC=2"
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="${D2VITA_WORK:-$ROOT/build-vita/bancs}"; mkdir -p "$W"
SUITE="${SUITE:-suite}"; L="$W/$SUITE.log"
[ $# -ge 1 ] || { echo "usage: SUITE=<nom> suite_console.sh 'LAB:knobs' ..."; exit 2; }
echo "== SUITE $SUITE $(date '+%F %H:%M') jambes: $*" >> "$L"
for spec in "$@"; do
  lab="${spec%%:*}"; knobs="${spec#*:}"; [ "$knobs" = "$lab" ] && knobs=""
  bash "$ROOT/tools/bancs/passe_console.sh" "$lab" $knobs >> "$L" 2>&1
done
echo "SUITE $SUITE TERMINEE $(date +%H:%M)" >> "$L"
echo "journal: $L"
