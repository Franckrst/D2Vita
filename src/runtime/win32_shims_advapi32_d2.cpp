// win32_shims_advapi32_d2.cpp -- see win32_shims_advapi32_d2.h. Helpers
// cur_tib/set_lasterr/env_filelog/pc_dirty/misc/g_calls/g_traceOn are defined
// in rt_boot.cpp and exposed via runtime/rt_host.h.
#include "win32_shims_advapi32_d2.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>            // mkdir/chmod: the keystore is created with 0700/0600 permissions
using namespace d2rt;

// Paths of the two persistence files, exposed for the D2KEYSTORETEST
// self-test: it must be able to RE-READ them ON DISK rather than rely on
// in-memory state -- the file is what can leak, not the in-memory table.
static std::string g_advRegFile, g_advKeystoreFile;
const char* d2_registry_file() { return g_advRegFile.c_str(); }
const char* d2_keystore_file() { return g_advKeystoreFile.c_str(); }

void win32_shims_advapi32_d2_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto A=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("ADVAPI32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("ADVAPI32.dll",name,s);
    };
    // Real registry: a genuine 1.13c install has Software\Blizzard Entertainment\
    // Diablo II populated (InstallPath etc.). Without it D2 falls back to CD mode
    // and demands discs. PERSISTENT in-memory store: Bnclient records its gateway
    // list selection via RegSetValueExA and re-reads it — a write-discarding shim
    // makes it rebuild the list EVERY FRAME (2296 re-opens of Realms.bin/
    // gateways.txt, Storm heap ramps to 221 MB, SCOMP alloc dies "error 8").
    struct RegVal { uint32_t type; std::vector<uint8_t> data; std::string name; };
    static std::map<std::string,std::map<std::string,RegVal>> g_regKeys;  // keypath(lc) -> valname(lc) -> val
    static std::map<uint32_t,std::string> g_regH;                          // handle -> keypath(lc)
    static uint32_t g_regNextH=0x83000010u;
    // Persist everything the game writes to the registry (settings, gateway
    // lists, and — once entered in D2's own key dialog — the CD keys, in
    // whatever encoding the game itself uses). One line per value:
    // keypath|valname|origname|type|hex. Rewritten on every RegSet/RegDelete
    // (rare) and fclose'd immediately (Vita: writes only durable on close).
    // ---- THE CD KEY DOES NOT LIVE IN registry.txt --------------------------
    // The key's encoded blob (HKCU\Software\Battle.net, REG_BINARY value) is
    // kept out of $D2WRITE/registry.txt: on console that file sits alongside
    // saves, network logs, and frame dumps -- everything an FTP pull grabs and
    // that ends up pasted into a report. Any routine copy of registry.txt
    // would otherwise silently exfiltrate the key.
    //
    // These values go instead to a SEPARATE file, OUTSIDE the write root, that
    // no pull script visits and that .gitignore covers: $D2SECRET if set,
    // otherwise ux0:data/d2vita_secret/keystore.bin on console,
    // <parent of $D2WRITE>/d2vita_secret/keystore.bin elsewhere.
    // Never logs content, length, or value name: at most present / absent / written.
    static std::string g_keystoreFile;
    static auto reg_is_secret=[](const std::string& keypath,const std::string& valname)->bool{
        // Store keys are already lowercased. Only one value name exists in
        // the 1.14d binary, but the match stays broad: if the name were
        // suffixed at runtime, the value must not fall back into
        // registry.txt by surprise.
        return keypath.find("battle.net")!=std::string::npos
            && valname.find("key")!=std::string::npos; };
    static std::string g_regFile;   // set once g_writeRoot is known
    static auto regSave=[](){ if(g_regFile.empty()) return;
        FILE* f=std::fopen(g_regFile.c_str(),"w"); pc_dirty(g_writeRoot); if(!f) return;
        FILE* ks=nullptr;
        for(auto& k:g_regKeys) for(auto& v:k.second){
            FILE* out=f;
            if(reg_is_secret(k.first,v.first)){
                if(!ks && !g_keystoreFile.empty()) ks=std::fopen(g_keystoreFile.c_str(),"w");
                if(!ks){   // no fallback to registry.txt: better to lose the value than leak it
                    static int cried=0;   // but we DO report it, so the key doesn't vanish silently
                    if(!cried++) jpline("keystore: ECRITURE IMPOSSIBLE — la valeur n'est PAS persistee");
                    continue; }
                out=ks; }
            std::fprintf(out,"%s|%s|%s|%u|",k.first.c_str(),v.first.c_str(),v.second.name.c_str(),v.second.type);
            for(uint8_t b:v.second.data) std::fprintf(out,"%02x",b);
            std::fprintf(out,"\n"); }
        std::fclose(f);
        if(ks){ std::fclose(ks); ::chmod(g_keystoreFile.c_str(),0600);
            static int said=0; if(!said++) jpline("keystore: ecrit (present)"); } };
    static auto regLoad=[](){ if(g_regFile.empty()) return;
        FILE* f=std::fopen(g_regFile.c_str(),"r"); if(!f) return; char line[2048]; int n=0;
        while(std::fgets(line,sizeof line,f)){
            char* nl=std::strpbrk(line,"\r\n"); if(nl)*nl=0;
            char* p1=std::strchr(line,'|'); if(!p1) continue; *p1++=0;
            char* p2=std::strchr(p1,'|');   if(!p2) continue; *p2++=0;
            char* p3=std::strchr(p2,'|');   if(!p3) continue; *p3++=0;
            char* p4=std::strchr(p3,'|');   if(!p4) continue; *p4++=0;
            RegVal v; v.name=p2; v.type=(uint32_t)std::strtoul(p3,nullptr,10);
            for(char* h=p4; h[0]&&h[1]; h+=2){ char b[3]={h[0],h[1],0};
                v.data.push_back((uint8_t)std::strtoul(b,nullptr,16)); }
            g_regKeys[line][p1]=std::move(v); ++n; }
        std::fclose(f);
        if(n) std::printf("    [reg] %d persisted value(s) loaded from %s\n",n,g_regFile.c_str()); };
    // The keystore is parsed by the SAME reader, from a separate file. It
    // never reports anything beyond present / absent.
    static auto ksLoad=[](){ if(g_keystoreFile.empty()) return;
        FILE* f=std::fopen(g_keystoreFile.c_str(),"r");
        if(!f){ jpline("keystore: absent"); return; }
        char line[4096]; int n=0;
        while(std::fgets(line,sizeof line,f)){
            char* nl=std::strpbrk(line,"\r\n"); if(nl)*nl=0;
            char* p1=std::strchr(line,'|'); if(!p1) continue; *p1++=0;
            char* p2=std::strchr(p1,'|');   if(!p2) continue; *p2++=0;
            char* p3=std::strchr(p2,'|');   if(!p3) continue; *p3++=0;
            char* p4=std::strchr(p3,'|');   if(!p4) continue; *p4++=0;
            RegVal v; v.name=p2; v.type=(uint32_t)std::strtoul(p3,nullptr,10);
            for(char* h=p4; h[0]&&h[1]; h+=2){ char b[3]={h[0],h[1],0};
                v.data.push_back((uint8_t)std::strtoul(b,nullptr,16)); }
            g_regKeys[line][p1]=std::move(v); ++n; }
        std::fclose(f);
        (void)n; jpline("keystore: present"); };
    g_regFile = g_writeRoot + "/registry.txt";
    {   // Keystore directory: OUTSIDE the write root, so outside the path that
        // tools/vita_drive.sh and push_console.sh visit.
        const char* sec=getenv("D2SECRET");
        std::string dir;
        if(sec && *sec) dir=sec;
#ifdef __vita__
        // EXPLICIT path on console. Deriving it from D2WRITE
        // (ux0:data/d2vita/save*) would give ux0:data/d2vita/d2vita_secret,
        // i.e. INSIDE the game tree -- exactly what this avoids.
        else dir = "ux0:data/d2vita_secret";
#else
        else { std::string w=g_writeRoot; size_t s=w.find_last_of("/\\");
               std::string parent = (s==std::string::npos)? std::string(".") : w.substr(0,s);
               if(parent.empty()) parent="/";
               dir = parent + "/d2vita_secret"; }
#endif
        ::mkdir(dir.c_str(),0700);
        g_keystoreFile = dir + "/keystore.bin";
        g_advRegFile = g_regFile; g_advKeystoreFile = g_keystoreFile; }
    regLoad();
    {   // MIGRATION: a key already sitting in registry.txt (pre-existing
        // installs) must be moved OUT on first startup, or only fresh
        // installs would be protected.
        bool mig=false;
        for(auto& k:g_regKeys) for(auto& v:k.second) if(reg_is_secret(k.first,v.first)) mig=true;
        if(mig){ regSave(); jpline("keystore: migration depuis registry.txt"); } }
    ksLoad();
    // Confirms the perspective knob is armed (on Vita, printf proves nothing:
    // jpline goes through d2vita_progress).
    if(const char* pe=getenv("D2_PERSPECTIVE"))
        if(*pe) jpline("registre: option video Perspective FORCEE a %d (D2_PERSPECTIVE) — ni lue ni ecrite dans registry.txt", atoi(pe)!=0);
    static auto lcs=[](std::string s){ for(auto&ch:s) ch=(char)std::tolower((unsigned char)ch); return s; };
    static const std::string D2KEY="software\\blizzard entertainment\\diablo ii";
    g_regH[0x83000001u]=D2KEY; g_regKeys[D2KEY];
    static auto regPath=[](uint32_t h,const std::string& sub)->std::string{
        std::string base; auto it=g_regH.find(h); if(it!=g_regH.end()) base=it->second;
        std::string s=lcs(sub);
        if(base.empty()) return s;
        if(s.empty())    return base;
        return base+"\\"+s; };
    static auto regHandle=[](const std::string& path)->uint32_t{
        if(path==D2KEY) return 0x83000001u;
        uint32_t h=g_regNextH++; g_regH[h]=path; return h; };
    auto regOpen=[](Cpu&c,uint32_t hRoot,uint32_t pName,uint32_t pOut)->uint32_t{
        std::string k=pName?gread_mb(c,pName,-1):"";
        std::string full=regPath(hRoot,k);
        if(env_filelog()) std::fprintf(stderr,"    [RegOpen] %s\n",full.c_str());
        if(g_regKeys.count(full)||full==D2KEY){ if(pOut) c.write_u32(pOut,regHandle(full)); return 0u; }
        return 2u; };   // ERROR_FILE_NOT_FOUND -> callers use defaults
    A("RegOpenKeyExA",5,[regOpen](Cpu&c){ return regOpen(c,c.arg(0),c.arg(1),c.arg(4)); });
    A("RegOpenKeyA",3,[regOpen](Cpu&c){ return regOpen(c,c.arg(0),c.arg(1),c.arg(2)); });
    auto regCreate=[](Cpu&c,uint32_t hRoot,uint32_t pName,uint32_t pOut,uint32_t pDisp)->uint32_t{
        std::string full=regPath(hRoot,pName?gread_mb(c,pName,-1):"");
        uint32_t disp=g_regKeys.count(full)?2u:1u;   // REG_OPENED_EXISTING / REG_CREATED_NEW
        g_regKeys[full];
        if(pOut) c.write_u32(pOut,regHandle(full));
        if(pDisp) c.write_u32(pDisp,disp); return 0u; };
    A("RegCreateKeyExA",9,[regCreate](Cpu&c){ return regCreate(c,c.arg(0),c.arg(1),c.arg(7),c.arg(8)); });
    A("RegCreateKeyA",3,[regCreate](Cpu&c){ return regCreate(c,c.arg(0),c.arg(1),c.arg(2),0); });
    A("RegQueryValueA",4,[](Cpu&){ return 2u; });
    A("RegEnumValueA",8,[](Cpu&c){ auto hit=g_regH.find(c.arg(0)); if(hit==g_regH.end()) return 259u;
        auto kit=g_regKeys.find(hit->second); if(kit==g_regKeys.end()) return 259u;
        uint32_t idx=c.arg(1); if(idx>=kit->second.size()) return 259u;   // ERROR_NO_MORE_ITEMS
        auto vit=kit->second.begin(); std::advance(vit,idx); RegVal& v=vit->second;
        if(c.arg(2)&&c.arg(3)){ uint32_t cap=c.read_u32(c.arg(3));
            uint32_t n=(uint32_t)v.name.size(); if(n+1>cap) return 234u;   // ERROR_MORE_DATA
            c.write(c.arg(2),v.name.c_str(),n+1); c.write_u32(c.arg(3),n); }
        if(c.arg(5)) c.write_u32(c.arg(5),v.type);
        if(c.arg(7)){ if(c.arg(6)&&c.read_u32(c.arg(7))>=v.data.size()&&!v.data.empty())
                          c.write(c.arg(6),v.data.data(),(uint32_t)v.data.size());
                      c.write_u32(c.arg(7),(uint32_t)v.data.size()); }
        return 0u; });
    A("RegDeleteValueA",2,[](Cpu&c){ auto hit=g_regH.find(c.arg(0)); if(hit==g_regH.end()) return 2u;
        auto kit=g_regKeys.find(hit->second); if(kit==g_regKeys.end()) return 2u;
        kit->second.erase(lcs(gread_mb(c,c.arg(1),-1))); regSave(); return 0u; });
    A("RegFlushKey",1,[](Cpu&){ return 0u; });
    A("RegDeleteKeyA",2,[](Cpu&c){ g_regKeys.erase(regPath(c.arg(0),gread_mb(c,c.arg(1),-1))); regSave(); return 0u; });
    A("RegOpenKeyW",3,[](Cpu&){ return 2u; });
    A("RegNotifyChangeKeyValue",5,[](Cpu&){ return 0u; });
    A("RegQueryValueExA",6,[](Cpu&c){
        std::string name=gread_mb(c,c.arg(1),-1);
        if(env_filelog()) std::fprintf(stderr,"    [RegQueryValueExA] h=0x%08x %s\n",c.arg(0),name.c_str());
        // 0) D2_PERSPECTIVE=0|1 -- forces D2's "Perspective" video option.
        //
        // WHY THIS KNOB EXISTS: the "Perspective" value is ABSENT from
        // tools/registry_console.txt. The game's reader (Game+0x14f10, called
        // from Game+0x7d194 with &object[0x124] as output) does NOT write its
        // output when the lookup fails: the field keeps whatever it already
        // held, and the game then PERSISTS that value (Game+0x7d1b1). On both
        // our benches and on console this lands on **Perspective=1**, which
        // nobody chose. The knob makes the choice DETERMINISTIC and
        // replayable for A/B testing. Absent = nothing changes.
        if(name=="Perspective"){ const char* e=getenv("D2_PERSPECTIVE");
            if(e&&*e){ const uint32_t v=(uint32_t)(atoi(e)!=0);
                if(c.arg(4)) c.write_u32(c.arg(4),v);
                if(c.arg(5)) c.write_u32(c.arg(5),4);
                if(c.arg(3)) c.write_u32(c.arg(3),4/*REG_DWORD*/);
                return 0u; } }
        // 1) the persistent store (anything the game itself wrote)
        auto hit=g_regH.find(c.arg(0));
        if(hit!=g_regH.end()){ auto kit=g_regKeys.find(hit->second);
            if(kit!=g_regKeys.end()){ auto vit=kit->second.find(lcs(name));
                if(vit!=kit->second.end()){ RegVal& v=vit->second;
                    if(c.arg(3)) c.write_u32(c.arg(3),v.type);
                    if(c.arg(4)&&c.arg(5)){ if(c.read_u32(c.arg(5))<v.data.size()){ c.write_u32(c.arg(5),(uint32_t)v.data.size()); return 234u; }
                        if(!v.data.empty()) c.write(c.arg(4),v.data.data(),(uint32_t)v.data.size()); }
                    if(c.arg(5)) c.write_u32(c.arg(5),(uint32_t)v.data.size());
                    return 0u; } } }
        // 2) install defaults on the Diablo II key
        if(c.arg(0)!=0x83000001u) return 2u;
        auto puts=[&](const char* v){ uint32_t n=(uint32_t)std::strlen(v)+1;
            if(c.arg(4)&&c.arg(5)&&c.read_u32(c.arg(5))>=n) c.write(c.arg(4),v,n);
            if(c.arg(5)) c.write_u32(c.arg(5),n); if(c.arg(3)) c.write_u32(c.arg(3),1/*REG_SZ*/); return 0u; };
        auto putd=[&](uint32_t v){ if(c.arg(4)) c.write_u32(c.arg(4),v);
            if(c.arg(5)) c.write_u32(c.arg(5),4); if(c.arg(3)) c.write_u32(c.arg(3),4/*REG_DWORD*/); return 0u; };
        if(name=="InstallPath")   return puts("C:\\Diablo II\\");
        if(name=="Save Path")     return puts("C:\\Diablo II\\Save\\");
        if(name=="NewSavePath")   return puts("C:\\Diablo II\\Save\\");
        if(name=="GamePath")      return puts("C:\\Diablo II\\Game.exe");
        if(name=="CmdLine")       return puts("");
        if(name=="InstallType")   return putd(3);   // full install (all files local)
        if(name=="Cinematics")    return putd(1);   // installed locally
        if(name=="Render")        return putd(0);   // renderer: 0 = GDI (D2Gdi.dll)
        return 2u; });
    A("RegSetValueExA",6,[](Cpu&c){ auto hit=g_regH.find(c.arg(0)); if(hit==g_regH.end()) return 2u;
        std::string name=gread_mb(c,c.arg(1),-1);
        // Under D2_PERSPECTIVE, the value must not be WRITTEN to
        // registry.txt: otherwise the next leg of an A/B run would inherit
        // the previous one and the control would no longer be a control.
        if(name=="Perspective"){ const char* e=getenv("D2_PERSPECTIVE"); if(e&&*e) return 0u; }
        uint32_t ty=c.arg(3),pd=c.arg(4),n=c.arg(5);
        RegVal v; v.type=ty; v.name=name; v.data.resize(n); if(n&&pd) c.read(pd,v.data.data(),n);
        if(env_filelog()) std::fprintf(stderr,"    [RegSetValueExA] %s\\%s type=%u len=%u\n",hit->second.c_str(),name.c_str(),ty,n);
        g_regKeys[hit->second][lcs(name)]=std::move(v); regSave(); return 0u; });
    A("RegCloseKey",1,[](Cpu&){ return 0u; });
    // Service Control Manager — D2's copy-protection/driver probe. Fail cleanly
    // (access denied) so the game skips the driver check.
    // Security/token API (D2Game realm-startup admin check, via dynamic
    // LoadLibrary("advapi32")+GetProcAddress): report "running as admin".
    // (the stateless subset with no dependency on misc()/set_lasterr() moved
    // to winx86 -- see win32_shims_advapi32.cpp there. What remains here are
    // the functions that rely on misc()/set_lasterr(), both hosted by d2vita.)
    A("AllocateAndInitializeSid",11,[](Cpu&c){ uint32_t out=c.arg(10);
        uint32_t sid=misc(12); c.write_u32(sid,0x00000101u);   // fake SID blob
        if(out) c.write_u32(out,sid); return 1u; });
    A("GetSidSubAuthority",2,[](Cpu&c){ static uint32_t sa=0; if(!sa) sa=misc(4); c.write_u32(sa,0); return sa; });
    A("GetSidSubAuthorityCount",1,[](Cpu&c){ static uint32_t sc=0; if(!sc) sc=misc(4); { uint8_t one=1; c.write(sc,&one,1);} return sc; });
    A("SetEntriesInAclA",4,[](Cpu&c){ if(c.arg(3)) c.write_u32(c.arg(3),misc(16)); return 0u; });  // ERROR_SUCCESS
    A("OpenSCManagerA",3,[](Cpu&c){ set_lasterr(c,5); return 0u; });
    A("OpenServiceA",3,[](Cpu&c){ set_lasterr(c,5); return 0u; });
    A("CreateServiceA",13,[](Cpu&c){ set_lasterr(c,5); return 0u; });
}
