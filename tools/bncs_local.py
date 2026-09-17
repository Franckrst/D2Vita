#!/usr/bin/env python3
"""Local mini BNCS (Battle.net classic) server — debug oracle for D2Vita.

Purpose: point D2 1.14d at 127.0.0.1 instead of Blizzard's servers (see the
connect() redirection in rt_boot.cpp, env D2BNCS_LOCAL). Makes all network
debugging SAFE (no traffic with real CD keys) and gives byte-by-byte
visibility into what D2 sends at each handshake step.

Protocol (reference: BNETDocs):
  - First byte of each connection = selector: 0x01 BNCS(game), 0x02 BNFTP, 0x03 chat.
  - BNCS message header: 0xFF, uint8 id, uint16 len (LE, total length).
  - Login handshake:
      C>S 0x50 SID_AUTH_INFO  ->  S>C 0x25 SID_PING ; S>C 0x50 SID_AUTH_INFO(reply)
      C>S 0x25 SID_PING(echo)
      C>S 0x51 SID_AUTH_CHECK ->  S>C 0x51 result=0 (success)
      C>S 0x3A SID_LOGONRESPONSE2 / 0x53 ... -> success

Usage:
  python3 tools/bncs_local.py            # listens on 127.0.0.1:6112, verbose log
  BNCS_PORT=6112 BNCS_HOST=127.0.0.1 python3 tools/bncs_local.py

Every connection and every byte is logged (stdout + tools/bncs_local.log).
Server replies are deliberately simple and easy to edit here, to help
bisect the D2 network thread crash (Game.exe+0x11f550).
"""
import os, socket, struct, sys, threading, time, binascii

HOST = os.environ.get("BNCS_HOST", "127.0.0.1")
PORT = int(os.environ.get("BNCS_PORT", "6112"))
LOGPATH = os.environ.get("BNCS_LOG", os.path.join(os.path.dirname(__file__), "bncs_local.log"))

# --- SID_AUTH_INFO reply (S>C): modeled on the real Blizzard 1.14d server ---
# Blizzard sends logon=0, mpq="CheckRevision.mpq", a real filetime, and a
# RANDOM FORMULA (valueString) per connection (lockdown seed, e.g. "+NYYsAAA").
SERVER_TOKEN = 0xDEADBEEF
UDP_VALUE    = 0x00000000
# filetime of CheckRevision.mpq as captured from the real server (see grab_checkrevision.py)
MPQ_FILETIME = int(os.environ.get("BNCS_FILETIME", "134124260780000000"))
MPQ_FILENAME = os.environ.get("BNCS_MPQ", "CheckRevision.mpq")
CR_FORMULA   = os.environ.get("BNCS_FORMULA", "")   # "" => random seed per connection

# --- Realm / MCP (closed Battle.net path: realm -> MCP server -> character list) ---
REALM_TITLE = os.environ.get("REALM_TITLE", "D2Vita")
REALM_DESC  = os.environ.get("REALM_DESC",  "Local D2Vita Realm")
MCP_IP      = os.environ.get("MCP_IP",   "127.0.0.1")   # MCP server IP announced in SID_LOGONREALMEX
MCP_PORT    = int(os.environ.get("MCP_PORT", "6113"))   # local MCP server port
MCP_UNIQUE  = os.environ.get("MCP_UNIQUE", "D2Vita")    # unique Battle.net name returned
LOGON_TYPE   = int(os.environ.get("BNCS_LOGON", "0"))  # 0 = Broken SHA-1 (D2)
# file served by BNFTP: the real signed MPQ from Blizzard
BNFTP_FILE   = os.environ.get("BNCS_BNFTP_FILE",
    os.path.expanduser("~/d2-vita-refs/1.14d/CheckRevision.mpq"))

SEED_BY_PEER = {}          # peer -> seed sent in SID_AUTH_INFO (strict mode)

_loglock = threading.Lock()
_logf = open(LOGPATH, "a", buffering=1)

def log(msg):
    line = "%.3f %s" % (time.time() % 100000, msg)
    with _loglock:
        print(line, flush=True)
        _logf.write(line + "\n")

def hexdump(b, limit=64):
    h = binascii.hexlify(b[:limit]).decode()
    h = " ".join(h[i:i+2] for i in range(0, len(h), 2))
    return h + (" ..." if len(b) > limit else "")

def cstr(buf, off):
    """Reads a C-string starting at off, returns (str, offset_after_null)."""
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("latin1"), end + 1

# --- BNCS S>C message builders -------------------------------------
def bncs(msg_id, payload=b""):
    return struct.pack("<BBH", 0xFF, msg_id, len(payload) + 4) + payload

def sid_ping(value=0x11223344):
    return bncs(0x25, struct.pack("<I", value))

def gen_formula():
    """Lockdown seed (valueString): base64 of [4 random bytes][00 00].
    The last 2 decoded bytes MUST be zero: otherwise CheckRevision.dll overflows
    a stack buffer (fastfail cookie GS 0xC0000409), on emulator and on real
    hardware alike. Blizzard only ever sends valid seeds (e.g. 'dp26DAAA' ->
    76 9d ba 0c 00 00)."""
    if CR_FORMULA:
        return CR_FORMULA
    import base64
    while True:
        f = base64.b64encode(os.urandom(4) + b"\x00\x00").decode("ascii")
        if "+" not in f and "/" not in f:   # clean base64 (matches Blizzard)
            return f

