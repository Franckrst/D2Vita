#!/usr/bin/env bash
# tools/oracle_authenticode.sh — TWO-LEG ORACLE for Authenticode verification
# (WINTRUST.dll!WinVerifyTrust + the CRYPT32 slice). NO NETWORK.
#
#   leg PC      tools/wtref/wtref.c        real Win32: mingw + Wine, under unshare -rn
#   leg runtime tools/wtref/wtref_host.cpp REAL winx86 shim bodies
#                                            (win32_shims_wintrust.cpp) + authenticode.cpp
#                                            on a mock CPU, same call sequence
#
# Both replay the arguments RECOVERED FROM DISASSEMBLY at the call sites
# (Game.exe 0x517ad6/0x517c30, CheckRevision.dll 0x10001f4b/0x10001fe4) against
# the original binaries and against variants altered in exactly one way
# (tools/wtref/mkcases.py), and print a canonical dump.
#
# CRITERION
#   1. Every CRYPT32 line (CryptQueryObject, CryptMsgGetParam, CMSG_SIGNER_INFO
#      field by field, CertFindCertificateInStore, CERT_CONTEXT/CERT_INFO field
#      by field, CertGetNameStringW) must be IDENTICAL.
#   2. WinVerifyTrust: same code, EXCEPT for the divergences below, which are
#      MEASURED LIMITATIONS of Wine's wintrust (not liberties taken by the
#      runtime):
#        CR_tssig.dll   Wine 0: does NOT verify the timestamper's signature
#                       (one bit flipped inside: always "trusted");
#        *_nots         Wine 0: does NOT judge time validity (leaf expired
#                       since 2018/2021, no timestamp: "trusted");
#        Game.exe, SystemSurvey.exe
#                       Wine 0: does not build the timestamper's chain; the
#                       runtime builds it and requires the "Thawte
#                       Timestamping CA" root, absent from its embedded roots
#                       -> CERT_E_CHAINING, unless WTREF_ROOTS supplies it.
#      For these cases the runtime must return the EXPECTED code recorded here.
#   3. GetLastError after a SUCCESSFUL WinVerifyTrust is not compared (Wine
#      leaves 0xEA there, a residue of an internal call; no consumer reads it).
#
#   bash tools/oracle_authenticode.sh
#   WTREF_ROOTS="thawte_ts.cer" bash tools/oracle_authenticode.sh   # added roots
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WX="$ROOT/third_party/winx86/src"
REFS="${REFS:-$HOME/d2-vita-refs/1.14d}"
WORK="${WORK:-$(mktemp -d /tmp/wtref.XXXXXX)}"
mkdir -p "$WORK"
fail=0

command -v i686-w64-mingw32-gcc >/dev/null && command -v wine >/dev/null || { echo "FAIL: mingw/wine absents"; exit 2; }
unshare -rn true 2>/dev/null || { echo "FAIL: unshare -rn indisponible (refus de lancer la jambe PC avec reseau)"; exit 2; }

echo "== cas =="
python3 "$ROOT/tools/wtref/mkcases.py" "$REFS" "$WORK/cases" || exit 2

echo "== compilation =="
i686-w64-mingw32-gcc -O2 -municode -o "$WORK/wtref.exe" "$ROOT/tools/wtref/wtref.c" -lwintrust -lcrypt32 \
  || { echo "FAIL: jambe PC"; exit 2; }
g++ -std=gnu++17 -O2 -Wall -Wextra -I"$WX" -o "$WORK/wtref_host" "$ROOT/tools/wtref/wtref_host.cpp" \
  "$WX/runtime/authenticode.cpp" "$WX/runtime/win32_shims_wintrust.cpp" \
  "$WX/runtime/guest_scratch.cpp" "$WX/runtime/guest_thread_ctx.cpp" "$WX/runtime/cpu.cpp" \
  || { echo "FAIL: jambe runtime"; exit 2; }

CASES="Game.exe CheckRevision.dll SystemSurvey.exe Diablo_II.exe notpe.txt missing.dll Game_text1.exe CR_text1.dll Game_sig.exe CR_sig.dll CR_tssig.dll Game_nots.exe CR_nots.dll"
ARGS=(); for c in $CASES; do ARGS+=("Z:$WORK/cases\\$c"); done
ROOTARGS=(); for r in ${WTREF_ROOTS:-}; do ROOTARGS+=(--add-root "$r"); done

echo "== jambe PC (Wine, sans reseau) =="
( cd "$WORK" && WINEDEBUG=-all unshare -rn wine ./wtref.exe "${ARGS[@]}" 2>/dev/null ) | tr -d '\r' > "$WORK/pc.txt"
echo "== jambe runtime (shims winx86) =="
"$WORK/wtref_host" "${ROOTARGS[@]}" "${ARGS[@]}" > "$WORK/rt.txt" || { echo "FAIL: jambe runtime a echoue"; fail=1; }

HAVE_THAWTE=1   # embedded in the engine
python3 - "$WORK/pc.txt" "$WORK/rt.txt" "$HAVE_THAWTE" <<'EOF' || fail=1
import sys
pc, rt, thawte = open(sys.argv[1]).read().splitlines(), open(sys.argv[2]).read().splitlines(), sys.argv[3] == '1'
EXPECT = {  # case -> WinVerifyTrust code the RUNTIME must return when Wine differs (see header)
    'CR_tssig.dll': 0x80096005, 'Game_nots.exe': 0x800B0101, 'CR_nots.dll': 0x800B0101,
    # Game.exe / SystemSurvey.exe: no divergence — Thawte Timestamping CA is
    # embedded (authroot.stl properties) and a real up-to-date Windows
    # reports Valid (per Get-AuthenticodeSignature).
}
if len(pc) != len(rt): print('FAIL: longueurs %d/%d' % (len(pc), len(rt))); sys.exit(1)
bad = 0; case = '?'; same_wvt = 0; lines = 0
for a, b in zip(pc, rt):
    if a.startswith('== '): case = a[3:]
    if a.startswith('WVT '):
        ka, kb = a.split(), b.split()
        hra, hrb = int(ka[2].split('=')[1], 16), int(kb[2].split('=')[1], 16)
        if case in EXPECT:
            ok = hrb == EXPECT[case]
            print('  %-18s %-13s Wine=0x%08x runtime=0x%08x attendu=0x%08x %s' % (case, ka[1], hra, hrb, EXPECT[case], 'OK' if ok else 'FAIL'))
        else:
            ok = hra == hrb and (hra == 0 or ka[3] == kb[3])
            same_wvt += ok
            if not ok: print('  %-18s %s\n  %-18s %s' % (case, a, '', b))
        bad += not ok
    else:
        lines += 1
        if a != b: print('  DIVERGENCE [%s]\n    PC : %s\n    RT : %s' % (case, a, b)); bad += 1
print('  lignes CRYPT32 comparees : %d ; WinVerifyTrust identiques hors table : %d' % (lines, same_wvt))
sys.exit(1 if bad else 0)
EOF
[ $fail -eq 0 ] && echo "PASS : CRYPT32 identique a Wine champ par champ ; WinVerifyTrust = Wine ou divergence attendue (limite Wine mesuree)" \
                || echo "FAIL (sorties dans $WORK)"
exit $fail
