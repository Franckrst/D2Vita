#!/usr/bin/env bash
# tools/deploy_eboot.sh — envoie un eboot sur la console sous un NOM TEMPORAIRE,
# compare la taille a l'octet pres, puis bascule (DELE + RNFR/RNTO).
#
# Tant que la bascule n'a pas eu lieu, la console garde un eboot valide : un
# transfert coupe (FTPVita tombe a ~60 Ko/s certains jours, un reboot ou un
# timeout le tronque) ne laisse JAMAIS un eboot.bin tronque sur la console.
# Ne touche ni aux sauvegardes ni a env.txt — contrairement a push_console.sh,
# qui est le protocole de banc (il efface les personnages apres sauvegarde) et
# ecrit directement sur eboot.bin.
#
#   VITA_IP=<ip-console> tools/deploy_eboot.sh [eboot.bin]
#       eboot par defaut : build-vita/eboot.bin du worktree ou vit ce script
#   LAUNCH=1  relance DTWO00001 par VitaCompanion (port 1338) apres la bascule.
#
# Codes de sortie : 0 OK ; 1 usage ; 2 transfert jamais complet (console
# inchangee) ; 3 bascule douteuse (verifier a la main avant de lancer).
set -uo pipefail
IP="${VITA_IP:?export VITA_IP=<ip-console>}"
FTP="ftp://$IP:1337/ux0:"
APP="app/DTWO00001"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EBOOT="${1:-$ROOT/build-vita/eboot.bin}"
[ -f "$EBOOT" ] || { echo "ECHEC: $EBOOT introuvable — construire d'abord (tools/build_rt_boot_vpk.sh)"; exit 1; }
LOCAL=$(stat -c%s "$EBOOT")
HDR=$(head -c4 "$EBOOT" | od -An -tx1 | tr -d ' \n')
[ "$HDR" = "53434500" ] || { echo "ECHEC: $EBOOT n'a pas l'en-tete SCE (53434500), lu $HDR"; exit 1; }

say(){ printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
vcmd(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" 1338 2>&1 | tr -d '\r'; }
# Taille distante d'un fichier de l'app, lue dans le listing FTPVita
# ("-rw-r--r-- 1 vita vita <taille> <date> <nom>") ; vide si absent.
rsize(){ timeout 30 curl -sf "$FTP/$APP/" | tr -d '\r' | awk -v f="$1" '$NF==f{print $5}'; }

say "source $EBOOT ($LOCAL octets, en-tete SCE)"
timeout 5 bash -c "exec 3<>/dev/tcp/$IP/1337" 2>/dev/null || { echo "FTP $IP:1337 injoignable — VitaShell est-il en mode FTP ?"; exit 1; }
say "arret du jeu: $(vcmd destroy)"
# curl -T echoue en silence juste apres un destroy : laisser la console souffler.
sleep 12

ok=0
for essai in 1 2 3; do
  say "essai $essai: envoi de eboot_new.bin"
  if [ -n "$(rsize eboot_new.bin)" ]; then
    timeout 30 curl -s -o /dev/null "$FTP/$APP/" -Q "-DELE ux0:/$APP/eboot_new.bin" && say "  ancien eboot_new.bin supprime"
  fi
  t0=$(date +%s)
  timeout 1500 curl -sf -T "$EBOOT" "$FTP/$APP/eboot_new.bin"; rc=$?
  t1=$(date +%s)
  R=$(rsize eboot_new.bin)
  say "  curl rc=$rc, $((t1-t0)) s, taille distante=${R:-absente} (attendu $LOCAL)"
  if [ "$rc" = 0 ] && [ "$R" = "$LOCAL" ]; then ok=1; break; fi
  sleep 10
done
[ "$ok" = 1 ] || { say "ECHEC: transfert jamais complet, eboot.bin de la console INCHANGE"; exit 2; }

# Le "ux0:" doit etre repete dans les commandes -Q (hors URL), sinon FTPVita
# repond "550 Invalid directory".
say "bascule: DELE eboot.bin"
timeout 30 curl -s -o /dev/null "$FTP/$APP/" -Q "-DELE ux0:/$APP/eboot.bin"; say "  rc=$?"
say "bascule: RNFR eboot_new.bin -> RNTO eboot.bin"
timeout 30 curl -s -o /dev/null "$FTP/$APP/" -Q "-RNFR ux0:/$APP/eboot_new.bin" -Q "-RNTO ux0:/$APP/eboot.bin"; say "  rc=$?"
F=$(rsize eboot.bin); N=$(rsize eboot_new.bin)
say "verification: eboot.bin=${F:-absent} eboot_new.bin=${N:-absent}"
if [ "$F" = "$LOCAL" ] && [ -z "$N" ]; then
  say "DEPLOIEMENT OK"
else
  say "ECHEC de bascule — NE PAS LANCER, verifier a la main (eboot_new.bin est peut-etre encore la)"; exit 3
fi
if [ "${LAUNCH:-0}" = 1 ]; then
  say "lancement: $(vcmd 'launch DTWO00001')"
  say "le journal boot_progress.txt ne tourne qu'apres un moment (~90 s observes) ; reports/session.txt donne alors le build_id"
fi
