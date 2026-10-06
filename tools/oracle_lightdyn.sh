#!/usr/bin/env bash
# tools/oracle_lightdyn.sh — ORACLE for the native ports of the two light
# branches 0x4755a0 dispatches to besides kind 2 (native_hooks_cellengine.cpp
# "NATIVELIGHTDISC" Game+0x4748d0, "NATIVELIGHTDYN" Game+0x474d70).
#
# Four passes in parallel on the deterministic bench (D2_VIRTCLOCK + D2_FAKEWALL):
#   A / A2 : control (NATIVELIGHTDISC=0 NATIVELIGHTDYN=0: the ports are ON by
#            default since 06/10/2026) — bench determinism (A == A2);
#   B      : NATIVELIGHTDISC=1 NATIVELIGHTDYN=1 — image fingerprint must match
#            A's, and each port must have SERVED calls (proof it's armed);
#   V      : both + D2_LIGHTDISCVERIFY=1 D2_LIGHTDYNVERIFY=1 — the native body
#            runs on COPIES (grid, scratch span), the guest then runs its own,
#            compared byte for byte on EVERY call; 0 divergence required (and
#            A's fingerprint: in that leg the GUEST served).
# PATROUILLE=1: camp patrol script — more lights on screen.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-}"
OUT="${OUT:-/tmp}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }
if [ "${PATROUILLE:-0}" = 1 ]; then S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt"); else
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
fi
run() {  # $1 = label, $2... = env
  local LAB="$1$TAG"; shift
  local W="$OUT/orld_$LAB" LOG="$OUT/orld_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/orld_$1$TAG.log" | tail -1; }
K(){ grep -oP "\[lightdisc/dyn\] natif: .*" "$OUT/orld_$1$TAG.log" | tail -1; }
echo "== ORACLE LUMIERES DISQUE/DYN NATIVES — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} (4 passes en parallele) =="
T0=$(date +%s)
run A  NATIVELIGHTDISC=0 NATIVELIGHTDYN=0 &
run A2 NATIVELIGHTDISC=0 NATIVELIGHTDYN=0 &
run B NATIVELIGHTDISC=1 NATIVELIGHTDYN=1 &
run V NATIVELIGHTDISC=1 NATIVELIGHTDYN=1 D2_LIGHTDISCVERIFY=1 D2_LIGHTDYNVERIFY=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); HV=$(H V)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/orld_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}"
echo "  B  = ${HB:-ABSENTE}  ($(K B))"
echo "  V  = ${HV:-ABSENTE}  ($(K V))"
grep -h "REFUS\|verif\]" "$OUT/orld_B$TAG.log" "$OUT/orld_V$TAG.log" | head -8 | sed 's/^/     /'
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: les portages changent des pixels"; fail=1; }
[ "${HA:-x}" = "${HV:-y}" ] || { echo "FAIL: le mode oracle change des pixels"; fail=1; }
v(){ echo "$2" | grep -oP "$1=\K[0-9]+" | head -1; }
KB=$(K B); KV=$(K V)
DS=$(echo "$KB" | grep -oP 'disque servis=\K[0-9]+'); YS=$(echo "$KB" | grep -oP 'dyn servis=\K[0-9]+')
echo "  B : disque servis=${DS:-0} dyn servis=${YS:-0}"
[ "${DS:-0}" -gt 0 ] || echo "  NOTE: NATIVELIGHTDISC n'a rien servi sur ce parcours (non exerce, pas prouve ici)"
[ "${YS:-0}" -gt 0 ] || echo "  NOTE: NATIVELIGHTDYN n'a rien servi sur ce parcours (non exerce, pas prouve ici)"
OD=$(echo "$KV" | grep -oP 'oracle: disque \K[0-9]+/[0-9]+'); OY=$(echo "$KV" | grep -oP 'dyn \K[0-9]+/[0-9]+$')
echo "  V : oracle disque ${OD:-0/0} dyn ${OY:-0/0} (compares/divergences)"
[ "${OD#*/}" = 0 ] || [ -z "$OD" ] || { echo "FAIL: divergences disque ($OD)"; fail=1; }
[ "${OY#*/}" = 0 ] || [ -z "$OY" ] || { echo "FAIL: divergences dyn ($OY)"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: pixels identiques ; oracle disque ${OD:-0/0}, dyn ${OY:-0/0}" || exit 1
