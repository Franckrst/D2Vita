#!/usr/bin/env bash
# tools/net_check.sh — network primitives bench, POSIX branch (host).
# Launches bncs_local.py, builds the bench, requires a PASS on every primitive.
# Known gotcha: two servers listening at once give inconsistent results. This
# refuses to start if the port is already taken.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ss -ltn | grep -q ':6112 '; then
  echo "FAIL: le port 6112 est deja pris (un bncs_local.py tourne deja ?)"; exit 1
fi

setsid python3 "$ROOT/tools/bncs_local.py" >/tmp/net_check_server.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true' EXIT
for _ in $(seq 25); do ss -ltn | grep -q ':6112 ' && break; sleep 0.2; done
ss -ltn | grep -q ':6112 ' || { echo "FAIL: le serveur n'ecoute pas"; exit 1; }

mkdir -p "$ROOT/build"
BIN="$ROOT/build/d2vita_net_bench"
# vita_net.cpp calls into winx86 (d2rt::wx86_net_resolve/wx86_net_set_nonblock,
# net_nonblock.h) — compile that translation unit in too.
# net_guard.cpp (the exit guard): vita_net.cpp AND net_nonblock.cpp both call
# wx86_net_private_only(). It's a LEAF unit, meant to be linked in directly
# here — do NOT link win32_shims_wsock32.cpp instead, it would pull in the
# bridge, the CPU and the guest scratch allocator, i.e. half the engine, into
# a primitives bench.
# vita_host.cpp, same reasoning: the progress log belongs to the engine and
# the call into it is DIRECT (no weak reference to stub out). Off console
# it's just a no-op — but it still has to be provided.
g++ -std=gnu++17 -O1 -DD2VITA_NET_MAIN \
  -I"$ROOT/third_party/winx86/src" \
  "$ROOT/src/platform/vita_net.cpp" \
  "$ROOT/third_party/winx86/src/runtime/net_nonblock.cpp" \
  "$ROOT/third_party/winx86/src/runtime/net_guard.cpp" \
  "$ROOT/third_party/winx86/src/platform/vita_host.cpp" \
  -o "$BIN"

# The binary is the sole judge: no hardcoded test list, otherwise any probe
# added later to d2vita_net_selftest would stay invisible to this harness.
# timeout covers a primitive hanging instead of failing (e.g. broken nonblock
# -> recv blocks on an empty socket).
set +e
OUT="$(timeout 30 "$BIN" 127.0.0.1 6112 2>&1)"
rc=$?
set -e
echo "$OUT"

fail=0
[ "$rc" -eq 0 ] || { echo "FAIL: le banc a rendu le code $rc"; fail=1; }
# Guard: a binary that dies before printing (crash, signal, timeout) would
# leave neither FAIL nor PASS behind — output with no [nettest] line counts
# as failure, not silence mistaken for success.
echo "$OUT" | grep -q '^\[nettest\] ' || { echo "FAIL: aucune ligne [nettest] dans la sortie"; fail=1; }
echo "$OUT" | grep -qE '^\[nettest\].*FAIL' && fail=1
[ "$fail" -eq 0 ] && echo "net_check: OK"
exit "$fail"
