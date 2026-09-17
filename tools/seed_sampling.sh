#!/usr/bin/env bash
# seed_sampling.sh — estimate the toxic-map-seed rate on the Vita3K load path.
#   seed_sampling.sh <hexseed> [hexseed...]     (~4 min per seed)
# Runs tools/vita3k_seed_test.sh per seed and prints a tally.
# Confirmed a Vita3K-only artifact (allocation-layout dependent on the
# emulator's own JIT), not reproduced on real hardware — not a portage bug.
# Historical sample: ~30% of seeds toxic on Vita3K's load path (10-seed run).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HALT=0; OK=0; OTHER=0
for S in "$@"; do
  bash "$ROOT/tools/vita3k_seed_test.sh" "$S"; rc=$?
  case $rc in 1) HALT=$((HALT+1));; 0) OK=$((OK+1));; *) OTHER=$((OTHER+1));; esac
done
echo "tally: HALT=$HALT OK=$OK OTHER=$OTHER / $#"
