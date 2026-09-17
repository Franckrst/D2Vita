// pristine_audit.cpp -- see pristine_audit.h.
#include "pristine_audit.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "platform/vita_present.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
using namespace d2rt;

// Integrity checker for "guest pristine" mode (test #8): compare a loaded module's
// CODE (executable sections) in guest memory against the ORIGINAL file on disk,
// relocation-aware. Reports every byte that differs and is NOT explained by a base
// relocation — i.e. an actual modification of the Blizzard image. Read-only; never
// feeds the anti-cheat. Returns the modified-byte count (0 = pristine, -1 = error).
int pristine_check(Cpu& c, uint32_t base, const std::string& diskpath, const char* name){
    std::vector<uint8_t> f=slurp(diskpath);
    auto rd32=[&](size_t o)->uint32_t{ uint32_t v=0; if(o+4<=f.size()) std::memcpy(&v,&f[o],4); return v; };
    auto rd16=[&](size_t o)->uint16_t{ uint16_t v=0; if(o+2<=f.size()) std::memcpy(&v,&f[o],2); return v; };
    if(f.size()<0x40 || f[0]!='M'||f[1]!='Z'){ std::printf("  [pristine] %-12s : cannot read %s\n",name,diskpath.c_str()); return -1; }
    uint32_t pe=rd32(0x3c); if(pe+0x18>f.size()||f[pe]!='P'||f[pe+1]!='E'){ std::printf("  [pristine] %-12s : bad PE\n",name); return -1; }
    uint32_t opt=pe+24, pref=rd32(opt+28), optsz=rd16(pe+20);
    uint16_t nsec=rd16(pe+6);
    uint32_t reloc_rva=rd32(opt+0x88), reloc_sz=rd32(opt+0x8c), so=opt+optsz;   // data dir[5] = BASE_RELOC
    int32_t delta=(int32_t)(base-pref);
    struct Sec{uint32_t rva,vsz,raw,rsz,ch;};
    std::vector<Sec> secs;
    for(int i=0;i<nsec;i++){ uint32_t b=so+(uint32_t)i*40;
        secs.push_back({rd32(b+12),rd32(b+8),rd32(b+20),rd32(b+16),rd32(b+36)}); }
    auto rva2raw=[&](uint32_t rva)->long{ for(auto&s:secs) if(rva>=s.rva&&rva<s.rva+std::max(s.vsz,s.rsz)) return (long)s.raw+(long)(rva-s.rva); return -1; };
    std::map<uint32_t,bool> relocd;                                 // reloc'd dword RVAs (type 3 HIGHLOW)
    if(reloc_sz){ long ro=rva2raw(reloc_rva); if(ro>=0){ uint32_t p=(uint32_t)ro,end=(uint32_t)ro+reloc_sz;
        while(p+8<=end && p+8<=f.size()){ uint32_t page=rd32(p),blk=rd32(p+4); if(blk<8) break;
            for(uint32_t k=0;k<(blk-8)/2 && p+8+k*2+2<=f.size();k++){ uint16_t e=rd16(p+8+k*2);
                if((e>>12)==3) relocd[page+(e&0xfff)]=true; }
            p+=blk; } } }
    int mods=0,relocs=0;
    for(auto&s:secs){ if(!(s.ch&0x20000000u)) continue;             // IMAGE_SCN_MEM_EXECUTE
        uint32_t n=std::min(s.vsz,s.rsz);
        for(uint32_t i=0;i<n;i++){ uint32_t rva=s.rva+i;
            uint8_t db=f[s.raw+i], mb=0; c.read(base+rva,&mb,1);
            if(db==mb) continue;
            bool isreloc=false;                                     // byte q of a reloc'd dword?
            for(int q=0;q<4;q++){ auto it=relocd.find(rva-q); if(it!=relocd.end()){
                long dr=rva2raw(rva-q); if(dr>=0){ uint32_t exp=rd32((size_t)dr)+(uint32_t)delta;
                    if((uint8_t)(exp>>(8*q))==mb){ isreloc=true; break; } } } }
            if(isreloc){ relocs++; continue; }
            if(mods<16) std::printf("  [pristine] %-12s MOD @0x%08x (RVA 0x%05x): disk %02x -> mem %02x\n",name,base+rva,rva,db,mb);
            mods++; }
    }
    std::printf("  [pristine] %-12s : %d modified code byte(s), %d reloc byte(s)  %s\n",
        name, mods, relocs, mods?"<-- NOT pristine":"OK (byte-identical to disk)");
    { char m[160]; std::snprintf(m,sizeof m,"pristine: %s : %d octet(s) de code modifie(s), %d octet(s) de relocation — %s",
          name, mods, relocs, mods?"NON INTACT":"identique au disque"); d2vita_progress(m); }
    return mods;
}