# ===================== STRICT MODE (the server COMPARES) =================
# A permissive server that always answers PASSED hides client bugs instead of
# catching them.
#
#   BNCS_STRICT=1              server recomputes (version, checksum, info)
#                              via the tools/checkrevision_ref.py oracle and
#                              REJECTS any mismatch, logging the delta field by field.
#   BNCS_FORMULA=dp26DAAA      pins the seed (otherwise it's random per
#                              connection and nothing is reproducible).
#   BNCS_STRICT_AUTH=1|0       expected Authenticode byte. 1 = what a real PC
#                              would send; 0 = what our shims currently
#                              produce (our Authenticode shims are fabricated).
#                              Default is 1: the private server should demand
#                              what an official one would.
#   BNCS_EXE_VERSION           expected version quad (default 1.14.3.71).
STRICT       = os.environ.get("BNCS_STRICT", "") not in ("", "0")
STRICT_AUTH  = int(os.environ.get("BNCS_STRICT_AUTH", "1"))
EXE_VERSION  = os.environ.get("BNCS_EXE_VERSION", "1.14.3.71")
# Real SID_AUTH_CHECK rejection codes (BNETDocs): 0x100 version too old,
# 0x101 invalid version, 0x102 version needs downgrade.
AUTH_INVALID_VERSION = 0x101

_ref = None
def _oracle(seed, auth):
    """Returns (checksum, info) via the OFFLINE oracle. Lazy import: the
    server must still start even if the repo is incomplete."""
    global _ref
    if _ref is None:
        import importlib.util
        sp = importlib.util.spec_from_file_location(
            "checkrevision_ref", os.path.join(os.path.dirname(__file__), "checkrevision_ref.py"))
        _ref = importlib.util.module_from_spec(sp); sp.loader.exec_module(_ref)
    _v, ck, info, _b, _m = _ref.check_revision(seed, auth, ":%s:" % EXE_VERSION)
    return ck, info


def parse_auth_check(body):
    """Decodes SID_AUTH_CHECK WITHOUT ever returning secret material.
    Returns (fields, safe_summary); `safe_summary` contains NEITHER the public
    value NOR the hash — both are derived from the user's CD key and have no
    place in a shareable log."""
    ctok, ver, cksum, nkeys, spawn = struct.unpack("<5I", body[:20])
    off, keys = 20, []
    for _ in range(nkeys):
        if off + 36 > len(body):
            break
        klen, prod = struct.unpack("<2I", body[off:off + 8])
        keys.append((klen, prod))           # length + product ONLY
        off += 36
    info = ""
    try:
        info, off2 = cstr(body, off)
    except Exception:
        off2 = off
    return dict(ctok=ctok, ver=ver, cksum=cksum, nkeys=nkeys, spawn=spawn,
                keys=keys, info=info), (
        "client_token=0x%08x version=0x%08x checksum=0x%08x nkeys=%d spawn=%d "
        "cles=[%s] exeInfo=%r"
        % (ctok, ver, cksum, nkeys, spawn,
           ", ".join("len=%d product=0x%02x <valeur publique et condensat MASQUES>"
                     % (l, p) for l, p in keys), info))


def strict_verdict(seed, f):
    """Returns (result, delta_lines). result 0x000 = accepted."""
    want_ck, want_info = _oracle(seed, STRICT_AUTH)
    deltas = []
    # The 1.14d DLL returns *version = 0 for a Blizzard seed (flagA==0): this is
    # NOT the EXE version. 0x100 is the signature of the 27-byte stub.
    if f["ver"] != 0:
        deltas.append("version: recu 0x%08x, attendu 0x00000000%s"
                      % (f["ver"], "   <-- 0x100 = le STUB de rt_boot.cpp" if f["ver"] == 0x100 else ""))
    if f["cksum"] != want_ck:
        deltas.append("checksum: recu 0x%08x, attendu 0x%08x (graine %s, auth=%d, %s)"
                      % (f["cksum"], want_ck, seed, STRICT_AUTH, EXE_VERSION))
    if f["info"] != want_info:
        deltas.append("exeInfo: recu %r, attendu %r" % (f["info"], want_info))
    return (0x000 if not deltas else AUTH_INVALID_VERSION), deltas


def _selftest_strict():
    """MANDATORY NEGATIVE TEST: without it, "strict mode accepted" proves
    nothing — a strict mode that accepts EVERYTHING would too. Builds two
    packets and requires accept / REJECT."""
    seed = "dp26DAAA"
    ck, info = _oracle(seed, STRICT_AUTH)

    def pkt(version, checksum, exeinfo):
        b = struct.pack("<5I", 0x11223344, version, checksum, 2, 0)
        b += (struct.pack("<4I", 26, 0x18, 0x011cb862, 0) + b"\x00" * 20) * 2
        return b + exeinfo.encode() + b"\x00" + b"tester\x00"

    ok = True
    f, sur = parse_auth_check(pkt(0, ck, info))
    r, d = strict_verdict(seed, f)
    print("  [%s] paquet JUSTE  -> result=0x%03x %s" % ("PASS" if r == 0 else "FAIL", r, d))
    ok &= (r == 0)
    print("       resume: %s" % sur)
    ok &= ("011cb862" not in sur and "condensat" in sur)
    print("  [%s] le resume ne contient NI valeur publique NI condensat" % ("PASS" if ok else "FAIL"))
    for label, p in (("version du STUB (0x100)", pkt(0x100, ck, info)),
                     ("checksum faux",           pkt(0, ck ^ 1, info)),
                     ("exeInfo faux",            pkt(0, ck, "XXXX"))):
        f, _ = parse_auth_check(p)
        r, d = strict_verdict(seed, f)
        print("  [%s] %-24s -> result=0x%03x  %s"
              % ("PASS" if r != 0 else "FAIL", label, r, " | ".join(d)))
        ok &= (r != 0)
    print("PASS : mode strict comparant (accepte le juste, REFUSE le faux)" if ok
          else "FAIL : mode strict")
    return 0 if ok else 1


