#!/usr/bin/env bash
# tools/bancs/profil_dessin_qemu.sh — PER-GUEST-THREAD EIP PROFILE of the Glide
# draw traversal (console path: -3dfx + glide3x.dll ring + GXM sink).
#
# Glide sibling of tools/bancs/profil_fil_io.sh (which profiles the GDI path
# and the I/O thread). Binary built with -DPROF_COUNTERS (build-arm/oracle_prof):
# one EXACT EIP sample per preemption, attributed to the guest thread, plus
# the EXACT count of translated blocks per thread. Virtual clock + frozen wall
# clock = deterministic. Cumulative dumps at frame 3400 (camp, loaded) and at
# the end (patrol).
#
#   OUT=/tmp/profil_dessin REFS=<1.14d with glide3x.dll> bash tools/bancs/profil_dessin_qemu.sh
# Analysis: python3 tools/eip_fils.py $OUT/eip.txt --fil 1 --top 25 --delta
#
# Warning: these are BLOCK counts, not time — cross-check against
# D2_PHASEPROF_X before drawing conclusions.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build-arm/oracle_prof}"
REFS="${REFS:?REFS= (dossier 1.14d avec glide3x.dll) obligatoire}"
OUT="${OUT:-/tmp/profil_dessin}"; mkdir -p "$OUT"
MAXFRAMES="${MAXFRAMES:-6200}"
QUANTUM="${QUANTUM:-8000}"          # /8 = 1000 blocks per slice = 1 sample / 1000 blocks
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (LIBTAG=_prof EXTRA=-DPROF_COUNTERS EXTRA_DEFS=-DPROF_COUNTERS OUTBIN=build-arm/oracle_prof tools/build_oracle_arm.sh)"; exit 1; }
[ -f "$REFS/glide3x.dll" ] || { echo "FAIL: $REFS/glide3x.dll absent"; exit 1; }
S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt")
S="$S,3400:eipdump:12,$((MAXFRAMES-10)):eipdump:12"
S=$(echo "$S" | tr ',' '\n' | sort -t: -k1,1n | paste -sd,)   # rt_boot consumes events in text order
W="$OUT/w"; rm -rf "$W"; mkdir -p "$W/Save"
cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
T0=$(date +%s)
GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=200000000 MAXFRAMES="$MAXFRAMES" \
  D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
  D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
  D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
  D2_GLIDERING=1 D2_GLIDEGXM=1 \
  QUANTUM="$QUANTUM" D2_EIPDUMP="$OUT/eip.txt" \
  env "$@" timeout 7200 qemu-arm -B 0x10000 "$BIN" "$REFS" > "$OUT/prof.log" 2>&1
echo "  [profil-dessin] rc=$?  $(( $(date +%s) - T0 ))s  -> $OUT/eip.txt $OUT/prof.log"
