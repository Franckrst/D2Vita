#!/usr/bin/env python3
# tools/tests/crashreport_oracle.py — oracle for the streaming dump reader.
#
#   crashreport_oracle.py --reader BIN DUMP...
#
# For each dump, runs the reference autopsy (tools/autopsie_psp2dmp.py) and the
# C++ reader (BIN --dump-facts DUMP) and requires the same faulting-thread tid,
# pc, lr, sp, cpsr and guest EBP chain (frame and RVA).
#
# Both rules are compared: the production one (the first thread whose
# THREAD_INFO stop reason is not zero) and the older "first registered thread"
# (--first-thread on both sides).
#
# Bases: the script prints VA = ra - game_base + 0x400000 and reads the guest
# stack at arena_host_base + EBP; the reader takes both from the session
# record. Both are given the same values here (Game.exe at 0x01900000, arena
# host view at 0x84000000).
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SCRIPT = os.path.join(ROOT, 'tools', 'autopsie_psp2dmp.py')
GB = 0x01500000
GAME_BASE = GB + 0x400000
HOST = 0x84000000


def run_script(dump, first_thread=False):
    argv = [sys.executable, SCRIPT, dump, '--game-base', hex(GAME_BASE), '--arena-host-base', hex(HOST)]
    if first_thread:
        argv.append('--first-thread')
    p = subprocess.run(argv, capture_output=True, text=True)
    if p.returncode != 0:
        return None, (p.stderr.strip().splitlines() or ['exit %d' % p.returncode])[-1]
    out = {'chain': [], 'not_in_dump': False}
    for line in p.stdout.splitlines():
        m = re.match(r'== FIL FAUTIF tid=(0x[0-9a-f]+)', line)
        if m:
            out['tid'] = int(m.group(1), 16)
        m = re.match(r'\s+pc=([0-9a-f]{8}) lr=([0-9a-f]{8}) sp=([0-9a-f]{8}) cpsr=([0-9a-f]{8})$', line)
        if m:
            out['pc'], out['lr'], out['sp'], out['cpsr'] = (int(g, 16) for g in m.groups())
        m = re.match(r'\s+VA 0x([0-9a-f]+)\s+\(RVA 0x([0-9a-f]+), pour set_alternate\)\s+cadre=([0-9a-f]{8})$', line)
        if m:
            out['chain'].append((int(m.group(3), 16), int(m.group(2), 16)))
        if re.match(r'\s+\(cadre [0-9a-f]+ hors dump\)$', line):
            out['not_in_dump'] = True
    return out, None


def run_reader(reader, dump, first_thread=False):
    argv = [reader, '--dump-facts', dump, '--game-base', '%x' % GAME_BASE, '--host-base', '%x' % HOST]
    if first_thread:
        argv.append('--first-thread')
    p = subprocess.run(argv, capture_output=True, text=True)
    kv = {}
    for line in p.stdout.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            kv[k] = v
    if kv.get('ok') != '1':
        return None, kv.get('error', 'reader failed (exit %d)' % p.returncode)
    chain = []
    if kv.get('chain'):
        for item in kv['chain'].split(','):
            frame, rva = item.split(':')
            chain.append((int(frame, 16), int(rva, 16)))
    return {'tid': int(kv['tid'], 16), 'pc': int(kv['pc'], 16), 'lr': int(kv['lr'], 16), 'sp': int(kv['sp'], 16),
            'cpsr': int(kv['cpsr'], 16), 'chain': chain, 'chain_end': kv.get('chain_end', ''),
            'peak_heap': kv.get('peak_heap', 'n/a'), 'decompressed': kv.get('decompressed', '?')}, None


def compare(dump, reader, first_thread):
    """0 when the script and the reader agree on that dump, 1 otherwise."""
    name = os.path.basename(dump)
    rule = 'first thread' if first_thread else 'stop reason'
    ref, err = run_script(dump, first_thread)
    if ref is None:
        print('  FAIL %s [%s]: reference script failed (%s)' % (name, rule, err))
        return 1
    got, err = run_reader(reader, dump, first_thread)
    if got is None:
        print('  FAIL %s [%s]: reader failed (%s)' % (name, rule, err))
        return 1
    diffs = [k for k in ('tid', 'pc', 'lr', 'sp', 'cpsr', 'chain') if ref.get(k) != got[k]]
    if ref['not_in_dump'] and got['chain_end'] not in ('not_in_dump',):
        diffs.append('chain_end (script: frame not in dump, reader: %s)' % got['chain_end'])
    if diffs:
        print('  FAIL %s [%s]: differs on %s' % (name, rule, ', '.join(diffs)))
        for k in ('tid', 'pc', 'lr', 'sp', 'cpsr', 'chain'):
            if ref.get(k) != got[k]:
                print('       %s script=%r reader=%r' % (k, ref.get(k), got[k]))
        return 1
    print('  OK   %s [%s]: tid=0x%x pc=%08x lr=%08x frames=%d (reader peak heap %s bytes, %s bytes inflated)'
          % (name, rule, got['tid'], got['pc'], got['lr'], len(got['chain']), got['peak_heap'], got['decompressed']))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reader', required=True)
    ap.add_argument('dumps', nargs='+')
    a = ap.parse_args()
    failures = 0
    for dump in a.dumps:
        for first_thread in (False, True):
            failures += compare(dump, a.reader, first_thread)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