def sid_auth_info_reply(formula):
    p  = struct.pack("<I", LOGON_TYPE)
    p += struct.pack("<I", SERVER_TOKEN)
    p += struct.pack("<I", UDP_VALUE)
    p += struct.pack("<Q", MPQ_FILETIME)          # FILETIME (8 bytes)
    p += MPQ_FILENAME.encode("latin1") + b"\x00"
    p += formula.encode("latin1") + b"\x00"
    return bncs(0x50, p)

def sid_auth_check_reply(result=0x000, info=""):
    return bncs(0x51, struct.pack("<I", result) + info.encode("latin1") + b"\x00")

MSG_NAMES = {
    0x00:"SID_NULL", 0x25:"SID_PING", 0x50:"SID_AUTH_INFO", 0x51:"SID_AUTH_CHECK",
    0x3A:"SID_LOGONRESPONSE2", 0x53:"SID_AUTH_ACCOUNTLOGON",
    0x54:"SID_AUTH_ACCOUNTLOGONPROOF", 0x1E:"SID_JOINCHANNEL", 0x0B:"SID_ENTERCHAT",
    0x14:"SID_UDPPINGRESPONSE", 0x2C:"SID_STARTVERSIONING", 0x06:"SID_STARTVERSIONING",
    0x0A:"SID_ENTERCHAT", 0x33:"SID_GETFILETIME", 0x40:"SID_QUERYREALMS2",
    0x3E:"SID_LOGONREALMEX", 0x3D:"SID_CREATEACCOUNT2",
}

def handle_bncs(conn, peer):
    log("[%s] protocole 0x01 (BNCS jeu)" % peer)
    # Real BNCS: the server sends SID_PING right after connecting.
    conn.sendall(sid_ping())
    log("[%s] S>C SID_PING envoye" % peer)
    buf = b""
    while True:
        chunk = conn.recv(4096)
        if not chunk:
            log("[%s] fermeture (recv 0)" % peer); return
        buf += chunk
        while len(buf) >= 4:
            if buf[0] != 0xFF:
                log("[%s] !! octet inattendu 0x%02x, resync" % (peer, buf[0])); buf = buf[1:]; continue
            mid = buf[1]; mlen = struct.unpack("<H", buf[2:4])[0]
            if mlen < 4 or len(buf) < mlen:
                break
            msg = buf[:mlen]; buf = buf[mlen:]
            name = MSG_NAMES.get(mid, "0x%02x" % mid)
            # SID_AUTH_CHECK (0x51) carries, for EACH CD key, a public value
            # (derived directly from the key, STABLE across sessions) and a
            # 20-byte hash. Dumping that in hex would leak secret material
            # into a log file that's easy to paste into a report, so only the
            # header is printed here; the body is summarized field by field,
            # masked, by parse_auth_check().
            if mid == 0x51:
                log("[%s] C>S %s len=%d : <corps MASQUE (cles CD)>" % (peer, name, mlen))
            else:
                log("[%s] C>S %s len=%d : %s" % (peer, name, mlen, hexdump(msg)))
            dispatch_bncs(conn, peer, mid, msg[4:])

