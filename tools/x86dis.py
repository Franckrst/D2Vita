#!/usr/bin/env python3
"""x86dis.py — disassembles a VA range of Game.exe (1.14d, ImageBase 0x400000) with objdump.
Usage: x86dis.py <va_hex> [length_hex=0x80] [exe=~/d2-vita-refs/1.14d/Game.exe]
Also accepts a console/qemu-relocated address: base 0x03900000 (current pack,
since the 2026-09-24 heap raise), 0x02100000 (0.1.7 .. 0.1.11-beta5) or
0x01900000 (builds up to 0.1.6) -> 0x400000. The three windows do not overlap,
so an address names its own era and old reports stay readable."""
import struct, subprocess, sys, os, tempfile
va = int(sys.argv[1], 16); n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x80
exe = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/d2-vita-refs/1.14d/Game.exe')
if 0x03900000 <= va < 0x04200000: va = va - 0x03900000 + 0x400000
elif 0x02100000 <= va < 0x02A00000: va = va - 0x02100000 + 0x400000
elif 0x01900000 <= va < 0x02000000: va = va - 0x01900000 + 0x400000
d = open(exe, 'rb').read(); pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 24 + 28)[0]; sec = pe + 24 + optsz
for i in range(nsec):
    name, vsz, vad, rsz, raw = struct.unpack_from('<8sIIII', d, sec + 40 * i)
    if base + vad <= va < base + vad + max(vsz, rsz):
        off = raw + (va - base - vad); break
else: sys.exit(f'VA {va:#x} hors sections')
with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as t: t.write(d[off:off + n]); tmp = t.name
out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel', f'--adjust-vma={va:#x}', tmp], capture_output=True, text=True).stdout
os.unlink(tmp); print('\n'.join(l for l in out.splitlines() if ':\t' in l))
