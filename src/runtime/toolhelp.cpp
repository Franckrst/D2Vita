// toolhelp.cpp -- see toolhelp.h.
#include "toolhelp.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_toolhelp.h"
#include "runtime/guest_sync.h"             // WxThread, wx86_handle_add/wx86_handle_next_id
#include "runtime/pe_image.h"
#include "runtime/win32_shims_kernel32.h"   // wx86_win_pid/wx86_win_tid/wx86_sched_tid
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#ifdef __vita__
extern "C" void dyn86_set_seh_filter(uint32_t va);   // engine, fault_vita.c
#endif
using namespace d2rt;

using KThread = WxThread;         // engine (guest_sync.h); same alias as tools/rt_boot.cpp's own
using SnapState = WxSnapState;    // engine (guest_toolhelp.h)
// Separate reference to the SAME underlying engine table as tools/rt_boot.cpp's
// own g_snaps -- wx86_snapshots() always returns the one true map.
static std::map<uint32_t,SnapState>& g_snaps = wx86_snapshots();

// Toolhelp module enumeration (Module32First/NextW): a SNAPMODULE snapshot on
// Windows ALWAYS contains at least the process's own modules. Enumerate the
// real g_modByName so Toolhelp agrees with GetModuleHandle/GetProcAddress
// instead of returning an empty (divergent, flaggable) snapshot.
std::vector<M32E> g_mod32list;
size_t g_mod32cur=0;
void mod32_snapshot(){ g_mod32list.clear();
    for(auto&p:g_modByName) g_mod32list.push_back({p.second->load_base(),p.second->image_size(),p.first});
    g_mod32cur=0; }
uint32_t mod32_fill(Cpu&c, uint32_t p, const M32E& m, bool wide){
    c.write_u32(p+0x00, wide?0x424u:0x224u);   // dwSize
    c.write_u32(p+0x04, m.base);               // th32ModuleID
    c.write_u32(p+0x08, wx86_win_pid());        // th32ProcessID (never 1)
    c.write_u32(p+0x0C, 0xFFFFu);               // GlblcntUsage
    c.write_u32(p+0x10, 0xFFFFu);               // ProccntUsage
    c.write_u32(p+0x14, m.base);                // modBaseAddr
    c.write_u32(p+0x18, m.size);                // modBaseSize
    c.write_u32(p+0x1C, m.base);                // hModule
    std::string full=std::string("C:\\Diablo II\\")+m.name;
    if(wide){
        uint32_t sm=p+0x20, se=p+0x220;
        size_t i=0; for(; i<m.name.size()&&i<255; i++) gwrite_wc(c,sm+2*i,(uint16_t)(uint8_t)m.name[i]); gwrite_wc(c,sm+2*i,0);
        size_t j=0; for(; j<full.size()&&j<259; j++) gwrite_wc(c,se+2*j,(uint16_t)(uint8_t)full[j]); gwrite_wc(c,se+2*j,0);
    } else {
        uint32_t sm=p+0x20, se=p+0x120;   // MODULEENTRY32 (ANSI): szModule[256]@0x20, szExePath[260]@0x120
        c.write(sm,m.name.c_str(),(uint32_t)std::min(m.name.size(),(size_t)255)); uint8_t z=0; c.write(sm+std::min(m.name.size(),(size_t)255),&z,1);
        c.write(se,full.c_str(),(uint32_t)std::min(full.size(),(size_t)259)); c.write(se+std::min(full.size(),(size_t)259),&z,1);
    }
    return 1u;
}

// THREADENTRY32 (0x1C): dwSize0x00 cntUsage0x04 th32ThreadID0x08
//   th32OwnerProcessID0x0C tpBasePri0x10 tpDeltaPri0x14 dwFlags0x18
uint32_t te32_fill(Cpu&c, uint32_t p, uint32_t tid){
    c.write_u32(p+0x00,0x1Cu); c.write_u32(p+0x04,1u);
    // GUEST view: tid and pid are multiples of 4, never 1. Numbering belongs
    // to the engine (wx86_win_tid/pid) — redoing it here would let the two
    // views drift apart with nothing to flag it.
    c.write_u32(p+0x08,wx86_win_tid(tid)); c.write_u32(p+0x0C,wx86_win_pid());
    c.write_u32(p+0x10,8u);    c.write_u32(p+0x14,0u); c.write_u32(p+0x18,0u);
    return 1u; }
// PROCESSENTRY32(W): dwSize0x00 cntUsage0x04 th32ProcessID0x08 DefaultHeapID0x0C
//   ModuleID0x10 cntThreads0x14 ParentPID0x18 pcPriClassBase0x1C dwFlags0x20
//   szExeFile@0x24 (ANSI[260]=>size0x128 / WCHAR[260]=>size0x22C)
uint32_t pe32_fill(Cpu&c, uint32_t p, bool wide){
    uint32_t nthr = (g_sched? (uint32_t)g_sched->live_thread_count():1u);
    c.write_u32(p+0x00, wide?0x22Cu:0x128u); c.write_u32(p+0x04,1u);
    c.write_u32(p+0x08,wx86_win_pid()); c.write_u32(p+0x0C,0u); c.write_u32(p+0x10,0u);
    c.write_u32(p+0x14,nthr);c.write_u32(p+0x18,0u); c.write_u32(p+0x1C,8u);
    c.write_u32(p+0x20,0u);
    const char* exe="game.exe"; uint32_t s=p+0x24;
    if(wide){ size_t i=0; for(;exe[i];++i) gwrite_wc(c,s+2*i,(uint16_t)(uint8_t)exe[i]); gwrite_wc(c,s+2*i,0); }
    else    { c.write(s,exe,9); }   // 8 chars + NUL
    return 1u; }
