#!/usr/bin/env bash
# tools/bancs/profil_fil_io.sh — PER-GUEST-THREAD PROFILE of Storm's I/O
# thread (Game+0x1a550) under qemu, on the console scenario (camp patrol).
#
# Two binaries, two measurements that cross-check each other:
#   oracle_prof (-DPROF_COUNTERS): PER-THREAD EIP samples on every preemption
#       (short QUANTUM = 1 sample / 1000 blocks) + exact block count per
#       thread; cumulative dumps at frame 2850 (before entering the game),
#       3400 (camp, loaded), end (patrol = sprites moving) -> D2_EIPDUMP.
#   oracle_ep (-DD2_EMITPROF): EXACT execution count per translated block,
#       across all threads, and ARM bytes per block -> D2_EMITCSV.
# Analysis: python3 tools/eip_fils.py <dump> --fil 4 --delta --emit <csv>
# These are BLOCK counts, not time.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
OUT="${OUT:-/tmp/profil_fil_io}"; mkdir -p "$OUT"
MAXFRAMES="${MAXFRAMES:-8300}"
QUANTUM="${QUANTUM:-8000}"       # /8 = 1000 blocks per slice
S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt")
S="$S,2850:eipdump:12,3400:eipdump:12,$((MAXFRAMES-10)):eipdump:12"
# events are consumed in TEXT ORDER (rt_boot injection): sort by frame
S=$(echo "$S" | tr ',' '\n' | sort -t: -k1,1n | paste -sd,)
run() {  # $1 label $2 binary $3... env
  local LAB="$1" B="$2"; shift 2
  local W="$OUT/$LAB"; rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=200000000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 D2_LAZYSEEK=1 \
    D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 7200 qemu-arm -B 0x10000 "$B" "$DIR" > "$OUT/$LAB.log" 2>&1
  echo "  [$LAB] rc=$?"
}
T0=$(date +%s)
LEGS="${LEGS:-prof ep}"
for L in $LEGS; do case $L in
  prof) run prof "$ROOT/build-arm/oracle_prof" QUANTUM="$QUANTUM" D2_EIPDUMP="$OUT/eip.txt" & ;;
  ep)   run ep   "$ROOT/build-arm/oracle_ep"   D2_EMITCSV="$OUT/emit.csv" & ;;
esac; done
wait
echo "duree: $(( $(date +%s) - T0 ))s ; vidages: $OUT/eip.txt $OUT/emit.csv"
