#!/usr/bin/env bash
# tools/vita_drive.sh — drive the console remotely (clicks, keys, captures).
#
# The runtime reads $D2CMDFILE while running and dumps the screen as .raw
# into the write folder; both are reachable over FTP. So the game can be
# driven and its result SEEN without touching the console.
#
#   vita_drive.sh cmd  click:400:360     sends a command
#   vita_drive.sh shot                   pulls back the last frame as PNG
#   vita_drive.sh log                    pulls back boot_progress + netlog
#   vita_drive.sh reset                  clears the command file
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IP="${VITA_IP:?export VITA_IP=<console-ip>}"
FTP="ftp://$IP:1337/ux0:"
SAVE="data/d2vita/save"
CMD="$ROOT/build-vita/d2cmd.txt"
OUT="$ROOT/build-vita/shots"; mkdir -p "$OUT"

case "${1:-}" in
  cmd)
    shift
    # The ENTIRE file is re-sent every time: the runtime keeps a byte cursor
    # and only replays APPENDED lines, so resending everything has no side
    # effect and avoids depending on an append on the console side.
    for c in "$@"; do echo "$c" >> "$CMD"; done
    timeout 20 curl -s -f -T "$CMD" "$FTP/$SAVE/../d2cmd.txt" || { echo "FAIL: envoi commandes"; exit 1; }
    echo "envoye: $*" ;;
  reset)
    : > "$CMD"
    timeout 20 curl -s -f -T "$CMD" "$FTP/$SAVE/../d2cmd.txt" && echo "fichier de commandes vide" ;;
  shot)
    LAST=$(timeout 20 curl -s --list-only "$FTP/$SAVE/" 2>/dev/null | tr -d '\r' \
           | grep -E '^d2_frame_[0-9]+_.*\.raw$' \
           | sed -E 's/^d2_frame_([0-9]+)_.*/\1 &/' | sort -n | tail -1 | cut -d' ' -f2)
    [ -n "$LAST" ] || { echo "FAIL: aucune capture sur la console (FBDUMP actif ?)"; exit 1; }
    timeout 60 curl -s -f -o "$OUT/$LAST" "$FTP/$SAVE/$LAST" || { echo "FAIL: rapatriement"; exit 1; }
    PAL=$(echo "$LAST" | sed -E 's/\.raw$/.pal/')
    timeout 30 curl -s -f -o "$OUT/$PAL" "$FTP/$SAVE/$PAL" 2>/dev/null
    python3 "$ROOT/tools/frame2png.py" "$OUT/$LAST" >/dev/null 2>&1
    PNG="${LAST%.raw}.png"
    [ -f "$OUT/$PNG" ] && echo "$OUT/$PNG" || echo "$OUT/$LAST (conversion PNG echouee)" ;;
  log)
    timeout 25 curl -s -f -o /tmp/vita_boot.txt "$FTP/data/d2vita/boot_progress.txt" && echo "boot_progress -> /tmp/vita_boot.txt"
    timeout 25 curl -s -f -o /tmp/vita_net.txt  "$FTP/$SAVE/netlog.txt" && echo "netlog -> /tmp/vita_net.txt" ;;
  *)
    echo "usage: vita_drive.sh {cmd <act:a:b>... | shot | log | reset}"; exit 2 ;;
esac
