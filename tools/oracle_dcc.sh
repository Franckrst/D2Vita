#!/usr/bin/env bash
# tools/oracle_dcc.sh — ORACLE for the native DCC decoder (NATIVEDCC=1,
# rt_boot.cpp "native DCC decoder", src/runtime/dcc_native.cpp).
#
# Two proofs, run in parallel, deterministic bench (D2_VIRTCLOCK + D2_FAKEWALL):
#   A / A2 : control (default) — bench determinism (A == A2);
#   B      : NATIVEDCC=1 — the image fingerprint over 4000 frames must match
#            A's (the native decoder produces the same sprites) and the
#            "servis" counter must be > 0 (proof it's armed);
#   V      : NATIVEDCC=1 D2_DCCVERIFY=1 — a SPECIFIC oracle: native decodes
#            into a host buffer, the guest then decodes too, and the block
#            chain, offset table and size are compared byte for byte on
#            EVERY call; requires 0 divergence (and A's fingerprint, since
#            the guest also ran).
# PATROUILLE=1: camp patrol script (console legs) — more sprites.
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
  local W="$OUT/ordcc_$LAB" LOG="$OUT/ordcc_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/ordcc_$1$TAG.log" | tail -1; }
echo "== ORACLE DCC NATIF — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} (4 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run B NATIVEDCC=1 &
run V NATIVEDCC=1 D2_DCCVERIFY=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); HV=$(H V)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/ordcc_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}"
echo "  B  = ${HB:-ABSENTE}  (NATIVEDCC=1)"
echo "  V  = ${HV:-ABSENTE}  (NATIVEDCC=1 D2_DCCVERIFY=1)"
grep -h "^\s*\[dcc\] natif" "$OUT/ordcc_B$TAG.log" "$OUT/ordcc_V$TAG.log" | sed 's/^/     /'
grep -h "DIVERGENCE" "$OUT/ordcc_V$TAG.log" | head -5 | sed 's/^/     /'
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: NATIVEDCC=1 change des pixels"; fail=1; }
[ "${HA:-x}" = "${HV:-y}" ] || { echo "FAIL: D2_DCCVERIFY change des pixels"; fail=1; }
SB=$(grep -oP '\[dcc\] natif: servis=\K[0-9]+' "$OUT/ordcc_B$TAG.log" | tail -1)
[ "${SB:-0}" -gt 0 ] || { echo "FAIL: NATIVEDCC=1 n'a servi aucun decodage (armement non prouve)"; fail=1; }
FB=$(grep -oP '\[dcc\] natif: servis=[0-9]+ replis=\K[0-9]+' "$OUT/ordcc_B$TAG.log" | tail -1)
[ "${FB:-0}" -eq 0 ] || echo "  NOTE: ${FB} replis invite en B (voir le journal)"
VN=$(grep -oP 'oracle: compares=\K[0-9]+' "$OUT/ordcc_V$TAG.log" | tail -1)
VD=$(grep -oP 'oracle: compares=[0-9]+ divergences=\K[0-9]+' "$OUT/ordcc_V$TAG.log" | tail -1)
[ "${VN:-0}" -gt 0 ] || { echo "FAIL: oracle D2_DCCVERIFY n'a compare aucun decodage"; fail=1; }
[ "${VD:-1}" -eq 0 ] || { echo "FAIL: D2_DCCVERIFY : ${VD:-?} divergences sur ${VN:-0}"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: decodeur DCC natif pixel-identique, ${SB} decodages servis, oracle croise ${VN} decodages / 0 divergence" || exit 1
