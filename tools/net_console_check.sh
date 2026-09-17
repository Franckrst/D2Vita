#!/usr/bin/env bash
# tools/net_console_check.sh — network primitives bench ON THE CONSOLE.
#
# Pushes the current eboot and a test env.txt, launches the game via
# VitaCompanion, then pulls back boot_progress.txt and reports the verdict
# from the [nettest] lines.
#
# NON-DESTRUCTIVE, unlike push_console.sh: no character save is erased. This
# bench only tests the network, it has no reason to touch .d2s files — and a
# test that destroys data for nothing is a test people hesitate to rerun.
#
# EXPLICIT TARGET, never implicit:
#   D2TARGET=local  (default) -> D2BNCS_LOCAL redirects EVERYTHING to the PC server.
#   D2TARGET=officiel         -> no redirection: the console talks to the real
#                                Blizzard servers, with the associated risks.
# A target with consequences this heavy must never be the side effect of an
# incomplete config file — it must always be picked explicitly.
#
#   net_console_check.sh [CONSOLE_IP] [PC_IP]   (or export D2VITA_CONSOLE_IP)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IP="${1:-${D2VITA_CONSOLE_IP:?usage: net_console_check.sh <console-ip> [pc-ip]  (or export D2VITA_CONSOLE_IP)}}"
PC="${2:-$(ip -4 addr show | grep -oP 'inet \K[\d.]+' | grep "^${IP%.*}\." | head -1)}"
# The FTPVita root lists DEVICES (ux0:, ur0:), not the contents of ux0:. A
# path without the "ux0:" prefix returns "550 Invalid directory" — and curl
# reports that via an exit code that's easy to discard unnoticed. Hence the
# explicit prefix everywhere.
FTP="ftp://$IP:1337/ux0:"
CMD=1338
EBOOT="$ROOT/build-vita/eboot.bin"
TITLE=DTWO00001

say(){ printf '\n== %s\n' "$*"; }
vcmd(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" "$CMD" >/dev/null 2>&1; }

[ -n "$PC" ] || { echo "FAIL: pas d'IP PC sur le sous-reseau de $IP"; exit 1; }
[ -f "$EBOOT" ] || { echo "FAIL: $EBOOT absent — lancer build_rt_boot_vpk.sh"; exit 1; }
say "console $IP, serveur PC $PC"

timeout 5 bash -c "exec 3<>/dev/tcp/$IP/1337" 2>/dev/null || { echo "FAIL: FTP 1337 injoignable"; exit 1; }
timeout 5 bash -c "exec 3<>/dev/tcp/$IP/$CMD" 2>/dev/null || { echo "FAIL: VitaCompanion 1338 injoignable"; exit 1; }

# Only one server, otherwise the console talks to the stale one.
if ss -ltn | grep -qE ":6112 "; then
  echo "FAIL: le port 6112 est deja pris — tuer l'ancien bncs_local.py"; exit 1
fi
say "serveur BNCS sur toutes les interfaces (la console doit l'atteindre)"
# MCP_IP / D2GS_IP are ANNOUNCED IN THE PACKETS: after realm selection, the
# server tells the client where to connect next. Left at their 127.0.0.1
# default, a CONSOLE client gets pointed back at itself and fails with
# "unable to connect to the realm server". An announced address must be one
# the CLIENT can reach, never the server's view of itself.
BNCS_HOST=0.0.0.0 MCP_IP="$PC" D2GS_IP="$PC" \
  setsid python3 "$ROOT/tools/bncs_local.py" >/tmp/bncs_console.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true' EXIT
for _ in $(seq 25); do ss -ltn | grep -q ':6112 ' && break; sleep 0.2; done
ss -ltn | grep -q ':6112 ' || { echo "FAIL: le serveur n'ecoute pas"; exit 1; }

say "sauvegarde de l'env.txt de la console AVANT de l'ecraser"
# Contains the user's own settings (perf, diagnostics). Copied first and
# restored on exit no matter what: a network bench shouldn't cost a
# configuration.
BK="$ROOT/build-vita/console_backup"; mkdir -p "$BK"
ENVBK="$BK/env.txt.$(date +%Y%m%d_%H%M%S)"
if timeout 20 curl -s -f -o "$ENVBK" "$FTP/data/d2vita/env.txt" 2>/dev/null; then
  echo "  sauvegarde -> $ENVBK"; cat "$ENVBK"
  trap 'echo; echo "== restauration de env.txt"; timeout 20 curl -s -T "$ENVBK" "$FTP/data/d2vita/env.txt" >/dev/null 2>&1 && echo "  restaure"; kill "$SRV" 2>/dev/null || true' EXIT
else
  echo "  (pas de env.txt sur la console)"
fi

say "env.txt de test"
# An empty D2SCRIPT neutralizes the solo game-entry script baked into the VPK:
# without it, it would drive the console into the Rogue camp instead of
# letting the bench take over.
ENVT=$(mktemp)
TARGET="${D2TARGET:-local}"
case "$TARGET" in
  local)
    REDIR="D2BNCS_LOCAL=$PC"
    echo "  cible: LOCAL — tout connect distant est redirige vers $PC" ;;
  officiel)
    REDIR="# pas de redirection : SERVEURS BLIZZARD OFFICIELS"
    echo "  cible: OFFICIEL — la console parlera aux vrais serveurs Blizzard"
    echo "  (clés CD reelles en jeu, risque de bannissement du compte)" ;;
  *) echo "FAIL: D2TARGET doit valoir 'local' ou 'officiel' (recu: $TARGET)"; exit 1 ;;
