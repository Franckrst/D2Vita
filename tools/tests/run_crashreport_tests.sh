#!/usr/bin/env bash
# tools/tests/run_crashreport_tests.sh — host-side tests for src/crashreport/
# (crash-report extraction library).
#
# Legs:
#   0. src/crashreport/ must not include any VitaSDK header — EXCEPT the
#      files tools/crashreport_srcs.sh lists in CRASHREPORT_VITA_SRCS
#      (cr_io_vita.cpp, cr_net_vita.cpp, cr_consent_vita.cpp,
#      cr_boot.cpp), which are VitaSDK by design and excluded by name;
#   1. strict build (plan command + -Werror) and run of crashreport_test.cpp;
#   2. same tests under ASan+UBSan (the parsers read untrusted files);
#   3. oracle against tools/autopsie_psp2dmp.py on the synthetic dumps, and on
#      the maintainer's real dumps when D2V_CRASH_FIXTURES points at a folder
#      (skipped otherwise: real dumps never enter the repository); a
#      boot_progress.txt in that folder also gets the collector heap peaks
#      measured (build_evidence and Outbox::create, 1 MiB budget);
#   4. the claims of the end-to-end test (every kind and host region) against
#      the D2Vita-website contract when D2V_CONTRACT names its contract/
#      directory: schema claim.v1, 16 KiB, canon and signature (skipped
#      otherwise, and skipped with a message when no Python has jsonschema —
#      the contract's .venv, or one named by D2V_PYTHON; leg 1 still checks
#      the address rules on every run);
#   5. the vendored Monocypher 4.0.3 (third_party/monocypher): the copies match
#      SHA256SUMS, they build on their own for the PC and, when VitaSDK is
#      installed, for the console, the reporter's own unit
#      (src/crashreport/cr_monocypher.c) comes out as the rolled BLAKE2 build
#      given no define at all — the 150 KiB budget of leg 7 depends on that
#      flag, so it lives in the source and not in this script — and, when
#      D2V_CONTRACT is set, the contract's own check_sealed.c built against
#      THESE sources reproduces every sealed and signature vector (4.0.3
#      changes no output);
#   6. transport tests (crashreport_transport_test.cpp): sealing, response
#      signatures, sockets, HTTP/1.1 and the upload state machine, strict build
#      then ASan+UBSan, against the contract vectors copied in
#      tests/crashreport/vectors/. Its last test sends a report holding a
#      2 MiB dump from a thread with the 64 KiB stack of spec 4.9 and prints
#      the heap peak (budget 1 MiB);
#   7. .text of the whole reporter at -Os with VitaSDK, per unit and in total
#      (spec 4.9 aims at 150 KiB, zlib excluded); the units are the ones
#      tools/crashreport_srcs.sh names, built with the flags of the VPK, so
#      the figure is the eboot's; skipped without VitaSDK;
#   8. one report against a REAL API when D2V_API_DEV_URL names a local
#      `wrangler dev` of D2Vita-website/api (D2V_API_DIR, or the api/
#      directory next to D2V_CONTRACT, must hold the .dev.client.json that
#      `npm run dev:secrets` writes): a throwaway build is registered, the
#      console sends claim, pieces and complete, the pieces are read back
#      through the admin API and opened with the contract key, and the test
#      installation is erased. Prints SKIPPED when the variable is not set.
#      Note: wrangler dev 4.124 exits when a client disconnects mid-body, so
#      the cut-connection tests stay with the fake server (leg 6).
#   9. D2_CRASHTEST=native|halt value parsing AND the frame-threshold
#      trigger predicate (src/crashreport/cr_crashtest.h, spec section 10
#      "Console"): pure, entirely #ifdef D2V_CRASHTEST, tested by a small
#      binary that defines the macro for itself — includes a contrived
#      skipped-frame sequence proving the >= fix still fires where an exact
#      == match would not. The runtime triggers themselves (tools/rt_boot.cpp)
#      need a live Cpu/Bridge and a real Game.exe boot, so they are verified
#      separately, under qemu-arm: tools/crashtest_arm_check.sh.
#
# A leg that needs a tool or a variable it did not get is skipped, and the last
# line names every skipped leg ("PASS: crashreport tests (4 contract schemas,
# 8 live API skipped)"): a PASS alone never means everything was checked.
#
# Usage: tools/tests/run_crashreport_tests.sh [test-name-filter]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/d2cr_test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
FILTER="${1:-}"
rc=0
# What did not run this time: a leg that needs a tool or an environment
# variable it did not get. The last line names them, so a PASS is never read
# as "everything was checked".
SKIPPED_LEGS=()

