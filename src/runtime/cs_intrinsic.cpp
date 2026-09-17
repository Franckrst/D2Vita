// cs_intrinsic.cpp -- see cs_intrinsic.h.
//
// ---- Tier-1 intrinsics: critical sections (D2_CSINTRIN=1) -----------------
// These three slots are, along with the clock, among the most-taken in the
// profile (1.28M Enter/Leave pairs measured, contention ~0.0003%). Served
// HERE, they no longer go through Bridge::trap_handler: no slot
// division/bounding, no D2_WATCH check, no note_shim_enter/exit, no
// std::function indirection for the shim body, no take_redirect(). The
// executed body is the SAME.
//
// ⚠️ THIS IS NOT THE GUEST STUB REJECTED TWICE ELSEWHERE. The two documented
// rejections are:
//   (R1) rt_boot.cpp, above KCrit: "CS state lives in C++ (owner/count),
//        INVISIBLE to the guest. This was tried guest-backed [...] and
//        REVERTED: with visible state the level load livelocks".
//   (R2) rt_boot.cpp, in inline_hot_imports: "a guest-inlined CriticalSection
//        fast path was tried and REVERTED: Storm's worker deterministically
//        calls EnterCS on garbage pointers [...]. No single-compare bound
//        check can separate those from legitimate CS addresses".
// Neither is being worked around:
//   * R1 is about STATE. Here the state stays KCrit (owner/count) on the host
//     side; not one byte of the guest RTL_CRITICAL_SECTION is written. The
//     OwningThread/RecursionCount fields stay zero, exactly as today.
//   * R2 is about GUEST CODE that would need to DEREFERENCE the section
//     pointer to decide. Here nothing is emitted in the guest (the IAT and
//     .text stay intact, so pristine/Warden are unaffected) and the pointer
//     is NEVER dereferenced: it only serves as a KEY into g_crits, just like
//     in the shim. A garbage pointer from Storm therefore creates the same
//     phantom KCrit as today, at the same place, with the same effect — no
//     bounds check is necessary, and none is done.
//
// GIL: the dispatch site that calls try_intrinsic (the run() loop,
// cpu_box86.cpp) takes a `gil::Guard` BEFORE the call — documented at the top
// of gil.h ("taken at the CpuBox86 trap dispatch — NOT in Bridge::trap_handler,
// because Tier-1 intrinsics bypass the Bridge"). An intrinsic therefore runs
// under EXACTLY the same GIL as a shim body: try_acquire, notify and g_crits
// stay serialized.
//
// WHAT THE INTRINSIC DOES NOT DO: block. Enter only serves the UNCONTENDED
// case (try_acquire true), which is literally the first line of both wait()
// implementations (sched_cooperative.cpp:132 and sched_native.cpp:299:
// `if (w->try_acquire(t)) return w->result();`). As soon as try_acquire fails
// it returns false and the trap goes to the bridge, where the full shim
// retries and then blocks via the normal path — this matters: cooperative
// blocking goes through request_yield(), whose only consumer is
// trap_handler's QUEUE (`if (t_b.yield_pending) ... return false`). An
// intrinsic that returned true after a yield would leave a thread marked
// Blocked still running. A failed try_acquire has NO SIDE EFFECTS (see
// KCrit::try_acquire), so retrying it in the shim changes nothing.
// Same reasoning for the native K1 case (thread already Finished):
// wait_common installs a redirect to the sentinel, which only the bridge's
// queue consumes.
#include "cs_intrinsic.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_sync.h"
using namespace d2rt;

uint64_t g_csEnterIn=0, g_csEnterFast=0, g_csLeaveFast=0, g_csTryFast=0;
uint64_t g_csEspSaved=0;   // R_ESP resolutions saved (census)
bool     g_csIntrin=false; // D2_CSINTRIN=1; default = old path
// A SINGLE R_ESP resolution serves both the return address AND arg(0) — the
// "arg(0) hoisting" optimization carried all the way through: the shim path's
// trap_retaddr()+arg(0) pair does it twice.
struct CsFrame { uint32_t ret, cs; };
static inline CsFrame cs_frame(Cpu& c){
    const uint32_t esp=c.reg(R_ESP);
    ++g_csEspSaved;                     // one resolution instead of two
    return CsFrame{ c.read_u32(esp), c.read_u32(esp+4) }; }
bool cs_enter_intrinsic(Cpu& c, uint32_t /*slot*/){
    if(g_traceOn) return false;                 // let the shim do the logging
    ++g_csEnterIn;
    const CsFrame f=cs_frame(c);
    WxCrit* k=crit_for(f.cs);
    if(g_sched){
        GuestThread* t=g_sched->current();
        if(t && t->state==GuestThread::State::Finished) return false;   // K1 -> shim
        if(!k->try_acquire(t)) return false;                            // contention -> shim
    }   // no scheduler = Enter is a NOP (same rule as the shim)
    c.trap_epilogue(0u, 8u, f.ret);             // EAX=0 ; ret 4 (stdcall, argc=1)
    ++g_csEnterFast; return true; }
bool cs_leave_intrinsic(Cpu& c, uint32_t /*slot*/){
    if(g_traceOn) return false;
    const CsFrame f=cs_frame(c);
    wx86_crit_leave(f.cs, g_sched);   // SAME body as the shim, plus the epilogue
    c.trap_epilogue(0u, 8u, f.ret);
    ++g_csLeaveFast; return true; }
bool cs_try_intrinsic(Cpu& c, uint32_t /*slot*/){
    if(g_traceOn) return false;
    const CsFrame f=cs_frame(c);
    uint32_t eax=1u;                             // !g_sched: the shim returns 1
    if(g_sched){ WxCrit* k=crit_for(f.cs);
        eax = k->try_acquire(g_sched->current()) ? 1u : 0u; }
    c.trap_epilogue(eax, 8u, f.ret);
    ++g_csTryFast; return true; }
// ---- Census of critical sections served via intrinsic ---------------------
// Prints ONLY if the path actually served something: the CONTROL leg (knob
// off) has NO line at all, the armed leg must have one with non-zero counts —
// the non-vacuity check for the oracle. jpline, not printf: on Vita stdout
// goes nowhere, and an invisible counter on the only surface where the
// measurement happens proves nothing.
// "esp-hoisted" = R_ESP resolutions saved: a slot served here does ONE
// instead of the TWO of the shim path (trap_retaddr() then arg(0)).
// "enter-fallbacks" = contended Enters (or a Finished thread) handed to the full shim.
void cs_intrin_report(){
    if(!(g_csEnterIn|g_csLeaveFast|g_csTryFast)) return;
    jpline("[csintrin] enter=%llu (replis=%llu) leave=%llu tryenter=%llu esp-hisse=%llu",
        (unsigned long long)g_csEnterFast,
        (unsigned long long)(g_csEnterIn-g_csEnterFast),
        (unsigned long long)g_csLeaveFast,
        (unsigned long long)g_csTryFast,
        (unsigned long long)g_csEspSaved); }
// Same figures, but readable MID-RUN. The final report never arrives on
// console under native + MAXFRAMES, so a bench pass would otherwise have no
// proof the intrinsic actually SERVED anything (only that it was armed). The
// 10 s window (vita_present) reads these values through this accessor.
extern "C" unsigned long long d2rt_cs_stat(int k){
    switch(k){ case 0: return g_csEnterFast;
               case 1: return g_csEnterIn>g_csEnterFast ? g_csEnterIn-g_csEnterFast : 0;
               case 2: return g_csLeaveFast;
               case 3: return g_csEspSaved;
               default: return 0; } }