// Full-image pristine AUDIT (Warden fidelity, test #7). Unlike pristine_check
// (executable sections only) this scans the ENTIRE image — PE headers + every
// section, including the IAT — comparing guest memory to the on-disk original
// and CLASSIFYING every differing byte:
//   RELOCATION       – explained by base relocations (.reloc), given the load delta.
//   IMPORT_RESOLUTION – inside the IAT / import thunk arrays (a real Windows loader
//                       overwrites these with resolved addresses too).
//   RUNTIME_DATA     – inside a writable section (.data), legitimately mutated at run.
//   HEADER           – inside the PE headers (loader-written fields, e.g. ImageBase).
//   D2VITA_PATCH     – matches a KNOWN D2Vita patch site (blit hook, etc.); only in
//                      non-pristine mode.
//   UNKNOWN          – anything else. Target in pristine mode: D2VITA_PATCH==0 && UNKNOWN==0.
// Read-only; never feeds the anti-cheat. Returns D2VITA_PATCH+UNKNOWN count.
int pristine_audit(Cpu& c, uint32_t base, const std::string& diskpath, const char* name){
    std::vector<uint8_t> f=slurp(diskpath);
    auto rd32=[&](size_t o)->uint32_t{ uint32_t v=0; if(o+4<=f.size()) std::memcpy(&v,&f[o],4); return v; };
    auto rd16=[&](size_t o)->uint16_t{ uint16_t v=0; if(o+2<=f.size()) std::memcpy(&v,&f[o],2); return v; };
    if(f.size()<0x40||f[0]!='M'||f[1]!='Z'){ std::printf("  [audit] %-12s : cannot read %s\n",name,diskpath.c_str()); return -1; }
    uint32_t pe=rd32(0x3c); if(pe+0x18>f.size()||f[pe]!='P'||f[pe+1]!='E'){ std::printf("  [audit] %-12s : bad PE\n",name); return -1; }
    uint32_t opt=pe+24, pref=rd32(opt+28), optsz=rd16(pe+20);
    uint16_t nsec=rd16(pe+6);
    uint32_t sizeHdrs=rd32(opt+0x3c);
    uint32_t reloc_rva=rd32(opt+0x88), reloc_sz=rd32(opt+0x8c);   // data dir[5]  = BASE_RELOC
    uint32_t imp_rva =rd32(opt+0x68);                             // data dir[1]  = IMPORT
    uint32_t iat_rva =rd32(opt+0xC0), iat_sz =rd32(opt+0xC4);     // data dir[12] = IAT
    uint32_t so=opt+optsz;
    int32_t delta=(int32_t)(base-pref);
    struct Sec{uint32_t rva,vsz,raw,rsz,ch;};
    std::vector<Sec> secs;
    for(int i=0;i<nsec;i++){ uint32_t b=so+(uint32_t)i*40;
        secs.push_back({rd32(b+12),rd32(b+8),rd32(b+20),rd32(b+16),rd32(b+36)}); }
    auto rva2raw=[&](uint32_t rva)->long{ for(auto&s:secs) if(rva>=s.rva&&rva<s.rva+std::max(s.vsz,s.rsz)) return (long)s.raw+(long)(rva-s.rva); return -1; };
    std::map<uint32_t,bool> relocd;                              // reloc'd dword RVAs (type 3 HIGHLOW)
    if(reloc_sz){ long ro=rva2raw(reloc_rva); if(ro>=0){ uint32_t p=(uint32_t)ro,end=(uint32_t)ro+reloc_sz;
        while(p+8<=end && p+8<=f.size()){ uint32_t page=rd32(p),blk=rd32(p+4); if(blk<8) break;
            for(uint32_t k=0;k<(blk-8)/2 && p+8+k*2+2<=f.size();k++){ uint16_t e=rd16(p+8+k*2);
                if((e>>12)==3) relocd[page+(e&0xfff)]=true; }
            p+=blk; } } }
    // Resolved-thunk spans: the IAT directory plus every import descriptor's
    // FirstThunk array (covers binaries whose dir[12] is empty). A real Windows
    // loader overwrites these with function addresses — so does ours (trap VAs).
    std::vector<std::pair<uint32_t,uint32_t>> iatSpans;
    if(iat_sz) iatSpans.push_back({iat_rva,iat_rva+iat_sz});
    if(imp_rva){ long io=rva2raw(imp_rva); if(io>=0){ uint32_t p=(uint32_t)io; int guard=0;
        for(; guard<256; guard++){ if(p+20>f.size()) break; uint32_t oft=rd32(p),nm=rd32(p+12),ft=rd32(p+16);
            if(oft==0 && nm==0 && ft==0) break;
            long walk=rva2raw(oft?oft:ft); uint32_t cnt=0;
            if(walk>=0){ uint32_t wp=(uint32_t)walk; while(wp+4<=f.size() && rd32(wp)!=0 && cnt<8192){ cnt++; wp+=4; } }
            if(ft) iatSpans.push_back({ft, ft+cnt*4});
            p+=20; } } }
    auto isIAT=[&](uint32_t rva){ for(auto&s:iatSpans) if(rva>=s.first && rva<s.second) return true; return false; };
    // KNOWN D2Vita patch sites. Deliberately EMPTY: nothing on the shipped
    // path writes a byte of the guest image any more (every native hook goes
    // through a translation-time redirect). A differing byte must therefore
    // classify as UNKNOWN and be investigated, not excused.
    struct Patch{uint32_t rva,len;const char*what;};
    std::vector<Patch> patches = {};
    auto isPatch=[&](uint32_t rva)->const char*{ for(auto&p:patches) if(rva>=p.rva&&rva<p.rva+p.len) return p.what; return nullptr; };
    uint64_t cReloc=0,cImport=0,cRuntime=0,cHeader=0,cPatch=0,cUnknown=0; int shown=0;
    auto classify=[&](uint32_t rva,uint8_t db,uint8_t mb){
        for(int q=0;q<4;q++){ auto it=relocd.find(rva-q); if(it!=relocd.end()){
            long dr=rva2raw(rva-q); if(dr>=0){ uint32_t exp=rd32((size_t)dr)+(uint32_t)delta;
                if((uint8_t)(exp>>(8*q))==mb){ cReloc++; return; } } } }
        if(isIAT(rva)){ cImport++; return; }
        const char* pw=isPatch(rva);
        bool writable=false; for(auto&s:secs) if(rva>=s.rva&&rva<s.rva+std::max(s.vsz,s.rsz)){ writable=(s.ch&0x80000000u)!=0; break; }
        if(pw){ cPatch++; if(shown<24){ std::printf("  [audit] %-12s D2VITA_PATCH @RVA 0x%05x (%s): disk %02x -> mem %02x\n",name,rva,pw,db,mb); shown++; } return; }
        if(writable){ cRuntime++; return; }
        cUnknown++; if(shown<24){ std::printf("  [audit] %-12s UNKNOWN @0x%08x (RVA 0x%05x): disk %02x -> mem %02x\n",name,base+rva,rva,db,mb); shown++; }
    };
    // PE headers.
    { uint32_t n=std::min<uint32_t>(sizeHdrs,(uint32_t)f.size());
      if(n){ std::vector<uint8_t> mem(n); c.read(base,mem.data(),n);
        for(uint32_t i=0;i<n;i++) if(f[i]!=mem[i]) cHeader++; } }
    // Every section.
    for(auto&s:secs){ if(s.raw>=f.size()) continue; uint32_t n=std::min(s.vsz,s.rsz);
        if(s.raw+n>f.size()) n=(uint32_t)(f.size()-s.raw); if(!n) continue;
        std::vector<uint8_t> mem(n); c.read(base+s.rva,mem.data(),n);
        for(uint32_t i=0;i<n;i++){ if(f[s.raw+i]==mem[i]) continue; classify(s.rva+i,f[s.raw+i],mem[i]); }
    }
    std::printf("  [audit] %-12s : reloc=%llu import=%llu runtime=%llu header=%llu | D2VITA_PATCH=%llu UNKNOWN=%llu  %s\n",
        name,(unsigned long long)cReloc,(unsigned long long)cImport,(unsigned long long)cRuntime,(unsigned long long)cHeader,
        (unsigned long long)cPatch,(unsigned long long)cUnknown,
        (cPatch==0&&cUnknown==0)?"OK (no unexplained modification)":"<-- unexplained diffs present");
    return (int)(cPatch+cUnknown);
}