# Sourced here (not just below, where the rest of this script expects it) so
# leg 0 can build its exclusion list from CRASHREPORT_VITA_SRCS instead of
# repeating those four names by hand.
. "$ROOT/tools/crashreport_srcs.sh"

echo "== 0. no VitaSDK include under src/crashreport (except CRASHREPORT_VITA_SRCS) =="
VITA_SRC_PATTERN="$(printf '%s\n' "${CRASHREPORT_VITA_SRCS[@]}" | xargs -n1 basename | paste -sd'|')"
if grep -rnE '#[[:space:]]*include[[:space:]]*[<"](psp2|vitasdk|taihen)' "$ROOT/src/crashreport" \
     | grep -vE "/($VITA_SRC_PATTERN):"; then
  echo "   FAIL: VitaSDK include found outside CRASHREPORT_VITA_SRCS"; rc=1
else
  echo "   OK"
fi

# The units of the reporter, the same list the eboot builds (already
# sourced above, before leg 0).
SRCS=("${CRASHREPORT_SRCS[@]}" "${CRASHREPORT_HOST_SRCS[@]}")
# Same platform-independent core, but with the Vita implementations in place
# of the POSIX ones (plus build_id.cpp) — what leg 7 measures, because it is
# what the eboot actually links.
VITA_TEXT_SRCS=("${CRASHREPORT_SRCS[@]}" "${CRASHREPORT_VITA_SRCS[@]}" "$ROOT/src/crashreport/build_id.cpp")
MC="$ROOT/third_party/monocypher"
# cr_consent_vita.cpp and cr_boot.cpp (CRASHREPORT_VITA_SRCS) reach into the
# winx86 submodule for platform/vita_kb.h and platform/vita_host.h, exactly
# as tools/build_rt_boot_vpk.sh's own CXXFLAGS_BASE does — leg 7 below needs
# the same include to build them for the Vita.
WX86="$ROOT/third_party/winx86"
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)

