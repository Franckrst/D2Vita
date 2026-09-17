// emutls_probe.cpp -- see emutls_probe.h.
//
// emutls probe: two pthreads publish the address of their own copy of a
// thread_local sentinel; equal addresses = emutls inert = native would be a
// silent corruption (thread_local SHARED between threads — a
// -Wl,-u,pthread_cancel dropped in a build refactor would be enough to cause it).
// HARDENED: each thread SPINS on `go` after publishing, and main waits until
// BOTH slots are non-null — both emutls blocks are proven alive
// SIMULTANEOUSLY. Without this, thread a could finish, its emutls block be
// FREED (pthread key destructor / newlib), and thread b's malloc (same size,
// free-list) return the SAME address: a false FATAL that would send someone
// hunting a linker bug on console at great cost per iteration.
// ~1 ms at boot, native only.
// ---- REAL call counter for the emutls chain (measurement build only) ------
// Armed by D2VPK_TLSWRAP=1, which adds -DD2_TLSWRAP and
// -Wl,--wrap=__emutls_get_address. NEVER SHIPPED.
//
// Why a link-time wrap and not a static site count: previous attempts counted
// STATIC sites and were off by a factor of 2. Here the call itself is
// intercepted: the number is an OBSERVATION, not a deduction.
//
// Deliberately a 32-bit counter: a measurement run lasts a few minutes at up
// to a few million calls/s, staying well under 32-bit overflow. The increment
// is not atomic; on a single core, a preemption between load and store loses
// AT MOST one count, negligible next to an order of magnitude.
#include "emutls_probe.h"
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <sched.h>

#ifdef D2_TLSWRAP
// Histogram PER CALLER (return address), same pattern as d2rt_eipprof:
// open-addressed table, bounded linear probing, never reallocates. Without it
// the TOTAL is known but not where to focus effort — and refactoring hundreds
// of call sites based on a guess is exactly what this avoids.
#define D2_TLSW_N 128
extern "C" {
    void* __real___emutls_get_address(void*);
    unsigned d2_emutls_calls = 0;
    static unsigned d2_tlsw_key[D2_TLSW_N];
    static unsigned d2_tlsw_hit[D2_TLSW_N];
    // noinline + no tail call: BOTH are necessary, and neither is obvious.
    //  - without no-optimize-sibling-calls, gcc turns the body into a `b` to
    //    __real___emutls_get_address, and __builtin_return_address(0) no
    //    longer names the call site;
    //  - reading LR in inline asm doesn't work either: gcc uses LR as a
    //    scratch register BEFORE the asm (observed: `movw lr, #12120`).
    // Both traps were caught by DISASSEMBLY: a return address always follows
    // a `bl`, and these did not.
    __attribute__((noinline, optimize("no-optimize-sibling-calls")))
    void* __wrap___emutls_get_address(void* p){
        ++d2_emutls_calls;
        const unsigned ra = (unsigned)(uintptr_t)__builtin_return_address(0);
        const unsigned h  = (ra >> 2) & (D2_TLSW_N - 1);
        for (unsigned i = 0; i < 12; i++) {
            const unsigned s = (h + i) & (D2_TLSW_N - 1);
            if (!d2_tlsw_hit[s]) { d2_tlsw_key[s] = ra; d2_tlsw_hit[s] = 1; break; }
            if (d2_tlsw_key[s] == ra) { ++d2_tlsw_hit[s]; break; }
        }
        return __real___emutls_get_address(p);
    }
    // The top SIX callers, hottest to coldest. Selected by a full scan per
    // rank (128 slots, once every 10 s): trivial, and crucially stateless
    // between calls — an in-place sort would bias subsequent readouts.
    int d2_tlswrap_dump(char* b, unsigned n){
        unsigned used[6]; int nu = 0, w = 0;
        w = std::snprintf(b, n, "emutls-top:");
        for (int r = 0; r < 6; r++) {
            int best = -1;
            for (int i = 0; i < D2_TLSW_N; i++) {
                if (!d2_tlsw_hit[i]) continue;
                bool skip = false;
                for (int u = 0; u < nu; u++) if (used[u] == (unsigned)i) { skip = true; break; }
                if (skip) continue;
                if (best < 0 || d2_tlsw_hit[i] > d2_tlsw_hit[best]) best = i;
            }
            if (best < 0) break;
            used[nu++] = (unsigned)best;
            if (w > 0 && (unsigned)w < n)
                w += std::snprintf(b + w, n - (unsigned)w, " %08x:%u",
                                   d2_tlsw_key[best], d2_tlsw_hit[best]);
        }
        return w < 0 ? 0 : w;
    }
}
#endif

static thread_local int d2rt_tls_sentinel;
static void* volatile d2rt_tls_slot[2];
static volatile int d2rt_tls_go=0;
static void* d2rt_tls_thread(void* n){
    d2rt_tls_slot[(int)(intptr_t)n]=(void*)&d2rt_tls_sentinel;
    while(!d2rt_tls_go) sched_yield();
    return nullptr;
}
// 0 = per-thread TLS OK; -1 = emutls INERT; >0 = failed pthread_create rc.
int d2rt_tls_probe(){
    d2rt_tls_slot[0]=d2rt_tls_slot[1]=nullptr; d2rt_tls_go=0;
    pthread_t th[2];
    for(int i=0;i<2;i++){
        int rc=pthread_create(&th[i],nullptr,d2rt_tls_thread,(void*)(intptr_t)i);
        if(rc){ d2rt_tls_go=1; if(i) pthread_join(th[0],nullptr); return rc; }
    }
    while(!d2rt_tls_slot[0]||!d2rt_tls_slot[1]) sched_yield();
    void *pa=d2rt_tls_slot[0], *pb=d2rt_tls_slot[1];
    d2rt_tls_go=1;
    pthread_join(th[0],nullptr); pthread_join(th[1],nullptr);
    return (pa&&pb&&pa!=pb)?0:-1;
}
