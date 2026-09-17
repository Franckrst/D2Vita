#!/bin/bash
# run_torture.sh — thin wrapper: the BENCH belongs to the engine, this port
# only supplies its ARM runtime binary and its reference-data folder.
#
# torture.c, bench_gtc.c and gil_ident.cpp live in
# third_party/winx86/tools/torture/; this file is what remains
# project-specific.
#
#   tools/torture/run_torture.sh
#   TORTURE_FAULT=1 tools/torture/run_torture.sh
#   TORTURE_MT=1 tools/torture/run_torture.sh
#
# The engine's own variables remain usable as-is (OUT, REFDIR...).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export RT_BIN="${RT_BIN:-$ROOT/build-arm/rt_boot_arm}"
export REFDIR="${REFDIR:-$HOME/d2-vita-refs/1.14d}"
exec "$ROOT/third_party/winx86/tools/torture/run_torture.sh" "$@"