def dispatch_bncs(conn, peer, mid, body):
    if mid == 0x50:  # client SID_AUTH_INFO -> decode + reply
        try:
            (proto, plat, prod, verb, lang, lip, tz, mpqloc, ulang) = struct.unpack("<9I", body[:36])
            cabbr, o = cstr(body, 36); country, _ = cstr(body, o)
            log("[%s]     platform=%s product=%s verbyte=%d pays=%s/%s"
                % (peer, struct.pack("<I", plat)[::-1].decode("latin1", "replace"),
                   struct.pack("<I", prod)[::-1].decode("latin1", "replace"), verb, cabbr, country))
        except Exception as e:
            log("[%s]     (decode 0x50 partiel: %s)" % (peer, e))
        formula = gen_formula()
        SEED_BY_PEER[peer] = formula          # needed by strict mode
        conn.sendall(sid_auth_info_reply(formula))
        log("[%s] S>C SID_AUTH_INFO reponse (logon=%d token=0x%08x mpq=%s filetime=%d formula=%r)"
            % (peer, LOGON_TYPE, SERVER_TOKEN, MPQ_FILENAME, MPQ_FILETIME, formula))
    elif mid == 0x25:  # SID_PING echo
        log("[%s]     ping echo = %s" % (peer, hexdump(body)))
    elif mid == 0x51:  # SID_AUTH_CHECK
        try:
            f, sur = parse_auth_check(body)
            log("[%s]     %s" % (peer, sur))
        except Exception as e:
            log("[%s]     (decode 0x51 partiel: %s)" % (peer, e)); f = None
        if STRICT and f is not None:
            seed = SEED_BY_PEER.get(peer, CR_FORMULA or "?")
            res, deltas = strict_verdict(seed, f)
            for d in deltas:
                log("[%s]     STRICT !! %s" % (peer, d))
            conn.sendall(sid_auth_check_reply(res))
            log("[%s] S>C SID_AUTH_CHECK result=0x%03x (%s)"
                % (peer, res, "PASSED" if res == 0 else "REFUSE — version invalide"))
        else:
            conn.sendall(sid_auth_check_reply(0x000))
            log("[%s] S>C SID_AUTH_CHECK result=0x000 (PASSED — MODE COMPLAISANT, "
                "BNCS_STRICT=1 pour COMPARER)" % peer)
    elif mid == 0x33:  # SID_GETFILETIME -> echo reqID/unknown + filetime + name
        try:
            reqid = body[0:4]; unk = body[4:8]; fname, _ = cstr(body, 8)
        except Exception:
            reqid = b"\x00" * 4; unk = b"\x00" * 4; fname = "?"
        p = reqid + unk + struct.pack("<Q", MPQ_FILETIME) + fname.encode("latin1") + b"\x00"
        conn.sendall(bncs(0x33, p))
        log("[%s] S>C SID_GETFILETIME %r (filetime renvoyé)" % (peer, fname))
    elif mid == 0x3A:  # SID_LOGONRESPONSE2 -> success (0)
        conn.sendall(bncs(0x3A, struct.pack("<I", 0)))
        log("[%s] S>C SID_LOGONRESPONSE2 = 0 (succes)" % peer)
    elif mid == 0x3D:  # SID_CREATEACCOUNT2 -> success (result 0)
        conn.sendall(bncs(0x3D, struct.pack("<I", 0) + b"\x00"))
        log("[%s] S>C SID_CREATEACCOUNT2 = 0 (compte cree)" % peer)
    elif mid == 0x0A:  # SID_ENTERCHAT -> unique name / statstring / account
        acct = "d2vita"
        p = acct.encode() + b"\x00" + b"\x00" + acct.encode() + b"\x00"
        conn.sendall(bncs(0x0A, p))
        log("[%s] S>C SID_ENTERCHAT (name=%s)" % (peer, acct))
    elif mid == 0x0B:  # SID_GETCHANNELLIST -> minimal list
        p = b"Diablo II\x00" + b"\x00"
        conn.sendall(bncs(0x0B, p))
        log("[%s] S>C SID_GETCHANNELLIST" % peer)
    elif mid == 0x0C:  # SID_JOINCHANNEL -> CHATEVENT (enters the channel)
        try:
            chan, _ = cstr(body, 4)
        except Exception:
            chan = "Diablo II"
        def chatevent(eid, user, text):
            p = struct.pack("<6I", eid, 0, 0, 0, 0, 0) + user.encode() + b"\x00" + text.encode() + b"\x00"
            return bncs(0x0F, p)   # SID_CHATEVENT
        conn.sendall(chatevent(7, chan, chan))    # EID_CHANNEL: places the client in the channel
        conn.sendall(chatevent(1, "d2vita", "")) # EID_SHOWUSER (self)
        log("[%s] S>C JOINCHANNEL -> %r" % (peer, chan))
    elif mid == 0x0E:  # SID_CHATCOMMAND -> possible echo (ignored)
        log("[%s]     chatcommand: %s" % (peer, hexdump(body, 40)))
    elif mid == 0x1F:  # SID_STARTADVEX3 (create game) -> success (0)
        conn.sendall(bncs(0x1F, struct.pack("<I", 0)))
        log("[%s] S>C SID_STARTADVEX3 = 0 (partie creee)" % peer)
    elif mid == 0x09:  # SID_GETADVLISTEX (game list) -> empty (0 games)
        conn.sendall(bncs(0x09, struct.pack("<I", 0)))
        log("[%s] S>C SID_GETADVLISTEX = 0 partie" % peer)
    elif mid == 0x40:  # SID_QUERYREALMS2 -> announces 1 realm (Realm/MCP path)
        # S>C (BNETDocs /packet/277): (UINT32)unknown=0, (UINT32)count,
        #   per realm: (UINT32)unknown=1, (STRING)title, (STRING)description.
        p = struct.pack("<II", 0, 1)
        p += struct.pack("<I", 1) + REALM_TITLE.encode() + b"\x00" + REALM_DESC.encode() + b"\x00"
        conn.sendall(bncs(0x40, p))
        log("[%s] S>C SID_QUERYREALMS2 = 1 royaume (%s)" % (peer, REALM_TITLE))
    elif mid == 0x3E:  # SID_LOGONREALMEX -> connection info for the MCP server (realm)
        # C>S: (UINT32)ClientToken, (UINT8)[20] realm password hash, (STRING)realm title.
        client_token = struct.unpack("<I", body[:4])[0] if len(body) >= 4 else 0
        # S>C (classic BNETDocs /packet/237 format; 1.14d reads it backward-compat:
        #   IP @offset 16, Port @offset 20):
        #   (UINT32)MCP Cookie, (UINT32)MCP Status=0, (UINT32)[2]Chunk1,
        #   (UINT32)IP (BE), (UINT32)Port (2 bytes BE + 2 unknown),
        #   (UINT32)[12]Chunk2, (STRING)unique Battle.net name.
        # The "MCP chunk" (Cookie+Status+Chunk1+Chunk2 = 64 bytes) is sent back as-is
        # by the client to MCP_STARTUP; our MCP server accepts it without validating it.
        cookie  = client_token or 0xCAFED2D2
        chunk1  = b"\x00" * 8
        ip_be   = socket.inet_aton(MCP_IP)                     # 127.0.0.1 in network byte order
        port_be = struct.pack(">H", MCP_PORT) + b"\x00\x00"    # port, 2 bytes BE + 2 unknown
        chunk2  = b"\x00" * 48
        uniq    = MCP_UNIQUE.encode() + b"\x00"
        p = struct.pack("<II", cookie, 0) + chunk1 + ip_be + port_be + chunk2 + uniq
        conn.sendall(bncs(0x3E, p))
        log("[%s] S>C SID_LOGONREALMEX -> MCP %s:%d (cookie=0x%08x, len=%d)" % (peer, MCP_IP, MCP_PORT, cookie, len(p)))
    else:
        log("[%s]     (pas de reponse pour id 0x%02x)" % (peer, mid))

