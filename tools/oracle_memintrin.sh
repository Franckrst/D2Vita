#!/usr/bin/env bash
# tools/oracle_memintrin.sh — ORACLE for the native memcpy/memset intrinsics
# (D2_MEMINTRIN, src/dynarec86/dyn86_memintrin.{h,c}, emitted in
# third_party/box86-dynarec/dynarec/dynarec_arm_pass.c).
#
# Four passes, deterministic bench (D2_VIRTCLOCK + D2_FAKEWALL), in parallel:
#   A / A2 : control (default) — bench determinism (A == A2);
#   P      : D2_MEMINTRIN=2 (PROFILE ONLY) — the helper measures and always
#            returns 0, the guest does all the work: the fingerprint must
#            match A's, proving the emitted prologue (native call + fallback
#            branch) is neutral;
#   B      : D2_MEMINTRIN=1 — memcpy/memset served natively: the fingerprint
#            must STAY A's and "servis" must be > 0 (proof it's armed).
#   V      : D2_MEMINTRIN=1 D2_MEMVERIFY=1 — cross-check oracle, call by call:
#            native writes into a host buffer, the guest writes the real
#            buffer, compared byte for byte on return; 0 divergence required.
# PATROUILLE=1: camp patrol script (sprites moving, console leg).
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
  local W="$OUT/ormi_$LAB" LOG="$OUT/ormi_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/ormi_$1$TAG.log" | tail -1; }
echo "== ORACLE MEMINTRIN — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} (5 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run P D2_MEMINTRIN=2 &
run B D2_MEMINTRIN=1 &
run V D2_MEMINTRIN=1 D2_MEMVERIFY=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HP=$(H P); HB=$(H B); HV=$(H V)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/ormi_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})"
echo "  A2 = ${HA2:-ABSENTE}"
echo "  P  = ${HP:-ABSENTE}  (D2_MEMINTRIN=2, profil seul)"
echo "  B  = ${HB:-ABSENTE}  (D2_MEMINTRIN=1)"
echo "  V  = ${HV:-ABSENTE}  (D2_MEMINTRIN=1 D2_MEMVERIFY=1)"
grep -h "\[memintrin\]" "$OUT/ormi_P$TAG.log" "$OUT/ormi_B$TAG.log" "$OUT/ormi_V$TAG.log" | sed 's/^/     /'
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HP:-y}" ] || { echo "FAIL: le prologue emis (profil seul) change des pixels"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ] || { echo "FAIL: D2_MEMINTRIN=1 change des pixels"; fail=1; }
[ "${HA:-x}" = "${HV:-y}" ] || { echo "FAIL: D2_MEMVERIFY change des pixels"; fail=1; }
SC=$(grep -oP '\[memintrin\] memcpy: appels=[0-9]+ servis=\K[0-9]+' "$OUT/ormi_B$TAG.log" | tail -1)
SS=$(grep -oP 'memset: appels=[0-9]+ servis=\K[0-9]+' "$OUT/ormi_B$TAG.log" | tail -1)
[ "${SC:-0}" -gt 0 ] || { echo "FAIL: aucun memcpy servi (armement non prouve)"; fail=1; }
[ "${SS:-0}" -gt 0 ] || { echo "FAIL: aucun memset servi (armement non prouve)"; fail=1; }
PS=$(grep -oP '\[memintrin\] memcpy: appels=[0-9]+ servis=\K[0-9]+' "$OUT/ormi_P$TAG.log" | tail -1)
[ "${PS:-1}" -eq 0 ] || { echo "FAIL: le mode PROFIL a servi ${PS} appels (il doit toujours replier)"; fail=1; }
VN=$(grep -oP 'oracle: compares=\K[0-9]+' "$OUT/ormi_V$TAG.log" | tail -1)
VD=$(grep -oP 'oracle: compares=[0-9]+ divergences=\K[0-9]+' "$OUT/ormi_V$TAG.log" | tail -1)
[ "${VN:-0}" -gt 0 ] || { echo "FAIL: oracle D2_MEMVERIFY n'a compare aucun appel"; fail=1; }
[ "${VD:-1}" -eq 0 ] || { echo "FAIL: D2_MEMVERIFY : ${VD:-?} divergences sur ${VN:-0}"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: memcpy/memset natifs pixel-identiques (${SC} memcpy + ${SS} memset servis, oracle croise ${VN} appels / 0 divergence)" || exit 1
