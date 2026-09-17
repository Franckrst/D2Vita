// kernel32_interlocked.cpp -- see kernel32_interlocked.h.
#include "kernel32_interlocked.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_atomics.h"          // wx86_ilk_ptr/wx86_ilk_cas
#include "runtime/win32_shims_kernel32.h"   // wx86_win_tid
#include "runtime/guest_thread.h"           // full d2rt::ThreadScheduler (g_sched->current()/yield())
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

// Atomics over guest memory: the implementation lives in the engine
// (third_party/winx86/src/runtime/guest_atomics.{h,cpp}). Short local aliases,
// same convention as tools/rt_boot.cpp's own (see the comment there).
static inline uint32_t* ilk_ptr(Cpu& c, uint32_t va){ return wx86_ilk_ptr(c,va); }
static inline uint32_t ilk_cas(uint32_t* h, uint32_t exp, uint32_t des){ return wx86_ilk_cas(h,exp,des); }
static uint32_t g_casCalls=0;   // spin-break: InterlockedCompareExchange call counter (periodic cooperative yield)

void kernel32_interlocked_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // Interlocked*: really atomic (see ilk_ptr above). SEQ_CST memory order
    // everywhere: on x86 these primitives are full barriers, that's their
    // contract and the game relies on it.
    // Increment/Decrement return the NEW value; every other one returns the OLD.
    // arg(1) used to be reread on BOTH paths (one R_ESP read each): a local
    // suffices. The values are identical — even before this change, the
    // argument was read BEFORE the write (parameter evaluation order).
    // CompareExchange(dest,exchange,comparand): if [dest]==comparand set exchange;
    // return old [dest]. Cooperative scheduler = no real races, so non-atomic ok.
    K("InterlockedCompareExchange",3,[](Cpu&c)->uint32_t{ uint32_t p=c.arg(0),xch=c.arg(1),cmp=c.arg(2),o;
        if(uint32_t* h=ilk_ptr(c,p)){
            // exp carries the OLD value in both cases: unchanged (hence equal to
            // the comparand, hence equal to the old value) on success,
            // reloaded with the real value on failure.
            o=ilk_cas(h,cmp,xch);
        } else {
            o=c.read_u32(p);
            if(o==cmp) c.write_u32(p,xch);
        }
        // Cooperative spin-break. D2's online act-load hangs in a tight acquire-check-
        // release spin (profiled: 0x7f000a00 InterlockedCompareExchange is the hottest
        // EIP by far, with no Sleep/SwitchToThread on this path). The CAS SUCCEEDS every
        // pass (the lock is free) — what is unmet is the FLAG the spin guards, which a
        // starved sibling thread must set. Under the single-runner scheduler the spinner
        // never yields, so the setter never runs. Yield every N calls so it does; any
        // lock momentarily held is released on the spin's next pass, so the sibling
        // still makes progress. Cheap in normal play (CAS is not called in bursts there).
        if((++g_casCalls & 31u)==0 && g_sched) g_sched->yield();
        return o; });
    // Extended Interlocked (S6): pointer variants alias the 32-bit ones on x86-32;
    // bitwise RMW return the OLD value.
    K("InterlockedCompareExchangePointer",3,[](Cpu&c)->uint32_t{ uint32_t p=c.arg(0),xch=c.arg(1),cmp=c.arg(2);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_cas(h,cmp,xch);
        uint32_t o=c.read_u32(p); if(o==cmp) c.write_u32(p,xch); return o; });
    K("GetCurrentThreadId",0,[](Cpu&){ return wx86_win_tid(g_sched&&g_sched->current()?g_sched->current()->id:1u); });
    K("GetCurrentProcess",0,[](Cpu&){ return 0xFFFFFFFFu; });
    K("GetCurrentThread",0,[](Cpu&){ return 0xFFFFFFFEu; });
}
