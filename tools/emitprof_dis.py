#!/usr/bin/env python3
"""tools/emitprof_dis.py — breakdown of a D2_EMITDUMP dump.

The [emitprof] report from the instrumented binary gives the profile PER x86
FAMILY. This script provides the other half: what each EMITTED ARM
instruction actually does, by disassembling the emitted blocks and
classifying instruction by instruction, weighted by the block's execution
count.

WHY THIS SCRIPT EXISTS. The instrumented "flag archiving" sub-cost (hooked
into the UFLAG_* macros) reports 0.00%. Disassembly reports 8.1%: archiving
mostly goes through STR instructions written directly in emit_cmp32/... and
through SET_DFNONE, never through those macros. Instrumenting a macro only
covers its callers; disassembly can't miss a path. The two measurements
cross-check each other — keep both.

Usage:
  D2_EMITDUMP=/path/to/dump.txt D2_EMITDUMPN=300 <qemu run>
  python3 tools/emitprof_dis.py dump.txt [--membase 0x41000000] [--dis OUTPUT/]

--membase: the fastmmu base of the run (console model = D2ARENA arena).
           Without it the "fastmmu" bucket reads zero, which is CORRECT for a
           run without an arena and WRONG for a run with one.
--dis DIR: also writes an annotated per-block disassembly (x86 -> ARM).

All quantities are EMITTED CODE BYTES weighted by BLOCK ENTRIES. This is
neither time nor a retired-instruction count: loops internal to a block
aren't captured by the weighting, so everything here is a LOWER BOUND.
"""
import sys, os, re, subprocess, argparse, tempfile

OBJDUMP_ARM = os.environ.get('ARM_OBJDUMP', 'arm-linux-gnueabihf-objdump')
OBJDUMP_X86 = os.environ.get('X86_OBJDUMP', 'objdump')
# xFlags is r12 in the Box86/ARM convention => objdump prints it as "ip".
# op1/op2/res/df in x86emu_t (checked via offsetof on the ARM target).
EMU_FLAG_FIELDS = {596: 'df', 600: 'op1', 604: 'op2', 608: 'res'}
ALU = re.compile(r'^(cmp|test|sub|add|and|or|xor|inc|dec|sbb|adc|neg)\b')
JCC = re.compile(r'^j(?!mp)\w+\b')
# conditions ARM reads directly from NZCV (everything except parity/AF)
JCC_NZCV = re.compile(r'^j(e|ne|z|nz|s|ns|g|ge|l|le|a|ae|b|be|o|no)\b')

CATS = ['travail-invite', 'drapeaux/pose', 'drapeaux/lecture', 'drapeaux/differe',
        'fastmmu(+membase)', 'aiguillage-table-saut', 'prologue/budget-vif',
        'prologue/budget-MORT', 'sonde-d2ep']


def parse(path):
    blocks, cur = [], None
    for line in open(path):
        line = line.strip()
        if line.startswith('BLOCK'):
            cur = dict(re.findall(r'(\w+)=(\S+)', line))
            cur['addr'] = int(cur['x86'], 16)
            cur['exec'] = int(cur['exec'])
            blocks.append(cur)
        elif line.startswith('MAP'):
            cur['map'] = [(int(a, 16), int(b))
                          for a, b in (t.split(':') for t in line.split()[1:])]
        elif line.startswith('X86'):
            cur['x86b'] = bytes(int(t, 16) for t in line.split()[1:])
        elif line.startswith('ARM'):
            cur['arm'] = [int(t, 16) for t in line.split()[1:]]
    return blocks


def disasm(tmp, words, base_x86, x86bytes):
    a = os.path.join(tmp, 'a.bin'); x = os.path.join(tmp, 'x.bin')
    open(a, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in words))
    open(x, 'wb').write(x86bytes)
    A, X = {}, {}
    out = subprocess.run([OBJDUMP_ARM, '-D', '-b', 'binary', '-m', 'arm', a],
                         capture_output=True, text=True).stdout
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)', l)
        if m: A[int(m.group(1), 16)] = m.group(3).strip()
    out = subprocess.run([OBJDUMP_X86, '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel',
                          '--adjust-vma=0x%x' % base_x86, x], capture_output=True, text=True).stdout
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)', l)
        if m: X[int(m.group(1), 16)] = m.group(3).strip()
    return A, X


