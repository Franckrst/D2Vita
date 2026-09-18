// kernel32_modules.cpp -- see kernel32_modules.h.
//
// GetModuleHandleA/W and GetModuleHandleExA/W independently duplicate the
// same "resolve a DLL name to a handle" logic (real Windows does too -- these
// are genuinely separate entry points, not thin wrappers on one another).
// LoadLibraryA/LoadLibraryExA/LoadLibraryW literally share one local helper
// (loadlib) -- including the runtime PE-load path for the lockdown
// CheckRevision.dll (map + relocate + resolve imports + run DllMain under a
// loader-lock scheduler pin). None of that trust/crypto DECISION making lives
// here -- only the mechanism to map and run a DLL once LoadLibrary is asked
// for it; the CryptoAPI/Authenticode shims the loaded DLL then calls stay in
// tools/rt_boot.cpp for their own dedicated pass.
#include "kernel32_modules.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/pe_image.h"        // full d2rt::PeImage (load_base/export_va/image_size/entry_va/imports)
#include "runtime/guest_thread.h"    // full d2rt::ThreadScheduler (g_sched->set_no_preempt)
#include "runtime/path_cache.h"      // host_path()
#include "platform/vita_present.h"   // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

// Loader-lock pin trap (see loadlib() below): set once, the first time a
// runtime-loaded DLL's DllMain needs to run under the pin.
static uint32_t g_dllmainDoneTrap=0;

