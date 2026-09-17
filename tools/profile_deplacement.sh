#!/usr/bin/env bash
# tools/profile_deplacement.sh — EIP profile of the IN-GAME MOVEMENT phase.
#
# Why a separate bench from profile_rogue.sh: that one profiles the camp over
# 6000 frames, level loading included. Loading is 10x heavier than gameplay
# and happens only once, so it dominates the profile. Here the VALIDATED
# repro scenario is replayed instead (a "god" save, entering the game,
# walking, Rogue <-> Cold Plains waypoint) and only the requested frame
# window is sampled (D2_EIPPROF_FROM/TO), i.e. only while the character walks.
#
#   FEN=3000:9000   -> walking in town (no monsters, no loading)
#   FEN=10500:20000 -> outdoors: Cold Plains, monsters, zone loading
#
# Usage: SAVEDIR=<save folder> [FEN=a:b] [TAG=x] [FRAMES=n] bash tools/profile_deplacement.sh
# Output: $LOG, sections [EIPPROF] (window) and [FRAMEPROF] (slow frames).
set -u; cd "$(dirname "$0")/.."
SAVEDIR="${SAVEDIR:?dossier de sauvegarde (ABC.d2s ABC.key registry.txt)}"
TAG="${TAG:-depl}"; FEN="${FEN:-3000:9000}"; FROM="${FEN%%:*}"; TO="${FEN##*:}"
FRAMES="${FRAMES:-$((TO+400))}"; LOG="${LOG:-/tmp/profil_$TAG.log}"
W=/tmp/d2vita_write_$TAG; rm -rf "$W"; mkdir -p "$W"; cp "$SAVEDIR"/* "$W"/
SC="$(sed -n 's/^SC="\(.*\)"$/\1/p' tools/qemu_halt1420_repro.sh)"
[ -n "$SC" ] || { echo "FAIL: scenario introuvable dans qemu_halt1420_repro.sh"; exit 1; }
echo "== profil deplacement : fenetre $FROM..$TO (run de $FRAMES images) -> $LOG"
env GAMEEXE=1 D2ARGS="game.exe -w" D2LAYOUT=compact D2ARENA=12900000 WLOG=1 \
    MAXSW=60000000 MAXFRAMES="$FRAMES" FBDUMP=100000 D2_ROOMGUARD=1 \
    D2_EIPPROF=1 D2_EIPPROF_FROM="$FROM" D2_EIPPROF_TO="$TO" WX86_FRAMEPROF=1 \
    D2WRITE="$W" D2SCRIPT="$SC" timeout 9000 qemu-arm -B 0x10000 "${BIN:-build-arm/rt_boot_arm}" "$HOME/d2-vita-refs/1.14d" > "$LOG" 2>&1
echo "QEMU EXIT=$?" >> "$LOG"
sed -n '/EIPPROF: fenetre OUVERTE/,/^\[EIPPROF\/EXACT\]/p' "$LOG" | head -20
awk '/\[EIPPROF\/EXACT\]/{f=1} f&&/Game\+0x/{print; if(++n>=25) exit}' "$LOG"
