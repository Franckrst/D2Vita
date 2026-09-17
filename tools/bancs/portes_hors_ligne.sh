#!/usr/bin/env bash
# tools/bancs/portes_hors_ligne.sh — runtime self-tests that run under
# qemu-arm without ever booting the game (each returns before the game loop)
# and without touching the network. Wired into tools/bancs/portes.sh: a gate
# that isn't wired in anywhere isn't a gate.
#
#   D2FIDTEST       runtime fidelity (dynamic code, VirtualQuery, modules)
#   D2NETGUARDTEST  exit guard + the two UDP shims (sendto/recvfrom)
#
# ---------------------------------------------------------------------------
# Network isolation is mandatory for the guard leg: its fault-injection check
# (a deliberately broken guard) can itself open a real network connection —
# exactly what the guard exists to prevent. This leg therefore runs under
# `unshare -rn` when available; otherwise it says so and refuses to run
# rather than executing it exposed.
# ---------------------------------------------------------------------------
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${REFS:-$HOME/d2-vita-refs/1.14d}"
BIN="$ROOT/build-arm/rt_boot_arm"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (lancer tools/rt_boot_arm_check.sh d'abord)"; exit 1; }
[ -f "$DIR/Game.exe" ] || { echo "FAIL: refs absentes dans $DIR"; exit 1; }

NETNS=0
if unshare -rn sh -c "ip link set lo up" 2>/dev/null; then NETNS=1; fi

fail=0
# run <label> <isolation 0|1> <env...>
run(){
  local tag="$1" iso="$2"; shift 2
  local W; W="$(mktemp -d /tmp/d2vita_hl.XXXXXX)"
  local out rc
  if [ "$iso" = 1 ]; then
    # `ip link set lo up`: loopback is DOWN by default in a fresh namespace.
    # Without it, the "127.0.0.1 passes the guard" check and the UDP bench
    # fail for a reason unrelated to the guard itself.
    out=$(unshare -rn sh -c "ip link set lo up; exec env D2WRITE='$W' D2_FAKEWALL=1756000000 GAMEEXE=1 $* \
          timeout 300 qemu-arm -B 0x10000 '$BIN' '$DIR'" 2>&1); rc=$?
  else
    out=$(env D2WRITE="$W" D2_FAKEWALL=1756000000 GAMEEXE=1 "$@" \
          timeout 300 qemu-arm -B 0x10000 "$BIN" "$DIR" 2>&1); rc=$?
  fi
  rm -rf "$W"
  local line; line=$(echo "$out" | grep -E '=== \[(FIDTEST|NETGUARD)\] [0-9]+ passed' | tail -1)
  echo "  $tag : ${line:-<aucun verdict>} (rc=$rc)"
  [ $rc -eq 0 ] || fail=1
}

run "fidelite            " 0 D2FIDTEST=1

# Keystore: two processes, the second reads back what the first wrote — so
# both need the same write root and secrets dir. The generic `run` helper
# creates a fresh one per call, which would make the "read" leg pass for the
# wrong reason (nothing to read back).
KW="$(mktemp -d /tmp/d2vita_ks.XXXXXX)"; KS="$(mktemp -d /tmp/d2vita_sec.XXXXXX)"
for leg in write read; do
  out=$(env D2WRITE="$KW" D2SECRET="$KS" D2_FAKEWALL=1756000000 GAMEEXE=1 D2KEYSTORETEST=$leg \
        timeout 300 qemu-arm -B 0x10000 "$BIN" "$DIR" 2>&1); rc=$?
  line=$(echo "$out" | grep -E '=== \[KEYSTORE\] [0-9]+ passed' | tail -1)
  printf '  keystore (%-5s)     : %s (rc=%d)\n' "$leg" "${line:-<aucun verdict>}" "$rc"
  [ $rc -eq 0 ] || fail=1
done
# Final check on the file itself: the marker must exist only in the keystore.
# The test already asserts this internally; here it's re-checked from outside.
if grep -qi 'blizzardkey' "$KW/registry.txt" 2>/dev/null; then
  echo "  !! le marqueur est dans registry.txt — la separation n'a pas tenu"; fail=1
fi
rm -rf "$KW" "$KS"
if [ $NETNS = 1 ]; then
  # The guard leg runs inside an empty network namespace: loopback still
  # works (the 127.0.0.1 check stays valid), nothing else is reachable — so a
  # guard regression here cannot leak a packet out.
  run "verrou reseau + UDP " 1 D2NETGUARDTEST=1
else
  echo "  verrou reseau + UDP : IGNORE — unshare -rn indisponible sur cette machine."
  echo "      Cette jambe exerce des destinations d'internet public ; sans espace de"
  echo "      noms reseau vide, une regression du verrou les atteindrait pour de vrai."
  fail=1
fi

# Kill-switch check: with the pre-fidelity-fix responses, this gate must
# fail — a test that passes with or without the fix proves nothing.
AW="$(mktemp -d /tmp/d2vita_av.XXXXXX)"
out=$(env D2WRITE="$AW" D2_FAKEWALL=1756000000 GAMEEXE=1 D2FIDTEST=1 D2_FID_AVANT=1 \
      timeout 300 qemu-arm -B 0x10000 "$BIN" "$DIR" 2>&1); rcav=$?
rm -rf "$AW"
nav=$(echo "$out" | grep -oE '[0-9]+ failed' | tail -1)
echo "  bouton coupant       : D2_FID_AVANT=1 -> ${nav:-?} (rc=$rcav, un echec est ATTENDU)"
[ $rcav -ne 0 ] || { echo "  !! la porte de fidelite passe AUSSI sans le correctif : elle ne coupe pas"; fail=1; }

[ $fail -eq 0 ] && echo "PASS : auto-tests runtime hors ligne" || echo "FAIL : auto-tests runtime hors ligne"
exit $fail