def handle_bnftp(conn, peer, first):
    """BNFTP (0x02): D2 downloads CheckRevision.mpq. Serves the REAL signed MPQ
    (BNFTP_FILE) with Blizzard's EXACT reply format (WORD padding included)."""
    log("[%s] protocole 0x02 (BNFTP)" % peer)
    buf = first
    while len(buf) < 2:
        c = conn.recv(4096)
        if not c: log("[%s] BNFTP ferme avant header" % peer); return
        buf += c
    hdr_len = struct.unpack("<H", buf[:2])[0]
    while len(buf) < hdr_len:
        c = conn.recv(4096)
        if not c: break
        buf += c
    log("[%s] BNFTP requete hdr_len=%d : %s" % (peer, hdr_len, hexdump(buf, 96)))
    # v1 request: [hdrlen 2][ver 2][platform 4][product 4][banner 4][bannerext 4]
    #              [filePos 4][filetime 8][filename\0]  -> filename @ offset 32
    filepos = 0
    try:
        filepos = struct.unpack("<I", buf[20:24])[0]
        fname, _ = cstr(buf, 32)
    except Exception:
        fname = "?"
    log("[%s] BNFTP fichier demandé=%r startpos=%d" % (peer, fname, filepos))
    # picks which file to serve by name: CheckRevision.mpq = the real signed
    # MPQ; anything else (bnserver-*.ini = realm list, etc.) = minimal file
    # so D2 continues on to login.
    payload = b""
    respname = fname if fname and fname != "?" else MPQ_FILENAME
    if "checkrevision" in fname.lower() and BNFTP_FILE and os.path.exists(BNFTP_FILE):
        payload = open(BNFTP_FILE, "rb").read()
        log("[%s] BNFTP sert %s (%d octets)" % (peer, BNFTP_FILE, len(payload)))
    else:
        # serves the requested file from the game dir (bnserver-D2DV.ini = gateway
        # list, captured keyless from Blizzard) if it exists.
        cand = os.path.join(os.path.dirname(BNFTP_FILE), os.path.basename(fname)) if BNFTP_FILE else ""
        if cand and os.path.exists(cand):
            payload = open(cand, "rb").read()
            log("[%s] BNFTP sert %s (%d octets)" % (peer, cand, len(payload)))
        else:
            log("[%s] BNFTP %r introuvable -> 0 octet" % (peer, fname))
    total = len(payload)
    if 0 < filepos <= total:
        payload = payload[filepos:]
    # BNFTP v1 reply (EXACT Blizzard format):
    #   [WORD hdrlen][WORD padding=0][DWORD filesize][DWORD bannerId]
    #   [DWORD bannerExt][FILETIME 8][filename\0][data]
    name = respname.encode("latin1") + b"\x00"
    rhdr  = struct.pack("<H", 0)                 # padding
    rhdr += struct.pack("<I", total)             # filesize (total file size)
    rhdr += struct.pack("<I", 0)                 # banner id
    rhdr += struct.pack("<I", 0)                 # banner ext
    rhdr += struct.pack("<Q", MPQ_FILETIME)      # filetime (8)
    rhdr += name
    resp = struct.pack("<H", 2 + len(rhdr)) + rhdr
    conn.sendall(resp + payload)
    log("[%s] BNFTP réponse hdrlen=%d + %d octets envoyée" % (peer, 2 + len(rhdr), len(payload)))
    # Signals end of file cleanly (FIN half-close) WITHOUT a reset: many
    # transfer clients read until EOF. Then keeps the connection open (like
    # Blizzard) until D2 closes it — never close first (avoids an RST that
    # would truncate the read on D2's side).
    try:
        conn.shutdown(socket.SHUT_WR)
    except Exception:
        pass
    try:
        conn.settimeout(120)
        while True:
            if not conn.recv(4096):
                break
    except Exception:
        pass

def handle(conn, addr):
    peer = "%s:%d" % addr
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        sel = conn.recv(1)
        if not sel:
            log("[%s] connexion vide" % peer); return
        b = sel[0]
        log("[%s] === NOUVELLE CONNEXION, selecteur=0x%02x ===" % (peer, b))
        if b == 0x01:
            handle_bncs(conn, peer)
        elif b == 0x02:
            handle_bnftp(conn, peer, b"")
        elif b == 0x03:
            log("[%s] protocole 0x03 (chat) — non gere" % peer)
        else:
            log("[%s] selecteur inconnu 0x%02x" % (peer, b))
    except ConnectionResetError:
        log("[%s] reset par le pair" % peer)
    except Exception as e:
        log("[%s] ERREUR: %r" % (peer, e))
    finally:
        try: conn.close()
        except Exception: pass
        log("[%s] connexion close" % peer)

# ===================== MCP server (realm) =====================
# 2nd TCP connection (IP:port announced by SID_LOGONREALMEX). Framing:
#   (UINT16 LE length incl. 3-byte header)(UINT8 id)(data). First byte = selector 0x01.
MCP_MSG = {0x01:"MCP_STARTUP",0x07:"MCP_CHARLOGON",0x19:"MCP_CHARLIST2",0x17:"MCP_CHARLIST",
           0x02:"MCP_CHARCREATE",0x03:"MCP_CREATEGAME",0x04:"MCP_JOINGAME",0x05:"MCP_GAMELIST"}
D2GS_IP   = os.environ.get("D2GS_IP",   "127.0.0.1")   # game server
D2GS_PORT = int(os.environ.get("D2GS_PORT", "4000"))
_chars = []   # realm characters, in memory: {name, cls, flags}

def mcp(mid, data=b""):
    return struct.pack("<HB", 3 + len(data), mid) + data

