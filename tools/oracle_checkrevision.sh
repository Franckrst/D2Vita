#!/usr/bin/env bash
# tools/oracle_checkrevision.sh — THREE-LEG COMPARATOR for the 1.14d version
# check. NO NETWORK: none of the three legs talks to anyone.
#
#   P0/P1  pure oracle          tools/checkrevision_ref.py --selftest
#   P2     RUNTIME leg          the REAL CheckRevision.dll, loaded and run by
#                              our PE loader + dynarec (qemu-arm)
#   P3     PC leg               the SAME DLL, on real Win32 (mingw + Wine),
#                              original CryptoAPI — independent referee
#
# CRITERION: across several seeds, all THREE must return the same (version,
# checksum) pair and the same `info`. Today equality is on the auth=0 line:
# neither P2 nor P3 can return auth=1, for the same underlying reason (neither
# actually verifies Game.exe's Authenticode; our shims are fabricated, the P3
# harness isn't signed by Blizzard). Concluding "auth=1" from this bench is
# therefore FORBIDDEN — a comparator with two fake legs always reads "equal".
# What the bench does prove, and it's a lot: SHA-1, base64 (NOCR),
# CryptStringToBinaryW, GetModuleFileNameW, GetFileVersionInfoSizeW/
# VerQueryValueW, the `info` format and the runtime PE loader are ALL correct,
# and the ONLY remaining divergence from a real client is the Authenticode byte.
#
#   bash tools/oracle_checkrevision.sh                 # 3 seeds
#   SEEDS="dp26DAAA" bash tools/oracle_checkrevision.sh
#   SKIP_PC=1 bash tools/oracle_checkrevision.sh       # without the Wine leg
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${REFS:-$HOME/d2-vita-refs/1.14d}"
# 3 seeds, one of them RANDOM: a single seed can't distinguish a correct
# computation from a lucky constant.
RAND=$(python3 -c "import base64,os;print(base64.b64encode(os.urandom(4)+b'\x00\x00').decode())")
SEEDS="${SEEDS:-dp26DAAA PlK71AAA $RAND}"
fail=0

echo "== P0/P1 : oracle pur =="
python3 "$ROOT/tools/checkrevision_ref.py" --selftest > /tmp/cr_oracle.log 2>&1 || fail=1
tail -1 /tmp/cr_oracle.log
grep -q "^PASS" /tmp/cr_oracle.log || { echo "FAIL: oracle P0/P1"; fail=1; }

# --- PC leg (P3): mingw + Wine harness -------------------------------------
PCDIR=/tmp/crref_build
if [ -z "${SKIP_PC:-}" ] && command -v i686-w64-mingw32-gcc >/dev/null && command -v wine >/dev/null; then
  mkdir -p "$PCDIR"
  ( cd "$ROOT/tools/crref" && i686-w64-mingw32-windres crref.rc -O coff -o "$PCDIR/crref.res" \
    && i686-w64-mingw32-gcc -O2 -o "$PCDIR/Game.exe" crref.c "$PCDIR/crref.res" -lversion ) 2>/tmp/cr_pc_build.log \
    || { echo "  (jambe PC : compilation KO, voir /tmp/cr_pc_build.log)"; PCDIR=""; }
  [ -n "$PCDIR" ] && cp -f "$DIR/CheckRevision.dll" "$PCDIR/" 2>/dev/null
else
  echo "  (jambe PC sautee : mingw/wine absents ou SKIP_PC)"; PCDIR=""
fi
NETNS=""
if unshare -rn true 2>/dev/null; then NETNS="unshare -rn"
else echo "  (unshare -rn indisponible : la jambe PC tourne AVEC reseau — verifier a la main qu'aucun paquet ne sort)"; fi

# --- runtime leg (P2) -------------------------------------------------------
[ -x "$ROOT/build-arm/rt_boot_arm" ] || { echo "FAIL: build-arm/rt_boot_arm absent (bash tools/rt_boot_arm_check.sh d'abord)"; exit 1; }

