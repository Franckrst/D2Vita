#!/usr/bin/env python3
"""d2s_items.py — list the player items of a Diablo II 1.14d save, and append
simple test items to it (for console tests: runes, a socketed normal base).

  d2s_items.py list <save.d2s> <excel_dir>
  d2s_items.py add  <save.d2s> <excel_dir> <out.d2s> <spec> [<spec> ...]
      spec = code@store:x,y[:sockets]   store: inv | stash | cube
      e.g.  r07@inv:0,0   crs@stash:0,0:4
      a first spec `clear:inv` empties the carried inventory first
      (equipment and belt stay) — for throwaway test copies only.

<excel_dir> holds itemstatcost/weapons/armor/misc .txt extracted from the
player's own MPQs (tools never ship Blizzard data). Items are LSB-first bit
records starting with "JM"; only the player list is touched, size and
checksum are recomputed.
"""
import csv, os, struct, sys

def table(d, name):
    with open(os.path.join(d, name + '.txt'), encoding='latin-1') as f:
        return list(csv.DictReader(f, delimiter='\t'))

class Excel:
    def __init__(s, d):
        s.stat = {}
        for r in table(d, 'itemstatcost'):
            if r.get('ID', '').isdigit():
                s.stat[int(r['ID'])] = (int(r['Save Bits'] or 0), int(r['Save Param Bits'] or 0))
        s.kind, s.stack, s.size, s.dur = {}, set(), {}, {}
        for k in ('weapons', 'armor', 'misc'):
            for r in table(d, k):
                c = (r.get('code') or '').strip()
                if not c:
                    continue
                s.kind[c] = k
                if r.get('stackable') == '1':
                    s.stack.add(c)
                s.size[c] = (int(r.get('invwidth') or 1), int(r.get('invheight') or 1))
                s.dur[c] = int(r.get('durability') or 0)

class R:
    def __init__(s, d, p): s.d, s.p = d, p
    def get(s, n):
        v = 0
        for k in range(n):
            v |= ((s.d[s.p >> 3] >> (s.p & 7)) & 1) << k; s.p += 1
        return v

