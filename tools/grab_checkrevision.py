#!/usr/bin/env python3
# =============================================================================
# !!! WARNING — THIS SCRIPT CONNECTS TO AN OFFICIAL BATTLE.NET SERVER !!!
# It dials useast/europe/uswest/asia.battle.net. HOUSE RULE #2 forbids any
# official connection, whether from qemu, from the PC, or by any other means.
# DO NOT RUN THIS.
#
# It no longer has a reason to exist: the reference MPQ is ALREADY captured
# (~/d2-vita-refs/1.14d/CheckRevision.mpq, 168,704 bytes) and the DLL it
# contains is in the reference repo. Everything needed is computed OFFLINE
# with tools/checkrevision_ref.py (oracle) and verified with
# tools/oracle_checkrevision.sh (runtime leg).
#
# Safety guard: the script REFUSES to start without
#     D2_ALLOW_OFFICIAL=je-comprends-le-risque
# =============================================================================
import os as _os, sys as _sys
if _os.environ.get("D2_ALLOW_OFFICIAL") != "je-comprends-le-risque":
    _sys.stderr.write(
        "grab_checkrevision.py: REFUS — ce script parle a un serveur Battle.net "
        "OFFICIEL (regle de la maison n2).\n"
        "Le MPQ de reference est deja capture ; l'oracle hors ligne est "
        "tools/checkrevision_ref.py.\n")
    _sys.exit(2)
"""KEYLESS capture of the CheckRevision.mpq file (ExtraWork) from Battle.net classic.

D2 1.14d uses the LOCKDOWN/ExtraWork version check: the server sends the name
of a signed MPQ (CheckRevision.mpq) in SID_AUTH_INFO; the client downloads it
via BNFTP, authenticates the archive's signature, extracts an ExtraWork DLL
from it, loads it, and runs it. We run the real Game.exe, so D2 itself does
all of that — we only need the CORRECT signed bytes.

This script speaks ONLY the strict minimum, keyless:
    0x01  (selector)  ->  SID_AUTH_INFO(0x50)  [replays D2 1.14d's exact bytes]
    <- SID_PING(0x25)  ->  echo
    <- SID_AUTH_INFO(0x50)  : reads mpqFilename + filetime + formula
    new connection: 0x02 (BNFTP)  ->  file request  ->  saves the bytes
It **NEVER sends** SID_AUTH_CHECK(0x51). CD keys are only ever sent in 0x51,
so no key is ever transmitted, by construction.

Saves the MPQ to ~/d2-vita-refs/1.14d/CheckRevision.mpq and logs the raw
headers (to reproduce the exact format in the local BNCS server).

Usage:
    python3 tools/grab_checkrevision.py                 # tries several gateways
    python3 tools/grab_checkrevision.py europe.battle.net
"""
import os, socket, struct, sys, binascii, time

OUT = os.path.expanduser(os.environ.get(
    "CR_OUT", "~/d2-vita-refs/1.14d/CheckRevision.mpq"))

# Battle.net classic gateways (the protocol is still live).
GATEWAYS = sys.argv[1:] or [
    "useast.battle.net", "europe.battle.net", "uswest.battle.net", "asia.battle.net",
]

# EXACT SID_AUTH_INFO sent by D2 1.14d LoD (captured byte-for-byte against the
# local server). platform=IX86 product=D2XP verbyte=14. Keyless.
CLIENT_AUTH_INFO = bytes.fromhex(
    "ff502e00" "00000000" "36385849" "50583244" "0e000000" "52467266"
    "7f000001" "00000000" "09040000" "09040000" "31000000" "3100")

def hexdump(b, n=96):
    h = binascii.hexlify(b[:n]).decode()
    return " ".join(h[i:i+2] for i in range(0, len(h), 2)) + (" ..." if len(b) > n else "")

def recv_msg(s):
    """Reads one complete BNCS message (0xFF id lenLE)."""
    hdr = b""
    while len(hdr) < 4:
        c = s.recv(4 - len(hdr))
        if not c:
            return None
        hdr += c
    if hdr[0] != 0xFF:
        raise IOError("octet BNCS inattendu 0x%02x" % hdr[0])
    mlen = struct.unpack("<H", hdr[2:4])[0]
    body = b""
    while len(body) < mlen - 4:
        c = s.recv(mlen - 4 - len(body))
        if not c:
            break
        body += c
    return hdr[1], hdr + body, body

def bncs(mid, payload=b""):
    return struct.pack("<BBH", 0xFF, mid, len(payload) + 4) + payload

