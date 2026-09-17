#!/usr/bin/env bash
# tools/oracle_b5.sh — PIXEL AND SEQUENCE ORACLE for the B5 lever.
#
# B5 = two savings on the hottest slot in the binary (the clock):
#   * D2_B5CLOCK: GROUPED trap exit (trap_ret0) — 2 state resolutions per
#     thread per call instead of 4 then 2 (so that many fewer emutls chains on
#     the Vita --disable-tls toolchain);
#   * WX86_B5INDEX: DIRECT index into the intrinsics table instead of linear
#     probing — no more table reads for slots outside the intrinsics window.
#   * WX86_B5=1 arms both. DEFAULT (knobs absent) = OLD PATH.
#
# ⚡ WHY A SEQUENCE ORACLE ON TOP OF THE PIXEL FINGERPRINT.
# A bug on the clock path does NOT show up in pixels: it shows up as desync
# (sprite-cache LRU, network timers, pacing). Both legs must therefore return
# the SAME ORDERED SEQUENCE of values to the game, not just the same image.
# D2_CLOCKLOG=N hashes the first N values the intrinsic returns and prints
# the first 32 in the clear: that hash, and those 32 values, are what gets
# diffed here.
#
# The COOP scheduler is used here because it is the only deterministic one,
# so the only one where a pixel fingerprint proves anything. GetTickCount
# always reaches the trap, hence the intrinsic, hence the code B5 modifies —
# nothing rewrites the IAT any more, so both legs carry it identically and
# the oracle can never be silently empty.
#
# Three qemu-arm passes STRICTLY identical except for the knob:
#   A  = knobs absent (old path)
#   A2 = knobs absent (bench DETERMINISM CONTROL)
#   B  = WX86_B5=1
# VERDICT = A == A2 (the bench is an oracle), then A == B on THREE things:
# pixel fingerprint, clock-sequence fingerprint, first 32 values.
# NON-EMPTY TEST: servis>0 in all three passes, and the arming line must say
# "non/non" in A/A2 and "OUI/OUI" in B — otherwise leg B is inert.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${ORACLE_BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2 = WX86_B5 value ("" = absent)
  local LAB="$1" KNOB="$2" W=/tmp/ob5_$1 LOG=/tmp/ob5_$1.log
  # FRESH WRITE FOLDER PER LEG: a reused folder inherits the arena (saves,
  # registry, cache) from the previous pass and manufactures false
  # divergences.
  rm -rf "$W"; mkdir -p "$W/Save"
  # SEED THE CONSOLE REGISTRY: without it the bench starts with no persisted
  # video settings and does NOT cover the same code (zero SCALE cells on the
  # qemu side).
  [ -z "${NOREG:-}" ] && cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"
  local T0=$(date +%s)
  if [ -n "$KNOB" ]; then export WX86_B5="$KNOB"; else unset WX86_B5; fi
  # DETERMINISM IS MANDATORY: without D2_VIRTCLOCK + D2_FAKEWALL, two passes
  # of the SAME binary return different fingerprints, and a FAIL can no
  # longer distinguish "the code diverges" from "the bench diverges".
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" NATIVECELLLOOP=1 \
    INLINEHOT=0 D2_CLOCKLOG=100000000 \
    D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    timeout 2700 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  local RC=$?
  unset WX86_B5
  echo "  [$LAB] rc=$RC  $(( $(date +%s) - T0 ))s"
  grep -E "^\[b5|^\s*\[fbhash\]|CLEAN EXIT" "$LOG" | sed 's/^/     /'
}
H(){  grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+'          "/tmp/ob5_$1.log" | tail -1; }
CH(){ grep -oP '\[b5/horloge\] valeurs=[0-9]+ empreinte=\K0x[0-9a-f]+' "/tmp/ob5_$1.log" | tail -1; }
CN(){ grep -oP '\[b5/horloge\] valeurs=\K[0-9]+'              "/tmp/ob5_$1.log" | tail -1; }
SV(){ grep -oP '^\[b5\] servis=\K[0-9]+'                      "/tmp/ob5_$1.log" | tail -1; }
ARM(){ grep -oP '^\[b5\] servis=[0-9]+ \| \K.*'               "/tmp/ob5_$1.log" | tail -1; }
VAL(){ grep -oP '^\[b5/horloge\] v[0-9]+:\K.*'                "/tmp/ob5_$1.log"; }

echo "== ORACLE B5 (horloge groupee + index direct) — MAXFRAMES=$MAXFRAMES =="
echo "-- passe A  : knob ABSENT --";                          run A  ""
echo "-- passe A2 : knob ABSENT (CONTROLE determinisme) --";  run A2 ""
if [ "$(H A)" != "$(H A2)" ] || [ "$(CH A)" != "$(CH A2)" ]; then
  echo; echo "  A  pixel=$(H A) horloge=$(CH A)"; echo "  A2 pixel=$(H A2) horloge=$(CH A2)"
  echo "FAIL: LE BANC N EST PAS DETERMINISTE — deux passes temoins different."
  echo "      Aucune comparaison n a de sens tant que ce n est pas resolu."; exit 2
fi
echo "  controle de determinisme : A == A2 (pixel $(H A), horloge $(CH A))"
echo "-- passe B  : WX86_B5=1 --"; run B "1"

echo
printf "  A  pixel=%s  horloge=%s (%s valeurs)  servis=%s  [%s]\n" "$(H A)"  "$(CH A)"  "$(CN A)"  "$(SV A)"  "$(ARM A)"
printf "  B  pixel=%s  horloge=%s (%s valeurs)  servis=%s  [%s]\n" "$(H B)"  "$(CH B)"  "$(CN B)"  "$(SV B)"  "$(ARM B)"
fail=0
[ -n "$(H A)" ] && [ -n "$(H B)" ]   || { echo "FAIL: empreinte pixel manquante"; fail=1; }
[ -n "$(CH A)" ] && [ -n "$(CH B)" ] || { echo "FAIL: empreinte de sequence manquante"; fail=1; }
[ "$(H A)"  = "$(H B)"  ] || { echo "FAIL: EMPREINTES PIXEL DIFFERENTES"; fail=1; }
[ "$(CH A)" = "$(CH B)" ] || { echo "FAIL: SEQUENCE D'HORLOGE DIFFERENTE — desynchronisation"; fail=1; }
[ "$(CN A)" = "$(CN B)" ] || { echo "FAIL: nombre de valeurs d'horloge different"; fail=1; }
diff <(VAL A) <(VAL B) >/dev/null || { echo "FAIL: les 32 premieres valeurs different :"; diff <(VAL A) <(VAL B) | head; fail=1; }
for L in A A2 B; do
  n="$(SV "$L")"
  [ "${n:-0}" -gt 0 ] || { echo "FAIL: TEST VIDE — la passe $L n a servi AUCUNE intrinseque d'horloge"; fail=1; }
done
case "$(ARM A)" in *"groupee=non index direct=non"*) ;; *) echo "FAIL: la jambe A n est PAS le chemin historique"; fail=1;; esac
case "$(ARM B)" in *"groupee=OUI index direct=OUI"*) ;; *) echo "FAIL: la jambe B n est PAS armee"; fail=1;; esac
[ $fail -eq 0 ] && echo "PASS: pixel IDENTIQUE, sequence d'horloge IDENTIQUE, jambes distinctes et non vides" || exit 1
