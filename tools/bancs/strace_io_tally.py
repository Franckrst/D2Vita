#!/usr/bin/env python3
# tools/bancs/strace_io_tally.py — counts, in a `strace -f -e trace=openat,read,lseek,_llseek,close`
# trace of a qemu-arm run, what the HOST actually asked the filesystem for each
# game file: number of read()s, bytes returned, lseek()s. This measures stdio
# AMPLIFICATION (4 KiB requested by the game -> 256 KiB read by the buffer), which
# the ReadFile shim can't see from the inside.
#   e.g.: python3 tools/bancs/strace_io_tally.py /tmp/orra_A.strace
import re, sys, collections, os
fdname = {}
per = collections.defaultdict(lambda: [0, 0, 0, collections.Counter()])   # name -> [reads, bytes, seeks, size histogram]
for path in sys.argv[1:]:
    for ln in open(path, errors='ignore'):
        ln = re.sub(r'^\d+\s+', '', ln)             # pid
        m = re.match(r'openat\(\w+, "([^"]+)".*\)\s+= (\d+)', ln)
        if m: fdname[int(m.group(2))] = os.path.basename(m.group(1)); continue
        m = re.match(r'close\((\d+)\)', ln)
        if m: fdname.pop(int(m.group(1)), None); continue
        m = re.match(r'(?:read|pread64)\((\d+), .*?, (\d+)(?:, \d+)?\)\s+= (-?\d+)', ln)
        if m:
            fd = int(m.group(1)); n = fdname.get(fd)
            if n and n.lower().endswith(('.mpq', '.dll', '.exe', '.d2s', '.txt', '.key', '.bin', '.map')):
                p = per[n]; p[0] += 1; got = max(0, int(m.group(3))); p[1] += got
                p[3][int(m.group(2))] += 1
            continue
        m = re.match(r'(?:lseek|_llseek)\((\d+), ', ln)
        if m:
            n = fdname.get(int(m.group(1)))
            if n and n.lower().endswith('.mpq'): per[n][2] += 1
tot = [0, 0, 0]
for n, p in sorted(per.items(), key=lambda kv: -kv[1][1]):
    if not n.lower().endswith('.mpq'): continue
    tot[0] += p[0]; tot[1] += p[1]; tot[2] += p[2]
    top = ' '.join(f'{k>>10}K={v}' if k >= 1024 else f'{k}o={v}' for k, v in p[3].most_common(4))
    print(f'  {n:<24} read()={p[0]:>6}  octets={p[1]>>10:>8}K  lseek={p[2]:>6}  demandes: {top}')
print(f'  TOTAL .mpq            read()={tot[0]:>6}  octets={tot[1]>>10:>8}K  lseek={tot[2]:>6}')
