#!/usr/bin/env python3
"""tools/eip_fils.py — PER GUEST THREAD breakdown of a D2_EIPDUMP dump
(binary -DPROF_COUNTERS, D2SCRIPT "eipdump" action).

The dump contains, per guest thread, the EXACT EIP of each preemption sample
(one draw per QUANTUM translated blocks, so a draw uniform over BLOCKS, not
over time) and the EXACT count of blocks executed by the thread. This script
maps each EIP to a Game.exe (1.14d) FUNCTION and reports, per thread, the
hottest functions by share of samples, converted to blocks (share x thread's
exact block count).

Function bounds: targets of `.text` `call rel32` + `push ebp ; mov ebp,esp` /
`sub esp,imm` prologues not reached by a call (LTCG), plus a list of known
symbols. This is a heuristic: verify against the disassembly
(tools/x86dis.py) before drawing conclusions about a function.

Usage:
  python3 tools/eip_fils.py <dump> [--fil 4] [--top 12] [--exe ~/d2-vita-refs/1.14d/Game.exe]
                             [--emit <emitprof report>]   (cross-referenced with exact blocks)
"""
import argparse, os, struct, sys, bisect, re, collections

KNOWN = {  # VA -> name
    0x41a550: 'Storm!IoThread', 0x417040: 'Storm!ReadDecompress(0x417040)', 0x41aad0: 'Storm!SFileReadFileEx',
    0x4154b0: 'Storm!ReadSectors(0x4154b0)', 0x415240: 'Storm!ExplodeWrap(0x415240)', 0x41e000: 'Storm!SCompExplode',
    0x6b01b0: 'Storm!explode_core', 0x5ff6e0: 'D2!SpriteLoadCb(0x5ff6e0)', 0x60bff0: 'D2!DccDecode(0x60bff0)',
    0x4fa590: 'Fog!MainPump', 0x419790: 'Storm!IoEnqueue', 0x41a450: 'Storm!IoPick',
}

def load_exe(path):
    d = open(path, 'rb').read(); pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]; sec = pe + 24 + optsz
    secs = []
    for i in range(nsec):
        name, vsz, vad, rsz, raw = struct.unpack_from('<8sIIII', d, sec + 40 * i)
        secs.append((name.rstrip(b'\0').decode(), base + vad, max(vsz, rsz), raw))
    return d, base, secs

