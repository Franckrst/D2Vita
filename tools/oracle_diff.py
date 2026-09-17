#!/usr/bin/env python3
# tools/oracle_diff.py — diffs the dynarec OUTPUT frozen on console at Halt 1420
# (save2/oracle_<sprite>.out + .txt, build fee0f26+) against the CORRECT
# REFERENCE for the same sprite (tools/dcc_decode.py), block by block, field by
# field, and names the FIRST diverging field.
#
#   oracle_diff.py <oracle.out> <oracle.txt> [<reference>] [--dir N] [--max-blocks N] [--hex]
#     <reference> = the sprite's .dcc (decoded on the fly, direction d from the
#                   .txt or --dir), or a binary chain produced by dcc_decode.py --out.
#     Without a reference: parses and prints the Vita chain cleanly.
#
# Model (0x5fec90): eax = cel data base of the Asyn sub-object ([+0x3c]); ecx0 =
# [obj + d*4 + 0x2c] = offset of the 1st block; count = frames*dirs chained
# blocks, stride = 0x23 + [+0x14] + [+0x1c]. The .out starts at dump_base
# (= eax): the 1st block is therefore at offset ecx0 IN the .out.
# Fields of a block (DC6 frame in memory):
#   +0x00 flip (=0)  +0x04 width  +0x08 height  +0x0c xoff  +0x10 yoff
#   +0x14 unknown (=0)  +0x18 next block (REWRITTEN by the validator: pointer
#   to its 0x2c-byte record -> never compared strictly)  +0x1c length
#   then [length] bytes of RLE, then 0xEE 0xEE 0xEE.
import sys, os, struct, argparse, re

FIELDS = [  # (offset, name, format, strict, interpretation)
    (0x00, 'flip',     '<I', True,  'champ0 (doit etre 0 ; !=0 => Halt 1420)'),
    (0x04, 'largeur',  '<I', True,  'largeur du cadre (>256 => Halt 1420)'),
    (0x08, 'hauteur',  '<I', True,  'hauteur du cadre (>256 => Halt 1420)'),
    (0x0c, 'xoff',     '<i', True,  'decalage x du cadre'),
    (0x10, 'yoff',     '<i', True,  'decalage y du cadre (ligne du bas)'),
    (0x14, 'taille14', '<I', True,  'taille [+0x14] (entre dans le pas ; attendu 0)'),
    (0x18, 'suivant',  '<I', False, '[+0x18] bloc suivant / pointeur d enregistrement (informatif)'),
    (0x1c, 'longueur', '<I', True,  'longueur RLE [+0x1c] (entre dans le pas)'),
]


def parse_txt(path):
    """Key=value pairs from the capture's .txt (both variants: HALTREP and H1420BLK)."""
    hexkeys = {'objet', 'Asyn', '[+0x48]', 'base(eax)', 'ecx0', 'bloc', 'f0', 'dump_base'}
    p = {}
    txt = open(path, 'r', errors='replace').read()
    for m in re.finditer(r'(\S+?)=(\S+)', txt):
        k, v = m.group(1), m.group(2)
        if k in hexkeys:
            try:
                p[k] = int(v, 16)
            except ValueError:
                p[k] = v
        elif re.fullmatch(r'-?\d+', v):
            p[k] = int(v)
        else:
            p[k] = v
    # "fautif i=3" -> key i
    p.setdefault('i', None)
    return p, txt


def walk_chain(buf, start, count, cap=4096):
    """Walks the chain like 0x5fec90: returns a list of per-block dicts.
    Stops at the end of the dump (truncated block marked 'tronque')."""
    blocks = []
    off = start
    n = min(count, cap) if count else cap
    for i in range(n):
        if off + 0x20 > len(buf):
            blocks.append({'i': i, 'off': off, 'tronque': True})
            break
        f = {}
        for o, name, fmt, strict, _ in FIELDS:
            f[name] = struct.unpack_from(fmt, buf, off + o)[0]
        f['i'] = i
        f['off'] = off
        f['pas'] = 0x23 + f['taille14'] + f['longueur']
        f['fautif'] = f['flip'] != 0 or f['largeur'] > 0x100 or f['hauteur'] > 0x100
        f['data'] = buf[off + 0x20: off + 0x20 + f['longueur']] if f['pas'] < 0x100000 else b''
        f['term'] = buf[off + 0x20 + f['longueur']: off + 0x23 + f['longueur']] if f['pas'] < 0x100000 else b''
        f['tronque'] = (off + f['pas'] > len(buf)) if f['pas'] < 0x100000 else True
        blocks.append(f)
        if f['fautif'] or f['pas'] >= 0x100000:
            break
        off += f['pas']
    return blocks