def statstring(ch):
    # t_d2charinfo_portrait struct (PvPGN d2cs) = 33 bytes + the 0x00
    # terminator added by the caller. Header 0x8084; chclass = class+1 (never
    # 0); status | 0x80 (PORTRAIT_MASK); gfx/color/u2 = 0xFF, u1 = 0x80. NO 0x00
    # before the end (caller appends based on strlen -> an internal 0x00 would truncate).
    cls    = ch.get("cls", 1) & 0xFF                       # 0=Ama..6=Asn
    flags  = ch.get("flags", 0x20)                         # 0x20 = expansion by default
    lvl    = ch.get("level", 1) or 1
    status = ((0x01 | (flags & (0x04 | 0x08 | 0x20 | 0x40))) | 0x80) & 0xFF   # INIT|flags|MASK
    ladder = 0x01 if (flags & 0x40) else 0xFF
    s  = b"\x84\x80"          # header 0x8084
    s += b"\xFF" * 11         # gfx[11] (no equipment)
    s += bytes([(cls + 1) & 0xFF])
    s += b"\xFF" * 11         # color[11]
    s += bytes([lvl & 0xFF])
    s += bytes([status])
    s += b"\x80\x80\x80"      # u1[3] = MASK
    s += bytes([ladder])
    s += b"\xFF\xFF"          # u2[2]
    return s                  # 33 bytes; caller appends the final 0x00

def dispatch_mcp(conn, peer, mid, body):
    log("[%s] C>S MCP %s (0x%02x) len=%d : %s" % (peer, MCP_MSG.get(mid,"?"), mid, len(body), hexdump(body,48)))
    if mid == 0x01:    # MCP_STARTUP -> success (the 1.14d blob isn't validated, undocumented)
        conn.sendall(mcp(0x01, struct.pack("<I", 0)))
        log("[%s] S>C MCP_STARTUP result=0 (OK)" % peer)
    elif mid == 0x07:  # MCP_CHARLOGON -> 0
        conn.sendall(mcp(0x07, struct.pack("<I", 0)))
        log("[%s] S>C MCP_CHARLOGON result=0" % peer)
    elif mid == 0x19:  # MCP_CHARLIST2 -> requested(u16) exist(u32) returned(u16) [expir,name,stat]*
        req = struct.unpack("<I", body[:4])[0] if len(body) >= 4 else 0
        chs = _chars[:8]
        p = struct.pack("<HIH", req & 0xFFFF, len(_chars), len(chs))
        for ch in chs:
            p += struct.pack("<I", 0x7FFFFFFF) + ch["name"].encode() + b"\x00" + statstring(ch) + b"\x00"  # expiration = never
        conn.sendall(mcp(0x19, p))
        log("[%s] S>C MCP_CHARLIST2 req=%d exist=%d returned=%d" % (peer, req, len(_chars), len(chs)))
    elif mid == 0x02:  # MCP_CHARCREATE -> stores + result 0
        cls = struct.unpack("<I", body[:4])[0] if len(body) >= 4 else 0
        flags = struct.unpack("<H", body[4:6])[0] if len(body) >= 6 else 0
        cname, _ = cstr(body, 6)
        _chars.append({"name": cname, "cls": cls, "flags": flags})
        conn.sendall(mcp(0x02, struct.pack("<I", 0)))
        log("[%s] S>C MCP_CHARCREATE '%s' cls=%d flags=0x%x -> 0" % (peer, cname, cls, flags))
    elif mid == 0x03:  # MCP_CREATEGAME -> reqid(u16) token(u16) unk(u16) result(u32)
        reqid = struct.unpack("<H", body[:2])[0] if len(body) >= 2 else 0
        conn.sendall(mcp(0x03, struct.pack("<HHHI", reqid, 0x0001, 0, 0)))
        log("[%s] S>C MCP_CREATEGAME reqid=%d token=1 result=0" % (peer, reqid))
    elif mid == 0x04:  # MCP_JOINGAME -> reqid,token,unk, D2GS IP (network order), hash, result(LE)
        reqid = struct.unpack("<H", body[:2])[0] if len(body) >= 2 else 0
        p = struct.pack("<HHH", reqid, 0x0001, 0) + socket.inet_aton(D2GS_IP) + struct.pack("<II", 0xD2D20001, 0)
        conn.sendall(mcp(0x04, p))
        log("[%s] S>C MCP_JOINGAME reqid=%d D2GS=%s:%d hash=0xd2d20001 result=0" % (peer, reqid, D2GS_IP, D2GS_PORT))
    else:
        log("[%s]     (MCP: pas de reponse pour id 0x%02x)" % (peer, mid))

def handle_mcp(conn, peer):
    log("[%s] protocole MCP (royaume)" % peer)
    buf = b""
    while True:
        while len(buf) < 3:
            c = conn.recv(4096)
            if not c: return
            buf += c
        ln = struct.unpack("<H", buf[:2])[0]
        if ln < 3:
            buf = buf[1:]; continue
        while len(buf) < ln:
            c = conn.recv(4096)
            if not c: return
            buf += c
        dispatch_mcp(conn, peer, buf[2], buf[3:ln])
        buf = buf[ln:]

def handle_mcp_conn(conn, addr):
    peer = "%s:%d" % addr
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        sel = conn.recv(1)
        if not sel: return
        log("[%s] === CONNEXION MCP, selecteur=0x%02x ===" % (peer, sel[0]))
        handle_mcp(conn, peer)
    except ConnectionResetError:
        log("[%s] MCP reset" % peer)
    except Exception as e:
        log("[%s] MCP ERREUR: %r" % (peer, e))
    finally:
        try: conn.close()
        except Exception: pass
        log("[%s] MCP close" % peer)

