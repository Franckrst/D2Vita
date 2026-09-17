#!/usr/bin/env python3
"""tools/verif_intrin_capture.py — CAPTURE PROOF for an intrinsic target.

The src/dynarec86/dyn86_intrin.h mechanism recognizes a function by its
BLOCK-START address. This only captures 100% of entries if nothing ever
enters anywhere but the first byte. This script checks that against the
binary:

  - DIRECT `jmp` rel32 to the entry        -> FORBIDDEN (tail call: the block
                                             doesn't restart at the entry)
  - jump landing INSIDE the body           -> FORBIDDEN
  - direct `call` rel32                    -> allowed (creates a block at the entry)
  - DATA reference (vtable)                -> allowed (same, via indirect call)

  usage: verif_intrin_capture.py <Game.exe> <VA_entry> <VA_body_end>
  e.g. : verif_intrin_capture.py ~/d2-vita-refs/1.14d/Game.exe 0x50dd60 0x50de0d
"""
import struct, sys

def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    path, va0, va1 = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    f = open(path, 'rb').read()
    pe = struct.unpack_from('<I', f, 0x3c)[0]
    base = struct.unpack_from('<I', f, pe + 24 + 28)[0]
    nsec = struct.unpack_from('<H', f, pe + 6)[0]
    txt = None
    for i in range(nsec):
        o = pe + 24 + struct.unpack_from('<H', f, pe + 20)[0] + i * 40
        if f[o:o+8].rstrip(b'\0') == b'.text':
            vsz, va, rsz, raw = struct.unpack_from('<IIII', f, o + 8)
            txt = (raw, base + va, vsz)
    if not txt: print("pas de .text"); return 1
    OFF, TVA, TSZ = txt
    body = range(va0 + 1, va1 + 1)

    refs = sum(1 for i in range(0, len(f) - 4)
               if struct.unpack_from('<I', f, i)[0] == va0)
    calls = jmps = inside = 0
    bad = []
    for i in range(OFF, OFF + TSZ - 6):
        op = f[i]
        if op in (0xE8, 0xE9):
            t = (i + 5 - OFF + TVA) + struct.unpack_from('<i', f, i + 1)[0]
            n = 5
        elif op == 0x0F and 0x80 <= f[i+1] <= 0x8F:
            t = (i + 6 - OFF + TVA) + struct.unpack_from('<i', f, i + 2)[0]
            n = 6
        else:
            continue
        src = i - OFF + TVA
        if t == va0:
            if op == 0xE8: calls += 1
            else: jmps += 1; bad.append(("jmp entree", src))
        elif t in body:
            inside += 1; bad.append(("saut dans le corps", src))

    print(f"cible 0x{va0:08x}..0x{va1:08x}  ({path})")
    print(f"  references en donnee : {refs}")
    print(f"  call rel32 directs   : {calls}")
    print(f"  jmp  rel32 entree    : {jmps}   (doit etre 0)")
    print(f"  sauts dans le corps  : {inside} (doit etre 0)")
    for k, s in bad[:10]: print(f"    !! {k} depuis 0x{s:08x}")
    ok = (jmps == 0 and inside == 0)
    print("VERDICT: CAPTURE COMPLETE" if ok else "VERDICT: CAPTURE INCOMPLETE — ne pas armer")
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
