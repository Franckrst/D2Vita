#!/usr/bin/env python3
# tools/checkrevision_ref.py — OFFLINE ORACLE for the 1.14d version check
# (lockdown CheckRevision.dll). NO NETWORK: this script talks to no one, it
# RECOMPUTES what a real Windows client would send.
#
# WHY THIS FILE EXISTS
# --------------------
# The D2Vita client used to fabricate its version-check reply (27-byte stub,
# tools/rt_boot.cpp): *version=0x100, *checksum=0, empty info. A private
# server that accepts everything can't reveal that gap. Proving our
# computation is correct requires an independent reference — this is it.
#
# WHAT THE 1.14d DLL ACTUALLY DOES (established by disassembly)
# -------------------------------------------------------------------------
#   BOOL __stdcall CheckRevision(a0,a1,a2, const char* seedB64,
#                                DWORD* version, DWORD* checksum, char* info)
#   - a0/a1/a2 (the file names from the CLASSIC checkrevision) are NEVER
#     read. The DLL imports neither ReadFile nor MapViewOfFile: NO game file
#     byte is ever hashed. Our runtime patches are invisible to it.
#   - seed: CryptStringToBinaryW(seed, BASE64) -> >=4 decoded bytes.
#       flagA = (last decoded byte != 0)   -> with Blizzard seeds
#               (6 bytes, 2 of them zero) flagA = 0
#       dword = LE32(first 4 decoded bytes)
#   - *version: 0 if flagA==0 (this is NOT the EXE version).
#   - hashed message = LE32(dword) ‖ ":%hu.%hu.%hu.%hu:" ‖ byte(auth)
#       the version quad comes from GetModuleFileNameW(NULL) -> Game.exe ->
#       VS_FIXEDFILEINFO (HIWORD(MS).LOWORD(MS).HIWORD(LS).LOWORD(LS)).
#       Fallback on failure: the literal string ":0.0.0.0:".
#       auth = 1 ONLY if BOTH Authenticode checks pass (Game.exe AND
#       CheckRevision.dll). On a real PC: 1. On our side today: 0,
#       because our Authenticode shims are fabricated.
#   - SHA-1(message) -> base64 (flags BASE64|NOCR) -> 28 characters + "\n"
#       *checksum = LE32 of the first 4 base64 characters
#       info      = characters 4.. (24 characters + the "\n"), NUL-terminated
#
# USAGE
#   python3 tools/checkrevision_ref.py                       # oracle + P0 + P1
#   python3 tools/checkrevision_ref.py --seed dp26DAAA --auth 1
#   python3 tools/checkrevision_ref.py --exe ~/d2-vita-refs/1.14d/Game.exe
#   python3 tools/checkrevision_ref.py --selftest            # P0+P1, return code
#
# P0 (prove the oracle before drawing conclusions): two witness runs with the
# same seed must return the same pair, otherwise a later FAIL is just noise.
# P1 (sensitivity test): a seed bit, then auth, then the version must EACH
# change the checksum. An oracle that always returns the same thing proves
# nothing.
import argparse, base64, hashlib, os, struct, sys

DEFAULT_EXE  = os.path.expanduser("~/d2-vita-refs/1.14d/Game.exe")
DEFAULT_SEED = "dp26DAAA"           # the seed referenced by tools/bncs_local.py
FALLBACK_VER = ":0.0.0.0:"          # .rdata 0x1003d7f0 — DLL's fallback


def fixed_file_info(path):
    """(ms, ls) from a PE's VS_FIXEDFILEINFO, read DIRECTLY from the file.
    No emulation: this is the same 0xFEEF04BD signature that build_verinfo
    looks for in tools/rt_boot.cpp."""
    with open(path, "rb") as f:
        blob = f.read()
    sig = blob.find(b"\xBD\x04\xEF\xFE")
    if sig < 0 or sig + 52 > len(blob):
        raise ValueError("VS_FIXEDFILEINFO introuvable dans %s" % path)
    ms, ls = struct.unpack_from("<II", blob, sig + 8)   # +8 = FileVersionMS/LS
    return ms, ls


def version_string(ms, ls):
    return ":%u.%u.%u.%u:" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def check_revision(seed_b64, auth, ver_str):
    """Returns (version, checksum, info, sha1_b64, message) — the EXACT output of
    the export for this seed, this Authenticode byte, and this string."""
    raw = base64.b64decode(seed_b64)
    if len(raw) < 4:
        raise ValueError("graine trop courte (%d octets decodes)" % len(raw))
    flag_a = raw[-1] != 0
    version = 0 if not flag_a else None      # flagA!=0 -> MessageBoxW, out of scope
    msg = raw[:4] + ver_str.encode("ascii") + bytes([auth & 0xFF])
    b64 = base64.b64encode(hashlib.sha1(msg).digest()).decode("ascii") + "\n"
    checksum = struct.unpack("<I", b64[:4].encode("ascii"))[0]
    # `info` = characters 4..27 of the base64, WITHOUT the newline. This
    # contradicts the disassembly note ("24 characters + \n"): two independent
    # implementations agree exactly with each other instead — our runtime
    # under qemu (D2CRTEST) and a real Win32 build (tools/crref under Wine,
    # original CryptoAPI). The DLL copies 24 characters then NUL; the newline
    # stays in the base64 buffer. Follow the measured behavior, not the note.
    info = b64[4:28]
    return version, checksum, info, b64, msg