esac
cat > "$ENVT" <<ENVEOF
D2NET=1
D2NETTEST=$PC:6112
$REDIR
D2_NETWATCH=1
D2NETLOG=1
D2SCRIPT=
ENVEOF
cat "$ENVT"
timeout 20 curl -s -f -T "$ENVT" "$FTP/data/d2vita/env.txt" || { echo "FAIL: envoi env.txt"; exit 1; }
rm -f "$ENVT"

say "arret du jeu, puis envoi de l'eboot ($(stat -c%s "$EBOOT") octets)"
vcmd destroy; sleep 2
timeout 300 curl -s -f -T "$EBOOT" "$FTP/app/$TITLE/eboot.bin" || { echo "FAIL: envoi eboot"; exit 1; }
SZ=$(timeout 20 curl -s -f -I "$FTP/app/$TITLE/eboot.bin" 2>/dev/null | tr -d '\r' | grep -i "^Content-Length" | awk '{print $2}')
[ "$SZ" = "$(stat -c%s "$EBOOT")" ] || echo "  (avertissement) taille console $SZ != $(stat -c%s "$EBOOT")"

say "lancement"
vcmd "launch $TITLE" || { echo "FAIL: lancement"; exit 1; }

if [ "${D2PLAY:-0}" = "1" ]; then
    # PLAY mode: the console is launched, go play. The server must STAY up and
    # env.txt must NOT be restored right away — the bench itself returns in
    # two minutes, which would cut the server mid-session.
    trap - EXIT
    say "mode JEU — serveur laisse debout (pid $SRV), env.txt NON restaure"
    echo "  journal serveur : /tmp/bncs_console.log"
    echo "  sauvegarde env  : $ENVBK"
    echo "  pour rendre la console a son etat initial :"
    echo "    curl -s -f -T '$ENVBK' '$FTP/data/d2vita/env.txt' && kill $SRV"
    exit 0
fi

say "attente du journal de demarrage"
PROG=$(mktemp)
for i in $(seq 40); do
  sleep 3
  timeout 15 curl -s -f -o "$PROG" "$FTP/data/d2vita/boot_progress.txt" 2>/dev/null
  grep -q "nettest:" "$PROG" 2>/dev/null && break
done

say "journal"
cat "$PROG"

fail=0
# boot_progress lines are PREFIXED with a timestamp "[   0.00s] ", so a ^
# anchor never matches — the checks below grep unanchored on purpose. A
# harness that lies red is as useless as one that lies green.
grep -q "reseau:" "$PROG" || { echo "FAIL: aucune ligne 'reseau:' — l'init n'a pas ete atteinte"; fail=1; }
grep -q "\[nettest\]" "$PROG" || { echo "FAIL: aucune ligne [nettest] — le banc n'a pas tourne"; fail=1; }
grep -qE "\[nettest\].*FAIL" "$PROG" && { echo "FAIL: une primitive a echoue"; fail=1; }
grep -q "nettest: 0 echec" "$PROG" || { echo "FAIL: le banc n'a pas rendu 0 echec"; fail=1; }
echo
say "trafic vu par le serveur"
grep -E "protocole|SID_|ECHEC" /tmp/bncs_console.log | head -10

[ "$fail" -eq 0 ] && echo "net_console_check: OK"
rm -f "$PROG"
exit "$fail"
