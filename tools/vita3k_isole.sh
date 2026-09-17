#!/usr/bin/env bash
# tools/vita3k_isole.sh — ISOLATED Vita3K bench.
#
# WHY. tools/vita3k_headless.sh runs `pkill -x Vita3K`: two jobs measuring at
# the same time kill each other, and the second believes it's measuring after
# having just restarted the first. This script gives a job its OWN pref-path
# and its OWN X server: both instances coexist.
#
# HOW. The isolated pref-path is a tree of SYMLINKS into the shared Vita3K's
# — so the 2 GiB of MPQs are not copied — EXCEPT what a run WRITES: env.txt,
# save/, shaders/, boot_progress.txt, and ux0/app/. It reads the shared
# bench, never writes into it.
#
# ⚠️ Vita3K's COMMAND-LINE VALIDATOR builds its title list from the DEFAULT
# pref-path BEFORE reading -c. A title that only exists in the isolated
# pref-path is therefore rejected (`--installed-path: X not in {...}`).
# `init` works around this by creating an EMPTY folder with the right name in
# the shared tree; it holds nothing, and `purge` removes it.
#
#   V3K2ROOT=/path TITLE=DTWO00077 bash tools/vita3k_isole.sh init
#   VPK=build-vita/d2vita_boot.vpk  ... install
#   DUR=90 ... launch      # then `cap file.png`, `stop`
#   ... purge              # removes the placeholder folder from the shared tree
set -uo pipefail

V1="${V3K1:-$HOME/.local/share/Vita3K/Vita3K}"
V2="${V3K2ROOT:-/tmp/vita3k_isole}"
CFG="$V2.config.yml"
DISP="${V3K2DISPLAY:-:78}"
TITLE="${TITLE:-DTWO00077}"
D="$V2/ux0/data/d2vita"

export APPDIR="${APPDIR:-$HOME/tools/vita3k/squashfs-root}" APPIMAGE=fake DISPLAY="$DISP"
export PATH="$APPDIR/usr/bin:$PATH" LD_LIBRARY_PATH="$APPDIR/usr/lib" QT_PLUGIN_PATH="$APPDIR/usr/plugins"

case "${1:-}" in
  init)
    pgrep -f "Xvfb $DISP" >/dev/null || { nohup Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 2; }
    rm -rf "$V2"; mkdir -p "$V2/ux0/app" "$D/save" "$D/shaders" "$V2/ux0/user"
    for d in data grw0 host0 imc0 os0 pd0 sa0 sd0 shaders-builtin tm0 ud0 uma0 ur0 vd0 vs0 xmc0; do
      [ -e "$V1/$d" ] && ln -s "$V1/$d" "$V2/$d"
    done
    for d in music picture theme video; do [ -e "$V1/ux0/$d" ] && ln -s "$V1/ux0/$d" "$V2/ux0/$d"; done
    cp -r "$V1/ux0/user/." "$V2/ux0/user/" 2>/dev/null
    for f in "$V1"/ux0/data/d2vita/*; do
      b=$(basename "$f")
      case "$b" in env.txt|save|shaders|boot_progress*.txt) continue;; esac
      ln -s "$f" "$D/$b"
    done
    sed "s#^pref-path:.*#pref-path: $V2/#" "$HOME/.config/Vita3K/config.yml" > "$CFG"
    mkdir -p "$V1/ux0/app/$TITLE"          # see the warning at the top
    echo "pref-path isole -> $V2 (X $DISP, config $CFG, titre temoin $TITLE)"
    ;;
  install)
    APP="$V2/ux0/app/$TITLE"; rm -rf "$APP"; mkdir -p "$APP"
    7z x -y -o"$APP" "${VPK:?VPK=... requis}" >/dev/null && echo "installe -> $APP"
    rm -f "$D/save/crash.log" "$D/boot_progress.txt"
    ;;
  launch)
    pkill -f "Vita3K .*-c $CFG" 2>/dev/null; sleep 1
    timeout -k 10 "${DUR:-300}" "$APPDIR/usr/bin/Vita3K" -c "$CFG" -w -l 4 -r "$TITLE" \
      >"$V2.stdout.log" 2>&1 &
    echo "lance pid=$! titre=$TITLE display=$DISP"
    ;;
  cap)  import -window root "${2:?fichier de sortie}" 2>/dev/null && echo "capture -> $2";;
  stop) pkill -f "Vita3K .*-c $CFG" 2>/dev/null; sleep 2; pkill -9 -f "Vita3K .*-c $CFG" 2>/dev/null; echo stop;;
  purge)
    pkill -f "Vita3K .*-c $CFG" 2>/dev/null
    rmdir "$V1/ux0/app/$TITLE" 2>/dev/null && echo "dossier temoin retire de l'arbre partage"
    echo "(le pref-path isole $V2 reste : rm -rf pour le supprimer)"
    ;;
  *) echo "usage: $0 {init|install|launch|cap FICHIER|stop|purge}"; exit 1;;
esac
