#!/usr/bin/env bash
# tools/oracle_readahead.sh — PIXEL ORACLE + I/O MEASUREMENT for read-ahead
# (D2_READAHEAD) and the D2_IOSTAT instrument.
#
# Question: does serving the game's reads from a per-handle buffer (lseek+read
# on the descriptor, unbuffered stdio stream) change a SINGLE byte read? An
# offset drift doesn't show up right away — it surfaces as a corrupted sprite
# minutes later: only a pixel oracle on a loaded game can settle it. And how
# many reads still go to the memory card?
#
#   REF = the PREVIOUS binary (build-arm/oracle_arm_REF, if present), no knob
#   A   = current binary, D2_IOSTAT=1 alone (fread path unchanged)
#   A2  = A replayed (determinism control)
#   B<k> = D2_IOSTAT=1 D2_READAHEAD=<k> for each k in LEGS (default "16 64 64m16");
#          a m<j> suffix adds D2_READAHEAD_MIN=<j> (adaptive: jump j KiB -> k KiB)
#   EXTRAENV="D2_LAZYSEEK=1": variables added to A, A2 and B* (not to REF)
# VERDICT = all fingerprints equal (REF == A == A2 == B*) AND B* serve reads
# from RAM (ram=... > 0) AND A has no readahead line at all.
# STRACE=1: each pass runs under strace (host) and
# tools/bancs/strace_io_tally.py counts real read()/lseek() calls per .mpq —
# this measures the stdio amplification the counter itself can't see.
# PATROUILLE=1: camp patrol script (console legs) instead of the oracles'
# short scenario (the fingerprint will then differ from the standard oracle
# scenario's).
# Passes run IN PARALLEL (deterministic bench, time plays no part).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
REFBIN="${REFBIN:-$ROOT/build-arm/oracle_arm_REF}"
MAXFRAMES="${MAXFRAMES:-4000}"
LEGS="${LEGS:-16 64 64m16}"
EXTRAENV="${EXTRAENV:-}"
TAG="${TAG:-}"
OUT="${OUT:-/tmp}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

if [ "${PATROUILLE:-0}" = 1 ]; then S=$(cat "$ROOT/tools/bancs/d2script_camp_patrouille.txt"); else
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
fi

run() {  # $1 = label, $2 = binary, $3... = environment variables
  local LAB="$1$TAG" B="$2"; shift 2
  local W="$OUT/orra_$LAB" LOG="$OUT/orra_$LAB.log" ST=()
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  [ "${STRACE:-0}" = 1 ] && ST=(strace -f -s 0 -e trace=openat,read,pread64,lseek,_llseek,close -o "$OUT/orra_$LAB.strace")
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" timeout 5400 "${ST[@]}" qemu-arm -B 0x10000 "$B" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "$OUT/orra_$1$TAG.log" | tail -1; }

echo "== ORACLE READAHEAD — MAXFRAMES=$MAXFRAMES legs=[$LEGS] extraenv=[$EXTRAENV] strace=${STRACE:-0} patrouille=${PATROUILLE:-0} =="
T0=$(date +%s)
[ -x "$REFBIN" ] && run REF "$REFBIN" D2_IOHIST=1 &
run A  "$BIN" $EXTRAENV D2_IOSTAT=1 &
run A2 "$BIN" $EXTRAENV D2_IOSTAT=1 &
for k in $LEGS; do case "$k" in *m*) run "B$k" "$BIN" $EXTRAENV D2_IOSTAT=1 D2_READAHEAD="${k%%m*}" D2_READAHEAD_MIN="${k#*m}" & ;;
                                 *)   run "B$k" "$BIN" $EXTRAENV D2_IOSTAT=1 D2_READAHEAD="$k" & ;; esac; done
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
fail=0
HA=$(H A); HA2=$(H A2)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "$OUT/orra_A$TAG.log" | tail -1)
echo "  empreinte A  = ${HA:-ABSENTE} (frames hachees=${NA:-0})"
echo "  empreinte A2 = ${HA2:-ABSENTE}"
[ -n "$HA" ] && [ "$HA" = "$HA2" ] || { echo "FAIL: LE BANC N EST PAS DETERMINISTE (A != A2)"; fail=1; }
if [ -x "$REFBIN" ]; then HR=$(H REF); echo "  empreinte REF= ${HR:-ABSENTE} (binaire d'avant)"
  [ "$HR" = "$HA" ] || { echo "FAIL: le binaire courant sans knob differe du binaire d'avant"; fail=1; }; fi
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees"; fail=1; }
[ "$(grep -c 'readahead' "$OUT/orra_A$TAG.log")" -eq 0 ] || { echo "FAIL: readahead a tourne cote A"; fail=1; }
for k in $LEGS; do HB=$(H "B$k"); echo "  empreinte B$k = ${HB:-ABSENTE}"
  [ "$HB" = "$HA" ] || { echo "FAIL: EMPREINTE B$k DIFFERENTE — la lecture anticipee corrompt les lectures"; fail=1; }
  RAM=$(grep -oP 'io\(cumul\).*\| ram=\K[0-9]+' "$OUT/orra_B$k$TAG.log" | tail -1)
  [ "${RAM:-0}" -gt 0 ] || { echo "FAIL: B$k ne sert rien depuis la RAM (test vide)"; fail=1; }
done
echo
echo "== io(cumul) par jambe =="
for L in A $(for k in $LEGS; do echo "B$k"; done); do echo "-- $L"; grep -E 'io\(cumul\)|io/fichiers\(final\)' "$OUT/orra_$L$TAG.log" | sed 's/^/   /'; done
if [ "${STRACE:-0}" = 1 ]; then echo; echo "== hote (strace) : read()/lseek() reels par .mpq =="
  for L in $( [ -x "$REFBIN" ] && echo REF ) A $(for k in $LEGS; do echo "B$k"; done); do echo "-- $L"; python3 "$ROOT/tools/bancs/strace_io_tally.py" "$OUT/orra_$L$TAG.strace"; done; fi
[ $fail -eq 0 ] && echo "PASS: empreintes identiques, lecture anticipee active, temoin propre" || { echo "FAIL"; exit 1; }
