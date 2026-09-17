#!/usr/bin/env bash
# tools/oracle_intrin.sh — PIXEL ORACLE for the native intrinsics (D2_INTRIN).
#
# Sibling of tools/oracle_cellloop.sh. The current target (VA 0x50dd60,
# world -> screen projection) produces only COORDINATES that decide where
# everything gets drawn: a single-pixel divergence moves entire sprites.
# FBHASH is therefore a STRONG oracle here, not a fallback.
#
# Three legs, single binary:
#   A  = D2_INTRIN absent            -> translated guest body (control)
#   B  = D2_INTRIN=1                 -> native helper
#   V  = D2_INTRIN=1 D2_PROJVERIFY=1 -> native computes but does NOT serve;
#                                       it compares its values against what
#                                       the guest actually wrote.
#
# GREEN VERDICT requires all THREE:
#   1. B's FBHASH fingerprint identical to A's;
#   2. NON-EMPTY test: B reports servis > 0 (otherwise two equal fingerprints
#      prove nothing);
#   3. leg V: comparisons > 0 AND divergences == 0.
#
# Leg V is the real arithmetic proof: FBHASH would say "green" even if the
# native code were wrong in a way the scene doesn't exercise.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/oracle_arm"
MAXFRAMES="${MAXFRAMES:-4600}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (tools/build_oracle_arm.sh)"; exit 1; }

SCRIPT="$(cat "$ROOT/tools/bancs/d2script_camp_patrouille_v2.txt")"

run() {  # $1 = label, $2.. = knobs
  local LAB="$1"; shift
  local W=/tmp/oracle_intrin_$LAB LOG=/tmp/oracle_intrin_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$ROOT/tools/registry_console.txt" "$W/registry.txt" 2>/dev/null || true
  # ⚡ THE GLIDE PATH IS MANDATORY. Target 0x50dd60 is slot +0x64 of the D2gfx
  # vtable in VIDEO MODE 4; in GDI (-w) it's never reached and the oracle
  # returns "servis=0" on two equal fingerprints, i.e. a FALSE GREEN.
  # D2_GLIDEGXM=1: under qemu nothing is drawn, but the ring and the atlas
  # still run in full, and FBHASH remains the oracle.
  env GAMEEXE=1 D2ARGS="game.exe -3dfx" MAXFRAMES="$MAXFRAMES" \
      NATIVECELLLOOP=1 NATIVEDCC=1 NATIVELIGHTMAP=1 \
      D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 FBHASH=1 \
      D2LAYOUT=compact D2ARENA=12900000 \
      D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMLOTSHASH=1 \
      D2WRITE="$W" D2SCRIPT="$SCRIPT" "$@" \
      timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "$LOG"
}

echo "== jambe A (témoin, D2_INTRIN absent)"
LA=$(run A)
echo "== jambe B (D2_INTRIN=1)"
LB=$(run B D2_INTRIN=1)
echo "== jambe V (D2_INTRIN=1 D2_PROJVERIFY=1, oracle croisé)"
LV=$(run V D2_INTRIN=1 D2_PROJVERIFY=1)

hash_of() { grep -o "empreinte=0x[0-9a-f]*" "$1" | tail -1; }
HA=$(hash_of "$LA"); HB=$(hash_of "$LB")
ring_of() { grep -o "lh=0x[0-9a-f]*" "$1" | tail -1; }
RA=$(ring_of "$LA"); RB=$(ring_of "$LB")
echo
echo "empreinte A : ${HA:-<aucune>}"
echo "empreinte B : ${HB:-<aucune>}"
echo "ring      A : ${RA:-<aucune>}"
echo "ring      B : ${RB:-<aucune>}"

SERVED=$(grep -o "proj50dd60 appels=[0-9]* servis=[0-9]* replis=[0-9]*" "$LB" | tail -1)
echo "armement  B : ${SERVED:-<aucune ligne intrin>}"
ARMA=$(grep -c "intrin: ARME" "$LA" || true)
echo "témoin A doit être muet : $ARMA ligne(s) « intrin: ARME »"
VER=$(grep -o "projverify: compares=[0-9]* divergences=[0-9]* sautes=[0-9]*" "$LV" | tail -1)
echo "oracle    V : ${VER:-<aucune ligne projverify>}"

fail=0
[ -n "$HA" ] && [ "$HA" = "$HB" ] || { echo "ECHEC 1 : empreintes FBHASH différentes ou absentes"; fail=1; }
[ -n "$RA" ] && [ "$RA" = "$RB" ] || { echo "ECHEC 1b : empreintes ring (lh=) différentes ou absentes"; fail=1; }
echo "$SERVED" | grep -qE "servis=[1-9]" || { echo "ECHEC 2 : test VIDE (servis=0)"; fail=1; }
[ "$ARMA" = 0 ] || { echo "ECHEC 2b : le témoin A était armé"; fail=1; }
echo "$VER" | grep -qE "compares=[1-9]" || { echo "ECHEC 3a : oracle croisé jamais exercé"; fail=1; }
echo "$VER" | grep -q "divergences=0"   || { echo "ECHEC 3b : DIVERGENCE arithmétique"; fail=1; }
[ $fail = 0 ] && echo "VERDICT: PASS" || { echo "VERDICT: FAIL"; exit 1; }
