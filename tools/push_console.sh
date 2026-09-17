#!/usr/bin/env bash
# push_console.sh — backs up the console's .d2s files, erases them, pushes
# the new eboot and relaunches the game. Mandatory order: COPY before erasing.
#   push_console.sh [IP]      (or export D2VITA_CONSOLE_IP)
set -uo pipefail
IP="${1:-${D2VITA_CONSOLE_IP:?usage: push_console.sh <console-ip>  (or export D2VITA_CONSOLE_IP)}}"
# "ux0:" prefix is REQUIRED: the FTPVita root lists devices, not the contents
# of ux0:. Without it, every command returns "550 Invalid directory" — the
# copy step would copy NOTHING, erasing would erase nothing, and the eboot
# upload would fail, all without the script noticing.
FTP="ftp://$IP:1337/ux0:"
CMD=1338
SAVE="ux0:/data/d2vita/save"
# Paths DERIVED from the script's location, never hardcoded: this repo is
# worked in parallel worktrees, and an absolute path to /repos/d2-vita would
# deploy the eboot from a DIFFERENT worktree than the one just built.
# Overridable if needed.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BK="${BK:-$ROOT/build-vita/console_saves_$(date +%Y%m%d_%H%M%S)}"
EBOOT="${EBOOT:-$ROOT/build-vita/eboot.bin}"
[ -f "$EBOOT" ] || { echo "ECHEC: eboot introuvable ($EBOOT) — construire d'abord (tools/build_rt_boot_vpk.sh)"; exit 1; }

say(){ printf '\n== %s\n' "$*"; }
vcmd(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" "$CMD" >/dev/null 2>&1; }

say "console $IP"
timeout 5 bash -c "exec 3<>/dev/tcp/$IP/1337" 2>/dev/null || { echo "FTP 1337 injoignable — VitaShell est-il en mode USB/FTP ?"; exit 1; }
timeout 5 bash -c "exec 3<>/dev/tcp/$IP/$CMD" 2>/dev/null || echo "(avertissement) VitaCompanion 1338 injoignable — pas de relance auto"

say "arret du jeu"
vcmd destroy; sleep 2

say "sauvegardes existantes"
LIST=$(timeout 20 curl -s --list-only "$FTP/data/d2vita/save/" 2>/dev/null)
echo "$LIST" | grep -icE '\.(d2s|key|map|ma[0-9])$' | xargs -I{} echo "  {} fichiers de personnage"

mkdir -p "$BK"
say "recopie AVANT effacement -> $BK"
n=0
for f in $(echo "$LIST" | grep -iE '\.(d2s|key|map|ma[0-9])$' | tr -d '\r'); do
  if timeout 30 curl -s -o "$BK/$f" "$FTP/data/d2vita/save/$f"; then n=$((n+1)); fi
done
echo "  $n fichiers recopies"
ls -la "$BK" | tail -n +2 | head -20

say "effacement sur la console"
# The DELE command goes through -Q, i.e. OUTSIDE the URL: the "ux0:" prefix
# must be repeated there too. Without it FTPVita returns "550 Invalid
# directory".
d=0
for f in $(echo "$LIST" | grep -iE '\.(d2s|key|map|ma[0-9])$' | tr -d '\r'); do
  if timeout 20 curl -s -o /dev/null "$FTP/data/d2vita/save/" -Q "-DELE ux0:/data/d2vita/save/$f"; then
    d=$((d+1))
  else
    echo "  ECHEC effacement: $f"
  fi
done
echo "  $d fichier(s) reellement efface(s)"
REST=$(timeout 20 curl -s --list-only "$FTP/data/d2vita/save/" 2>/dev/null | grep -icE '\.(d2s|key|map|ma[0-9])$')
echo "  reste $REST fichier(s) de personnage (0 attendu)"

say "envoi de l'eboot ($(stat -c%s "$EBOOT") octets)"
timeout 300 curl -s -T "$EBOOT" "$FTP/app/DTWO00001/eboot.bin" && echo "  televerse"
SZ=$(timeout 20 curl -s -I "$FTP/app/DTWO00001/eboot.bin" 2>/dev/null | tr -d '\r' | grep -i "^Content-Length" | awk '{print $2}')
echo "  taille sur la console : ${SZ:-inconnue} (attendu $(stat -c%s "$EBOOT"))"

say "relance"
vcmd "launch DTWO00001" && echo "  demande de lancement envoyee"