def cstr(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("latin1"), end + 1

def do_handshake(host):
    ip = socket.gethostbyname(host)
    print("[*] %s -> %s : connexion BNCS 6112" % (host, ip)); sys.stdout.flush()
    s = socket.socket(); s.settimeout(10); s.connect((ip, 6112))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"\x01")                       # game protocol selector
    s.sendall(CLIENT_AUTH_INFO)              # SID_AUTH_INFO (keyless)
    print("    C>S SID_AUTH_INFO envoyé (keyless)")
    mpqname = None; filetime = 0; formula = None
    t0 = time.time()
    while time.time() - t0 < 15:
        m = recv_msg(s)
        if not m:
            break
        mid, full, body = m
        print("    S>C id=0x%02x len=%d : %s" % (mid, len(full), hexdump(full)))
        if mid == 0x25:                      # SID_PING -> echo
            s.sendall(bncs(0x25, body))
            print("    C>S SID_PING echo")
        elif mid == 0x50:                    # SID_AUTH_INFO reply
            logon, token, udp = struct.unpack("<3I", body[0:12])
            filetime = struct.unpack("<Q", body[12:20])[0]
            mpqname, o = cstr(body, 20)
            formula, _ = cstr(body, o)
            print("    >>> logon=%d token=0x%08x udp=0x%08x filetime=%d" % (logon, token, udp, filetime))
            print("    >>> mpq=%r formule=%r" % (mpqname, formula))
            break
    s.close()
    if not mpqname:
        raise IOError("pas de SID_AUTH_INFO reply")
    return ip, mpqname, filetime, formula

def bnftp_download(ip, mpqname, filetime):
    print("[*] BNFTP : téléchargement de %r (filetime=%d)" % (mpqname, filetime))
    s = socket.socket(); s.settimeout(20); s.connect((ip, 6112))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"\x02")                        # BNFTP selector
    name = mpqname.encode("latin1") + b"\x00"
    # BNFTP v1 request: replays D2's EXACT bytes (platform/product codes in
    # REVERSED order like D2: "IX86"->36385849, "D2DV"->56443244), filetime=0
    # => full file. Only the filename changes.
    body = bytes.fromhex("0001"          # protocol version (D2's EXACT bytes: 00 01)
                         "36385849"       # platform  IX86 (reversed)
                         "56443244"       # product   D2DV (reversed)
                         + "00" * 20)     # banner(4)+bannerExt(4)+filePos(4)+filetime(8)
    body += name
    req = struct.pack("<H", 2 + len(body)) + body     # WORD header length
    s.sendall(req)
    print("    C>S BNFTP req : %s" % hexdump(req))
    # response: reads everything that arrives (the file is small), robust
    s.settimeout(8)
    buf = b""
    try:
        while True:
            c = s.recv(65536)
            if not c:
                break
            buf += c
    except socket.timeout:
        pass
    s.close()
    if len(buf) < 2:
        print("    S>C BNFTP : réponse VIDE (%d octet)" % len(buf)); return b""
    hdrlen = struct.unpack("<H", buf[:2])[0]
    print("    S>C BNFTP header brut (hdrlen annoncé=%d, total reçu=%d) : %s"
          % (hdrlen, len(buf), hexdump(buf, min(hdrlen, 96))))
    if hdrlen < 6 or hdrlen > len(buf):
        print("    !! hdrlen incohérent, je renvoie tout le buffer brut")
        return buf
    filesize = struct.unpack("<I", buf[2:6])[0]
    print("    >>> filesize annoncé = %d octets ; données dispo = %d" % (filesize, len(buf) - hdrlen))
    data = buf[hdrlen:]
    return data[:filesize] if filesize and filesize <= len(data) else data

def main():
    last = None
    for host in GATEWAYS:
        try:
            ip, mpqname, filetime, formula = do_handshake(host)
            data = bnftp_download(ip, mpqname, filetime)
            if not data:
                print("[!] %s : 0 octet reçu" % host); continue
            magic = data[:4]
            print("[*] %d octets reçus, magic=%s" % (len(data), binascii.hexlify(magic).decode()))
            outp = os.path.expanduser(OUT)
            os.makedirs(os.path.dirname(outp), exist_ok=True)
            with open(outp, "wb") as f:
                f.write(data)
            ok = magic in (b"MPQ\x1a", b"MPQ\x1b")
            print("[%s] sauvé %d octets -> %s (magic MPQ: %s)"
                  % ("OK" if ok else "??", len(data), outp, ok))
            if ok:
                return 0
            last = "magic inattendu %r" % magic
        except Exception as e:
            print("[!] %s : %r" % (host, e)); last = repr(e)
    print("[X] échec sur toutes les passerelles. Dernier: %s" % last)
    return 1

if __name__ == "__main__":
    sys.exit(main())