# ===================== D2GS server (game) =====================
# D2GS speaks first (0xAF NEGOTIATECOMPRESSION, RAW without prefix). Test mode
# 0x00 = NO compression -> S>C = [size prefix=L][id][fields] (L = 2+len),
# C>S = [id][fields] without prefix/compression. Town-load sequence (BNETDocs +
# MephisTools data/1.14 + d2prox + PvPGN): AF00 -> C:0x68 -> S:0x00 -> C:0x6B ->
# S:0x01,0x02,0x03(act0/areaId1),0x59,attrs,0x04 -> ping/pong. **Warden: we
# NEVER send 0xAE** (server-initiated; client stays dormant, plays normally).
# Uncompressed mode 0x00 is unconfirmed against a real 1.14d client (real bnet
# negotiated 0x01); if the client disconnects after AF00, implement Huffman.
PLAYER_NAME = os.environ.get("PLAYER_NAME", "Vita")
D2GS_CS_LEN = {0x68:37, 0x6B:1, 0x6D:13, 0x69:1, 0x6F:1}   # known C>S lengths (incl. id)

# --- REPLAY mode: replays the OFFICIAL captured D2GS bootstrap (1.14d oracle) ---
# D2GS_REPLAY_DIR = directory with d2gs_replay_{af,mid,post}.bin extracted from a
# real Wireshark capture. Replays the actual SERVER bytes (we ARE the server),
# split at 0x6B; NO forged client packet, NO 0xAE Warden replayed (post.bin is
# cleaned). Local character is pre-seeded with the capture's name to match
# ASSIGNPLAYER.
_REPLAY = {}
def load_replay():
    d = os.environ.get("D2GS_REPLAY_DIR", "")
    if d and not _REPLAY:
        for k in ("af", "mid", "post"):
            p = os.path.join(d, "d2gs_replay_%s.bin" % k)
            _REPLAY[k] = open(p, "rb").read() if os.path.exists(p) else b""
    return _REPLAY if _REPLAY.get("post") else None

# S>C mode 0 framing: the SIZE PREFIX is required (with it, the client
# recognizes GAMELOADING and sends 0x6B; without it, it never progresses and
# closes). Modeled on MephisTools/dump, not d2prox. D2GS_NOPREFIX=1 = without
# (diagnostic).
D2GS_PREFIX = os.environ.get("D2GS_NOPREFIX", "") == ""
def d2gs_pkt(mid, fields=b""):
    if not D2GS_PREFIX:
        return bytes([mid]) + fields          # bare [id][fields] (diag)
    L = 2 + len(fields)                        # [L][id][fields], L = total incl. prefix (< 240 -> 1 byte)
    if L < 240:
        return bytes([L, mid]) + fields
    return bytes([0xF0 | (L >> 8), L & 0xFF, mid]) + fields

def d2gs_town(conn, peer, cls, name):
    # Town-load is BISECTABLE via env D2GS_TOWN (list of groups, default = all) to
    # isolate which packet triggers a fault in the loading thread. Groups:
    #   loadsucc(0x02) gameflags(0x01) loadact(0x03) player(0x59) attrs(0x1F)
    #   handshake(0x0B) loadcomplete(0x04). Absolute minimum = "loadsucc,loadcomplete".
    def a32(a, v): return d2gs_pkt(0x1F, struct.pack("<BI", a, v))
    sel = os.environ.get("D2GS_TOWN", "")
    groups = set(x.strip() for x in sel.split(",") if x.strip()) if sel else None
    def inc(g): return groups is None or g in groups
    sent = []
    if inc("loadsucc"):   conn.sendall(d2gs_pkt(0x02)); sent.append("loadsucc")
    if inc("gameflags"):  conn.sendall(d2gs_pkt(0x01, struct.pack("<BHHBB", 0, 0, 0, 1, 0))); sent.append("gameflags")
    if inc("loadact"):    conn.sendall(d2gs_pkt(0x03, struct.pack("<BIHI", 0, 0x11223344, 0x0001, 0))); sent.append("loadact")
    if inc("player"):
        nm = name.encode()[:15].ljust(16, b"\x00")
        conn.sendall(d2gs_pkt(0x59, struct.pack("<IB", 1, cls & 0xFF) + nm + struct.pack("<HH", 5048, 5093))); sent.append("player")
    if inc("attrs"):
        for a, v in [(12, 1), (13, 0), (0, 20), (2, 20), (3, 20), (1, 20), (7, 100 << 8), (6, 100 << 8), (9, 100 << 8), (8, 100 << 8)]:
            conn.sendall(a32(a, v))
        sent.append("attrs")
    if inc("handshake"):  conn.sendall(d2gs_pkt(0x0B, struct.pack("<BI", 0, 1))); sent.append("handshake")
    if inc("loadcomplete"): conn.sendall(d2gs_pkt(0x04)); sent.append("loadcomplete")
    log("[%s] S>C D2GS town-load ENVOYE: [%s]" % (peer, ",".join(sent)))

