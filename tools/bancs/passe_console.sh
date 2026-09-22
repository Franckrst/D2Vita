#!/usr/bin/env bash
# tools/bancs/passe_console.sh — one console bench pass: writes the bench env
# (native baseline + knobs from arguments), deploys it over FTP, launches the
# game (VitaCompanion), waits for the frame counter to stop advancing (3 x 15s)
# or a CLEAN EXIT, pulls back boot_progress -> $D2VITA_WORK/bp_<LAB>.txt.
# Variables: VITA_IP VITA_FTP_PORT VITA_VC_PORT D2VITA_WORK NOCAP=1 GLIDE=1 MAXFRAMES=6200 SCRIPT=
#   e.g. NOCAP=1 GLIDE=1 bash tools/bancs/passe_console.sh C_CLEAR D2_GXMASYNC=2 D2_GXMCLEAR=plat
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IP="${VITA_IP:-10.113.1.159}"; FTPP="${VITA_FTP_PORT:-1337}"; VCP="${VITA_VC_PORT:-1338}"
FTP="ftp://$IP:$FTPP/ux0:"                       # ux0: prefix is REQUIRED (see tools/push_console.sh)
W="${D2VITA_WORK:-$ROOT/build-vita/bancs}"; mkdir -p "$W"
NOCAP="${NOCAP:-1}"; GLIDE="${GLIDE:-1}"; MAXFRAMES="${MAXFRAMES:-6200}"
SCRIPT="${SCRIPT:-$ROOT/tools/bancs/d2script_camp_patrouille.txt}"
TITLE="${VITA_TITLE:-DTWO00001}"
[ $# -ge 1 ] || { echo "usage: passe_console.sh <LAB> [VAR=val ...]"; exit 2; }
LAB="$1"; shift; EXTRA="$*"
ENVF="$W/env_$LAB.txt"; BP="$W/bp_$LAB.txt"
vcmd(){ printf '%s\n' "$1" | timeout 10 nc -q1 "$IP" "$VCP" >/dev/null 2>&1; }

# --- bench env: native baseline + options + knobs ---
{ echo "D2SCRIPT=$(cat "$SCRIPT")"
  echo "D2CMDFILE=ux0:data/d2vita/d2cmd.txt"
  # SCHED= (empty) omits the line and lets the binary's DEFAULT play — that
  # default is native, so SCHED= and SCHED=native pick the same backend. The
  # variable exists to let a pass validate the default itself (a pass that
  # always pins the mode would never test what happens when nobody pins it),
  # and to allow SCHED=coop when needed.
  echo "QUANTUM=20000000"; [ -n "${SCHED-native}" ] && echo "D2SCHED=${SCHED-native}"; echo "D2WRITE=ux0:data/d2vita/save2"
  echo "NATIVECELLLOOP=1"; echo "D2_CALLRET=1"
  echo "D2_NOPUMPWAIT=1"; echo "WX86_FRAMEPROF=1"; echo "MAXFRAMES=$MAXFRAMES"
  [ "$NOCAP" = 1 ] && echo "D2_NOCAP=1"
  [ "$GLIDE" = 1 ] && echo "D2ARGS=game.exe -3dfx"
  for k in $EXTRA; do echo "$k"; done
} > "$ENVF"
timeout 40 curl -s -f -T "$ENVF" "$FTP/data/d2vita/env.txt" || { echo "$LAB: FAIL depot env.txt"; exit 1; }
timeout 30 curl -s -o /dev/null "$FTP/data/d2vita/" -Q "-DELE ux0:/data/d2vita/boot_progress.txt" >/dev/null 2>&1
# VitaCompanion ne lance rien tant qu'une autre appli (VitaShell, un jeu) est au premier plan : on la tue d'abord.
vcmd destroy; sleep 6
vcmd "launch $TITLE"
echo "$LAB lance [$EXTRA] $(date +%H:%M:%S)"

# --- wait: under native+MAXFRAMES a CLEAN EXIT doesn't always happen; wait for the frame counter to stall instead ---
prev=-1; stall=0
for i in $(seq 1 90); do sleep 15
  timeout 30 curl -s "$FTP/data/d2vita/boot_progress.txt" -o "$BP" 2>/dev/null
  n=$(grep -o 'frames=[0-9]*' "$BP" 2>/dev/null | tail -1 | cut -d= -f2); [ -z "$n" ] && n=0
  if [ "$n" = "$prev" ]; then stall=$((stall+1)); else stall=0; fi; prev=$n
  if [ "$stall" -ge 3 ] && [ "$n" -gt 100 ]; then echo "$LAB termine (frames=$n)"; break; fi
  grep -q "CLEAN EXIT" "$BP" 2>/dev/null && { echo "$LAB termine (clean, frames=$n)"; break; }
done
vcmd destroy; sleep 8
echo "$LAB journal -> $BP"
