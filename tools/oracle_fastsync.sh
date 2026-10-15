#!/usr/bin/env bash
# tools/oracle_fastsync.sh — ORACLE for D2_FASTSYNC (cs_intrinsic.cpp,
# tick_intrinsic.cpp, engine cpu_box86.cpp set_intrinsic_nogil): Enter/Leave
# CriticalSection and GetTickCount/timeGetTime served lock-free from the
# dynarec's in-line call, before the GIL.
#
# Same deterministic bench as tools/oracle_lightdyn.sh (D2SCHED=coop,
# D2_VIRTCLOCK + D2_FAKEWALL, FBHASH image fingerprint), three passes in
# parallel:
#   A / A2 : D2_FASTSYNC=0 (default is ON since 0.2.2-beta) — bench determinism (A == A2);
#   B      : D2_FASTSYNC=1 — fingerprint must equal A's, the guest clock
#            SEQUENCE too (D2_CLOCKLOG: hash of every value served, in order),
#            and the "[fastsync]" line must show calls actually served
#            lock-free (absent in A: non-vacuity).
# PATROUILLE=1: camp patrol script (more frames in game).
# ⚠️ Under coop there is one runner at a time: this proves the lock-free path
# returns what the GIL path returned, NOT that it is thread-safe. Concurrency
# is proven separately: D2_CSBENCH=2 (contention stress) and D2_CSTEST=1 under
# D2SCHED=native, plus tools/rt_boot_arm_check.sh with D2_FASTSYNC=1.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-}"
OUT="${OUT:-/tmp}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (bash tools/build_oracle_arm.sh)"; exit 1; }
if [ "${PATROUILLE:-0}" = 1 ]; then S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt"); else
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
fi
run() {  # $1 = label, $2... = env
  local LAB="$1$TAG"; shift
  local W="$OUT/orfs_$LAB" LOG="$OUT/orfs_$LAB.log"
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 D2_CLOCKLOG=100000000 \
    env "$@" timeout 5400 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -aoP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/orfs_$1$TAG.log" | tail -1; }
C(){ grep -aoP '\[b5/horloge\] valeurs=[0-9]+ empreinte=0x[0-9a-f]+' "$OUT/orfs_$1$TAG.log" | tail -1; }
F(){ grep -aoP '\[fastsync\] .*' "$OUT/orfs_$1$TAG.log" | tail -1; }
echo "== ORACLE D2_FASTSYNC — MAXFRAMES=$MAXFRAMES patrouille=${PATROUILLE:-0} =="
T0=$(date +%s)
run A  D2_FASTSYNC=0 &
run A2 D2_FASTSYNC=0 &
run B  D2_FASTSYNC=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); CA=$(C A); CA2=$(C A2); CB=$(C B)
NA=$(grep -aoP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/orfs_A$TAG.log" | tail -1)
echo "  A  = ${HA:-ABSENTE}  (frames hachees=${NA:-0})  horloge: ${CA:-ABSENTE}  $(F A)"
echo "  A2 = ${HA2:-ABSENTE}  horloge: ${CA2:-ABSENTE}"
echo "  B  = ${HB:-ABSENTE}  horloge: ${CB:-ABSENTE}  $(F B)"
fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
[ "${CA:-x}" = "${CA2:-y}" ] || { echo "FAIL: sequence d'horloge non deterministe (A != A2)"; fail=1; }
[ "${HA:-x}" = "${HB:-y}" ]  || { echo "FAIL: D2_FASTSYNC=1 change des pixels"; fail=1; }
[ "${CA:-x}" = "${CB:-y}" ]  || { echo "FAIL: D2_FASTSYNC=1 change la sequence d'horloge servie"; fail=1; }
[ -z "$(F A)" ] || { echo "FAIL: A a servi sans GIL alors que l'interrupteur est coupe"; fail=1; }
FE=$(F B | grep -oP 'enter=\K[0-9]+'); FL=$(F B | grep -oP 'leave=\K[0-9]+'); FT=$(F B | grep -oP 'tick=\K[0-9]+')
[ "${FE:-0}" -gt 0 ] && [ "${FL:-0}" -gt 0 ] && [ "${FT:-0}" -gt 0 ] || { echo "FAIL: B n'a rien servi sans GIL (enter=${FE:-0} leave=${FL:-0} tick=${FT:-0})"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: pixels et horloge identiques (A == A2 == B), servis sans GIL : enter=$FE leave=$FL tick=$FT" || exit 1
