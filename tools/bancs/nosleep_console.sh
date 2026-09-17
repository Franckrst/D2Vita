#!/usr/bin/env bash
# tools/bancs/nosleep_console.sh — anti-sleep monitor: sends `nosleep on` to
# VitaCompanion every PERIODE s (default 240), with an FTP probe as a
# fallback; prints "console UP/DOWN" on every state change.
# Run this in the background during a long bench session — the console
# falling asleep mid-run kills the pass.
# Variables: VITA_IP VITA_FTP_PORT VITA_VC_PORT PERIODE
#   e.g. nohup bash tools/bancs/nosleep_console.sh > /tmp/nosleep.log 2>&1 &
set -u
IP="${VITA_IP:-10.113.1.159}"; FTPP="${VITA_FTP_PORT:-1337}"; VCP="${VITA_VC_PORT:-1338}"; PERIODE="${PERIODE:-240}"
prev=""; miss=0
while true; do
  if printf 'nosleep on\n' | timeout 8 nc -q1 "$IP" "$VCP" >/dev/null 2>&1 \
     || timeout 10 curl -s -f "ftp://$IP:$FTPP/ux0:/data/d2vita/" -o /dev/null; then miss=0; cur=UP
  else miss=$((miss+1)); cur=$prev; [ $miss -ge 3 ] && cur=DOWN; fi
  if [ "$cur" != "$prev" ] && [ -n "$cur" ]; then echo "console $cur $(date +%H:%M)"; prev=$cur; fi
  sleep "$PERIODE"
done
