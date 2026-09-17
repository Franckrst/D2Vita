#!/usr/bin/env python3
"""tools/wtref/mkcases.py — builds the Authenticode oracle's test cases.

    python3 tools/wtref/mkcases.py <1.14d refs dir> <output dir>

Copies the original signed binaries and derives ALTERED variants, each in
ONE single way (so a failure has only one possible cause):
  Game_text1.exe / CR_text1.dll  1 bit of .text flipped        -> wrong digest
  Game_sig.exe   / CR_sig.dll    1 bit of the signer's RSA signature
  CR_tssig.dll                   1 bit of the TIMESTAMPER's RSA signature
  Game_nots.exe  / CR_nots.dll   timestamp REMOVED (unauthenticated attributes:
                                 outside the signature, so the signature stays valid)
  Diablo_II.exe                  unsigned PE; notpe.txt: not a PE at all.
Offsets are found with openssl asn1parse (a tool independent of the library
under test).
"""
import os, shutil, struct, subprocess, sys

refs, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

def secdir(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24
    return opt + 96 + 4 * 8, struct.unpack_from('<II', d, opt + 96 + 4 * 8)

def text_off(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; soh = struct.unpack_from('<H', d, pe + 20)[0]
    for i in range(nsec):
        n, vs, va, rs, ro = struct.unpack_from('<8sIIII', d, opt + soh + 40 * i)
        if n.startswith(b'.text'): return ro
    raise SystemExit('pas de .text')

def asn1(d):
    _, (va, sz) = secdir(d)
    tmp = os.path.join(out, '_tmp.p7'); open(tmp, 'wb').write(d[va + 8:va + sz])
    r = subprocess.run(['openssl', 'asn1parse', '-inform', 'DER', '-in', tmp], capture_output=True, text=True).stdout
    os.remove(tmp)
    return va + 8, r.splitlines()

def octets256(d):
    base, lines = asn1(d)
    res = []
    for l in lines:
        if 'OCTET STRING' in l and 'l= 256' in l:
            off = int(l.split(':')[0]); hl = int(l.split('hl=')[1].split()[0]); dep = int(l.split('d=')[1].split()[0])
            res.append((dep, base + off + hl))
    return res

def flip(src, dst, off):
    d = bytearray(open(src, 'rb').read()); d[off] ^= 1; open(dst, 'wb').write(d)
    print('  %-16s bit inverse a 0x%x' % (os.path.basename(dst), off))

# --- minimal DER to strip the timestamp -------------------------------------
def parse(b, pos):
    tag = b[pos]; l = b[pos + 1]; h = 2
    if l & 0x80:
        k = l & 0x7f; l = int.from_bytes(b[pos + 2:pos + 2 + k], 'big'); h = 2 + k
    return tag, pos + h, l

def tree(b, pos):
    tag, v, l = parse(b, pos)
    if tag & 0x20:
        kids = []; p = v
        while p < v + l:
            k, p = tree(b, p); kids.append(k)
        return [tag, kids], v + l
    return [tag, bytes(b[v:v + l])], v + l

def enc_len(n):
    if n < 0x80: return bytes([n])
    s = n.to_bytes((n.bit_length() + 7) // 8, 'big'); return bytes([0x80 | len(s)]) + s

def build(n):
    tag, c = n
    body = b''.join(build(k) for k in c) if isinstance(c, list) else c
    return bytes([tag]) + enc_len(len(body)) + body

def strip_timestamp(src, dst):
    d = bytearray(open(src, 'rb').read())
    ent, (va, sz) = secdir(d)
    root, _ = tree(d[va + 8:va + sz], 0)
    signed = root[1][1][1][0]          # ContentInfo -> [0] -> SignedData
    si = signed[1][-1][1][0]           # signerInfos[0]
    n = len(si[1]); si[1] = [k for k in si[1] if k[0] != 0xA1]
    assert len(si[1]) == n - 1, 'pas d attributs non authentifies'
    p7 = build(root); wc = p7 + b'\0' * ((-(len(p7) + 8)) % 8)
    d = d[:va] + struct.pack('<IHH', len(wc) + 8, 0x200, 2) + wc
    struct.pack_into('<II', d, ent, va, len(wc) + 8)
    open(dst, 'wb').write(d)
    print('  %-16s horodatage retire' % os.path.basename(dst))

G, C = os.path.join(refs, 'Game.exe'), os.path.join(refs, 'CheckRevision.dll')
for f in ['Game.exe', 'CheckRevision.dll', 'SystemSurvey.exe', 'Diablo II.exe']:
    shutil.copy(os.path.join(refs, f), os.path.join(out, f.replace(' ', '_')))
open(os.path.join(out, 'notpe.txt'), 'w').write('hello\n')
gd, cd = open(G, 'rb').read(), open(C, 'rb').read()
flip(G, os.path.join(out, 'Game_text1.exe'), text_off(gd) + 0x1234)
flip(C, os.path.join(out, 'CR_text1.dll'), text_off(cd) + 0x1234)
# primary signer: first 256-byte OCTET STRING at depth 5
flip(G, os.path.join(out, 'Game_sig.exe'), [o for dep, o in octets256(gd) if dep == 5][0] + 100)
flip(C, os.path.join(out, 'CR_sig.dll'), [o for dep, o in octets256(cd) if dep == 5][0] + 100)
# timestamper: LAST 256-byte OCTET STRING in the PKCS#7
flip(C, os.path.join(out, 'CR_tssig.dll'), octets256(cd)[-1][1] + 10)
strip_timestamp(G, os.path.join(out, 'Game_nots.exe'))
strip_timestamp(C, os.path.join(out, 'CR_nots.dll'))