# A .cpp of src/crashreport/ that is not in ONE of these lists would be
# tested here (or, for the Vita-only/build_id ones, nowhere on the host) and
# missing from the eboot. CRASHREPORT_VITA_SRCS and build_id.cpp cannot build
# for the host by construction (VitaSDK headers, or per-build -D defines —
# see tools/crashreport_srcs.sh) so leg 1/2/6 never link them; leg 7 below
# builds them for the Vita instead.
shopt -s nullglob
GLOBBED=("$ROOT"/src/crashreport/*.cpp)
shopt -u nullglob
ALL_KNOWN=("${SRCS[@]}" "${CRASHREPORT_VITA_SRCS[@]}" "$ROOT/src/crashreport/build_id.cpp")
if [ "$(printf '%s\n' "${ALL_KNOWN[@]}" | sort)" != "$(printf '%s\n' "${GLOBBED[@]}" | sort)" ]; then
  echo "FAIL: tools/crashreport_srcs.sh does not list every .cpp of src/crashreport/"
  diff <(printf '%s\n' "${ALL_KNOWN[@]}" | sort) <(printf '%s\n' "${GLOBBED[@]}" | sort)
  exit 1
fi

# Monocypher is C, and src/crashreport/ links it (sealing, response
# signatures): one object per configuration, shared by every test binary. The
# unit is the reporter's own cr_monocypher.c, which sets BLAKE2_NO_UNROLLING
# itself (leg 5 checks that), so no build here or in the eboot can forget it.
MONO=("$WORK/mono.o" "$WORK/mono_ed.o")
MONO_SAN=("$WORK/mono_san.o" "$WORK/mono_ed_san.o")
if ! (cc -O1 -Wall -Wextra -Werror -c "${CRASHREPORT_C_SRCS[0]}" -o "$WORK/mono.o" &&
      cc -O1 -Wall -Wextra -Werror -I "$MC" -c "${CRASHREPORT_C_SRCS[1]}" -o "$WORK/mono_ed.o" &&
      cc -O1 -g -Wall -Wextra -Werror "${SAN[@]}" -c "${CRASHREPORT_C_SRCS[0]}" -o "$WORK/mono_san.o" &&
      cc -O1 -g -Wall -Wextra -Werror "${SAN[@]}" -I "$MC" -c "${CRASHREPORT_C_SRCS[1]}" \
         -o "$WORK/mono_ed_san.o"); then
  echo "FAIL: cannot build the reporter's Monocypher units"; exit 1
fi

# .text of one object, every .text.* section included (C++ puts inline and
# template code in comdat sections of its own).
text_of() { "${2:-size}" -A "$1" | awk '$1 ~ /^\.text/ {t += $2} END {print t + 0}'; }

echo "== synthetic psp2dmp dumps =="
if python3 "$ROOT/tools/tests/gen_fake_psp2dmp.py" --out "$WORK/dumps"; then
  echo "   OK ($(ls "$WORK/dumps" | wc -l) files)"
else
  echo "   FAIL: gen_fake_psp2dmp.py"; rc=1
fi
FIX=(--fixtures "$ROOT/tools/tests/fixtures/crashreport" --dumps "$WORK/dumps")

echo "== 1. strict build + tests =="
if g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$ROOT/src" -I "$MC" "${SRCS[@]}" \
       "$ROOT/tools/tests/crashreport_test.cpp" "${MONO[@]}" -lz -o "$WORK/crashreport_test"; then
  "$WORK/crashreport_test" "${FIX[@]}" --work "$WORK/run1" --claims-out "$WORK/claims" \
      ${FILTER:+--filter "$FILTER"} || rc=1
else
  echo "   FAIL: build"; rc=1
fi

echo "== 2. ASan+UBSan build + tests =="
if g++ -std=c++17 -O1 -g -Wall -Wextra -Werror "${SAN[@]}" \
       -DD2CR_NO_ALLOC_COUNTER -I "$ROOT/src" -I "$MC" "${SRCS[@]}" \
       "$ROOT/tools/tests/crashreport_test.cpp" "${MONO_SAN[@]}" -lz -o "$WORK/crashreport_test_asan"; then
  "$WORK/crashreport_test_asan" "${FIX[@]}" --work "$WORK/run2" ${FILTER:+--filter "$FILTER"} || rc=1
else
  echo "   FAIL: sanitizer build"; rc=1
fi

echo "== 3. oracle: tools/autopsie_psp2dmp.py vs read_psp2dmp =="
if [ -x "$WORK/crashreport_test" ]; then
  ORACLE_DUMPS=()
  for d in first_jit raw_elf worker_withheld eboot_nostamp other_app_sysmodule d2_sysmodule truncated long_chain big; do
    ORACLE_DUMPS+=("$WORK/dumps/$d.psp2dmp")
  done
  echo "   synthetic dumps:"
  python3 "$ROOT/tools/tests/crashreport_oracle.py" --reader "$WORK/crashreport_test" "${ORACLE_DUMPS[@]}" || rc=1
  if [ -n "${D2V_CRASH_FIXTURES:-}" ]; then
    shopt -s nullglob
    REAL=("$D2V_CRASH_FIXTURES"/*.psp2dmp)
    shopt -u nullglob
    if [ ${#REAL[@]} -gt 0 ]; then
      echo "   real dumps from D2V_CRASH_FIXTURES (${#REAL[@]}):"
      python3 "$ROOT/tools/tests/crashreport_oracle.py" --reader "$WORK/crashreport_test" "${REAL[@]}" || rc=1
    else
      echo "   D2V_CRASH_FIXTURES=$D2V_CRASH_FIXTURES holds no *.psp2dmp"
    fi
    if [ -f "$D2V_CRASH_FIXTURES/boot_progress.txt" ]; then
      echo "   collector heap on the real boot_progress.txt of D2V_CRASH_FIXTURES (read in place):"
      "$WORK/crashreport_test" --collect-peak "$D2V_CRASH_FIXTURES/boot_progress.txt" --work "$WORK/collect" || rc=1
    fi
  else
    echo "   real dumps: skipped (D2V_CRASH_FIXTURES not set)"
    SKIPPED_LEGS+=("3 real dumps")
  fi
else
  echo "   FAIL: no test binary"; rc=1
fi

echo "== 4. contract: end-to-end claims vs D2Vita-website contract/ =="
if [ -n "${D2V_CONTRACT:-}" ]; then
  CDIR="$D2V_CONTRACT"
  [ -d "$CDIR/contract/schemas" ] && CDIR="$CDIR/contract"
  # The schema validation needs jsonschema: the contract's own .venv, a Python
  # named by D2V_PYTHON, or the system one when it has the package.
  PY="${D2V_PYTHON:-}"
  [ -n "$PY" ] || PY="$CDIR/.venv/bin/python"
  [ -x "$PY" ] || PY=python3
  shopt -s nullglob
  CLAIMS=("$WORK/claims"/*.json)
  shopt -u nullglob
  if ! "$PY" -c "import jsonschema" 2>/dev/null; then
    SKIPPED_LEGS+=("4 contract schemas")
    echo "   SKIPPED: no Python with jsonschema ($PY). Create the contract venv"
    echo "            (make -C \"$CDIR/..\" venv, see contract/README.md) or set D2V_PYTHON."
  elif [ ${#CLAIMS[@]} -gt 0 ]; then
    "$PY" "$ROOT/tools/tests/crashreport_contract_check.py" --contract "$CDIR" "${CLAIMS[@]}" || rc=1
  elif [ -n "$FILTER" ]; then
    echo "   skipped (the filter excluded claims_end_to_end)"
    SKIPPED_LEGS+=("4 contract schemas")
  else
    echo "   FAIL: leg 1 wrote no claim"; rc=1
  fi
else
  echo "   skipped (D2V_CONTRACT not set)"
  SKIPPED_LEGS+=("4 contract schemas")
fi

echo "== 5. vendored Monocypher 4.0.3 =="
if (cd "$MC" && sha256sum --check --ignore-missing --quiet SHA256SUMS); then
  echo "   OK: copies match SHA256SUMS"
else
  echo "   FAIL: third_party/monocypher does not match SHA256SUMS"; rc=1
fi
echo "   OK: builds for the PC (the objects every test binary links)"
# The 150 KiB of spec section 4.9 is only met with BLAKE2_NO_UNROLLING, so the
# flag must not be a property of this script: what a build compiles is
# src/crashreport/cr_monocypher.c, which sets it itself. Given no define at
# all, it must come out as the rolled build, and the rolled build must be
# smaller than the unrolled one (otherwise this check has no teeth).
if cc -Os -Wall -Wextra -Werror -c "${CRASHREPORT_C_SRCS[0]}" -o "$WORK/mc_reporter.o" &&
   cc -Os -Wall -Wextra -Werror -DBLAKE2_NO_UNROLLING -c "$MC/monocypher.c" -o "$WORK/mc_rolled.o" &&
   cc -Os -Wall -Wextra -Werror -c "$MC/monocypher.c" -o "$WORK/mc_unrolled.o"; then
  mc_r=$(text_of "$WORK/mc_reporter.o"); mc_ro=$(text_of "$WORK/mc_rolled.o")
  mc_un=$(text_of "$WORK/mc_unrolled.o")
  if [ "$mc_r" = "$mc_ro" ] && [ "$mc_ro" -lt "$mc_un" ]; then
    echo "   OK: the reporter's unit rolls BLAKE2 on its own ($mc_r bytes of .text here, $mc_un unrolled)"
  else
    echo "   FAIL: cr_monocypher.c must roll BLAKE2 whatever the build passes"
    echo "         (reporter $mc_r, rolled $mc_ro, unrolled $mc_un)"; rc=1
  fi
else
  echo "   FAIL: cannot build ${CRASHREPORT_C_SRCS[0]}"; rc=1
fi
VITA_CC="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-gcc"
VITA_SIZE_5="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-size"
if [ -x "$VITA_CC" ]; then
  VMC=(-Os -Wall -Wextra -Werror -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  if "$VITA_CC" "${VMC[@]}" -c "${CRASHREPORT_C_SRCS[0]}" -o "$WORK/mc_arm.o" &&
     "$VITA_CC" "${VMC[@]}" -c "$MC/monocypher.c" -o "$WORK/mc_arm_unrolled.o" &&
     "$VITA_CC" "${VMC[@]}" -I "$MC" -c "${CRASHREPORT_C_SRCS[1]}" -o "$WORK/mc_ed_arm.o"; then
    echo "   OK: builds for the Vita"
    if [ -x "$VITA_SIZE_5" ]; then
      # The same comparison on the console's compiler: what the flag is worth
      # where the 150 KiB budget applies.
      a_r=$(text_of "$WORK/mc_arm.o" "$VITA_SIZE_5"); a_u=$(text_of "$WORK/mc_arm_unrolled.o" "$VITA_SIZE_5")
      if [ "$a_r" -lt "$a_u" ]; then
        echo "   OK: rolled on ARM too ($a_r bytes of .text, $a_u unrolled: $((a_u - a_r)) saved)"
      else
        echo "   FAIL: the reporter's unit is not the rolled build on ARM ($a_r vs $a_u)"; rc=1
      fi
    fi
  else
    echo "   FAIL: Monocypher does not build with VitaSDK"; rc=1
  fi
else
  echo "   Vita build: skipped (no $VITA_CC)"
  SKIPPED_LEGS+=("5 Vita build")
fi
if [ -n "${D2V_CONTRACT:-}" ]; then
  CDIR="$D2V_CONTRACT"
  [ -d "$CDIR/contract/schemas" ] && CDIR="$CDIR/contract"
  CC_DIR="$WORK/c_check"
  mkdir -p "$CC_DIR"
  # The contract's reference checker, built against the 4.0.3 sources of this
  # repository (its own copy is 4.0.2). Quoted includes must find 4.0.3, so
  # everything is copied into one directory.
  if cp "$CDIR"/tools/c_check/check_sealed.c "$CDIR"/tools/c_check/d2vseal.c "$CDIR"/tools/c_check/d2vseal.h \
        "$CDIR"/tools/c_check/jsonlite.c "$CDIR"/tools/c_check/jsonlite.h "$CC_DIR/" &&
     cp "$MC"/monocypher.c "$MC"/monocypher.h "$MC"/monocypher-ed25519.c "$MC"/monocypher-ed25519.h "$CC_DIR/" &&
     cc -O2 -Wall -Wextra -o "$CC_DIR/check_sealed" "$CC_DIR"/check_sealed.c "$CC_DIR"/d2vseal.c \
        "$CC_DIR"/jsonlite.c "$CC_DIR"/monocypher.c "$CC_DIR"/monocypher-ed25519.c -lm; then
    echo "   contract checker built against $(head -1 "$CC_DIR/monocypher.c" | sed 's,^// ,,')"
    # Its own banner says 4.0.2: that string is written in check_sealed.c.
    "$CC_DIR/check_sealed" "$CDIR/vectors" || rc=1
  else
    echo "   FAIL: cannot build the contract checker against third_party/monocypher"; rc=1
  fi
else
  echo "   contract vectors with 4.0.3: skipped (D2V_CONTRACT not set)"
  SKIPPED_LEGS+=("5 contract vectors")
fi

echo "== 6. transport tests (seal, signatures, sockets, HTTP, upload) =="
TSRC="$ROOT/tools/tests/crashreport_transport_test.cpp"
VEC="$ROOT/tests/crashreport/vectors"
TARGS=(--vectors "$VEC")
# The scripted server needs PyNaCl (it opens the sealed pieces and signs the
# answers with a key of the contract vectors). Without it the tests that need
# a server print SKIPPED and the others still run.
if python3 -c "import nacl" 2>/dev/null; then
  TARGS+=(--fake-api "$ROOT/tools/tests/fake_crash_api.py")
else
  echo "   note: python3 without PyNaCl, the server-backed tests will be skipped"
  SKIPPED_LEGS+=("6 server-backed tests")
fi
if g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$ROOT/src" -I "$MC" "${SRCS[@]}" "$TSRC" \
       "${MONO[@]}" -lz -lpthread -o "$WORK/transport_test"; then
  "$WORK/transport_test" "${TARGS[@]}" --work "$WORK/trun1" ${FILTER:+--filter "$FILTER"} || rc=1
else
  echo "   FAIL: build"; rc=1
fi
if g++ -std=c++17 -O1 -g -Wall -Wextra -Werror "${SAN[@]}" -DD2CR_NO_ALLOC_COUNTER \
       -I "$ROOT/src" -I "$MC" "${SRCS[@]}" "$TSRC" "${MONO_SAN[@]}" -lz -lpthread -o "$WORK/transport_test_asan"; then
  "$WORK/transport_test_asan" "${TARGS[@]}" --work "$WORK/trun2" ${FILTER:+--filter "$FILTER"} || rc=1
else
  echo "   FAIL: sanitizer build"; rc=1
fi

echo "== 7. .text of the reporter at -Os with VitaSDK =="
VITA_CXX="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-g++"
VITA_SIZE="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-size"
VITA_LD="${VITASDK:-/usr/local/vitasdk}/bin/arm-vita-eabi-ld"
if [ -x "$VITA_CXX" ] && [ -x "$VITA_SIZE" ]; then
  # The machine flags of tools/build_rt_boot_vpk.sh, at -Os. The units are
  # built twice: as they are for the table, and with -ffunction-sections for
  # the second figure (what a link with --gc-sections keeps).
  VFLAGS=(-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Os -Wall -Wextra -Werror)
  VSECT=(-ffunction-sections -fdata-sections)
  OBJS=()
  SECT_OBJS=()
  built=1
  for f in "${VITA_TEXT_SRCS[@]}"; do
    base="$(basename "$f" .cpp)"
    if "$VITA_CXX" -std=gnu++17 "${VFLAGS[@]}" -I "$ROOT/src" -I "$MC" -I "$WX86/src" -c "$f" -o "$WORK/os_$base.o" &&
       "$VITA_CXX" -std=gnu++17 "${VFLAGS[@]}" "${VSECT[@]}" -I "$ROOT/src" -I "$MC" -I "$WX86/src" -c "$f" \
         -o "$WORK/sect_$base.o"; then
      OBJS+=("$WORK/os_$base.o")
      SECT_OBJS+=("$WORK/sect_$base.o")
    else
      echo "   FAIL: $base does not build for the Vita"; built=0; rc=1
    fi
  done
  # The C units of the reporter, exactly as the eboot builds them (no define
  # of this script's own: cr_monocypher.c carries BLAKE2_NO_UNROLLING).
  for f in "${CRASHREPORT_C_SRCS[@]}"; do
    c="$(basename "$f" .c)"
    if "$VITA_CC" -std=gnu17 "${VFLAGS[@]}" -I "$MC" -c "$f" -o "$WORK/os_$c.o" &&
       "$VITA_CC" -std=gnu17 "${VFLAGS[@]}" "${VSECT[@]}" -I "$MC" -c "$f" -o "$WORK/sect_$c.o"; then
      OBJS+=("$WORK/os_$c.o")
      SECT_OBJS+=("$WORK/sect_$c.o")
    else
      echo "   FAIL: $c does not build for the Vita"; built=0; rc=1
    fi
  done
  BUDGET=$((150 * 1024))
  if [ "$built" = 1 ]; then
    # --no-fail: informational upper bound only (see text_size_report.py's
    # own doc comment) — Monocypher's one translation unit also carries
    # Argon2, EdDSA signing and Elligator, which the reporter never calls, so
    # the plain per-object sum overcounts by whatever --gc-sections would
    # have dropped. REACHABLE below is the real gate.
    python3 "$ROOT/tools/tests/text_size_report.py" --size "$VITA_SIZE" --budget "$BUDGET" --no-fail "${OBJS[@]}"
    # What a link with --gc-sections keeps: a partial link rooted at a probe
    # that calls everything the reporter offers, minus the same link of the
    # probe alone (its main, its zlib call and its IoApi/NetApi stubs, each
    # collected the same way, so only the scaffolding that survives is
    # subtracted). This is what the reporter would actually add to an eboot
    # that already has a main and zlib — the figure spec 4.9's 150 KiB is
    # checked against.
    if [ -x "$VITA_LD" ] &&
       "$VITA_CXX" -std=gnu++17 "${VFLAGS[@]}" "${VSECT[@]}" -I "$ROOT/src" -I "$MC" \
         -c "$ROOT/tools/tests/crashreport_size_probe.cpp" -o "$WORK/sect_probe.o" &&
       "$VITA_LD" -r --gc-sections -u main "$WORK/sect_probe.o" -o "$WORK/sect_probe_alone.o" &&
       "$VITA_LD" -r --gc-sections -u main "$WORK/sect_probe.o" "${SECT_OBJS[@]}" -o "$WORK/sect_reachable.o"; then
      probe_text=$(text_of "$WORK/sect_probe_alone.o" "$VITA_SIZE")
      kept_text=$(text_of "$WORK/sect_reachable.o" "$VITA_SIZE")
      reachable=$((kept_text - probe_text))
      printf "   %-28s %8d bytes with -ffunction-sections and --gc-sections\n" "REACHABLE" "$reachable"
      if [ "$reachable" -gt "$BUDGET" ]; then
        echo "   FAIL: REACHABLE is over the $((BUDGET / 1024)) KiB of spec 4.9"; rc=1
      else
        echo "   OK: REACHABLE leaves $((BUDGET - reachable)) bytes under the $((BUDGET / 1024)) KiB of spec 4.9"
      fi
    else
      # No fallback silently accepted: without --gc-sections there is no
      # real budget figure at all, and the pessimistic TOTAL above is not a
      # substitute (see text_size_report.py) — this leg cannot pass blind.
      echo "   FAIL: could not compute REACHABLE (ld -r --gc-sections unavailable) — no real budget figure to check"
      rc=1
    fi
  fi
else
  echo "   skipped (no $VITA_CXX)"
  SKIPPED_LEGS+=("7 .text budget")
fi

echo "== 8. one report against a local wrangler dev =="
if [ -n "${D2V_API_DEV_URL:-}" ]; then
  API_DIR="${D2V_API_DIR:-}"
  if [ -z "$API_DIR" ] && [ -n "${D2V_CONTRACT:-}" ]; then
    API_DIR="$(cd "$(dirname "${D2V_CONTRACT%/}")" && pwd)/api"
  fi
  if [ -x "$WORK/transport_test" ] && [ -d "$API_DIR" ]; then
    python3 "$ROOT/tools/tests/crash_dev_integration.py" --url "$D2V_API_DEV_URL" --api-dir "$API_DIR" \
      --reader "$WORK/transport_test" --work "$WORK/live" --vectors "$VEC" || rc=1
  else
    echo "   FAIL: no test binary, or D2V_API_DIR does not name the api/ directory ($API_DIR)"; rc=1
  fi
else
  echo "   SKIPPED (D2V_API_DEV_URL not set; start it with 'npm run dev' in D2Vita-website/api)"
  SKIPPED_LEGS+=("8 live API")
fi

echo "== 9. D2_CRASHTEST kind parsing (host, src/crashreport/cr_crashtest.h) =="
# The header is entirely #ifdef D2V_CRASHTEST; the test defines the macro for
# ITSELF ONLY (no shared build flag touched) — see the file's own comment.
if g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$ROOT/src" \
       "$ROOT/tools/tests/crashtest_kind_test.cpp" -o "$WORK/crashtest_kind_test"; then
  "$WORK/crashtest_kind_test" || rc=1
else
  echo "   FAIL: build"; rc=1
fi

verdict="$([ $rc -eq 0 ] && echo PASS || echo FAIL): crashreport tests"
if [ ${#SKIPPED_LEGS[@]} -gt 0 ]; then
  # Names, not a count: "PASS (4 contract schemas, 8 live API skipped)" says
  # what was not checked.
  list="$(printf '%s, ' "${SKIPPED_LEGS[@]}")"
  verdict="$verdict (${list%, } skipped)"
fi
echo "$verdict"
exit $rc