void kernel32_modules_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    K("GetProcAddress",2,[&br](Cpu&c){
        uint32_t pn=c.arg(1);
        auto sl=g_sysLib.find(c.arg(0));
        if(sl!=g_sysLib.end()){                     // system dll pseudo-handle -> shim trap
            if(pn<0x10000) return 0u;
            std::string nm=gread_mb(c,pn,-1);
            uint32_t va=br.shim_trap(sl->second,nm);
            if(env_filelog()) std::fprintf(stderr,"    [GetProcAddress SYS] %s!%s -> 0x%08x\n",sl->second.c_str(),nm.c_str(),va);
            if(!va) set_lasterr(c,127);                // ERROR_PROC_NOT_FOUND
            return va; }
        auto it=g_modByBase.find(c.arg(0)); if(it==g_modByBase.end()){ set_lasterr(c,126); return 0u; } // ERROR_MOD_NOT_FOUND
        // Real export table -> va = load_base + rva, lands in the module's mapped
        // .text (honest). A real miss stays a miss + ERROR_PROC_NOT_FOUND — never
        // fall back to a shim trap (a trap VA lies outside [base,base+size) and a
        // module-range check would flag it). Audit p4.
        uint32_t va = pn<0x10000 ? it->second->export_va_ordinal(pn) : it->second->export_va(gread_mb(c,pn,-1));
        if(!va){ set_lasterr(c,127);                    // ERROR_PROC_NOT_FOUND
            if(env_filelog()) std::fprintf(stderr,"    [GetProcAddress MISS] mod=0x%08x proc=%s\n",c.arg(0), pn<0x10000?"<ord>":gread_mb(c,pn,-1).c_str()); }
        return va; });
    K("GetModuleHandleA",1,[](Cpu&c)->uint32_t{ if(!c.arg(0)) return g_curBase;   // NULL = current exe
        std::string n=gread_mb(c,c.arg(0),-1);
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        for(auto&ch:base) ch=(char)std::tolower((unsigned char)ch);
        if(base.find('.')==std::string::npos) base+=".dll";
        auto it=g_modByName.find(base); if(it!=g_modByName.end()) return it->second->load_base();
        for(auto&p:g_sysLib) if(p.second==base) return p.first;
        for(const char** d=SYS_DLLS;*d;d++) if(base==*d){ uint32_t h=g_sysLibNextH++; g_sysLib[h]=base; return h; }
        return 0u; });
    K("GetModuleFileNameA",3,[](Cpu&c)->uint32_t{ std::string path=module_path_for(c.arg(0));
        if(path.empty()){ set_lasterr(c,6); return 0u; }                       // ERROR_INVALID_HANDLE
        uint32_t b=c.arg(1),n=c.arg(2); if(!b||!n) return 0u;
        uint32_t len=(uint32_t)path.size(); if(len>=n){ len=n-1; }               // truncate to nSize-1
        c.write(b,path.c_str(),len); uint8_t z=0; c.write(b+len,&z,1); return len; });
    // Dynamic loads: bridge modules first; then SYSTEM dlls we shim (advapi32
    // etc.) get a pseudo-handle so GetProcAddress can resolve to shim traps —
    // D2Game's realm startup does LoadLibraryA("advapi32.dll")+GetProcAddress.
    static auto loadlib=[](Cpu&c,uint32_t pName)->uint32_t{
        std::string n=gread_mb(c,pName,-1);
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        for(auto&ch:base) ch=(char)std::tolower((unsigned char)ch);
        if(base.find('.')==std::string::npos) base+=".dll";
        auto it=g_modByName.find(base); if(it!=g_modByName.end()) return it->second->load_base();
        // D2_GLIDERING=1: our REAL glide3x.dll (build-glide/glide3x.dll, x86
        // MinGW) sits in the game directory and must be LOADED, not stubbed
        // — otherwise the ~970 Glide calls per frame would stay traps. The
        // only change needed is to skip the SYS_DLLS entry for this name:
        // control then falls into the "real DLL on disk" path just below,
        // which maps, relocates, resolves imports and launches DllMain.
        // Without the knob: nothing changes.
        // This is now the default (validated in env.txt) — no more environment read.
        for(const char** d=SYS_DLLS;*d;d++){
          if(base=="glide3x.dll") break;
          if(base==*d){
            for(auto&p:g_sysLib) if(p.second==base) return p.first;
            uint32_t h=g_sysLibNextH++; g_sysLib[h]=base;
            if(env_filelog()) std::fprintf(stderr,"    [LoadLibrary SYS] %s -> 0x%08x\n",base.c_str(),h);
            return h; } }
        // Runtime PE load: a REAL DLL on disk that D2 extracted and wants to run
        // — the lockdown CheckRevision.dll (from CheckRevision.mpq at Battle.net
        // login). Map + link it into the guest so GetProcAddress("CheckRevision")
        // resolves and D2 can call the export in the dynarec.
        if(g_bridge){
            std::string hp=host_path(n);
            std::vector<uint8_t> b=slurp(hp);
#ifdef __vita__
            // glide3x.dll is the port's OWN Glide-renderer DLL, shipped inside
            // the VPK (app0:), NOT part of a Diablo II install. A fresh install
            // has none in the game directory, and since D2 is launched with
            // -3dfx it LoadLibrary's glide3x.dll for the renderer -- without it
            // the renderer init fails and D2 halts at frame 0 with "Error 1:
            // ... Unsupported graphics mode." Fall back to the bundled copy so
            // every install renders; a copy in the game dir still wins first
            // (dev override).
            if(b.empty() && base=="glide3x.dll") b=slurp("app0:glide3x.dll");
#endif
            if(b.size()>0x40 && b[0]=='M' && b[1]=='Z'){
                std::string lerr; PeImage* pi=g_bridge->load_library_runtime(base,b,lerr);
                if(pi){ g_modByBase[pi->load_base()]=pi; g_modByName[base]=pi;
                    uint32_t hmod=pi->load_base(), dm=pi->entry_va();
                    if(env_filelog()) std::fprintf(stderr,"    [LoadLibrary DISK] %s -> base 0x%08x size 0x%x, %zu imports; DllMain 0x%08x\n",
                        n.c_str(),hmod,pi->image_size(),pi->imports().size(),dm);
                    // The real lockdown CheckRevision hash overruns a stack buffer
                    // under our emulation with D2's live args (the D2CRTEST self-test
                    // with dummy args is clean). Since we control the BNCS server —
                    // any SID_AUTH_CHECK is accepted — replace the export with a stub
                    // that writes plausible outputs (*arg5=version, *arg6=0,
                    // arg7[0]=0) and returns success, so D2 sends SID_AUTH_CHECK and
                    // login proceeds. The REAL export is now the DEFAULT: under
                    // the native scheduler the real DLL runs fine on console,
                    // through actual gameplay. The stub fabricates a response
                    // (version 0x100, checksum 0); it remains only as a bench
                    // tool, opted into explicitly via D2CR_STUB=1 — and the
                    // network gate then refuses any connection to the
                    // official service.
                    if(base=="checkrevision.dll" && getenv("D2CR_STUB")){
                        uint32_t ev=pi->export_va("CheckRevision");
                        if(ev){ uint8_t s[48]; int j=0; uint32_t v100=0x100,z0=0,one=1;
                            s[j++]=0x8B;s[j++]=0x44;s[j++]=0x24;s[j++]=0x14;                         // mov eax,[esp+0x14] arg5
                            s[j++]=0xC7;s[j++]=0x00;std::memcpy(s+j,&v100,4);j+=4;                   // mov [eax],0x100
                            s[j++]=0x8B;s[j++]=0x44;s[j++]=0x24;s[j++]=0x18;                         // mov eax,[esp+0x18] arg6
                            s[j++]=0xC7;s[j++]=0x00;std::memcpy(s+j,&z0,4);j+=4;                     // mov [eax],0
                            s[j++]=0x8B;s[j++]=0x44;s[j++]=0x24;s[j++]=0x1C;                         // mov eax,[esp+0x1c] arg7
                            s[j++]=0xC6;s[j++]=0x00;s[j++]=0x00;                                     // mov byte[eax],0
                            s[j++]=0xB8;std::memcpy(s+j,&one,4);j+=4;                                // mov eax,1
                            s[j++]=0xC2;s[j++]=0x1C;s[j++]=0x00;                                     // ret 0x1c
                            c.write(ev,s,(uint32_t)j);
                            if(env_filelog()) std::fprintf(stderr,"    [CheckRevision] export @0x%08x stubbed (dummy success)\n",ev);
                        }
                        // SKIP DllMain for the stubbed export. The stub is fully
                        // self-contained (mov/ret — no security cookie, no TLS, no
                        // atexit) so the CRT init is unnecessary. Crucially, running
                        // the lockdown DLL's DllMain here faults INTERMITTENTLY: its
                        // CRT init jumps to an unrelocated .data address
                        // (EIP=0x10042ab8) when the checkrevision thread is preempted
                        // mid-init — a dynarec/TLS race under the cooperative
                        // scheduler (proven: da257a1's exact binary faults too, so it
                        // predates the visual-detection work; a "lucky" run once got
                        // through). Skipping DllMain makes the checkrevision path
                        // deterministic. Imports are already resolved by
                        // load_library_runtime above. D2CR_RUNDLLMAIN forces the old
                        // (flaky) behaviour for debugging.
                        if(!getenv("D2CR_RUNDLLMAIN")){
                            if(env_filelog()) std::fprintf(stderr,"    [CheckRevision] DllMain SKIPPED (stub needs no CRT init) -> deterministic\n");
                            return hmod;
                        }
                    }
                    // Run DllMain(DLL_PROCESS_ATTACH) as real LoadLibrary does —
                    // WITHOUT a nested cpu->run (which would clobber the main
                    // thread's stack / shared emu). We redirect the guest, after
                    // the trap cleanup, into a tiny stub that calls DllMain on the
                    // calling thread's OWN stack, restores EAX=hModule, and returns
                    // to the LoadLibrary call site. The CRT init (security cookie,
                    // TLS, atexit) MUST run before the DLL's export is callable.
                    // Loader-lock pin for the duration of DllMain: a time-slice
                    // preempt now re-runs THIS thread rather than letting another
                    // thread mutate the shared dynarec state mid-CRT-init (the
                    // cause of the intermittent EIP=0x10042ab8 fault). The stub
                    // clears the pin (calls g_dllmainDoneTrap) the instant DllMain
                    // returns, so scheduling is normal everywhere else.
                    if(!g_dllmainDoneTrap)
                        g_dllmainDoneTrap=g_bridge->shim_trap("KERNEL32.dll","__d2rt_dllmain_done");
                    if(g_sched && g_dllmainDoneTrap) g_sched->set_no_preempt(true);
                    uint32_t retaddr=c.read_u32(c.reg(R_ESP));
                    uint8_t st[48]; int k=0;
                    st[k++]=0x6A; st[k++]=0x00;                        // push 0 (lpvReserved)
                    st[k++]=0x6A; st[k++]=0x01;                        // push 1 (DLL_PROCESS_ATTACH)
                    st[k++]=0x68; std::memcpy(st+k,&hmod,4); k+=4;     // push hinstDLL
                    st[k++]=0xB8; std::memcpy(st+k,&dm,4);   k+=4;     // mov eax, DllMain
                    st[k++]=0xFF; st[k++]=0xD0;                        // call eax  (runs DllMain)
                    if(g_sched && g_dllmainDoneTrap){                  // clear the loader-lock pin
                        st[k++]=0xB8; std::memcpy(st+k,&g_dllmainDoneTrap,4); k+=4; // mov eax, done-trap
                        st[k++]=0xFF; st[k++]=0xD0; }                  // call eax (set_no_preempt(false))
                    st[k++]=0xB8; std::memcpy(st+k,&hmod,4); k+=4;     // mov eax, hModule
                    st[k++]=0x68; std::memcpy(st+k,&retaddr,4); k+=4;  // push retaddr
                    st[k++]=0xC3;                                      // ret -> back to caller
                    uint32_t stub=misc((uint32_t)k); c.write(stub,st,(uint32_t)k);
                    g_bridge->redirect_next(stub);
                    return hmod; }
                if(env_filelog()) std::fprintf(stderr,"    [LoadLibrary DISK FAIL] %s: %s\n",n.c_str(),lerr.c_str());
                { char m[200]; std::snprintf(m,sizeof m,"LoadLibrary ECHEC %s: %s",n.c_str(),lerr.c_str()); d2vita_progress(m); }
            }
        }
        if(env_filelog()) std::fprintf(stderr,"    [LoadLibrary MISS] %s\n",n.c_str());
        set_lasterr(c,126); return 0u; };   // ERROR_MOD_NOT_FOUND (real failure, not a fake handle)
    K("LoadLibraryA",1,[](Cpu&c){ return loadlib(c,c.arg(0)); });
    K("LoadLibraryExA",3,[](Cpu&c){ return loadlib(c,c.arg(0)); });
    K("GetModuleFileNameW",3,[](Cpu&c)->uint32_t{ std::string path=module_path_for(c.arg(0));
        if(path.empty()){ set_lasterr(c,6); return 0u; }
        uint32_t buf=c.arg(1),sz=c.arg(2);
        const char* p=path.c_str(); uint32_t n=(uint32_t)path.size();
        if(buf&&sz){ uint32_t w=n<sz?n:sz-1; for(uint32_t i=0;i<w;i++) gwrite_wc(c,buf+2*i,(uint16_t)(uint8_t)p[i]);
            if(w<sz) gwrite_wc(c,buf+2*w,0); return w; } return 0u; });
    K("GetModuleHandleW",1,[](Cpu&c)->uint32_t{ if(!c.arg(0)) return g_curBase;
        std::string n=wnarrow(c,c.arg(0));
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        for(auto&ch:base) ch=(char)std::tolower((unsigned char)ch);
        if(base.find('.')==std::string::npos) base+=".dll";
        auto it=g_modByName.find(base); if(it!=g_modByName.end()) return it->second->load_base();
        for(auto&q:g_sysLib) if(q.second==base) return q.first;
        // parity with GetModuleHandleA (audit p4 GAP-2): mint a stable sys handle.
        for(const char** d=SYS_DLLS;*d;d++) if(base==*d){ uint32_t h=g_sysLibNextH++; g_sysLib[h]=base; return h; }
        return 0u; });
    K("LoadLibraryW",1,[](Cpu&c){ return loadlib(c,put_cstr(c,wnarrow(c,c.arg(0)).c_str())); });
    // GetModuleHandleExA/W: dwFlags | *phModule=out. FROM_ADDRESS(0x4) resolves
    // the module containing an address (Warden's "which module owns this hook").
    // GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT(0x2) is a no-op for us.
    // Honest by-name resolver (audit p4 GAP-1): the old code returned g_d2base for
    // EVERY name (reported Game.exe for user32/kernel32/anything) and never even
    // read the string. Resolve against the real registry; unknown -> 0 (FALSE).
    auto gmh_named=[](const std::string& raw)->uint32_t{
        size_t s=raw.find_last_of("\\/"); std::string base=s==std::string::npos?raw:raw.substr(s+1);
        for(auto&ch:base) ch=(char)std::tolower((unsigned char)ch);
        if(base.find('.')==std::string::npos) base+=".dll";
        auto it=g_modByName.find(base); if(it!=g_modByName.end()) return it->second->load_base();
        for(auto&p:g_sysLib) if(p.second==base) return p.first;
        for(const char** d=SYS_DLLS;*d;d++) if(base==*d){ uint32_t h=g_sysLibNextH++; g_sysLib[h]=base; return h; }
        return 0u; };
    auto gmhex=[gmh_named](Cpu&c,bool wide)->uint32_t{ uint32_t fl=c.arg(0),name=c.arg(1),out=c.arg(2); uint32_t h=0;
        if(fl&0x4u){ for(auto&p:g_modByBase){ uint32_t b=p.first,e=b+p.second->image_size();
                if(name>=b&&name<e){ h=b; break; } } }                       // FROM_ADDRESS
        else if(!name) h=g_curBase?g_curBase:g_d2base;                        // NULL = self
        else h=gmh_named(wide?wnarrow(c,name):gread_mb(c,name,-1));           // by-name (honest)
        if(out) c.write_u32(out,h); return h?1u:0u; };
    K("GetModuleHandleExA",3,[gmhex](Cpu&c){ return gmhex(c,false); });
    K("GetModuleHandleExW",3,[gmhex](Cpu&c){ return gmhex(c,true ); });
}