for SEED in $SEEDS; do
  echo "== graine $SEED =="
  ORA=$(python3 "$ROOT/tools/checkrevision_ref.py" --seed "$SEED" --auth 0 --quiet)
  OCK=${ORA%% *}; OINFO=${ORA#* }
  echo "  oracle(auth=0) : $OCK $OINFO"

  # A pass with no result line is NOT a divergence: it's a failed run (e.g.
  # qemu killed by the timeout under load). Conflating the two would flag
  # noise as FAIL — this retries once, and says so.
  RT=""
  for try in 1 2; do
    W=/tmp/d2vita_write_cr_$$_$try; rm -rf "$W"; mkdir -p "$W"
    RT=$(env GAMEEXE=1 D2CRTEST=1 D2CRTEST_SEED="$SEED" D2WRITE="$W" D2_FAKEWALL=1756000000 \
         timeout 900 qemu-arm -B 0x10000 "$ROOT/build-arm/rt_boot_arm" "$DIR" 2>&1 | grep '^CRTEST-RESULT')
    rm -rf "$W"
    [ -n "$RT" ] && break
    echo "  runtime        : passe $try SANS ligne CRTEST-RESULT (banc en echec, pas une divergence) — on rejoue"
  done
  if [ -z "$RT" ]; then echo "  FAIL: BANC — deux passes sans ligne CRTEST-RESULT (ce n'est PAS une divergence)"; fail=1; continue; fi
  echo "  runtime        : $RT"
  RCK=$(echo "$RT" | sed -n 's/.*checksum=\(0x[0-9a-f]*\).*/\1/p')
  RINFO=$(echo "$RT" | sed -n 's/.*info=\([^ ]*\).*/\1/p')
  RVER=$(echo "$RT" | sed -n 's/.*ver=\([^ ]*\).*/\1/p')
  [ "$RVER" = ":1.14.3.71:" ] || { echo "  FAIL: chaine hachee = ${RVER:-?} (attendu :1.14.3.71: ; :0.0.0.0: => host_path a echoue)"; fail=1; }
  [ "$RCK" = "$OCK" ]   || { echo "  FAIL: runtime checksum $RCK != oracle $OCK"; fail=1; }
  [ "$RINFO" = "$OINFO" ] || { echo "  FAIL: runtime info $RINFO != oracle $OINFO"; fail=1; }

  if [ -n "$PCDIR" ]; then
    # The PC leg runs inside an EMPTY NETWORK NAMESPACE when `unshare -rn` is
    # available: the DLL calls WinVerifyTrust, and a Windows (or Wine) stack
    # may want to fetch a CRL/AIA from the internet. Here it CANNOT — this
    # isn't "we assume nothing goes out", it's "nothing can go out". The
    # result is identical with and without network access.
    PC=$(cd "$PCDIR" && $NETNS env WINEDEBUG=-all timeout 300 wine ./Game.exe CheckRevision.dll "$SEED" 2>/dev/null | tr -d '\r' | grep '^CRREF-RESULT')
    echo "  PC (Wine)      : ${PC:-<aucune ligne CRREF-RESULT>}"
    PCK=$(echo "$PC" | sed -n 's/.*checksum=\(0x[0-9a-f]*\).*/\1/p')
    PINFO=$(echo "$PC" | sed -n 's/.*info=\([^ ]*\).*/\1/p')
    [ "$PCK" = "$OCK" ]     || { echo "  FAIL: PC checksum $PCK != oracle $OCK"; fail=1; }
    [ "$PINFO" = "$OINFO" ] || { echo "  FAIL: PC info $PINFO != oracle $OINFO"; fail=1; }
  fi
done

if [ $fail -eq 0 ]; then
  echo "PASS : oracle == runtime$([ -n "$PCDIR" ] && echo " == PC") sur toutes les graines (ligne auth=0)"
  echo "       RAPPEL : auth=1 n'est PAS prouve — il exige un Authenticode REEL (E6)."
else
  echo "FAIL : divergence CheckRevision"
fi
exit $fail