// Canonical x86 CONTEXT integer block: Edi@0x9C Esi@0xA0 Ebx@0xA4 Edx@0xA8
//   Ecx@0xAC Eax@0xB0 Ebp@0xB4 Eip@0xB8 EFlags@0xC0 Esp@0xC4 (SegCs@0xBC/SegSs@0xC8
//   left as caller-init). Used by RtlCaptureContext + GetThreadContext so both agree.
void ctx_write(Cpu&c, uint32_t p, uint32_t edi,uint32_t esi,uint32_t ebx,
        uint32_t edx,uint32_t ecx,uint32_t eax,uint32_t ebp,uint32_t eip,
        uint32_t efl,uint32_t esp){
    c.write_u32(p+0x9C,edi); c.write_u32(p+0xA0,esi); c.write_u32(p+0xA4,ebx);
    c.write_u32(p+0xA8,edx); c.write_u32(p+0xAC,ecx); c.write_u32(p+0xB0,eax);
    c.write_u32(p+0xB4,ebp); c.write_u32(p+0xB8,eip); c.write_u32(p+0xC0,efl);
    c.write_u32(p+0xC4,esp); }

// On a dynarec-detected guest fault, note whether the game had an SEH chain
// installed at fs:[0], then let the thread die exactly as it did before.
//
// This runtime does NOT dispatch exceptions to guest handlers: it never calls
// one, never resumes, never fabricates a record (documented gap — see
// docs/FIDELITY_TODO.md). The one thing worth recording is that the game had
// installed a handler, i.e. a real Windows would have given its `__except` a
// chance and the process might have survived. That single fact turns an
// otherwise inexplicable crash report into an explained one.
//
// jpline, not stderr: on console stderr goes nowhere, and this line is only
// worth writing if it reaches the boot log and the crash report.
int fault_note_seh_chain(Cpu& c, GuestThread* t, uint32_t code){
    uint32_t frame = c.read_u32(t->tib + 0x00);          // fs:[0] chain head
    if(frame != 0xFFFFFFFFu && frame != 0)
        jpline("[seh] chaine SEH installee a la faute (code=0x%08x eip=0x%08x fil=%u fs:[0]=0x%08x)"
               " — non rattrapee, terminaison", code, c.reg(R_EIP), t->id, frame);
    return 0;                                            // terminate, as before
}