def fmt_block(b, tag=''):
    if b.get('tronque') and 'flip' not in b:
        return '  i=%-3d @+0x%05x  <hors du dump>' % (b['i'], b['off'])
    s = '  i=%-3d @+0x%05x f0=%08x L=%-4d H=%-4d x=%-5d y=%-5d t14=%-5d t18=%08x len=%-6d pas=%d%s%s' % (
        b['i'], b['off'], b['flip'], b['largeur'], b['hauteur'], b['xoff'], b['yoff'], b['taille14'],
        b['suivant'], b['longueur'], b['pas'], '  <== FAUTIF' if b['fautif'] else '',
        '  (tronque)' if b.get('tronque') else '')
    return s + tag


def hexline(buf, off, n=32):
    return ' '.join('%02x' % c for c in buf[off:off + n])


def rle_locate(data, pos, h):
    """Interprets an offset into the RLE data: (row from the BOTTOM, current x, which code)."""
    i = 0
    row = 0
    x = 0
    while i < len(data) and i <= pos:
        c = data[i]
        if i == pos:
            if c == 0x80:
                return row, x, 'fin de ligne (0x80)'
            if c & 0x80:
                return row, x, 'saut de %d transparents' % (c & 0x7f)
            return row, x, 'litteral de %d pixels' % c
        if c == 0x80:
            row += 1
            x = 0
            i += 1
        elif c & 0x80:
            x += c & 0x7f
            i += 1
        else:
            if i < pos <= i + c:
                return row, x + (pos - i - 1), 'pixel litteral (palette) n°%d du run' % (pos - i - 1)
            x += c
            i += 1 + c
    return row, x, '?'


def load_reference(path, params, args, log):
    """Returns (list of reference blocks [bytes], description)."""
    data = open(path, 'rb').read()
    if data[:1] == b'\x74' and (path.lower().endswith('.dcc') or len(data) > 15 and data[1] == 6):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dcc_decode as D
        dcc = D.DCC(data, path)
        dirs = params.get('dirs', 1) or 1
        d = args.dir if args.dir is not None else (params.get('d', 0) or 0)
        if dirs == 1:
            if d >= dcc.ndirs:
                log('ATTENTION: d=%d >= %d directions du DCC ; direction 0 prise' % (d, dcc.ndirs))
                d = 0
            dd = dcc.read_direction(d)
            tot, exp, ok = D.check_direction(dd)
            log('reference : %s direction %d (%d cadres), OutSizeCoded %s (%d/%d)' % (
                os.path.basename(path), d, dcc.nframes, 'OK' if ok else 'ECART', tot, exp))
            return D.build_chain(dd), 'dcc dir %d' % d
        # multi-direction object: chain = directions concatenated starting at d
        blocks = []
        log('ATTENTION: dirs=%d dans l objet : reference = directions %d..%d concatenees (hypothese, --dir pour changer)' % (
            dirs, d, min(d + dirs, dcc.ndirs) - 1))
        for k in range(d, min(d + dirs, dcc.ndirs)):
            dd = dcc.read_direction(k)
            blocks += D.build_chain(dd, base_offset=sum(len(b) for b in blocks))
        return blocks, 'dcc dirs %d..%d' % (d, d + dirs - 1)
    # raw binary chain (dcc_decode.py --out)
    blocks = []
    off = 0
    while off + 0x20 <= len(data):
        ln = struct.unpack_from('<I', data, off + 0x1c)[0]
        t14 = struct.unpack_from('<I', data, off + 0x14)[0]
        blk = data[off: off + 0x23 + ln + t14]
        blocks.append(blk)
        off += 0x23 + ln + t14
    log('reference : chaine brute %s (%d blocs, %d o)' % (os.path.basename(path), len(blocks), len(data)))
    return blocks, 'chaine brute'


