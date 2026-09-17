#!/usr/bin/env bash
# tools/oracle_saveload.sh — ORACLE for loading an EXISTING save.
#
# ⚡ WHY IT EXISTS. The pixel oracle (oracle_*.sh) starts from an EMPTY save
# folder: D2 creates a fresh character there instead. It therefore never
# exercises reading an existing .d2s — exactly the path a change to
# read-ahead (D2_READAHEAD_JUMP / _WIN) can break while still passing every
# other pixel oracle.
#
# Here a REAL .d2s is dropped into the work folder and loaded. The verdict is
# binary: does the game reach the game screen, yes or no.
#
#   oracle_saveload.sh "<knobs>"   e.g.: "D2_READAHEAD_JUMP=8 D2_READAHEAD_WIN=32"
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${REFS:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/oracle_arm"
SRC="${SAVESRC:-$(ls -d "$ROOT"/build-vita/save_secours_* 2>/dev/null | tail -1)}"
CHAR="${CHAR:-ABCDE}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
[ -f "$SRC/$CHAR.d2s" ] || { echo "FAIL: $SRC/$CHAR.d2s absent"; exit 1; }

W=/tmp/oracle_saveload; rm -rf "$W"; mkdir -p "$W/Save"
cp "$ROOT/tools/registry_console.txt" "$W/registry.txt" 2>/dev/null || true
# A SINGLE character: fixed-coordinate selection becomes deterministic.
cp "$SRC/$CHAR.d2s" "$SRC/$CHAR.key" "$W/" 2>/dev/null
echo "  sauvegarde chargée : $CHAR.d2s ($(stat -c%s "$W/$CHAR.d2s") o)"

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,900:move:400:300,950:ldown:400:300,960:lup:400:300"
for f in 1400 1700 2000 2300 2600; do S="$S,$f:keydown:13,$((f+20)):keyup:13"; done
S="$S,3000:move:400:250,3010:ldown:400:250,3020:lup:400:250"

env GAMEEXE=1 D2ARGS="game.exe -3dfx" MAXFRAMES="${MAXFRAMES:-3600}" \
    NATIVECELLLOOP=1 NATIVEDCC=1 NATIVELIGHTMAP=1 \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 FBHASH=1 \
    D2LAYOUT=compact D2ARENA=12900000 D2_GLIDERING=1 D2_GLIDEGXM=1 D2_IOSTAT=1 \
    D2WRITE="$W" D2SCRIPT="$S" ${1:-} \
    timeout 1800 qemu-arm -B 0x10000 "$BIN" "$DIR" > /tmp/oracle_saveload.log 2>&1
rc=$?
echo "  rc=$rc"
echo "  lecture du .d2s : $(grep -c "$CHAR.d2s" /tmp/oracle_saveload.log) accès"
grep -o "SHORT read.*$CHAR.d2s" /tmp/oracle_saveload.log | head -3 | sed 's/^/    /'
E=$(grep -o "empreinte=0x[0-9a-f]*" /tmp/oracle_saveload.log | tail -1)
I=$(grep -o "images=[0-9]*" /tmp/oracle_saveload.log | tail -1)
echo "  $E  $I"
grep -qE "CLEAN EXIT|frames hachees" /tmp/oracle_saveload.log \
  && echo "  VERDICT: le jeu a tourné" || echo "  VERDICT: ARRÊT PRÉMATURÉ"