class W:
    def __init__(s): s.bits = []
    def put(s, v, n): s.bits += [(v >> k) & 1 for k in range(n)]
    def bytes(s):
        b = bytearray((len(s.bits) + 7) // 8)
        for i, x in enumerate(s.bits):
            b[i >> 3] |= x << (i & 7)
        return b

# Stats saved as a group after their first id, without their own id.
FOLLOW = {17: 1, 48: 1, 50: 1, 52: 1, 54: 2, 57: 2}

def props(r, xl):
    while True:
        sid = r.get(9)
        if sid == 0x1FF:
            return
        for k in range(1 + FOLLOW.get(sid, 0)):
            bits, par = xl.stat[sid + k]
            r.get(par); r.get(bits)

def parse_item(d, p, xl):
    """Return (item dict, bit position just after the item, byte-aligned)."""
    r = R(d, p)
    assert r.get(16) == 0x4D4A, f"pas de JM a l'octet {p >> 3}"
    fl = r.get(32); r.get(10)
    it = dict(start=p >> 3, loc=r.get(3), eq=r.get(4), x=r.get(4), y=r.get(4), store=r.get(3))
    it['code'] = bytes(r.get(8) for _ in range(4)).decode('latin-1').strip()
    it['nsock_items'] = r.get(3)
    it['simple'] = bool(fl >> 21 & 1)
    if not it['simple']:
        r.get(32); r.get(7); q = r.get(4); it['quality'] = q
        if r.get(1): r.get(3)
        if r.get(1): r.get(11)
        if q in (1, 3): r.get(3)
        elif q == 4: r.get(22)
        elif q in (5, 7): r.get(12)
        elif q in (6, 8):
            r.get(16)
            for _ in range(6):
                if r.get(1): r.get(11)
        if fl >> 26 & 1: r.get(16)
        if fl >> 24 & 1:
            while r.get(7): pass
        if it['code'] in ('ibk', 'tbk'): r.get(5)
        r.get(1)
        k = xl.kind.get(it['code'])
        if k == 'armor': r.get(xl.stat[31][0])
        if k in ('armor', 'weapons'):
            mx = r.get(xl.stat[73][0])
            if mx: r.get(xl.stat[72][0])
        if it['code'] in xl.stack: r.get(9)
        if fl >> 11 & 1: it['sockets'] = r.get(4)
        sets = r.get(5) if q == 5 else 0
        props(r, xl)
        for b in range(5):
            if sets >> b & 1: props(r, xl)
        if fl >> 26 & 1: props(r, xl)
    end = (r.p + 7) & ~7
    return it, end

def load(path):
    d = bytearray(open(path, 'rb').read())
    assert struct.unpack_from('<I', d, 0)[0] == 0xAA55AA55
    return d

def player_items(d, xl):
    hdr = d.index(b'JM', 765)
    n = struct.unpack_from('<H', d, hdr + 2)[0]
    p, items = (hdr + 4) * 8, []
    for _ in range(n):
        it, p = parse_item(d, p, xl)
        items.append(it)
        for _ in range(it['nsock_items']):
            _, p = parse_item(d, p, xl)
    return hdr, n, items, p >> 3

STORE = {'inv': 1, 'cube': 4, 'stash': 5}
GRID = {1: (10, 4), 4: (3, 4), 5: (6, 8)}

def occupied(items, xl, store):
    cells = set()
    for it in items:
        if it['loc'] == 0 and it['store'] == store:
            w, h = xl.size.get(it['code'], (1, 1))
            cells |= {(it['x'] + i, it['y'] + j) for i in range(w) for j in range(h)}
    return cells

def make_item(code, store, x, y, sockets, xl, uid):
    w = W()
    fl = (1 << 4)                                     # identified
    simple = xl.kind.get(code) == 'misc' and not sockets
    if simple: fl |= 1 << 21
    if sockets: fl |= 1 << 11
    w.put(0x4D4A, 16); w.put(fl, 32); w.put(101, 10)
    w.put(0, 3); w.put(0, 4); w.put(x, 4); w.put(y, 4); w.put(store, 3)
    for ch in code.ljust(4).encode(): w.put(ch, 8)
    w.put(0, 3)
    if not simple:
        w.put(uid, 32); w.put(30, 7); w.put(2, 4)        # ilvl 30, normal quality
        w.put(0, 1); w.put(0, 1)                         # no picture, no class affix
        w.put(0, 1)                                      # realm bit
        k = xl.kind[code]
        if k == 'armor': w.put(10 + 20, xl.stat[31][0])
        if k in ('armor', 'weapons'):
            dur = xl.dur.get(code, 0)
            w.put(dur, xl.stat[73][0])
            if dur: w.put(dur, xl.stat[72][0])
        if code in xl.stack: w.put(1, 9)
        if sockets: w.put(sockets, 4)
        w.put(0x1FF, 9)
    return w.bytes()

def checksum(d):
    c = 0
    for i, b in enumerate(d):
        if 12 <= i < 16: b = 0
        c = (((c << 1) | (c >> 31)) + b) & 0xFFFFFFFF
    return c

def main():
    cmd, path, xd = sys.argv[1], sys.argv[2], sys.argv[3]
    xl, d = Excel(xd), load(path)
    hdr, n, items, end = player_items(d, xl)
    if cmd == 'list':
        print(f"{n} objets (liste joueur @{hdr}, fin @{end}, suivant {bytes(d[end:end+2])})")
        for it in items:
            print(f"  @{it['start']:4} {it['code']:4} loc={it['loc']} store={it['store']} x={it['x']} y={it['y']}"
                  f" eq={it['eq']} {'simple' if it['simple'] else 'q=%d' % it['quality']}"
                  f"{' sockets=%d' % it['sockets'] if 'sockets' in it else ''} serties={it['nsock_items']}")
        return
    out, new, specs = sys.argv[4], bytearray(), sys.argv[5:]
    if specs and specs[0] == 'clear:inv':
        # Drop the carried inventory (stored items, no socketed children):
        # a test copy needs free cells; equipment and belt stay.
        specs = specs[1:]; keep, starts = [], [it['start'] for it in items] + [end]
        for it, nxt in zip(items, starts[1:]):
            if it['loc'] == 0 and it['store'] == 1 and not it['nsock_items']:
                continue
            keep.append((it, bytes(d[it['start']:nxt])))
        d[hdr + 4:end] = b''.join(b for _, b in keep)
        n, items = len(keep), [it for it, _ in keep]
        end = hdr + 4 + sum(len(b) for _, b in keep)
    for i, spec in enumerate(specs):
        code, rest = spec.split('@'); parts = rest.split(':')
        store = STORE[parts[0]]; x, y = map(int, parts[1].split(','))
        sock = int(parts[2]) if len(parts) > 2 else 0
        w, h = xl.size[code]; gw, gh = GRID[store]
        want = {(x + a, y + b) for a in range(w) for b in range(h)}
        assert x + w <= gw and y + h <= gh, f"{spec} deborde de la grille {gw}x{gh}"
        busy = occupied(items, xl, store)
        assert not (want & busy), f"{spec} chevauche un objet existant"
        items.append(dict(loc=0, store=store, x=x, y=y, code=code))
        new += make_item(code, store, x, y, sock, xl, 0x5EED0000 + i)
    d[end:end] = new
    struct.pack_into("<H", d, hdr + 2, n + len(specs))
    struct.pack_into('<I', d, 8, len(d)); struct.pack_into('<I', d, 12, checksum(d))
    open(out, 'wb').write(d)
    print(f"{out}: {n} -> {n + len(specs)} objets, {len(d)} octets")

if __name__ == '__main__':
    main()