def classify(A, offs, off, first, membase):
    """One emitted ARM instruction -> one category. Auditable rules:
       offsets 0..31   = d2ep probe (absent from the shipped binary)
       offsets 32..47  = preemption budget, LIVE path
       offsets 48..first = expiration path, SKIPPED by the BGT (dead)
       any instruction naming "ip" = flag traffic
       str [r0,#df/op1/op2/res] (+ the movw feeding it) = archiving
       add rX,rY,#membase = fastmmu address translation """
    if off < 32: return 'sonde-d2ep'
    if off < 48: return 'prologue/budget-vif'
    if off < first: return 'prologue/budget-MORT'
    t = A[off]
    i = offs.index(off)
    m = re.match(r'str\s+\w+, \[r0, #(\d+)\]', t)
    if m and int(m.group(1)) in EMU_FLAG_FIELDS: return 'drapeaux/differe'
    if i + 1 < len(offs):
        m2 = re.match(r'str\s+(\w+), \[r0, #(\d+)\]', A[offs[i + 1]])
        if m2 and int(m2.group(2)) in EMU_FLAG_FIELDS and \
           re.match(r'movw?\s+%s, #' % m2.group(1), t): return 'drapeaux/differe'
    if re.match(r'\w+\s+ip,', t): return 'drapeaux/pose'
    if re.search(r'\bip\b', t): return 'drapeaux/lecture'
    if membase is not None and re.search(r'add\s+\w+, \w+, #%d\b' % membase, t):
        return 'fastmmu(+membase)'
    if re.search(r'ldr\s+\w+, \[\w+, \w+, lsl #2\]', t) or \
       re.search(r'ubfx\s+\w+, lr, #0, #16', t) or re.search(r'lsr\s+\w+, lr, #16', t):
        return 'aiguillage-table-saut'
    return 'travail-invite'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--membase', default=None,
                    help='base fastmmu du run (ex. 0x41000000) ; absente = run sans arene')
    ap.add_argument('--dis', default=None, help='repertoire ou ecrire le desassemblage annote')
    a = ap.parse_args()
    membase = int(a.membase, 0) if a.membase else None
    blocks = parse(a.dump)
    if a.dis: os.makedirs(a.dis, exist_ok=True)

    tot = {c: 0 for c in CATS}; totw = {c: 0 for c in CATS}
    pw = {k: 0 for k in ('pose', 'lecture', 'differe', 'autre')}
    pairs_w = pairs_nzcv_w = pairs_bytes_w = 0
    gross_w = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n, b in enumerate(blocks, 1):
            A, X = disasm(tmp, b['arm'], b['addr'], b['x86b'])
            offs = sorted(A); mp = b.get('map', [])
            first = mp[0][1] if mp else len(b['arm']) * 4
            ex = b['exec']; gross_w += len(b['arm']) * 4 * ex
            cat = {o: classify(A, offs, o, first, membase) for o in offs}
            for o in offs:
                tot[cat[o]] += 4; totw[cat[o]] += 4 * ex
            bd = [(mp[k][0], mp[k][1], mp[k + 1][1] if k + 1 < len(mp) else len(b['arm']) * 4)
                  for k in range(len(mp))]
            for k in range(1, len(bd)):
                xa, lo, hi = bd[k]; pxa, plo, phi = bd[k - 1]
                if not JCC.match(X.get(xa, '')) or not ALU.match(X.get(pxa, '')): continue
                pairs_w += ex; pairs_bytes_w += ((hi - lo) + (phi - plo)) * ex
                if JCC_NZCV.match(X[xa]): pairs_nzcv_w += ex
                for o in list(range(plo, phi, 4)) + list(range(lo, hi, 4)):
                    if o not in A: continue
                    c = cat[o]
                    k2 = ('pose' if c == 'drapeaux/pose' else
                          'lecture' if c == 'drapeaux/lecture' else
                          'differe' if c == 'drapeaux/differe' else 'autre')
                    pw[k2] += 4 * ex
            if a.dis:
                with open(os.path.join(a.dis, 'b%d.txt' % n), 'w') as f:
                    f.write('BLOCK x86=0x%08x exec=%d insns=%s arm=%do\n'
                            % (b['addr'], ex, b.get('ninsts', '?'), len(b['arm']) * 4))
                    for o in range(0, first, 4):
                        f.write('    +%-5d %-46s [%s]\n' % (o, A.get(o, '??'), cat.get(o, '')))
                    for xa, lo, hi in bd:
                        f.write('  x86 %08x: %-42s [%d o ARM]\n' % (xa, X.get(xa, '??'), hi - lo))
                        for o in range(lo, hi, 4):
                            f.write('    +%-5d %-46s [%s]\n' % (o, A.get(o, '??'), cat.get(o, '')))

    net_t = sum(tot.values()) - tot['sonde-d2ep']
    net_w = sum(totw.values()) - totw['sonde-d2ep']
    print('blocs vides=%d | statique net=%d o | pondere net=%d o'
          % (len(blocks), net_t, net_w))
    print('modele memoire : %s' % ('arene, membase=0x%x (CONSOLE)' % membase if membase
                                   else 'IDENTITE (membase=0) — pas celui de la console'))
    print('%-24s %12s %8s %18s %8s' % ('categorie', 'ARMo-stat', '%net', 'ARMo-pondere', '%net'))
    for c in CATS:
        if c == 'sonde-d2ep': continue
        print('%-24s %12d %7.2f%% %18d %7.2f%%'
              % (c, tot[c], 100.0 * tot[c] / net_t, totw[c], 100.0 * totw[c] / net_w))
    print('%-24s %12d %8s %18d %8s'
          % ('(sonde, deduite)', tot['sonde-d2ep'], '-', totw['sonde-d2ep'], '-'))
    print()
    print('PAIRES « ALU qui pose les drapeaux » + « Jcc collee » (ponderees) :')
    print('  paires=%d  octets ARM=%d (%.2f%% du net)'
          % (pairs_w, pairs_bytes_w, 100.0 * pairs_bytes_w / net_w))
    print('  dont condition exprimable en NZCV : %d (%.1f%%)'
          % (pairs_nzcv_w, 100.0 * pairs_nzcv_w / max(pairs_w, 1)))
    for k in ('pose', 'lecture', 'differe', 'autre'):
        print('    %-9s %16d o  (%5.2f%% du net)' % (k, pw[k], 100.0 * pw[k] / net_w))
    gain = pw['pose'] + pw['lecture']
    print('  LEVIER L1 (brancher sur CPSR, archivage differe CONSERVE) : %d o = %.2f%% du net'
          % (gain, 100.0 * gain / net_w))


if __name__ == '__main__':
    main()
