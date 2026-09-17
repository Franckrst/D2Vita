#!/usr/bin/env bash
# vita3k_seed_test.sh — load-path seed test on headless Vita3K, TRUSTWORTHY verdict.
#
#   vita3k_seed_test.sh <hexseed> [base.d2s]
#
# Patches <base.d2s> (default: ~/d2-vita-refs/repro-saves/VITA_seed1234abcd_jouable.d2s)
# to the given map seed, seeds it as the live save, launches Vita3K (assumes the
# VPK is already installed — tools/vita3k_headless.sh install) and reports:
#   HALT    — D2 opened Crash.txt (Fog Halt-316 fired; content may be 0 bytes!)
#   OK      — frames climbed past 3600 (town running well beyond the halt point)
#   TIMEOUT — neither within ~4 min
#
# Confirmed a Vita3K-only artifact (allocation-layout dependent on the
# emulator's own JIT), NOT reproduced on real hardware with the same toxic
# seed — not a portage bug. Kept as a Vita3K load-path regression check.
#
# VERDICT RULES (hard-won — a git-bisect on this once converged on the wrong
# commit because of exactly the first pitfall below):
#  * NEVER trust Crash.txt CONTENT or absence: pre-b63b8c7-style failures leave a
#    0-byte Crash.txt (the reporter dies mid-write), and worker threads keep
#    pumping frames/reads after the halt — "reads high + no Crash.txt" is NOT ok.
#  * The halt signature is: frames stall at ~2611 + "file W Crash.txt" event.
#  * A real pass is frames RISING well past 2700 (and visually: the town).
set -u
SEED="${1:?usage: vita3k_seed_test.sh <hexseed> [base.d2s]}"
BASE="${2:-$HOME/d2-vita-refs/repro-saves/VITA_seed1234abcd_jouable.d2s}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$HOME/.local/share/Vita3K/Vita3K"; D="$V/ux0/data/d2vita"

python3 "$ROOT/tools/d2s_seed.py" set "$BASE" "$SEED" "$D/save/VITA.d2s" >/dev/null
rm -f "$D/save/VITA.key" "$D/save/VITA.map" "$D/save/VITA.ma0" \
      "$D/save/Crash.txt" "$D/save/crash.log" "$D/boot_progress.txt"
pkill -x Vita3K 2>/dev/null; sleep 1
bash "$ROOT/tools/vita3k_headless.sh" launch >/dev/null

R="TIMEOUT"
for i in $(seq 1 30); do
  sleep 8
  [ -f "$D/save/Crash.txt" ] && { R="HALT"; break; }
  F=$(grep -oaE 'frames=[0-9]+' "$D/boot_progress.txt" 2>/dev/null | grep -oE '[0-9]+' | sort -n | tail -1)
  [ "${F:-0}" -gt 3600 ] && { R="OK"; break; }
done
pkill -x Vita3K 2>/dev/null; sleep 2; pkill -9 -x Vita3K 2>/dev/null
echo "seed 0x$SEED : $R"
[ "$R" = "HALT" ] && exit 1
[ "$R" = "OK" ] || exit 2
exit 0
