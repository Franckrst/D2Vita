#!/usr/bin/env python3
"""x86dis.py — disassembles a VA range of Game.exe (1.14d, ImageBase 0x400000) with objdump.
Usage: x86dis.py <va_hex> [length_hex=0x80] [exe=~/d2-vita-refs/1.14d/Game.exe]
Also accepts a console/qemu-relocated address (0x019xxxxx): base 0x01900000 -> 0x400000."""
import struct, subprocess, sys, os, tempfile
va = int(sys.argv[1], 16); n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x80
exe = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/d2-vita-refs/1.14d/Game.exe')
if va >= 0x01900000 and va < 0x02000000: va = va - 0x01900000 + 0x400000
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
