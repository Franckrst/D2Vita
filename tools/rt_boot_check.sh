#!/usr/bin/env bash
# tools/rt_boot_check.sh — non-regression check for the runtime boot milestone.
#
# Builds rt_boot (Unicorn oracle) and asserts, against the real 1.13c DLLs:
#   * Storm/Fog/D2Common/D2Win/D2Lang all init DllMain==TRUE in one session
#   * Storm SMemAlloc/SMemFree work with real state
#   * Storm SFileOpenArchive opens a real MPQ via host-backed file I/O
#
# Requires: the genuine 1.13c PE set + Patch_D2.mpq in $DIR (never committed).
set -euo pipefail

# ---------------------------------------------------------------------------
# THIS SAFETY NET CANNOT RUN ANYMORE, AND THAT NEEDS TO BE VISIBLE.
#
# It's built on a UNICORN scaffold (src/runtime/cpu_unicorn.cpp) and on six
# units that have left this repo for the engine: bridge.cpp, cpu.cpp, gil.cpp,
# pe_image.cpp, sched_native.cpp, sched_cooperative.cpp. The Unicorn backend
# itself no longer exists ANYWHERE — not here, not in winx86. No edit to the
# compile line brings it back.
#
# So it refuses explicitly, instead of dying silently on a `set -e`: a broken
# gate that fails without a word is the same family of bug as a diagnostic
# that lies.
#
# What this file is STILL WORTH: the three 1.13c assertions it describes
# above. Whoever picks this back up should port them onto the live path
# (Box86 dynarec, tools/build_oracle_arm.sh) — not pretend this still runs.
echo "REFUS: tools/rt_boot_check.sh est mort (backend Unicorn supprime, 6 sources parties au moteur)." >&2
echo "       Chemin vivant : tools/rt_gameplay_arm_check.sh (dynarec Box86)." >&2
exit 2
# ---------------------------------------------------------------------------

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.13c}"
UNI="${UNICORN_ROOT:-$HOME/tools/aot-venv/lib/python3.12/site-packages/unicorn}"

g++ -std=c++17 -O2 -I"$ROOT/src" -I"$UNI/include" \
  "$ROOT/tools/rt_boot.cpp" "$ROOT/src/glide_ring/replay60.cpp" "$ROOT/src/glide_ring/gx_host.cpp" "$ROOT/src/runtime/phase_hooks.cpp" "$ROOT/src/runtime/cdkeys_file.cpp" "$ROOT/src/platform/vita_net.cpp" "$ROOT/src/runtime/pe_image.cpp" \
  "$ROOT/src/runtime/ds_emul.cpp" "$ROOT/src/runtime/audio_sink_host.cpp" "$ROOT/src/runtime/scomp_audio.cpp" \
  "$ROOT/third_party/StormLib/src/huffman/huff.cpp" "$ROOT/third_party/StormLib/src/adpcm/adpcm.cpp" \
  "$ROOT/src/runtime/bridge.cpp" "$ROOT/src/runtime/cpu_unicorn.cpp" "$ROOT/src/runtime/cpu.cpp" "$ROOT/src/runtime/sched_cooperative.cpp" "$ROOT/src/runtime/gil.cpp" "$ROOT/src/runtime/sched_native.cpp" \
  "$UNI/lib/libunicorn.a" -lpthread -lm -o "$ROOT/build/rt_boot" 2>/dev/null

OUT="$("$ROOT/build/rt_boot" "$DIR" 2>&1)"
echo "$OUT" | grep -E "initialized TRUE|SMemAlloc|SFileOpenArchive" || true

fail=0
echo "$OUT" | grep -q "14/14 DLLs initialized TRUE" || { echo "FAIL: not 5/5 init"; fail=1; }
echo "$OUT" | grep -q "SMemAlloc(0x100) -> .* (valid guest ptr)" || { echo "FAIL: SMemAlloc"; fail=1; }
echo "$OUT" | grep -q "write/read-back 0xcafebabe -> OK" || { echo "FAIL: mem coherence"; fail=1; }
echo "$OUT" | grep -q "SFileOpenArchive -> ret=1" || { echo "FAIL: SFileOpenArchive"; fail=1; }
echo "$OUT" | grep -q "4/4 files read from real MPQ, byte-identical" || { echo "FAIL: Storm file reads not byte-identical"; fail=1; }
[ $fail -eq 0 ] && echo "PASS : runtime boot + Storm end-to-end MPQ read (byte-identical) intact" || { echo "REGRESSION"; exit 1; }