def show(tag, seed, auth, ver_str):
    version, checksum, info, b64, msg = check_revision(seed, auth, ver_str)
    print("%-10s seed=%-10s auth=%d  version=%s  checksum=0x%08x" %
          (tag, seed, auth, "0x%08x" % version if version is not None else "<MessageBox>", checksum))
    print("%-10s   chaine hachee = %r" % ("", ver_str))
    print("%-10s   message       = %s" % ("", msg.hex()))
    print("%-10s   sha1 b64      = %r" % ("", b64))
    print("%-10s   info          = %r" % ("", info))
    return checksum, info


def selftest(exe, seed):
    """Runs P0 then P1. Returns 0 if everything passes."""
    ok = True

    def chk(name, cond):
        nonlocal ok
        print("  [%s] %s" % ("PASS" if cond else "FAIL", name))
        ok = ok and cond

    try:
        ms, ls = fixed_file_info(exe)
        ver = version_string(ms, ls)
        print("=== VS_FIXEDFILEINFO de %s : MS=0x%08x LS=0x%08x -> %r" % (exe, ms, ls, ver))
    except Exception as e:                       # no refs available: fall back to the known string
        ver = ":1.14.3.71:"
        print("=== Game.exe illisible (%s) — chaine de reference %r" % (e, ver))

    print("\n=== P0 — deux executions temoins (l'oracle doit etre stable) ===")
    a = check_revision(seed, 0, ver)[1:3]
    b = check_revision(seed, 0, ver)[1:3]
    chk("P0 auth=0 : deux passes identiques (0x%08x)" % a[0], a == b)
    a1 = check_revision(seed, 1, ver)[1:3]
    b1 = check_revision(seed, 1, ver)[1:3]
    chk("P0 auth=1 : deux passes identiques (0x%08x)" % a1[0], a1 == b1)

    print("\n=== P1 — banc de sensibilite (un oracle immobile ne prouve rien) ===")
    chk("auth 0 -> 1 change le checksum (0x%08x != 0x%08x)" % (a[0], a1[0]), a[0] != a1[0])
    raw = bytearray(base64.b64decode(seed))
    raw[0] ^= 1
    seed2 = base64.b64encode(bytes(raw)).decode()
    c2 = check_revision(seed2, 0, ver)[1]
    chk("1 bit de graine change le checksum (0x%08x != 0x%08x)" % (a[0], c2), a[0] != c2)
    c3 = check_revision(seed, 0, FALLBACK_VER)[1]
    chk("la version change le checksum (%r -> 0x%08x)" % (FALLBACK_VER, c3), a[0] != c3)
    # The silent ":0.0.0.0:" fallback is THE console trap (host_path failing):
    # print its value so a console log can spot it at a glance.
    print("  [info] checksum du repli %r = 0x%08x  (le voir => host_path a echoue)" % (FALLBACK_VER, c3))

    if ver == ":1.14.3.71:":
        print("\n=== Valeurs de reference (graine %s, chaine %r) ===" % (DEFAULT_SEED, ver))
        r0 = check_revision(DEFAULT_SEED, 0, ver)
        r1 = check_revision(DEFAULT_SEED, 1, ver)
        print("  auth=0 (nous aujourd'hui) : checksum=0x%08x info=%r" % (r0[1], r0[2]))
        print("  auth=1 (PC reel)          : checksum=0x%08x info=%r" % (r1[1], r1[2]))

    print("\n%s" % ("PASS : oracle CheckRevision arme (P0+P1)" if ok else "FAIL : oracle CheckRevision"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="Oracle hors ligne CheckRevision 1.14d (aucun reseau)")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--seed", default=DEFAULT_SEED)
    ap.add_argument("--auth", type=int, default=None, help="0 ou 1 ; par defaut les deux")
    ap.add_argument("--version-string", default=None, help="force la chaine, ex ':1.14.3.71:'")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--quiet", action="store_true", help="n'imprime que 'checksum info'")
    a = ap.parse_args()

    if a.version_string:
        ver = a.version_string
    else:
        try:
            ms, ls = fixed_file_info(a.exe)
            ver = version_string(ms, ls)
        except Exception as e:
            print("!! %s — repli sur ':1.14.3.71:'" % e, file=sys.stderr)
            ver = ":1.14.3.71:"

    if a.selftest:
        return selftest(a.exe, a.seed)

    if a.quiet:
        auth = 1 if a.auth is None else a.auth
        _, ck, info, _, _ = check_revision(a.seed, auth, ver)
        print("0x%08x %s" % (ck, info.rstrip("\n")))
        return 0

    print("=== oracle CheckRevision 1.14d — chaine hachee %r ===" % ver)
    for auth in ([a.auth] if a.auth is not None else [0, 1]):
        show("auth=%d" % auth, a.seed, auth, ver)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
