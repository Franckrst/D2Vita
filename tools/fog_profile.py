#!/usr/bin/env python3
# tools/fog_profile.py — build a VERSIONED signature profile of Fog.dll exports.
#
# Fog exports by ordinal and the D2MOO 1.10f ordinal->name map does NOT match
# 1.13c. Instead of trusting that map, we identify each export by what it does:
# which OS imports (KERNEL32/USER32/...) it calls, plus a position-independent
# signature hash of its opening bytes. Output: a profile keyed by ordinal, with
# the detected behavior and signature — no hardcoded addresses leak into the
# runtime; this is analysis metadata.
#
#   fog_profile.py <Fog.dll> [--only 10042,10043,...]
import sys, struct, subprocess, hashlib, re

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]

def load(path):
    d=open(path,'rb').read()
    pe=u32(d,0x3c); assert d[pe:pe+4]==b'PE\x00\x00'
    nsec=u16(d,pe+6); opt=pe+24; magic=u16(d,opt)
    image_base=u32(d,opt+28); exp_rva=u32(d,opt+96); imp_rva=u32(d,opt+104)
    secs=[]
    so=opt+ u16(d,pe+20)  # SizeOfOptionalHeader
    for i in range(nsec):
        b=so+i*40
        secs.append((d[b:b+8].rstrip(b'\0').decode('latin1'),u32(d,b+12),u32(d,b+8),u32(d,b+20),u32(d,b+16)))
    def rva2off(rva):
        for name,vrva,vsz,rptr,rsz in secs:
            if vrva<=rva<vrva+max(vsz,rsz): return rptr+(rva-vrva)
        return None
    return d,image_base,exp_rva,imp_rva,rva2off,secs

def parse_exports(d,exp_rva,r2o):
    o=r2o(exp_rva); base=u32(d,o+16); nfun=u32(d,o+20)
    afun=r2o(u32(d,o+28)); res={}
    for i in range(nfun):
        frva=u32(d,afun+i*4)
        if frva: res[base+i]=frva
    return res  # ordinal -> function rva

def parse_imports(d,imp_rva,r2o,image_base):
    iat={}   # IAT slot VA -> "dll!name"
    o=r2o(imp_rva)
    while True:
        oft=u32(d,o); name_rva=u32(d,o+12); first=u32(d,o+16)
        if oft==0 and name_rva==0 and first==0: break
        dll=b''; no=r2o(name_rva)
        while d[no]!=0: dll+=bytes([d[no]]); no+=1
        dll=dll.decode('latin1')
        lut=r2o(oft or first); slot=first
        k=0
        while True:
            t=u32(d,lut+k*4)
            if t==0: break
            if t & 0x80000000: nm='#%d'%(t&0xffff)
            else:
                pn=r2o(t)+2; s=b''
                while d[pn]!=0: s+=bytes([d[pn]]); pn+=1
                nm=s.decode('latin1')
            iat[image_base+slot+k*4]="%s!%s"%(dll,nm)
            k+=1
        o+=20
    return iat

def main():
    path=sys.argv[1]
    only=None
    if '--only' in sys.argv: only=set(int(x) for x in sys.argv[sys.argv.index('--only')+1].split(','))
    d,image_base,exp_rva,imp_rva,r2o,secs=load(path)
    exports=parse_exports(d,exp_rva,r2o)
    iat=parse_imports(d,imp_rva,r2o,image_base)
    # disassemble
    dis=subprocess.run(['objdump','-d','-M','intel',path],capture_output=True,text=True).stdout
    # index disasm lines by VA
    linere=re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f ]+?)\s+(\S.*)$')
    calls_by_va={}  # for 'call ds:0xADDR' extract ADDR ; map VA-> list of import names in its function window
    lines=[]
    for ln in dis.splitlines():
        m=linere.match(ln)
        if m: lines.append((int(m.group(1),16),m.group(3)))
    # sort export rvas to bound functions
    ords=sorted(exports.items(), key=lambda kv:kv[1])
    rva_list=[frva for _,frva in ords]
    def func_end(frva):
        import bisect
        i=bisect.bisect_right(rva_list,frva)
        return rva_list[i] if i<len(rva_list) else frva+0x400
    # for quick lookup of disasm in [va_start,va_end)
    lines.sort()
    import bisect
    va_index=[va for va,_ in lines]
    prof=[]
    for ordn,frva in sorted(exports.items()):
        if only and ordn not in only: continue
        va0=image_base+frva; va1=image_base+func_end(frva)
        i=bisect.bisect_left(va_index,va0); j=bisect.bisect_left(va_index,va1)
        body=lines[i:j]
        imports_called=[]
        sig=b''
        for va,txt in body[:24]:
            # signature: opcode mnemonic only (position independent)
            sig+=txt.split()[0].encode()+b';'
            m=re.search(r'ds:0x([0-9a-f]+)',txt)
            if m and 'call' in txt.lower():
                tgt=int(m.group(1),16)
                if tgt in iat: imports_called.append(iat[tgt])
        beh=[]
        s=' '.join(imports_called)
        if 'HeapAlloc' in s: beh.append('ALLOC')
        if 'HeapFree' in s: beh.append('FREE')
        if 'HeapReAlloc' in s: beh.append('REALLOC')
        if 'CreateFileA' in s or 'ReadFile' in s: beh.append('FILE')
        if 'OutputDebugString' in s or 'wsprintf' in s.lower(): beh.append('LOG')
        if 'socket' in s.lower() or 'WSOCK' in s: beh.append('NET')
        sighash=hashlib.sha1(sig).hexdigest()[:10]
        prof.append((ordn,frva,sighash,','.join(sorted(set(beh))) or '-', ';'.join(sorted(set(imports_called)))[:80]))
    print("# Fog 1.13c export profile (signature-based; ordinal is the stable key)")
    print("# ordinal   rva     sig        behavior   os-imports-called")
    for ordn,frva,sh,beh,imp in prof:
        print(f"{ordn:<8} 0x{frva:05x} {sh}  {beh:<10} {imp}")

main()
