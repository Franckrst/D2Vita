#!/usr/bin/env bash
# tools/ab_frame.sh — A/B comparison of FBHASH fingerprint between two binaries.
# Deterministic bench: same scenario and same knobs as the pixel oracles.
# Usage: ab_frame.sh <binary> <label> [refs]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$1"; LAB="$2"; DIR="${3:-$HOME/d2-vita-refs/1.14d}"
MAXFRAMES="${MAXFRAMES:-4000}"
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
W=/tmp/abf_$LAB; LOG=/tmp/abf_$LAB.log
rm -rf "$W"; mkdir -p "$W/Save"
cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
T0=$(date +%s)
GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
  FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" NATIVECELLLOOP=1 \
  D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
  timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
echo "[$LAB] rc=$? $(( $(date +%s) - T0 ))s"
grep -E "^\s*\[fbhash\]|CLEAN EXIT" "$LOG" | sed 's/^/   /'
