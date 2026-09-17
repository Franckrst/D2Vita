#!/usr/bin/env python3
"""d2s_stats.py — read/patch stats in a Diablo II 1.14d save (.d2s).

  d2s_stats.py show <save.d2s>
  d2s_stats.py set  <save.d2s> <out.d2s> [stat=value ...]
      stats: str energy dex vit statpts skillpts hp maxhp mana maxmana stamina maxstamina level exp gold goldbank
      hp/mana/stamina in whole points (converted to 1/256 internally)

"gf" section (offset ~765): LSB-first bitstream [id 9 bits][value w(id) bits], terminated by id=0x1FF.
The rest of the file (starting at "if", skills) is spliced back in as-is; size (off 8) and checksum (off 12) are recomputed.
Bench usage: give the character huge HP to survive in monster zones under qemu.
"""
import struct, sys

WIDTH = {0:10,1:10,2:10,3:10,4:10,5:8,6:21,7:21,8:21,9:21,10:21,11:21,12:7,13:32,14:25,15:25}
NAMES = ["str","energy","dex","vit","statpts","skillpts","hp","maxhp","mana","maxmana","stamina","maxstamina","level","exp","gold","goldbank"]
FIXED = {6,7,8,9,10,11}

def checksum(d):
    c = 0
    for i, b in enumerate(d):
        if 12 <= i < 16: b = 0
        c = (((c << 1) | (c >> 31)) + b) & 0xFFFFFFFF
    return c

class Bits:
    def __init__(s, data): s.d=data; s.p=0
    def read(s, n):
        v=0
        for k in range(n):
            byte=s.d[s.p>>3]; bit=(byte>>(s.p&7))&1; v|=bit<<k; s.p+=1
        return v

def parse(d):
    gf=d.find(b"gf"); assert gf>0, "section gf introuvable"
    b=Bits(d[gf+2:]); stats={}
    while True:
        sid=b.read(9)
        if sid==0x1FF: break
        stats[sid]=b.read(WIDTH[sid])
    end_bits=b.p; end=gf+2+((end_bits+7)//8)
    return gf, stats, end

def encode(stats):
    bits=[]
    for sid in sorted(stats):
        v=stats[sid]
        for k in range(9): bits.append((sid>>k)&1)
        for k in range(WIDTH[sid]): bits.append((v>>k)&1)
    for k in range(9): bits.append(1)   # 0x1FF
    out=bytearray((len(bits)+7)//8)
    for i,bit in enumerate(bits): out[i>>3]|=bit<<(i&7)
    return bytes(out)

def show(d):
    gf,stats,end=parse(d)
    ck=struct.unpack_from("<I",d,12)[0]
    print(f"taille={len(d)} (champ={struct.unpack_from('<I',d,8)[0]}) checksum={'OK' if checksum(d)==ck else 'BAD'} gf@{gf} fin@{end} (suivant: {d[end:end+2]!r})")
    for sid in sorted(stats):
        v=stats[sid]; print(f"  {NAMES[sid]:11s} = {v/256 if sid in FIXED else v}")

def main():
    a=sys.argv[1:]
    if not a or a[0] not in ("show","set"): print(__doc__); sys.exit(1)
    d=bytearray(open(a[1],"rb").read())
    if a[0]=="show": show(d); return
    out=a[2]; gf,stats,end=parse(d)
    assert d[end:end+2]==b"if", f"la section suivante n'est pas 'if' ({d[end:end+2]!r})"
    for kv in a[3:]:
        k,v=kv.split("="); sid=NAMES.index(k); v=int(v)
        if sid in FIXED: v<<=8
        assert v < (1<<WIDTH[sid]), f"{k}={v} depasse {WIDTH[sid]} bits"
        stats[sid]=v
    new=d[:gf+2]+encode(stats)+d[end:]
    struct.pack_into("<I",new,8,len(new)); struct.pack_into("<I",new,12,0); struct.pack_into("<I",new,12,checksum(new))
    open(out,"wb").write(new); print(f"ecrit {out}"); show(new)

if __name__=="__main__": main()
