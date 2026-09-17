#!/usr/bin/env bash
# vita3k_run.sh — install the fresh d2vita.vpk and launch it headless on
# Vita3K under Xvfb :77 (target config: no dev knobs). Backgrounded; the vision
# loop captures the display every 35s from the driver side.
#   vita3k_run.sh install   # extract VPK into ux0/app/DTWO00001 + fresh logs
#   vita3k_run.sh launch     # start Vita3K -r DTWO00001 (bg), pid -> /tmp/v3k.pid
#   vita3k_run.sh cap FILE   # screenshot the display into FILE
#   vita3k_run.sh stop       # SIGTERM Vita3K, wait, SIGKILL fallback
set -uo pipefail
# ROOT is derived from the script's location: a hardcoded path would install
# on Vita3K the VPK from a DIFFERENT worktree than the one just built — a
# bench that lies silently.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TITLE_DEF=DTWO00001
V=~/.local/share/Vita3K/Vita3K
TITLE="${TITLE:-$TITLE_DEF}"
export APPDIR=~/tools/vita3k/squashfs-root APPIMAGE=fake DISPLAY=:77
export PATH="$APPDIR/usr/bin:$PATH"
export LD_LIBRARY_PATH="$APPDIR/usr/lib"
export QT_PLUGIN_PATH="$APPDIR/usr/plugins"

case "${1:-}" in
  install)
    APP="$V/ux0/app/$TITLE"
    rm -rf "$APP"; mkdir -p "$APP"
    7z x -y -o"$APP" "$ROOT/build-vita/d2vita.vpk" >/dev/null && echo "installed VPK -> $APP"
    # fresh diagnostic logs
    D="$V/ux0/data/d2vita"
    rm -f "$D/save/crash.log" "$D/save/boot_progress.txt" "$D/boot_progress.txt" 2>/dev/null
    rm -f ~/.cache/Vita3K/logs/"$TITLE"*.log 2>/dev/null
    echo "cleared crash.log / boot_progress.txt / title logs"
    ;;
  launch)
    # ONE instance only (pkill -x by exact name)
    pkill -x Vita3K 2>/dev/null; sleep 1
    timeout -k 10 900 "$APPDIR/usr/bin/Vita3K" -l 4 -r "$TITLE" >/tmp/v3k_stdout.log 2>&1 &
    echo $! > /tmp/v3k.pid
    echo "Vita3K launched (pid $(cat /tmp/v3k.pid)), title=$TITLE, display=:77"
    ;;
  cap)
    OUT="${2:?need output file}"
    DISPLAY=:77 import -window root "$OUT" 2>/dev/null && echo "captured -> $OUT"
    ;;
  stop)
    pkill -x Vita3K 2>/dev/null; sleep 3; pkill -9 -x Vita3K 2>/dev/null; echo "stopped"
    ;;
  *) echo "usage: $0 {install|launch|cap FILE|stop}"; exit 1;;
esac
