#!/usr/bin/env bash
# tools/oracle_lightmap.sh — ORACLE for the native dynamic-light PLACEMENT
# (NATIVELIGHTMAP=1, rt_boot.cpp "native port of dynamic LIGHT PLACEMENT",
# Game+0x75420).
#
# Four passes run in parallel on the deterministic bench (D2_VIRTCLOCK + D2_FAKEWALL):
#   A / A2 : control (default) — bench determinism (A == A2);
#   B      : NATIVELIGHTMAP=1 — the image fingerprint must match A's and the
#            "servis" counter must be > 0 (proof it's armed);
#   V      : NATIVELIGHTMAP=1 D2_LIGHTMAPVERIFY=1 — a SPECIFIC oracle: native
#            places the light into a COPY of the grid's 18432 bytes, the guest
#            then places its own, compared byte for byte on EVERY call;
#            requires 0 divergence (and A's fingerprint, since the guest is
#            what served).
# PATROUILLE=1: camp patrol script (console legs) — more lights.
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
  local W="$OUT/orlm_$LAB" LOG="$OUT/orlm_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/orlm_$1$TAG.log" | tail -1; }
echo "== ORACLE POSE DE LUMIERE NATIVE — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} (4 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run B NATIVELIGHTMAP=1 &
run V NATIVELIGHTMAP=1 D2_LIGHTMAPVERIFY=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); HV=$(H V)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/orlm_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}"
echo "  B  = ${HB:-ABSENTE}  (NATIVELIGHTMAP=1)"
echo "  V  = ${HV:-ABSENTE}  (NATIVELIGHTMAP=1 D2_LIGHTMAPVERIFY=1)"
grep -h "^\s*\[lightmap\] natif" "$OUT/orlm_B$TAG.log" "$OUT/orlm_V$TAG.log" | sed 's/^/     /'
grep -h "lightmapverify" "$OUT/orlm_V$TAG.log" | head -5 | sed 's/^/     /'
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: NATIVELIGHTMAP=1 change des pixels"; fail=1; }
[ "${HA:-x}" = "${HV:-y}" ] || { echo "FAIL: D2_LIGHTMAPVERIFY change des pixels"; fail=1; }
SB=$(grep -oP '\[lightmap\] natif: servis=\K[0-9]+' "$OUT/orlm_B$TAG.log" | tail -1)
[ "${SB:-0}" -gt 0 ] || { echo "FAIL: NATIVELIGHTMAP=1 n'a servi aucune pose (armement non prouve)"; fail=1; }
FB=$(grep -oP '\[lightmap\] natif: servis=[0-9]+ replis=\K[0-9]+' "$OUT/orlm_B$TAG.log" | tail -1)
[ "${FB:-0}" -eq 0 ] || echo "  NOTE: ${FB} replis invite en B (voir le journal)"
VN=$(grep -oP '\[lightmap\].*oracle: compares=\K[0-9]+' "$OUT/orlm_V$TAG.log" | tail -1)
VD=$(grep -oP '\[lightmap\].*oracle: compares=[0-9]+ divergences=\K[0-9]+' "$OUT/orlm_V$TAG.log" | tail -1)
[ "${VN:-0}" -gt 0 ] || { echo "FAIL: oracle D2_LIGHTMAPVERIFY n'a compare aucune pose"; fail=1; }
[ "${VD:-1}" -eq 0 ] || { echo "FAIL: D2_LIGHTMAPVERIFY : ${VD:-?} divergences sur ${VN:-0}"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: pose de lumiere native pixel-identique, ${SB} poses servies, oracle croise ${VN} poses / 0 divergence" || exit 1