def ref_fields(blk):
    f = {}
    for o, name, fmt, strict, _ in FIELDS:
        f[name] = struct.unpack_from(fmt, blk, o)[0] if o + 4 <= len(blk) else None
    f['data'] = blk[0x20: 0x20 + (f['longueur'] or 0)]
    f['term'] = blk[0x20 + (f['longueur'] or 0): 0x23 + (f['longueur'] or 0)]
    f['pas'] = 0x23 + (f['taille14'] or 0) + (f['longueur'] or 0)
    return f


def main(argv=None):
    ap = argparse.ArgumentParser(description='diff oracle Vita (Halt 1420) <-> reference DCC')
    ap.add_argument('out')
    ap.add_argument('txt')
    ap.add_argument('reference', nargs='?')
    ap.add_argument('--dir', type=int, default=None, help='direction de reference (defaut : d du .txt)')
    ap.add_argument('--start', type=lambda s: int(s, 0), default=None, help='offset du 1er bloc dans le .out (defaut : ecx0)')
    ap.add_argument('--max-blocks', type=int, default=4096)
    ap.add_argument('--hex', action='store_true', help='hexdump des deux cotes a la divergence')
    ap.add_argument('--quiet', action='store_true', help='ne pas lister toute la chaine Vita')
    a = ap.parse_args(argv)
    log = lambda s: print(s)

    buf = open(a.out, 'rb').read()
    p, raw = parse_txt(a.txt)
    print('== capture : %s (%d o) ==' % (a.out, len(buf)))
    for line in raw.strip().splitlines():
        print('   ' + line)
    base = p.get('dump_base', p.get('base(eax)', 0))
    eax = p.get('base(eax)', base)
    ecx0 = p.get('ecx0', 0)
    count = p.get('count', 0) or 0
    dump_len = p.get('dump_len')
    if dump_len is not None and dump_len != len(buf):
        print('ATTENTION: dump_len=%d dans le .txt mais %d o dans le .out (capture TRONQUEE a 64 Ko, ou 0x4000 si le fautif est a >0xF000 de la base)' % (dump_len, len(buf)))
    start = a.start if a.start is not None else (ecx0 + (eax - base))
    print('base=%08x ecx0=%08x -> 1er bloc a +0x%x dans le dump ; count=%d ; fautif annonce i=%s bloc=%s' % (
        base, ecx0, start, count, p.get('i'), ('%08x' % p['bloc']) if isinstance(p.get('bloc'), int) else '?'))
    if start >= len(buf):
        print('FAIL: le 1er bloc (+0x%x) est HORS du dump (%d o) : la capture part de eax, pas de eax+ecx0 —'
              ' limite de fee0f26' % (start, len(buf)))
        return 3
    vita = walk_chain(buf, start, count, a.max_blocks)
    print('== chaine Vita (%d blocs lus) ==' % len(vita))
    if not a.quiet:
        for b in vita:
            print(fmt_block(b))
    else:
        for b in vita[:3] + ([vita[-1]] if len(vita) > 3 else []):
            print(fmt_block(b))
    fautifs = [b for b in vita if b.get('fautif')]
    if fautifs:
        b = fautifs[0]
        print('bloc fautif Vita : i=%d @+0x%x (adresse %08x) : %s' % (
            b['i'], b['off'], base + b['off'],
            'champ0!=0' if b['flip'] else ('LARGEUR>256' if b['largeur'] > 0x100 else 'HAUTEUR>256')))
        if isinstance(p.get('i'), int) and p['i'] != b['i']:
            print('ATTENTION: le .txt annonce i=%d, le parcours trouve i=%d' % (p['i'], b['i']))
        print('  octets : ' + hexline(buf, b['off'], 48))
    else:
        print('aucun bloc fautif dans la partie parcourue (chaine saine ici ; ou dump tronque avant le fautif, ou count=0)')

    if not a.reference:
        print('(pas de reference : diff impossible ; produire la reference avec tools/oracle_extract.sh + tools/dcc_decode.py)')
        return 0

    ref, desc = load_reference(a.reference, p, a, log)
    print('== diff Vita <-> reference (%s, %d blocs) ==' % (desc, len(ref)))
    first = None
    nok = 0
    advisories = []
    for b in vita:
        i = b['i']
        if i >= len(ref):
            print('  i=%d : la reference n a que %d blocs — la chaine Vita continue au-dela (count=%d) : count ou dirs incoherent ?' % (i, len(ref), count))
            break
        if b.get('tronque') and 'flip' not in b:
            print('  i=%d : bloc Vita hors du dump ; reference L=%d H=%d len=%d' % (
                i, ref_fields(ref[i])['largeur'], ref_fields(ref[i])['hauteur'], ref_fields(ref[i])['longueur']))
            break
        r = ref_fields(ref[i])
        # expected block offset in the reference chain (from the 1st block)
        roff = sum(len(x) for x in ref[:i])
        if b['off'] - start != roff:
            first = (i, 'offset', b['off'] - start, roff, 'offset du bloc depuis le 1er (derive des pas precedents)')
            break
        for o, name, fmt, strict, interp in FIELDS:
            if b[name] != r[name]:
                if strict:
                    first = (i, name, b[name], r[name], interp, o)
                    break
                advisories.append((i, name, b[name], r[name]))
        if first:
            break
        # RLE data
        vd, rd = b['data'], r['data']
        if b.get('tronque'):
            print('  i=%d : en-tete identique, donnees tronquees par la fin du dump (%d/%d o)' % (i, len(vd), len(rd)))
            nok += 1
            break
        if vd != rd:
            n = min(len(vd), len(rd))
            k = next((j for j in range(n) if vd[j] != rd[j]), n)
            row, x, what = rle_locate(rd, k, r['hauteur'])
            first = (i, 'donnees[+0x%x]' % k, vd[k] if k < len(vd) else None, rd[k] if k < len(rd) else None,
                     'octet RLE %d/%d : ligne %d depuis le bas, x=%d, %s' % (k, len(rd), row, x, what), 0x20 + k)
            break
        if b['term'] != r['term']:
            first = (i, 'terminateur', b['term'].hex(), r['term'].hex(), '3 octets 0xEE de fin de bloc', 0x20 + len(rd))
            break
        nok += 1
    if advisories:
        print('  (informatif) [+0x18] differe sur %d bloc(s) : Vita=%08x ref=%08x au bloc %d — attendu, reecrit par le validateur' % (
            len(advisories), advisories[0][2], advisories[0][3], advisories[0][0]))
    if first is None:
        print('RESULTAT : %d bloc(s) compares, AUCUNE divergence stricte dans la partie disponible' % nok)
        return 0
    i, name, v, rv, interp = first[:5]
    fo = first[5] if len(first) > 5 else None
    blk = vita[i]
    print('RESULTAT : PREMIERE DIVERGENCE au bloc i=%d (%d bloc(s) identiques avant)' % (i, nok))
    print('  champ      : %s%s' % (name, ('  (+0x%x dans le bloc)' % fo) if fo is not None else ''))
    print('  Vita       : %s' % (('0x%08x (%d)' % (v & 0xffffffff, v)) if isinstance(v, int) else v))
    print('  reference  : %s' % (('0x%08x (%d)' % (rv & 0xffffffff, rv)) if isinstance(rv, int) else rv))
    print('  sens       : %s' % interp)
    if 'off' in blk:
        print('  adresse    : bloc @+0x%x dans le dump = %08x invite%s' % (
            blk['off'], base + blk['off'], (' ; champ a %08x' % (base + blk['off'] + fo)) if fo is not None else ''))
        if isinstance(v, int) and isinstance(rv, int) and name in ('largeur', 'hauteur', 'longueur', 'taille14', 'flip'):
            nb = sum(1 for k in range(4) if ((v >> (8 * k)) & 255) != ((rv >> (8 * k)) & 255))
            print('  octets     : %d octet(s) du dword different (xor 0x%08x)' % (nb, (v ^ rv) & 0xffffffff))
    if True:  # hexdump is always useful at the divergence point
        o = blk.get('off', 0)
        ro = sum(len(x) for x in ref[:i])
        refbuf = b''.join(ref)
        print('  Vita  @+0x%05x : %s' % (o, hexline(buf, o, 40)))
        print('  ref   @+0x%05x : %s' % (ro, hexline(refbuf, ro, 40)))
        if fo is not None and fo >= 0x20:
            print('  Vita  @+0x%05x : %s' % (o + fo - 8, hexline(buf, o + fo - 8, 32)))
            print('  ref   @+0x%05x : %s' % (ro + fo - 8, hexline(refbuf, ro + fo - 8, 32)))
    return 1


if __name__ == '__main__':
    sys.exit(main())