def func_starts(d, base, secs):
    text = [s for s in secs if s[0] == '.text'][0]
    _, tva, tsz, traw = text
    code = d[traw:traw + tsz]
    starts = set(KNOWN)
    lo, hi = tva, tva + tsz
    # call rel32 targets
    i = 0; n = len(code)
    while i < n - 4:
        if code[i] == 0xE8:
            rel = struct.unpack_from('<i', code, i + 1)[0]
            t = tva + i + 5 + rel
            if lo <= t < hi: starts.add(t)
        i += 1
    # common prologues (LTCG: many functions are reached only via jmp/pointer)
    for m in re.finditer(rb'\x55\x8b\xec', code): starts.add(tva + m.start())
    for m in re.finditer(rb'\xcc\xcc(?=[\x55\x83\x81\x56\x53\x57\x8b\x6a\x68\xa1])', code):
        # after int3 padding: likely function start
        starts.add(tva + m.end())
    return sorted(starts), (lo, hi)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump'); ap.add_argument('--fil', type=int, default=None)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--exe', default=os.path.expanduser('~/d2-vita-refs/1.14d/Game.exe'))
    ap.add_argument('--base', default='0x03900000', help='base relogee du run (compact = 0x03900000 ; 0x02100000 de 0.1.7 a 0.1.11-beta5 ; 0x01900000 jusqu a 0.1.6)')
    ap.add_argument('--emit', default=None, help='rapport [emitprof] ou CSV D2_EMITCSV (blocs exacts, octets ARM)')
    ap.add_argument('--n', type=int, default=0, help='numero du vidage a lire (defaut : le dernier)')
    ap.add_argument('--delta', action='store_true', help='soustraire le vidage precedent (fenetre)')
    a = ap.parse_args()
    rb = int(a.base, 16)
    d, base, secs = load_exe(a.exe)
    starts, (lo, hi) = func_starts(d, base, secs)
    def fn_of(va):
        k = bisect.bisect_right(starts, va) - 1
        return starts[k] if k >= 0 and lo <= va < hi else None
    def name(f):
        return KNOWN.get(f, 'sub_%x' % f) if f else 'hors .text'
    # emitprof: exact exec counts per block, ARM bytes -> per function
    emit = {}
    if a.emit:
        rows = []
        et = open(a.emit).read()
        for m in re.finditer(r'x86=0x([0-9a-f]+) exec=(\d+)\s+insns=(\d+)\s+x86=(\d+)o\s+arm=(\d+)o', et):
            rows.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(5))))
        for m in re.finditer(r'^([0-9a-f]{8}),(\d+),(\d+),(\d+),(\d+)$', et, flags=re.M):   # CSV x86,x86len,armlen,ninsts,exec
            rows.append((int(m.group(1), 16), int(m.group(5)), int(m.group(4)), int(m.group(3))))
        for va, ex, ni, ab in rows:
            va = va - rb + base if rb <= va < rb + 0x1000000 else va
            f = fn_of(va); e = emit.setdefault(f, [0, 0, 0, 0])   # exec, arm*exec, x86insn*exec, nblocks
            e[0] += ex; e[1] += ex * ab; e[2] += ex * ni; e[3] += 1
    # multiple cumulative dumps can share one file ("# eipdump n=k"): takes
    # the LAST one by default (--n k for another); histograms accumulate
    # since boot, so "last - previous" gives the window.
    text = open(a.dump).read(); parts = [q for q in re.split(r'^# eipdump n=\d+\n', text, flags=re.M) if q.strip()]
    if not parts: sys.exit('vidage vide')
    sel = parts[a.n - 1] if a.n else parts[-1]
    prev = parts[a.n - 2] if (a.n and a.n >= 2 and a.delta) else (parts[-2] if (a.delta and len(parts) >= 2) else None)
    threads = {}; cur = None
    def parse(txt, sign, threads):
        for line in txt.splitlines():
            p = line.split()
            if not p: continue
            if p[0] == 'T':
                tid = int(p[1]); t = threads.setdefault(tid, {'entry': int(p[2], 16), 'samples': 0, 'blocks': 0, 'hist': collections.Counter()})
                t['samples'] += sign * int(p[3]); t['blocks'] += sign * int(p[4]); continue
            threads[int(p[0])]['hist'][int(p[1], 16)] += sign * int(p[2])
    parse(sel, 1, threads)
    if prev: parse(prev, -1, threads)
    for line in []:
        p = line.split()
        if p[0] == 'T':
            tid = int(p[1]); threads[tid] = {'entry': int(p[2], 16), 'samples': int(p[3]), 'blocks': int(p[4]), 'hist': collections.Counter()}
            cur = tid; continue
        tid, eip, hits = int(p[0]), int(p[1], 16), int(p[2])
        threads[tid]['hist'][eip] += hits
    for tid in sorted(threads):
        if a.fil is not None and tid != a.fil: continue
        t = threads[tid]; ent = t['entry']; entva = ent - rb + base if rb <= ent < rb + 0x1000000 else ent
        print('== fil %d  entree=0x%08x (%s)  echantillons=%d  blocs=%d' % (tid, entva, name(fn_of(entva)), t['samples'], t['blocks']))
        if not t['samples']: continue
        per = collections.Counter(); per_eip = {}
        for eip, h in t['hist'].items():
            va = eip - rb + base if rb <= eip < rb + 0x1000000 else eip
            f = fn_of(va); per[f] += h; per_eip.setdefault(f, collections.Counter())[va] += h
        tot = t['samples']
        print('   %-38s %7s %6s %14s %s' % ('fonction', 'ech.', 'part', 'blocs (est.)', 'emitprof: exec / ARM o/bloc / x86 insn/bloc'))
        for f, h in per.most_common(a.top):
            est = h / tot * t['blocks']
            e = emit.get(f)
            es = '' if not e else '%d / %.0f / %.1f' % (e[0], e[1] / e[0], e[2] / e[0])
            print('   %-38s %7d %5.1f%% %14.0f %s' % (name(f), h, 100.0 * h / tot, est, es))
            hot = per_eip[f].most_common(3)
            print('      ' + '  '.join('%08x:%d' % (v, c) for v, c in hot))
    if a.emit:
        print('== emitprof : fonctions par blocs executes (toutes threads confondues)')
        for f, e in sorted(emit.items(), key=lambda kv: -kv[1][0])[:a.top]:
            print('   %-38s exec=%-11d ARM/bloc=%6.0f  x86insn/bloc=%5.1f  blocs=%d' % (name(f), e[0], e[1] / e[0], e[2] / e[0], e[3]))

if __name__ == '__main__': main()
