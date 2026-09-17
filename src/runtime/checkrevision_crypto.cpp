// checkrevision_crypto.cpp -- see checkrevision_crypto.h.
#include "checkrevision_crypto.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/win32_shims_wsock32.h"  // wx86_net_enabled/wx86_net_set_enabled
#include "runtime/win32_shims_wintrust.h" // wx86_wintrust_set_file_source/_observer/_clock
#include "runtime/win32_shims_version.h"  // win32_shims_version_install
#include "runtime/path_cache.h"          // host_path()
#include "platform/vita_present.h"       // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
using namespace d2rt;

// --- SHA-1 (streaming) for the ADVAPI32 CryptoAPI hash that the lockdown
// CheckRevision.dll computes over the game files (CALG_SHA1). A real, faithful
// hash — the DLL's genuine version-check value, computed by its own code path.
struct Sha1 {
    uint32_t h[5]; uint64_t total; uint8_t buf[64]; uint32_t bufn;
    static uint32_t rol(uint32_t v,int b){ return (v<<b)|(v>>(32-b)); }
    void init(){ h[0]=0x67452301u;h[1]=0xEFCDAB89u;h[2]=0x98BADCFEu;h[3]=0x10325476u;h[4]=0xC3D2E1F0u; total=0; bufn=0; }
    void block(const uint8_t* p){
        uint32_t w[80];
        for(int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;i++){ uint32_t f,k;
            if(i<20){f=(b&c)|((~b)&d);k=0x5A827999u;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1u;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDCu;}
            else {f=b^c^d;k=0xCA62C1D6u;}
            uint32_t t=rol(a,5)+f+e+k+w[i]; e=d;d=c;c=rol(b,30);b=a;a=t; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    void update(const uint8_t* p,uint32_t n){ total+=n;
        while(n){ uint32_t take=64-bufn; if(take>n)take=n; std::memcpy(buf+bufn,p,take); bufn+=take;p+=take;n-=take;
            if(bufn==64){block(buf);bufn=0;} } }
    void finish(uint8_t out[20]){ uint64_t bits=total*8;
        buf[bufn++]=0x80; if(bufn>56){ while(bufn<64) buf[bufn++]=0; block(buf); bufn=0; }
        while(bufn<56) buf[bufn++]=0;
        for(int i=0;i<8;i++) buf[56+i]=(uint8_t)(bits>>(56-i*8)); block(buf);
        for(int i=0;i<5;i++){ out[i*4]=(uint8_t)(h[i]>>24);out[i*4+1]=(uint8_t)(h[i]>>16);out[i*4+2]=(uint8_t)(h[i]>>8);out[i*4+3]=(uint8_t)h[i]; }
    }
};
static std::map<uint32_t,Sha1> g_cryptHashes;      // HCRYPTHASH -> streaming SHA-1
static uint32_t g_cryptHashNext=0xC0DE0001u;
// Version string ACTUALLY hashed by CheckRevision (":1.14.3.71:" expected;
// ":0.0.0.0:" means host_path failed). Captured at the CryptHashData entry —
// see the shim, which explains why this capture exists.
static int g_crAuthByte=-1;   // Authenticode byte hashed by CheckRevision (-1 = not seen yet)

void checkrevision_crypto_install(Bridge& br){
    // --- CRYPT32 / WINTRUST / VERSION: imported by the lockdown CheckRevision.dll
    // (Authenticode + version anti-tamper). Correct argc keeps ESP balanced
    // (stdcall); benign returns let the hash path run. We control the BNCS server
    // so the checkrevision VALUE is irrelevant — only that the DLL runs to
    // completion and returns. Tuned iteratively against the real DLL.
    { auto SH=[&](const char* dll,const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string(dll)+"!"+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        s.fn=[fn,tag](Cpu&c){ uint32_t r=fn(c);
            if(getenv("CRYPTLOG")) std::fprintf(stderr,"    [crypto] %s(a0=0x%x a1=0x%x) -> 0x%x\n",tag.c_str(),c.arg(0),c.arg(1),r);
            return r; };
        br.register_shim(dll,name,s); };
      // AUTHENTICODE TRUST CHAIN: no constant response here at all.
      // CryptQueryObject, CryptMsgGetParam, CryptMsgClose,
      // CertFindCertificateInStore, CertGetNameStringW, CertFreeCertificateContext,
      // CertCloseStore and WinVerifyTrust are registered by the engine
      // (winx86 win32_shims_wintrust.cpp), which REALLY verifies: PE digest,
      // RSA signatures, timestamp, chain up to a trusted root, dates.
      // Wired in right after win32_shims_version_install() below.
      // Game.exe itself NEVER verifies its own signature — the gate at
      // 0x516de1 serves SystemSurvey.exe, CheckRevision.dll and BnDownload.
      // CheckRevision.dll converts its SHA-1 digest to a string with this (the
      // version-check result it hands back). The real checkrevision (0x100021eb)
      // REQUIRES success (test eax; je fail) — a 0 stub made it return failure so
      // D2 never sent SID_AUTH_CHECK. Implemented for real (binary -> base64/hex
      // wide string, honouring the dwFlags line-ending bits).
      SH("CRYPT32.dll","CryptBinaryToStringW",5,[](Cpu&c)->uint32_t{
          uint32_t pb=c.arg(0),cb=c.arg(1),flags=c.arg(2),psz=c.arg(3),pcch=c.arg(4);
          std::vector<uint8_t> in(cb); if(cb) c.read(pb,in.data(),cb);
          static const char* B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
          std::string s; uint32_t fmt=flags&0xff;
          if(fmt==4||fmt==5||fmt==0xa||fmt==0xb||fmt==0xc){ static const char* H="0123456789abcdef";
              for(uint8_t b:in){ s+=H[b>>4]; s+=H[b&0xf]; } }                         // hex
          else { for(size_t i=0;i<in.size();i+=3){ size_t n=in.size()-i; uint32_t v=(uint32_t)in[i]<<16;
              if(n>1) v|=(uint32_t)in[i+1]<<8; if(n>2) v|=in[i+2];
              s+=B[(v>>18)&63]; s+=B[(v>>12)&63]; s+= n>1?B[(v>>6)&63]:'='; s+= n>2?B[v&63]:'='; } }  // base64
          if(!(flags&0x40000000)){ if(!(flags&0x80000000)) s+='\r'; s+='\n'; }        // CRLF unless NOCRLF; NOCR drops CR
          uint32_t need=(uint32_t)s.size()+1;                                          // incl NUL
          if(!psz){ if(pcch) c.write_u32(pcch,need); return 1u; }                      // size query
          uint32_t cap=pcch?c.read_u32(pcch):need, w=(uint32_t)s.size(); if(w+1>cap) w=cap?cap-1:0;
          for(uint32_t i=0;i<w;i++) gwrite_wc(c,psz+2*i,(uint16_t)(uint8_t)s[i]);
          gwrite_wc(c,psz+2*w,0); if(pcch) c.write_u32(pcch,w);                        // chars written excl NUL
          if(getenv("CRYPTLOG")) std::fprintf(stderr,"    [crypto] CryptBinaryToStringW %u B -> \"%.16s…\" (%u chars)\n",cb,s.c_str(),w);
          return 1u; });
      // CheckRevision.dll base64-decodes the lockdown seed (the SID_AUTH_INFO
      // "formula") via CryptStringToBinaryW. Implement it for real so the DLL
      // completes and D2 sends SID_AUTH_CHECK.
      SH("CRYPT32.dll","CryptStringToBinaryW",7,[](Cpu&c)->uint32_t{
          uint32_t pStr=c.arg(0),cch=c.arg(1),flags=c.arg(2),pBin=c.arg(3),pcbBin=c.arg(4),pSkip=c.arg(5),pFlags=c.arg(6);
          std::string s; { auto w=gread_wc(c,pStr,cch?(int)cch:-1);
              for(uint16_t ch:w){ if(!cch && !ch) break; s+=(char)(ch&0xff); } }
          static const char* B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
          std::vector<uint8_t> out; int val=0,bits=0;
          for(char ch:s){ if(ch=='=') break; const char* p=ch?std::strchr(B,ch):nullptr; if(!p) continue;
              val=(val<<6)|(int)(p-B); bits+=6; if(bits>=8){ bits-=8; out.push_back((uint8_t)((val>>bits)&0xff)); } }
          uint32_t need=(uint32_t)out.size();
          if(pSkip) c.write_u32(pSkip,0); if(pFlags) c.write_u32(pFlags,flags);
          if(!pBin){ if(pcbBin) c.write_u32(pcbBin,need); return 1u; }
          uint32_t cap=pcbBin?c.read_u32(pcbBin):need, w=need<cap?need:cap;
          if(w) c.write(pBin,out.data(),w); if(pcbBin) c.write_u32(pcbBin,w);
          return 1u; });
      // ADVAPI32 CryptoAPI: the lockdown CheckRevision.dll hashes the game files
      // with SHA-1 (CryptAcquireContext -> CryptCreateHash(CALG_SHA1=0x8004) ->
      // CryptHashData* -> CryptGetHashParam(HP_HASHVAL)). Unshimmed, these drift
      // ESP (stdcall args left on the stack) and clobber the GS cookie, so the
      // DLL fastfails (0xC0000409, __report_gsfailure). Implemented for real so
      // the genuine version-check hash computes and D2 sends SID_AUTH_CHECK.
      SH("ADVAPI32.dll","CryptAcquireContextW",5,[](Cpu&c){ uint32_t pph=c.arg(0);
          if(pph) c.write_u32(pph,0x50000001u); return 1u; });
      SH("ADVAPI32.dll","CryptReleaseContext",2,[](Cpu&){ return 1u; });
      SH("ADVAPI32.dll","CryptCreateHash",5,[](Cpu&c){ uint32_t algid=c.arg(1),pph=c.arg(4);
          uint32_t hh=g_cryptHashNext++; g_cryptHashes[hh].init();
          if(getenv("CRYPTLOG")) std::fprintf(stderr,"    [crypto] CryptCreateHash algid=0x%x -> h=0x%x\n",algid,hh);
          if(pph) c.write_u32(pph,hh); return 1u; });
      SH("ADVAPI32.dll","CryptHashData",4,[](Cpu&c){ uint32_t hh=c.arg(0),p=c.arg(1),n=c.arg(2);
          auto it=g_cryptHashes.find(hh); if(it==g_cryptHashes.end()) return 0u;
          if(n){ std::vector<uint8_t> b(n); c.read(p,b.data(),n); it->second.update(b.data(),n);
              // ---- THE CONSOLE TRAP ----------------------------------------
              // The message hashed by CheckRevision 1.14d is
              //   LE32(seed) || ":<a>.<b>.<c>.<d>:" || byte(authenticode)
              // The four-part version comes from GetModuleFileNameW(NULL) ->
              // GetFileVersionInfoSizeW -> VS_FIXEDFILEINFO. If host_path
              // ("C:\\Diablo II\\game.exe") fails on console (data
              // directory, case, path index), build_verinfo returns an EMPTY
              // buffer, GetFileVersionInfoSizeW returns 0, and the DLL hashes
              // the literal fallback ":0.0.0.0:" WITH NO VISIBLE ERROR AT
              // ALL. No test caught this. So the string ACTUALLY hashed is
              // extracted and REPORTED — via d2vita_progress, because a
              // printf on Vita proves nothing.
              // ⚠️ A naive extraction that stops at the first "non-digit"
              // byte can be fooled by a SEED byte that happens to look like a
              // digit (0x30-0x39, ':' or '.'), never finding the real
              // string — which would leave the D2CR_REQUIRE_VERSION gate
              // below OPEN silently. This searches for the
              // ":<digits/dots>:" pattern anywhere instead.
              { std::string txt;
                for(uint32_t i=0;i<n && i<64 && txt.empty();i++){ if(b[i]!=':') continue;
                    uint32_t j=i+1; while(j<n && j<64 && (b[j]=='.'||(b[j]>='0'&&b[j]<='9'))) j++;
                    if(j<n && j<64 && b[j]==':' && j>i+1 && std::memchr(&b[i+1],'.',j-i-1)){
                        txt.assign((const char*)&b[i], j-i+1);
                        g_crAuthByte = (j+1<n) ? (int)b[j+1] : -1; } }
                size_t c1=txt.find(':');
                if(c1!=std::string::npos){ size_t c2=txt.find(':',c1+1);
                  if(c2!=std::string::npos && c2>c1+1){
                    std::string ver=txt.substr(c1,c2-c1+1);
                    if(ver!=g_crHashedVer){ g_crHashedVer=ver;
                      // The byte following the version is the AUTHENTICODE byte (1 =
                      // game.exe and the DLL verified, pinned keys matching, as on a real PC).
                      char m[96]; std::snprintf(m,sizeof m,"checkrevision: chaine hachee %s auth=%d",ver.c_str(),g_crAuthByte);
                      d2vita_progress(m); std::fprintf(stderr,"    [checkrevision] chaine hachee = %s\n",ver.c_str());
                      // ARMABLE guard: if the string isn't the one required,
                      // NOTHING goes out anymore. Better not to connect than
                      // to connect with a wrong computation. It compares our
                      // own reading against our own expectation: it catches a
                      // path accident, not a design error.
                      const char* req=getenv("D2CR_REQUIRE_VERSION");
                      if(req && *req){ std::string want=std::string(":")+req+":";
                        if(ver!=want){ wx86_net_set_enabled(false);
                          std::fprintf(stderr,"    [checkrevision] ATTENDU %s — RESEAU COUPE (D2CR_REQUIRE_VERSION)\n",want.c_str());
                          d2vita_progress("checkrevision: VERSION HACHEE FAUSSE — reseau coupe"); } } } } } }
              if(getenv("CRYPTLOG")){ std::fprintf(stderr,"    [crypto] CryptHashData h=0x%x len=%u src=0x%08x  bytes:",hh,n,p);
                  for(uint32_t i=0;i<n && i<32;i++) std::fprintf(stderr," %02x",b[i]); std::fprintf(stderr,"%s\n",n>32?" …":""); } }
          return 1u; });
      SH("ADVAPI32.dll","CryptGetHashParam",5,[](Cpu&c){ uint32_t hh=c.arg(0),param=c.arg(1),pb=c.arg(2),pcb=c.arg(3);
          auto it=g_cryptHashes.find(hh); if(it==g_cryptHashes.end()) return 0u;
          if(param==4){ if(pb) c.write_u32(pb,20); if(pcb) c.write_u32(pcb,4); return 1u; }   // HP_HASHSIZE=20
          if(param==2){                                                                       // HP_HASHVAL
              // NET CLOSED BY DEFAULT: if D2CR_REQUIRE_VERSION is set and NO
              // matching string has been seen before the DLL reads its
              // digest, the network is cut here — before SID_AUTH_CHECK is
              // sent. An unexpected message format can no longer leave the
              // gate open. (Game.exe imports no hashing API at all: only
              // CheckRevision.dll goes through this shim.)
              if(const char* req=getenv("D2CR_REQUIRE_VERSION")){ if(*req && g_crHashedVer!=std::string(":")+req+":" && wx86_net_enabled()){
                  wx86_net_set_enabled(false);
                  d2vita_progress("checkrevision: chaine hachee NON VERIFIEE au moment du condensat — reseau coupe (D2CR_REQUIRE_VERSION)"); } }
              if(pcb && !pb){ c.write_u32(pcb,20); return 1u; }                                // size query
              uint8_t dig[20]; Sha1 tmp=it->second; tmp.finish(dig);
              uint32_t cap=pcb?c.read_u32(pcb):20, w=20<cap?20:cap;
              if(pb&&w) c.write(pb,dig,w); if(pcb) c.write_u32(pcb,w);
              if(getenv("CRYPTLOG")) std::fprintf(stderr,"    [crypto] SHA1 h=0x%x -> %02x%02x%02x%02x… (%u B)\n",hh,dig[0],dig[1],dig[2],dig[3],w);
              return 1u; }
          return 1u; });
      SH("ADVAPI32.dll","CryptDestroyHash",1,[](Cpu&c){ g_cryptHashes.erase(c.arg(0)); return 1u; });
      // VERSION.dll (GetFileVersionInfo*/VerQueryValueW) -> winx86, generic:
      // checkrevision "3a" hashes the EXE's version string resource; making
      // that resource real makes our hash identical to a real Windows
      // client's. The only D2-specific side (guest filename -> real bytes)
      // stays here via the Bridge::set_version_resource_source extension point.
      br.set_version_resource_source([](const std::string& guestFileName) -> std::vector<uint8_t> {
          return slurp(host_path(guestFileName)); });
      win32_shims_version_install(br);
      // Real Authenticode (winx86). Byte source: same path resolution as
      // VERSION; "absent" and "empty" must not be confused (a missing file
      // returns the real Windows file-not-found error).
      wx86_wintrust_set_file_source([](const std::string& g, std::vector<uint8_t>& out)->bool{
          const std::string h=host_path(g); FILE* f=std::fopen(h.c_str(),"rb"); if(!f) return false;
          std::fclose(f); out=slurp(h); return true; });
      wx86_wintrust_set_observer([](const char* l){
          static int n=0; if(n<24){ ++n; char m[200]; std::snprintf(m,sizeof m,"authenticode: %s",l); d2vita_progress(m); }
          if(getenv("CRYPTLOG")) std::fprintf(stderr,"    [crypto] %s\n",l); });
      // Real clock for the trust chain (evaluation date for certs/roots when
      // no timestamp accompanies the signature). Without this call g_clock
      // stayed null on the winx86 side and opt.now was always 0 (1970) — this
      // went unnoticed because Game.exe carries a 2016 timestamp (T = the
      // timestamp's own date, never opt.now). rt_wall() is the same wall
      // clock as the rest of the engine (Vita RTC, freezable via
      // D2_FAKEWALL for deterministic A/Bs).
      wx86_wintrust_set_clock([]()->int64_t{ return (int64_t)rt_wall(); });
      win32_shims_wintrust_install(br); }
}
