#!/usr/bin/env python3
"""d2s_seed.py — read or patch the map seed of a Diablo II .d2s save (1.14d).

The map seed (u32 at offset 171) is what single-player uses to regenerate the
world on every load. Some seeds deterministically Halt-316 the Rogue town-load
on Vita3K ("Unable to find room for unit", player spawn placement) while the
same save loads fine on qemu and on real hardware with the same seed (a
Vita3K-only allocation-layout artifact of its JIT, not a portage bug).
Patching the seed of a valid save is the 1-command repro for any seed on the
LOAD path.

Usage:
  d2s_seed.py show <save.d2s>
  d2s_seed.py set  <save.d2s> <hexseed> [out.d2s]   (checksum recomputed)

Known-toxic seeds (Vita3K load-path Halt-316, ~30% of random seeds trigger it):
  0x3405dc82  0xc003fa7e  0x2ab8f5d9
Known-good: 0x1234abcd 0x7be49a11 0x15938d02 0xe8a1b6c4 0x5f27d3aa 0x91c46e08 0xd47c0b3e
"""
import struct, sys

SEED_OFF, CKSUM_OFF = 171, 12

def checksum(d: bytearray) -> int:
    # D2's rotating byte sum, computed with the checksum field zeroed.
    c = 0
    for i, b in enumerate(d):
        if CKSUM_OFF <= i < CKSUM_OFF + 4:
            b = 0
        c = (((c << 1) | (c >> 31)) + b) & 0xFFFFFFFF
    return c

def load(path):
    d = bytearray(open(path, 'rb').read())
    magic, _, size, ck = struct.unpack_from('<IIII', d, 0)
    assert magic == 0xAA55AA55, f"bad magic {magic:#x}"
    assert size == len(d), f"declared size {size} != file size {len(d)}"
    return d, ck

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    d, ck = load(path)
    seed = struct.unpack_from('<I', d, SEED_OFF)[0]
    ok = checksum(d) == ck
    if cmd == 'show':
        print(f"{path}: {len(d)}B seed={seed:#010x} checksum={'OK' if ok else 'BAD'} "
              f"name={bytes(d[20:36]).split(b'\\x00')[0].decode(errors='replace')}")
    elif cmd == 'set':
        new = int(sys.argv[3], 16)
        out = sys.argv[4] if len(sys.argv) > 4 else path
        struct.pack_into('<I', d, SEED_OFF, new)
        struct.pack_into('<I', d, CKSUM_OFF, checksum(d))
        open(out, 'wb').write(d)
        print(f"{out}: seed {seed:#010x} -> {new:#010x}, checksum recomputed")
    else:
        print(__doc__); sys.exit(1)

if __name__ == '__main__':
    main()
