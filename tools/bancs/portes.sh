#!/usr/bin/env bash
# tools/bancs/portes.sh — runs the two qemu gates (rt_boot_arm_check.sh:
# native, the shipped config, then D2SCHED=coop) under a flock on
# /tmp/rt_boot_gate.lock. /tmp/rt_boot_arm.log and /tmp/d2vita_write_bootcheck
# are shared across worktrees, so two gates running at once would read each
# other's state (false "stamp missing" FAILs) without the lock.
# Output: PASS/FAIL per leg (keyboard oracle, coop, native); logs under
# $D2VITA_WORK/porte_{clavier,coop,natif}.log. Run from the repo root.
#   e.g. bash tools/bancs/portes.sh     (or D2VITA_WORK=/tmp/x bash tools/bancs/portes.sh)
#
# SON=1 adds a third leg: tools/oracle_son.sh (reference fingerprint, sink
# pixel neutrality, codec non-emptiness, WAV spectrum, kill-switch check). It's
# optional because it needs the game assets ($REFS) and ~15 min, unlike the
# first two legs which only need the repo — but it is wired in here, not just
# documented, so it actually runs as part of the gate.
#   e.g. SON=1 bash tools/bancs/portes.sh
# Fresh-worktree prerequisite: git submodule update --init --recursive
# (third_party/StormLib; without it the build fails on huff.h).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="${D2VITA_WORK:-$ROOT/build-arm/bancs}"; mkdir -p "$W"
LOCK="${RT_BOOT_GATE_LOCK:-/tmp/rt_boot_gate.lock}"
cd "$ROOT" || exit 1
rc=0
# Leg 0 — host-side virtual keyboard oracle (tools/oracle_clavier.sh). Wired
# in here rather than left to a README: needs neither the game, qemu, nor the
# lock (a ten-second g++ build), and checks two properties code review can't —
# full 95-character ASCII coverage and that no log ever sees the typed text.
# Runs outside the flock: nothing here needs serializing.
# Leg -1 — has validated work actually landed in main? Takes a second, no
# prerequisites, runs outside the flock since it touches neither qemu nor the
# shared disk.
echo "== branches orphelines $(date +%H:%M)"
bash tools/bancs/porte_branches.sh > "$W/porte_branches.log" 2>&1; rbr=$?
cat "$W/porte_branches.log"; echo "   rc=$rbr"
echo "== oracle clavier $(date +%H:%M)"
bash tools/oracle_clavier.sh > "$W/porte_clavier.log" 2>&1; r0=$?
grep -E "^PASS|^FAIL" "$W/porte_clavier.log"; echo "   rc=$r0"
(
  flock -w 7200 9 || { echo "FAIL: verrou $LOCK non obtenu en 2 h"; exit 3; }
  # Reference gate (native) runs first: it's the shipped config, so a failure
  # here should be the first thing visible in the log. Runs without D2SCHED,
  # same as a normal run.
  echo "== porte natif ($ROOT) $(date +%H:%M)"
  timeout 1500 bash tools/rt_boot_arm_check.sh > "$W/porte_natif.log" 2>&1; r1=$?
  grep -E "^PASS|^FAIL" "$W/porte_natif.log"; echo "   rc=$r1"
  # Coop gate: not the shipped game config, but kept — it covers scheduler
  # health (virtual clock), which the tools/ oracles depend on. Losing it
  # would mean only discovering a scheduler regression when an oracle needs it.
  echo "== porte coop (oracles) $(date +%H:%M)"
  D2SCHED=coop timeout 1500 bash tools/rt_boot_arm_check.sh > "$W/porte_coop.log" 2>&1; r2=$?
  grep -E "^PASS|^FAIL" "$W/porte_coop.log"; echo "   rc=$r2"
  # Runtime self-tests that don't boot the game (fidelity, keystore, exit
  # guard + UDP shims). A few minutes, no network: the guard leg runs inside
  # an empty network namespace.
  # Does the sound bench still build? Cheap (~1 min) and catches silent
  # breakage, since this leg only runs when opted in (SON=1) and wouldn't
  # otherwise be exercised.
  echo "== le banc du son compile $(date +%H:%M)"
  bash tools/build_oracle_arm.sh > "$W/porte_oracle_build.log" 2>&1; rob=$?
  echo "   rc=$rob"; [ $rob -eq 0 ] || tail -5 "$W/porte_oracle_build.log"
  echo "== auto-tests hors ligne $(date +%H:%M)"
  bash tools/bancs/portes_hors_ligne.sh > "$W/porte_hors_ligne.log" 2>&1; rh=$?
  grep -E "^PASS|^FAIL|^  " "$W/porte_hors_ligne.log"; echo "   rc=$rh"
  # Secrets hygiene: wired in here, not just documented. All legs run without
  # network; only the CheckRevision oracle is slow (it replays the real DLL
  # under qemu).
  echo "== hygiene des secrets $(date +%H:%M)"
  bash tools/bancs/porte_secrets.sh > "$W/porte_secrets.log" 2>&1; rs=$?
  grep -E "^PASS|^FAIL|WARN" "$W/porte_secrets.log"; echo "   rc=$rs"
  echo "== serveur prive STRICT (test negatif) $(date +%H:%M)"
  python3 tools/bncs_local.py --selftest-strict > "$W/porte_bncs_strict.log" 2>&1; rb=$?
  grep -E "^PASS|^FAIL" "$W/porte_bncs_strict.log"; echo "   rc=$rb"
  echo "== oracle CheckRevision (3 jambes) $(date +%H:%M)"
  timeout 1800 bash tools/oracle_checkrevision.sh > "$W/porte_checkrevision.log" 2>&1; rc4=$?
  grep -E "^PASS|^FAIL" "$W/porte_checkrevision.log"; echo "   rc=$rc4"
  r3=0
  if [ -n "${SON:-}" ] && [ "${SON:-0}" != "0" ]; then
    echo "== oracle son $(date +%H:%M)"
    timeout 3000 bash tools/oracle_son.sh > "$W/porte_son.log" 2>&1; r3=$?
    grep -E "^PASS|^FAIL|^  [A-Z2]+ +=" "$W/porte_son.log" | tail -12; echo "   rc=$r3"
  fi
  [ $r1 -eq 0 ] && [ $r2 -eq 0 ] && [ $r3 -eq 0 ] && [ $rh -eq 0 ] && [ $rs -eq 0 ] && [ $rb -eq 0 ] && [ $rc4 -eq 0 ] && [ $rob -eq 0 ]
) 9>"$LOCK"; rc=$?
[ $r0 -eq 0 ] || rc=$(( rc == 0 ? 4 : rc ))
[ $rbr -eq 0 ] || rc=$(( rc == 0 ? 5 : rc ))
echo "PORTES-FINI rc=$rc"; exit $rc
