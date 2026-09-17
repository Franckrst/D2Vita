#!/usr/bin/env bash
# tools/bancs/banc_coeur2_qemu.sh — qemu bench for the SERVER thread on its own
# core. Real clock, D2SCHED=native,
# scenario is OUT OF CAMP (Rogue <-> Cold Plains waypoint, the one from
# qemu_halt1420_repro.sh): new terrain keeps the D2Game server thread busy,
# unlike camp. Three legs, same binary, same save:
#   F  : threads FREE on the host (the qemu regime of all native gates)
#   M0 : WX86_QEMU_MONOCOEUR=<b>          -> main + runners pinned to ONE host cpu (console topology)
#   M1 : WX86_QEMU_MONOCOEUR=<b> + WX86_COEUR_SERVEUR=1 -> same, server thread on <b>+1
# All with WX86_FILSTAT=1: the "fils:" line publishes per-thread blocks/s,
# traps/s, contended-lock-acquires/s and GIL wait ms/s. Legs run IN PARALLEL on
# DISJOINT host cpus (M0 on 0, M1 on 2-3; F free) — qemu durations are only
# meaningful RELATIVE to legs launched together.
#   SAVEDIR=<ABC.d2s ABC.key registry.txt> [FRAMES=12000] [TAG=x] [BIN=...] bash $0
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAVEDIR="${SAVEDIR:?dossier de sauvegarde (ABC.d2s ABC.key registry.txt)}"
BIN="${BIN:-$ROOT/build-arm/rt_boot_arm}"; FRAMES="${FRAMES:-12000}"; TAG="${TAG:-}"
SRV="${SRV:-}"          # e.g. WX86_COEUR_SERVEUR_ID=4 or WX86_COEUR_SERVEUR_RVA=... (thread identification)
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"; TMO="${TMO:-7200}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
SC="$(sed -n 's/^SC="\(.*\)"$/\1/p' "$ROOT/tools/qemu_halt1420_repro.sh")"
[ -n "$SC" ] || { echo "FAIL: scenario introuvable dans qemu_halt1420_repro.sh"; exit 1; }
run(){ # $1 label, $2... env
  local LAB="$1$TAG"; shift
  local W=/tmp/coeur2_$LAB LOG=/tmp/coeur2_$LAB.log
  rm -rf "$W"; mkdir -p "$W"; cp "$SAVEDIR"/* "$W"/
  local T0=$(date +%s)
  env GAMEEXE=1 D2ARGS="game.exe -w" D2LAYOUT=compact D2ARENA=12900000 WLOG=1 \
      MAXSW=60000000 MAXFRAMES="$FRAMES" FBDUMP=100000 D2_ROOMGUARD=1 WX86_FRAMEPROF=1 \
      D2SCHED=native NATIVECELLLOOP=1 D2_CALLRET=1 D2_NOPUMPWAIT=1 \
      WX86_FILSTAT=1 $SRV "$@" \
      D2WRITE="$W" D2SCRIPT="$SC" timeout "$TMO" qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?  $(( $(date +%s) - T0 ))s  -> $LOG"
  grep -E "coeur2:|\[CreateThread" "$LOG" | head -12 | sed 's/^/     /'
  grep -cE "^\s*fils:" "$LOG" | sed 's/^/     lignes fils: /'
}
echo "== BANC COEUR2 (qemu, horloge reelle, natif) — FRAMES=$FRAMES SRV='$SRV' =="
run F  &
run M0 WX86_QEMU_MONOCOEUR=0 &
run M1 WX86_QEMU_MONOCOEUR=2 WX86_COEUR_SERVEUR=1 &
wait
echo "== fils: (blocs/s ; /t traps/s /c contendues/s /w ms-attente-GIL/s) — 12 dernieres lignes par jambe =="
for L in F M0 M1; do echo "-- $L"; grep -E "^\s*fils:" /tmp/coeur2_$L$TAG.log | tail -12; grep -E "^\[frames:" /tmp/coeur2_$L$TAG.log | tail -4; done
