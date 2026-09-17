#!/usr/bin/env python3
"""tools/mmu_census.py — CENSUS of fastmmu sites in the EMITTED code.

fastmmu accounts for **12.6% of emitted bytes**: on Vita the guest memory is
not identity-mapped (D2ARENA), so every x86 memory access becomes

    add  rT, rGuest, #membase        <-- 1 LINK in the dependency chain
    ldr  rD, [rT, #dep]              <-- 1 LINK

This script doesn't count bytes: it counts **SHAPES**, because the shape says
how many links can be removed and BY WHICH MEANS. It classifies every
`add rT, rGuest, #membase` in the hot blocks emitted by D2_EMITDUMP according
to what PRECEDES it (how the guest address was built) and what FOLLOWS it
(the LDR/STR displacement).

    shape                      possible transform                link removed
    ------------------------   ------------------------------   --------------
    ABS   movw/movt just before  MOV32(rT, disp+membase)          1  (the ADD)
          (absolute addressing)  = D2_MMUFOLD
    IDX   add rT,rA,rB,lsl#s     add rT,rA,#membase               1  (the index
          just before, dep=0     ldr rD,[rT,rB,lsl#s]                ADD)
                                 = D2_MMUIDX (NO dedicated register)
    REG0  rGuest is an x86        ldr rD,[xBase,rGuest]           1  (the ADD)
          register, dep=0        REQUIRES A DEDICATED REGISTER
    REGn  rGuest x86, dep!=0     nothing (register-offset          0
                                 addressing doesn't accept an imm)
    AUTRE anything else           to be determined                ?

Weighted by BLOCK ENTRIES, like emitprof_dis.py: loops internal to a block
aren't counted, so everything here is a LOWER BOUND. And no microsecond comes
out of this: it's pure shape counting.

Usage:
  python3 tools/mmu_census.py /tmp/emitdump.txt --membase 0x41000000
"""
import sys, os, re, subprocess, argparse, tempfile

OBJDUMP_ARM = os.environ.get('ARM_OBJDUMP', 'arm-linux-gnueabihf-objdump')

LDST = re.compile(r'^(ldr|str)(b|h|sb|sh|d)?\s+(\w+), \[(\w+)(?:, #(-?\d+))?\]!?$')
ADDIDX = re.compile(r'^add\s+(\w+), (\w+), (\w+)(?:, lsl #(\d+))?$')
MOVT = re.compile(r'^movt\s+(\w+), #')
MOVW = re.compile(r'^movw\s+(\w+), #')
FORMS = ['ABS', 'IDX', 'REG0', 'REGn', 'AUTRE']


def parse(path):
    blocks, cur = [], None
    for line in open(path):
        line = line.strip()
        if line.startswith('BLOCK'):
            cur = dict(re.findall(r'(\w+)=(\S+)', line))
            cur['addr'] = int(cur['x86'], 16); cur['exec'] = int(cur['exec'])
            blocks.append(cur)
        elif line.startswith('MAP'):
            cur['map'] = [(int(a, 16), int(b)) for a, b in (t.split(':') for t in line.split()[1:])]
        elif line.startswith('ARM'):
            cur['arm'] = [int(t, 16) for t in line.split()[1:]]
    return blocks


def disasm(tmp, words):
    a = os.path.join(tmp, 'a.bin')
    open(a, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in words))
    out = subprocess.run([OBJDUMP_ARM, '-D', '-b', 'binary', '-m', 'arm', a],
                         capture_output=True, text=True).stdout
    A = {}
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)', l)
        if not m: continue
        # objdump: tabs + a "@ 0x..." comment at the end of the line.
        t = m.group(3).split('@')[0]
        A[int(m.group(1), 16)] = ' '.join(t.split()).strip()
    return A


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--membase', required=True)
    ap.add_argument('--exemples', type=int, default=0, help='imprimer N exemples par forme')
    a = ap.parse_args()
    membase = int(a.membase, 0)
    blocks = parse(a.dump)

    site = {f: 0 for f in FORMS}      # emitted sites
    wt = {f: 0 for f in FORMS}        # weighted by block entries
    ex_seen = {f: [] for f in FORMS}
    tot_arm_w = 0
    burst = {}                        # burst length (consecutive same rA/#membase)
    with tempfile.TemporaryDirectory() as tmp:
        for b in blocks:
            A = disasm(tmp, b['arm']); offs = sorted(A); ex = b['exec']
            tot_arm_w += len(b['arm']) * 4 * ex
            prev_src = None; run = 0
            for i, o in enumerate(offs):
                t = A[o]
                m = re.match(r'add\s+(\w+), (\w+), #%d$' % membase, t)
                if not m:
                    continue
                dst, src = m.group(1), m.group(2)
                nxt = A.get(offs[i + 1]) if i + 1 < len(offs) else ''
                prv = A.get(offs[i - 1]) if i else ''
                prv2 = A.get(offs[i - 2]) if i >= 2 else ''
                ls = LDST.match(nxt or '')
                dep = int(ls.group(5)) if (ls and ls.group(5)) else 0
                uses = bool(ls) and ls.group(4) == dst
                mi = ADDIDX.match(prv or '')
                if (MOVT.match(prv or '') and MOVW.match(prv2 or '')
                        and MOVT.match(prv).group(1) == src):
                    f = 'ABS'
                elif mi and mi.group(1) == src and uses and dep == 0:
                    f = 'IDX'
                elif uses and dep == 0:
                    f = 'REG0'
                elif uses:
                    f = 'REGn'
                else:
                    f = 'AUTRE'
                site[f] += 1; wt[f] += ex
                if len(ex_seen[f]) < a.exemples:
                    ex_seen[f].append('0x%08x +%d  %-34s | %-34s | %s'
                                      % (b['addr'], o, prv or '', t, nxt or ''))
                run = run + 1 if src == prev_src else 1
                prev_src = src
                burst[run] = burst.get(run, 0) + ex

    ts, tw = sum(site.values()), sum(wt.values())
    print('== RECENSEMENT fastmmu — %d blocs chauds vides, membase=0x%x ==' % (len(blocks), membase))
    print('sites `add rT,rG,#membase` : %d emis, %d ponderes (entrees de bloc)' % (ts, tw))
    print('octets ARM ponderes du vidage : %d' % tot_arm_w)
    print()
    print('%-6s %10s %7s %16s %7s   %s' % ('forme', 'sites', '%', 'pondere', '%', 'maillon retirable'))
    LEG = {'ABS': '1 (D2_MMUFOLD)', 'IDX': '1 (D2_MMUIDX, sans registre)',
           'REG0': '1 (EXIGE un registre dedie)', 'REGn': '0 (decalage de registre impossible)',
           'AUTRE': '?'}
    for f in FORMS:
        print('%-6s %10d %6.2f%% %16d %6.2f%%   %s'
              % (f, site[f], 100.0 * site[f] / max(ts, 1), wt[f], 100.0 * wt[f] / max(tw, 1), LEG[f]))
    print()
    print('RAFALES (ADD consecutifs sur la MEME source dans le bloc — L3b/L3c) :')
    for k in sorted(burst):
        if k > 1:
            print('  rang %d dans la rafale : %d (pondere)' % (k, burst[k]))
    reuse = sum(v for k, v in burst.items() if k > 1)
    print('  ADD ponderes qui REPETENT la traduction deja faite : %d (%.2f%% des sites)'
          % (reuse, 100.0 * reuse / max(tw, 1)))
    for f in FORMS:
        for l in ex_seen[f]:
            print('  [%s] %s' % (f, l))


if __name__ == '__main__':
    main()
