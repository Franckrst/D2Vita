#!/usr/bin/env bash
# tools/oracle_lightocc.sh — ORACLE for the native per-light OCCLUSION FIELD
# (NATIVELIGHTOCC=1, native_hooks_cellengine.cpp "NATIVELIGHTOCC", Game+0x750f0).
#
# Four passes run in parallel on the deterministic bench (D2_VIRTCLOCK + D2_FAKEWALL):
#   A / A2 : control (default, knob absent) — bench determinism (A == A2);
#   B      : NATIVELIGHTOCC=1 — the image fingerprint must match A's and the
#            "servis" counter must be > 0 (proof it's armed);
#   V      : NATIVELIGHTOCC=1 D2_LIGHTOCCVERIFY=1 — a SPECIFIC oracle: native
#            rebuilds into a COPY of the 33040-byte scratch span AND a copy of
#            the light's own buffer, the guest then rebuilds its own, compared
#            byte for byte on EVERY call; requires 0 divergence (and A's
#            fingerprint, since in that leg it is the GUEST that served).
# PATROUILLE=1: camp patrol script (console legs) — more lights, more rebuilds.
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
  local W="$OUT/orlo_$LAB" LOG="$OUT/orlo_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/orlo_$1$TAG.log" | tail -1; }
echo "== ORACLE CHAMP D'OCCLUSION NATIF — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} (4 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run B NATIVELIGHTOCC=1 &
run V NATIVELIGHTOCC=1 D2_LIGHTOCCVERIFY=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); HV=$(H V)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/orlo_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}"
echo "  B  = ${HB:-ABSENTE}  (NATIVELIGHTOCC=1)"
echo "  V  = ${HV:-ABSENTE}  (NATIVELIGHTOCC=1 D2_LIGHTOCCVERIFY=1)"
grep -h "^\s*\[lightocc\] natif" "$OUT/orlo_B$TAG.log" "$OUT/orlo_V$TAG.log" | sed 's/^/     /'
grep -h "lightocc: REFUS\|lightoccverify" "$OUT/orlo_B$TAG.log" "$OUT/orlo_V$TAG.log" | head -6 | sed 's/^/     /'
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: NATIVELIGHTOCC=1 change des pixels"; fail=1; }
[ "${HA:-x}" = "${HV:-y}" ] || { echo "FAIL: D2_LIGHTOCCVERIFY change des pixels"; fail=1; }
SB=$(grep -oP '\[lightocc\] natif: servis=\K[0-9]+' "$OUT/orlo_B$TAG.log" | tail -1)
[ "${SB:-0}" -gt 0 ] || { echo "FAIL: NATIVELIGHTOCC=1 n'a servi aucune reconstruction (armement non prouve)"; fail=1; }
CB=$(grep -oP '\[lightocc\] natif: servis=[0-9]+ replis=[0-9]+ cases=\K[0-9]+' "$OUT/orlo_B$TAG.log" | tail -1)
[ "${CB:-0}" -gt 0 ] || { echo "FAIL: 0 case calculee : le corps natif n'a jamais atteint la phase 1"; fail=1; }
FB=$(grep -oP '\[lightocc\] natif: servis=[0-9]+ replis=\K[0-9]+' "$OUT/orlo_B$TAG.log" | tail -1)
[ "${FB:-0}" -eq 0 ] || echo "  NOTE: ${FB} replis invite en B (voir le journal)"
VN=$(grep -oP '\[lightocc\].*oracle: compares=\K[0-9]+' "$OUT/orlo_V$TAG.log" | tail -1)
VD=$(grep -oP '\[lightocc\].*oracle: compares=[0-9]+ divergences=\K[0-9]+' "$OUT/orlo_V$TAG.log" | tail -1)
[ "${VN:-0}" -gt 0 ] || { echo "FAIL: oracle D2_LIGHTOCCVERIFY n'a compare aucune reconstruction"; fail=1; }
[ "${VD:-1}" -eq 0 ] || { echo "FAIL: D2_LIGHTOCCVERIFY : ${VD:-?} divergences sur ${VN:-0}"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: champ d'occlusion natif pixel-identique, ${SB} reconstructions servies (${CB} cases), oracle croise ${VN} reconstructions / 0 divergence" || exit 1