void toolhelp_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // Honest Toolhelp: a real snapshot of the REAL scheduler state (audit p5).
    // The old empty stub was DISHONEST — a live process always has itself and
    // >=1 thread. We expose the true guest view, never a fabricated one.
    K("CreateToolhelp32Snapshot",2,[](Cpu&c){
        uint32_t h=wx86_handle_next_id(); SnapState s;
        if(g_sched) s.tids=g_sched->live_thread_ids(); else s.tids={1u};
        s.pids={1u}; g_snaps[h]=std::move(s);          // Module32* uses its own g_mod32list
        return h; });                                  // real, unique, non-null handle
    K("Process32First",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.pids.empty()){ set_lasterr(c,18); return 0u; }
        it->second.pcur=1; return pe32_fill(c,c.arg(1),false); });
    K("Process32Next",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.pcur>=it->second.pids.size()){ set_lasterr(c,18); return 0u; }
        it->second.pcur++; return pe32_fill(c,c.arg(1),false); });
    // OpenProcess (stdcall 3 args) — the network game-load path calls it (anti-tamper
    // scan). DELIBERATELY still returns NULL/ACCESS_DENIED: returning a valid self
    // handle is honest, but there is NO ReadProcessMemory shim, so an anti-tamper
    // path that escalates through a non-NULL handle would hit the default shim ->
    // controlled stop. Keep denying until that flow is traced (audit p5 GAP-6).

    // OpenThread(access,inherit,tid): resolve a REAL guest thread -> KThread handle
    // (same object model as CreateThread). Honest: only real tids succeed.
    K("OpenThread",3,[](Cpu&c){ uint32_t tid=wx86_sched_tid(c.arg(2));   // guest view -> internal id
        GuestThread* g = (g_sched && tid)? g_sched->thread_by_id(tid):nullptr;
        if(!g){ set_lasterr(c,87); return 0u; }                              // ERROR_INVALID_PARAMETER
        KThread* k=new KThread(); k->gt=g;
        uint32_t h=wx86_handle_add(k); set_lasterr(c,0); return h; });
    // RtlCaptureContext(PCONTEXT): fill the caller's CONTEXT from the live x86
    // register file. CANONICAL x86 offsets (audit GAP-0: the old code was shifted
    // +0x10 -> D2's windows.h reads landed on the wrong fields). Eip = return
    // address (post-CALL), Esp = caller's ESP after the implicit return.
    K("RtlCaptureContext",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        uint32_t ret=c.read_u32(c.reg(R_ESP));
        c.write_u32(p+0x00,0x10007u);          // ContextFlags = CONTEXT_FULL
        ctx_write(c,p, c.reg(R_EDI),c.reg(R_ESI),c.reg(R_EBX),c.reg(R_EDX),
                  c.reg(R_ECX),c.reg(R_EAX),c.reg(R_EBP),ret,c.reg(R_EFLAGS),c.reg(R_ESP)+4);
        return 0u; });
    K("Thread32First",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.tids.empty()){ set_lasterr(c,18); return 0u; }
        it->second.tcur=0; return te32_fill(c,c.arg(1),it->second.tids[it->second.tcur++]); });
    K("Thread32Next",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.tcur>=it->second.tids.size()){ set_lasterr(c,18); return 0u; }
        return te32_fill(c,c.arg(1),it->second.tids[it->second.tcur++]); });
    // GetThreadContext(hThread,lpContext): fill from the target thread's REAL state
    // — live regs for the current thread, saved X86Context for another. Canonical
    // offsets (agrees with RtlCaptureContext). Honest introspection, no fabrication.
    K("GetThreadContext",2,[](Cpu&c)->uint32_t{ uint32_t h=c.arg(0), p=c.arg(1); if(!p) return 0u;
        GuestThread* cur = g_sched? g_sched->current():nullptr;
        GuestThread* tgt = nullptr;
        if(h==0xFFFFFFFEu) tgt=cur;                    // GetCurrentThread pseudo-handle
        else { Waitable* itw=wx86_handle_find(h);
               if(itw&&is_kind(itw,"thread")) tgt=static_cast<KThread*>(itw)->gt; }
        if(!tgt) return 0u;
        if(tgt==cur) ctx_write(c,p, c.reg(R_EDI),c.reg(R_ESI),c.reg(R_EBX),c.reg(R_EDX),
                  c.reg(R_ECX),c.reg(R_EAX),c.reg(R_EBP),c.reg(R_EIP),c.reg(R_EFLAGS),c.reg(R_ESP));
        else { const X86Context& s=tgt->ctx; ctx_write(c,p, s.gpr[R_EDI],s.gpr[R_ESI],s.gpr[R_EBX],
                  s.gpr[R_EDX],s.gpr[R_ECX],s.gpr[R_EAX],s.gpr[R_EBP],s.eip,s.eflags,s.gpr[R_ESP]); }
        return 1u; });
    K("Module32FirstW",2,[](Cpu&c){ mod32_snapshot(); if(g_mod32list.empty()){ set_lasterr(c,18); return 0u; }
        return mod32_fill(c,c.arg(1),g_mod32list[g_mod32cur++],true); });
    K("Module32NextW",2,[](Cpu&c){ if(g_mod32cur>=g_mod32list.size()){ set_lasterr(c,18); return 0u; }
        return mod32_fill(c,c.arg(1),g_mod32list[g_mod32cur++],true); });
    K("Module32First",2,[](Cpu&c){ mod32_snapshot(); if(g_mod32list.empty()){ set_lasterr(c,18); return 0u; }
        return mod32_fill(c,c.arg(1),g_mod32list[g_mod32cur++],false); });
    K("Module32Next",2,[](Cpu&c){ if(g_mod32cur>=g_mod32list.size()){ set_lasterr(c,18); return 0u; }
        return mod32_fill(c,c.arg(1),g_mod32list[g_mod32cur++],false); });
    K("Process32FirstW",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.pids.empty()){ set_lasterr(c,18); return 0u; }
        it->second.pcur=1; return pe32_fill(c,c.arg(1),true); });
    K("Process32NextW",2,[](Cpu&c){ auto it=g_snaps.find(c.arg(0));
        if(it==g_snaps.end()||it->second.pcur>=it->second.pids.size()){ set_lasterr(c,18); return 0u; }
        it->second.pcur++; return pe32_fill(c,c.arg(1),true); });
    // SetUnhandledExceptionFilter: store the guest's top-level filter and return
    // the PREVIOUS one (Win32 contract). On console with kubridge the engine's
    // abort handler dispatches a guest access violation to the fs:[0] chain
    // and then to this filter (dyn86_seh_deliver); on the desktop/qemu build
    // it is still recorded but never invoked. Tracking it honestly matters
    // either way: it stops D2/__report_gsfailure from mis-reading the
    // previous filter.
    K("SetUnhandledExceptionFilter",1,[](Cpu&c){ uint32_t prev=g_unhandledFilter; g_unhandledFilter=c.arg(0);
#ifdef __vita__
        // With kubridge the engine's abort handler delivers guest access
        // violations to this filter (dyn86_seh_deliver) -- the gap above
        // closes on console; the desktop/qemu build still records only.
        dyn86_set_seh_filter(c.arg(0));
#endif
        return prev; });
}