def handle_d2gs_conn(conn, addr):
    peer = "%s:%d" % addr
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    st = {"cls": 0, "name": PLAYER_NAME, "started": False, "pings": 0}
    try:
        log("[%s] === CONNEXION D2GS ===" % peer)
        rep = load_replay()
        conn.sendall(rep["af"] if rep else bytes([0xAF, 0x00]))   # AF00 (real capture or synthetic)
        log("[%s] S>C D2GS 0xAF mode=0x00 %s" % (peer, "(REPLAY capture officielle)" if rep else "(synth)"))
        buf = b""; first = True
        while True:
            c = conn.recv(4096)
            if not c: log("[%s] D2GS ferme par le client" % peer); break
            buf += c
            while buf:
                if first and buf[0] == 0x01:              # possible selector byte
                    log("[%s] C>S D2GS selecteur 0x01" % peer); buf = buf[1:]; first = False; continue
                first = False
                mid = buf[0]; need = D2GS_CS_LEN.get(mid)
                if need is None:
                    log("[%s] C>S D2GS id=0x%02x INCONNU: %s (skip 1o)" % (peer, mid, hexdump(buf, 32))); buf = buf[1:]; continue
                if len(buf) < need: break
                pkt = buf[:need]; buf = buf[need:]
                if mid == 0x68:                           # GAMELOGON (37 bytes)
                    gh, tok, cls = struct.unpack_from("<IHB", pkt, 1)
                    ver, exp, cst = struct.unpack_from("<III", pkt, 8)
                    nm = pkt[21:37].split(b"\x00")[0].decode("latin1", "replace")
                    st["cls"] = cls; st["name"] = nm or PLAYER_NAME
                    log("[%s] C>S D2GS 0x68 GAMELOGON hash=0x%08x token=0x%04x cls=%d ver=%d exp=0x%08x nom=%r" % (peer, gh, tok, cls, ver, exp, nm))
                    if rep:
                        conn.sendall(rep["mid"]); log("[%s] S>C D2GS mid REPLAY (GAMEFLAGS+GAMELOADING+LOADSUCCESSFUL, %do)" % (peer, len(rep["mid"])))
                    else:
                        conn.sendall(d2gs_pkt(0x00)); log("[%s] S>C D2GS 0x00 GAMELOADING" % peer)
                elif mid == 0x6B:                         # ENTERGAMEENVIRONMENT -> town-load
                    log("[%s] C>S D2GS 0x6B ENTERGAMEENVIRONMENT" % peer)
                    if not st["started"]:
                        st["started"] = True
                        if rep:
                            conn.sendall(rep["post"]); log("[%s] S>C D2GS bootstrap REPLAY (%do, capture officielle -> LOADCOMPLETE)" % (peer, len(rep["post"])))
                        else:
                            d2gs_town(conn, peer, st["cls"], st["name"])
                elif mid == 0x6D:                         # PING -> PONG (BARE 8f+32, matches the capture; d2gs_pkt would prefix it -> 22 8f... desyncs the client's bare parser)
                    conn.sendall(bytes([0x8F]) + b"\x00" * 32); st["pings"] += 1
                    if st["pings"] <= 3: log("[%s] C>S D2GS 0x6D PING -> S>C 0x8F PONG (#%d)" % (peer, st["pings"]))
                elif mid == 0x66:                         # Warden response UNEXPECTED (we never send 0xAE)
                    log("[%s] ⚠️ C>S D2GS 0x66 WARDEN RESPONSE INATTENDU: %s" % (peer, hexdump(pkt, 32)))
                else:
                    log("[%s] C>S D2GS id=0x%02x len=%d: %s" % (peer, mid, need, hexdump(pkt, 32)))
    except Exception as e:
        log("[%s] D2GS ERREUR: %r" % (peer, e))
    finally:
        try: conn.close()
        except Exception: pass
        log("[%s] D2GS close" % peer)

def serve_port(port, handler, label):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((HOST, port)); s.listen(8)
    except Exception as e:
        log("=== %s bind %s:%d ECHEC: %r ===" % (label, HOST, port, e)); return
    log("=== %s en ecoute sur %s:%d ===" % (label, HOST, port))
    while True:
        conn, addr = s.accept()
        threading.Thread(target=handler, args=(conn, addr), daemon=True).start()

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(8)
    log("=== BNCS local en ecoute sur %s:%d (log: %s) ===" % (HOST, PORT, LOGPATH))
    exists = os.path.exists(BNFTP_FILE)
    log("MPQ=%s filetime=%d logon=%d token=0x%08x formula=%s" %
        (MPQ_FILENAME, MPQ_FILETIME, LOGON_TYPE, SERVER_TOKEN, CR_FORMULA or "<aléatoire>"))
    log("BNFTP_FILE=%s (%s%s)" % (BNFTP_FILE, "OK " if exists else "ABSENT",
        ("%d o" % os.path.getsize(BNFTP_FILE)) if exists else ""))
    # Pre-seeds a character (D2_SEEDCHAR=name) to test CHARLIST2 rendering + the
    # D2GS path without going through the creation UI on every run. Real
    # creation (UI -> MCP_CHARCREATE) is still supported and also feeds _chars.
    if os.environ.get("D2_SEEDCHAR"):
        _chars.append({"name": os.environ["D2_SEEDCHAR"],
                       "cls": int(os.environ.get("D2_SEEDCLASS", "1")),
                       "flags": int(os.environ.get("D2_SEEDFLAGS", "0x20"), 0), "level": 1})
        log("perso pre-seede: %s (cls=%d flags=0x%x)" % (_chars[0]["name"], _chars[0]["cls"], _chars[0]["flags"]))
    # Realm (MCP) + game (D2GS) servers on their own ports.
    threading.Thread(target=serve_port, args=(MCP_PORT, handle_mcp_conn, "MCP"), daemon=True).start()
    threading.Thread(target=serve_port, args=(D2GS_PORT, handle_d2gs_conn, "D2GS"), daemon=True).start()
    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        log("=== arret ===")
    finally:
        srv.close()

if __name__ == "__main__" and "--selftest-strict" in sys.argv:
    print("=== [BNCS-STRICT] auto-test du serveur comparant (aucun reseau) ===")
    sys.exit(_selftest_strict())

if __name__ == "__main__":
    main()
