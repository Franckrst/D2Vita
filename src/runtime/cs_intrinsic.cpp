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
// ---- D2_FASTSYNC: Enter/Leave LOCK-FREE, before the GIL --------------------
// Called from the dynarec's in-line call (cpu_box86.cpp intrin_inline_cb,
// tag bit 1) BEFORE the GIL is taken; a decline (false) has NO side effect
// and the call continues exactly as without the knob: GIL, then the
// intrinsics above, then the shim.
//
// Neither rejection R1/R2 above is reopened: the guest RTL_CRITICAL_SECTION is
// still neither read nor written (the pointer is only a key), and nothing is
// emitted into guest code. The state stays the host-side WxCrit, whose owner
// word becomes atomic in this mode (guest_sync.h, WX86_CRIT_WAITERS):
//   Enter : free -> CAS 0 -> me ; already mine -> count+1 ; otherwise
//           (held by another thread, waiter bit, unknown section) DECLINE —
//           the GIL path then retries, publishes the waiter bit and blocks
//           exactly as before (wait/notify untouched).
//   Leave : count > 1 -> count-1 ; count == 1 -> CAS me -> 0, which fails if
//           the waiter bit is set (someone may need waking): count is put
//           back and the call DECLINES to wx86_crit_leave under the GIL,
//           which releases AND notifies. The bit and the release are one
//           word, so a waiter can never be missed.
// Ordering: the CAS are seq_cst (dmb on both sides), so the guest's own
// loads/stores inside the section cannot leak out of it — the guarantee the
// GIL's mutex gave before.
// Lookup: a direct-mapped cache cs VA -> WxCrit*, filled by crit_for (under
// the GIL) and checked against k->va; a miss declines. DeleteCriticalSection
// clears its entry (the map then forgets the object and the next Init makes
// a new one). WxCrit objects are never freed, so a stale pointer can only
// fail the va check or name a section the guest is racing with its own
// Delete — undefined in Win32 too.
bool     g_csFast=false;
uint64_t g_fsEnter=0, g_fsLeave=0, g_fsTick=0;
static WxCrit* s_csCache[1024];
static inline uint32_t cs_h(uint32_t cs){ return (cs*2654435761u)>>22; }
static inline WxCrit* cs_nogil_find(uint32_t cs){
    WxCrit* k=__atomic_load_n(&s_csCache[cs_h(cs)],__ATOMIC_ACQUIRE);
    return (k && k->va==cs) ? k : nullptr; }
void cs_nogil_note(uint32_t cs, WxCrit* k){
    if(g_csFast && k) __atomic_store_n(&s_csCache[cs_h(cs)],k,__ATOMIC_RELEASE); }
void cs_nogil_forget(uint32_t cs){
    if(!g_csFast) return;
    WxCrit** e=&s_csCache[cs_h(cs)];
    WxCrit* k=__atomic_load_n(e,__ATOMIC_ACQUIRE);
    if(k && k->va==cs) __atomic_store_n(e,(WxCrit*)nullptr,__ATOMIC_RELEASE); }
bool cs_enter_nogil(Cpu::IntrinFrame& f, uint32_t /*slot*/){
    if(g_traceOn || !g_sched) return false;      // no scheduler: the NOP rule lives in the GIL path
    WxCrit* k=cs_nogil_find(f.arg0);
    if(!k) return false;
    // Native: the backend hands the running GuestThread* from this thread's
    // own CPU state (cpu.h, set_cur_guest_thread) — same object current()
    // returns, without its TLS lookup (~330 ns on console, D2_CSBENCH).
    GuestThread* t=f.thread ? static_cast<GuestThread*>(f.thread) : g_sched->current();
    if(!t || t->state==GuestThread::State::Finished) return false;   // K1 -> GIL path
    const uint32_t me=t->id;
    uint32_t s=__atomic_load_n(&k->owner,__ATOMIC_SEQ_CST);
    if((s & ~WX86_CRIT_WAITERS)==me) ++k->count;                     // recursion
    else if(s==0 && __atomic_compare_exchange_n(&k->owner,&s,me,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) ++k->count;
    else return false;                                               // held elsewhere: GIL path (blocks there)
    f.eax=0; f.pop=4; ++g_fsEnter; return true; }
bool cs_leave_nogil(Cpu::IntrinFrame& f, uint32_t /*slot*/){
    if(g_traceOn) return false;
    WxCrit* k=cs_nogil_find(f.arg0);
    if(!k) return false;
    const int c=k->count;
    if(c>1) k->count=c-1;
    else if(c==1){
        uint32_t s=__atomic_load_n(&k->owner,__ATOMIC_SEQ_CST);
        if(s==0 || (s & WX86_CRIT_WAITERS)) return false;            // someone may wait: release + notify under the GIL
        k->count=0;
        if(!__atomic_compare_exchange_n(&k->owner,&s,0u,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){
            k->count=1; return false; }                              // a waiter arrived meanwhile: same
    } else return false;                                             // count 0: the shim's no-op, as before
    f.eax=0; f.pop=4; ++g_fsLeave; return true; }
extern "C" unsigned long long d2rt_fastsync_stat(int k){
    switch(k){ case 0: return g_fsEnter; case 1: return g_fsLeave; case 2: return g_fsTick; default: return 0; } }

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
    if(g_fsEnter|g_fsLeave|g_fsTick)
        jpline("[fastsync] enter=%llu leave=%llu tick=%llu (servis sans GIL)",
            (unsigned long long)g_fsEnter,(unsigned long long)g_fsLeave,(unsigned long long)g_fsTick);
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

// ---- D2_CSBENCH=1: cost per call, measured in the shipped dispatch --------
// Exclusive branch (like D2_CSTEST): does NOT run Game.exe. One guest main
// thread runs tight loops through the REAL slots (same IAT-style
// `call [slot]`, same in-line intrinsic dispatch as the game) and a Mark
// shim timestamps each phase on the host clock:
//   P0  N x { Enter(a) ; Leave(a) }             uncontended pair, MRU hit
//   P1  N x GetTickCount
//   P2  N x { Nop(a) ; Nop(a) }                 Nop = guest `ret 4`: the cost
//                                               of a guest call alone
//   P3  N x {}                                  bare loop
//   P4  N x { E(a) E(b) L(b) L(a) }             nested: the engine MRU misses
// A dummy guest thread is created first so the per-thread emu path (TLS) is
// the one the game uses. Host micro-loops follow (GIL pair, current(),
// reg(R_ESP), crit_for, clock read). Every figure printed is a measurement of
// this run; nothing is derived from a model.
#include "runtime/gil.h"
#include "runtime/guest_thread.h"
#include <vector>
#include <cstring>
// D2_CSBENCH=2: CONTENTION stress instead (the case the uncontended timing
// never reaches): 3 guest threads x N iterations of
//     Enter(a) ; Enter(a) ; x = x + 1 (plain load / add / store) ; Leave(a) ; Leave(a)
// with no Sleep inside, so on qemu-arm native (real host threads on several
// cores) Enter/Leave race for real — lock-free acquire/release against the
// waiter bit and the GIL wake-up. PASS = the unprotected counter ends at
// exactly 3N (no lost update => exclusion), the section ends free, and every
// thread finished (no lost wake-up => no hang; the main thread polls with
// Sleep(20), capped at 60 s).
static int cs_stress_main(Cpu* cpu, Bridge& br, ThreadScheduler& sched, uint32_t stackTop, uint32_t tib, uint32_t N);
int cs_bench_main(Cpu* cpu, Bridge& br, ThreadScheduler& sched, uint32_t stackTop, uint32_t tib){
    uint32_t N=200000;
    if(const char* m=getenv("D2_CSBENCH")) if(m[0]=='2'){
        uint32_t n=20000; if(const char* e=getenv("D2_CSBENCH_N")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) n=v; }
        return cs_stress_main(cpu,br,sched,stackTop,tib,n); }
    if(const char* e=getenv("D2_CSBENCH_N")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) N=v; }
    static uint64_t s_mark[8]; static uint32_t s_markN=0;
    std::memset(s_mark,0,sizeof s_mark);
    br.register_shim("D2BENCH.dll","Mark",Shim{[](Cpu& c)->uint32_t{
        const uint32_t i=c.arg(0); if(i<8){ s_mark[i]=rt_now_us(); if(i+1>s_markN) s_markN=i+1; } return 0u; },1,true,"D2BENCH.dll!Mark"});
    const uint32_t vE=br.shim_trap_existing("KERNEL32.dll","EnterCriticalSection");
    const uint32_t vL=br.shim_trap_existing("KERNEL32.dll","LeaveCriticalSection");
    const uint32_t vT=br.shim_trap_existing("KERNEL32.dll","GetTickCount");
    const uint32_t vI=br.shim_trap("KERNEL32.dll","InitializeCriticalSection");
    const uint32_t vM=br.shim_trap("D2BENCH.dll","Mark");
    if(!vE||!vL||!vT||!vI||!vM){ jpline("CSBENCH: FAIL (creneau introuvable E=%08x L=%08x T=%08x I=%08x M=%08x)",vE,vL,vT,vI,vM); return 1; }
    const uint32_t sE=misc(4), sL=misc(4), sT=misc(4), sM=misc(4), sN=misc(4);
    cpu->write_u32(sE,vE); cpu->write_u32(sL,vL); cpu->write_u32(sT,vT); cpu->write_u32(sM,vM);
    const uint32_t csa=misc(0x20), csb=misc(0x20), cnt=misc(4);
    struct Asm { std::vector<uint8_t> b;
        void u8(uint8_t v){ b.push_back(v); }
        void u32(uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)(v>>(8*i))); }
        void push(uint32_t v){ u8(0x68); u32(v); }
        void call(uint32_t slot){ u8(0xFF); u8(0x15); u32(slot); }
        void setcnt(uint32_t a,uint32_t v){ u8(0xC7); u8(0x05); u32(a); u32(v); }
        void loop(uint32_t a,size_t top){ u8(0xFF); u8(0x0D); u32(a);            // dec [cnt]
            u8(0x0F); u8(0x85); u32((uint32_t)((int32_t)top-(int32_t)(b.size()+4))); }   // jne top
        void mark(uint32_t sM,uint32_t i){ push(i); call(sM); } };
    auto emit=[&](Asm& a)->uint32_t{ uint32_t va=misc((uint32_t)a.b.size()); cpu->write(va,a.b.data(),(uint32_t)a.b.size()); return va; };
    { Asm n; n.u8(0xC2); n.u8(0x04); n.u8(0x00); cpu->write_u32(sN, emit(n)); }   // Nop: ret 4
    Asm a; size_t top;
    a.mark(sM,0);
    a.setcnt(cnt,N); top=a.b.size(); a.push(csa); a.call(sE); a.push(csa); a.call(sL); a.loop(cnt,top);
    a.mark(sM,1);
    a.setcnt(cnt,N); top=a.b.size(); a.call(sT); a.loop(cnt,top);
    a.mark(sM,2);
    a.setcnt(cnt,N); top=a.b.size(); a.push(csa); a.call(sN); a.push(csa); a.call(sN); a.loop(cnt,top);
    a.mark(sM,3);
    a.setcnt(cnt,N); top=a.b.size(); a.loop(cnt,top);
    a.mark(sM,4);
    a.setcnt(cnt,N); top=a.b.size();
    a.push(csa); a.call(sE); a.push(csb); a.call(sE); a.push(csb); a.call(sL); a.push(csa); a.call(sL); a.loop(cnt,top);
    a.mark(sM,5);
    a.u8(0x31); a.u8(0xC0); a.u8(0xC2); a.u8(0x04); a.u8(0x00);                      // xor eax,eax ; ret 4
    const uint32_t main=emit(a);
    Asm d; d.u8(0x31); d.u8(0xC0); d.u8(0xC2); d.u8(0x04); d.u8(0x00);
    const uint32_t dummy=emit(d);
    uint32_t rr=0; const char* ff=nullptr;
    br.call_va(vI,{csa},rr,&ff); br.call_va(vI,{csb},rr,&ff);
    GuestThread* mt=sched.set_main(main, stackTop, tib);
    { gil::Guard gg; sched.create_thread(dummy,0,0,false); }
    jpline("CSBENCH: N=%u fastsync=%s csintrin=%s gil=%s", N, g_csFast?"cs":"-", g_csIntrin?"1":"0", gil::active()?"actif":"inerte");
    sched.run();
    if(s_markN<6){ jpline("CSBENCH: FAIL (marques=%u exit=0x%x)", s_markN, mt?mt->exit_code:0xdead); return 1; }
    const double n=(double)N;
    auto ns=[&](int i)->double{ return (double)(s_mark[i+1]-s_mark[i])*1000.0/n; };
    const double pE=ns(0), pT=ns(1), pN=ns(2), p0=ns(3), pX=ns(4);
    jpline("CSBENCH: par iteration (ns) : E+L=%.0f tick=%.0f nop+nop=%.0f boucle=%.0f E+E+L+L(imbrique)=%.0f",pE,pT,pN,p0,pX);
    jpline("CSBENCH: par appel, boucle deduite (ns) : Enter/Leave=%.0f GetTickCount=%.0f appel-invite-nu=%.0f imbrique=%.0f",
           (pE-p0)/2, pT-p0, (pN-p0)/2, (pX-p0)/4);
    // Host micro-loops (same host thread, after the guest threads existed).
    const uint32_t H=1000000; volatile uint32_t sink=0; uint64_t t0,t1;
    if(gil::active()){ t0=rt_now_us(); for(uint32_t i=0;i<H;++i){ gil::lock(); gil::unlock(); } t1=rt_now_us();
        jpline("CSBENCH: hote gil lock+unlock=%.1f ns", (double)(t1-t0)*1000.0/H); }
    t0=rt_now_us(); for(uint32_t i=0;i<H;++i){ GuestThread* t=sched.current(); sink+=(uint32_t)(uintptr_t)t; } t1=rt_now_us();
    jpline("CSBENCH: hote sched.current()=%.1f ns", (double)(t1-t0)*1000.0/H);
    t0=rt_now_us(); for(uint32_t i=0;i<H;++i) sink+=cpu->reg(R_ESP); t1=rt_now_us();
    jpline("CSBENCH: hote cpu->reg(R_ESP)=%.1f ns", (double)(t1-t0)*1000.0/H);
    t0=rt_now_us(); for(uint32_t i=0;i<H;++i) sink+=(uint32_t)(uintptr_t)crit_for(csa); t1=rt_now_us();
    jpline("CSBENCH: hote crit_for (meme section)=%.1f ns", (double)(t1-t0)*1000.0/H);
    t0=rt_now_us(); for(uint32_t i=0;i<H;++i) sink+=(uint32_t)(uintptr_t)crit_for((i&1)?csa:csb); t1=rt_now_us();
    jpline("CSBENCH: hote crit_for (alternees)=%.1f ns", (double)(t1-t0)*1000.0/H);
    t0=rt_now_us(); for(uint32_t i=0;i<H/10;++i) sink+=(uint32_t)rt_now_ms(); t1=rt_now_us();
    jpline("CSBENCH: hote rt_now_ms()=%.1f ns", (double)(t1-t0)*1000.0/(H/10));
    (void)sink;
    jpline("CSBENCH: FIN (exit=0x%x)", mt?mt->exit_code:0xdead);
    return 0;
}

static int cs_stress_main(Cpu* cpu, Bridge& br, ThreadScheduler& sched, uint32_t stackTop, uint32_t tib, uint32_t N){
    const uint32_t vE=br.shim_trap_existing("KERNEL32.dll","EnterCriticalSection");
    const uint32_t vL=br.shim_trap_existing("KERNEL32.dll","LeaveCriticalSection");
    const uint32_t vI=br.shim_trap("KERNEL32.dll","InitializeCriticalSection");
    const uint32_t vZ=br.shim_trap("KERNEL32.dll","Sleep");
    if(!vE||!vL||!vI||!vZ){ jpline("CSSTRESS: FAIL (creneau introuvable)"); return 1; }
    const uint32_t sE=misc(4), sL=misc(4), sZ=misc(4);
    cpu->write_u32(sE,vE); cpu->write_u32(sL,vL); cpu->write_u32(sZ,vZ);
    const uint32_t cs=misc(0x20), x=misc(4), cap=misc(4);
    uint32_t cnt[3], done[3];
    for(int i=0;i<3;++i){ cnt[i]=misc(4); done[i]=misc(4); cpu->write_u32(cnt[i],N); cpu->write_u32(done[i],0); }
    cpu->write_u32(x,0); cpu->write_u32(cap,3000);
    struct Asm { std::vector<uint8_t> b;
        void u8(uint8_t v){ b.push_back(v); }
        void u32(uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)(v>>(8*i))); }
        void push(uint32_t v){ u8(0x68); u32(v); }
        void call(uint32_t slot){ u8(0xFF); u8(0x15); u32(slot); }
        void jcc(uint8_t op,size_t top){ u8(0x0F); u8(op); u32((uint32_t)((int32_t)top-(int32_t)(b.size()+4))); } };
    auto emit=[&](Asm& a)->uint32_t{ uint32_t va=misc((uint32_t)a.b.size()); cpu->write(va,a.b.data(),(uint32_t)a.b.size()); return va; };
    auto worker=[&](int i)->uint32_t{ Asm a; const size_t top=0;
        a.push(cs); a.call(sE); a.push(cs); a.call(sE);
        a.u8(0xA1); a.u32(x); a.u8(0x40); a.u8(0xA3); a.u32(x);             // mov eax,[x] ; inc eax ; mov [x],eax
        a.push(cs); a.call(sL); a.push(cs); a.call(sL);
        a.u8(0xFF); a.u8(0x0D); a.u32(cnt[i]); a.jcc(0x85,top);              // dec [cnt] ; jne top
        a.u8(0xC7); a.u8(0x05); a.u32(done[i]); a.u32(1);
        a.u8(0x31); a.u8(0xC0); a.u8(0xC2); a.u8(0x04); a.u8(0x00);          // xor eax,eax ; ret 4
        return emit(a); };
    const uint32_t w1=worker(1), w2=worker(2);
    // main = worker 0, then polls done[1..2] with Sleep(20)
    Asm m; m.b = std::vector<uint8_t>();
    { Asm a; const size_t top=0;
      a.push(cs); a.call(sE); a.push(cs); a.call(sE);
      a.u8(0xA1); a.u32(x); a.u8(0x40); a.u8(0xA3); a.u32(x);
      a.push(cs); a.call(sL); a.push(cs); a.call(sL);
      a.u8(0xFF); a.u8(0x0D); a.u32(cnt[0]); a.jcc(0x85,top);
      a.u8(0xC7); a.u8(0x05); a.u32(done[0]); a.u32(1);
      const size_t wait=a.b.size();
      a.push(20); a.call(sZ);
      a.u8(0xA1); a.u32(done[1]); a.u8(0x23); a.u8(0x05); a.u32(done[2]);  // mov eax,[d1] ; and eax,[d2]
      a.u8(0x85); a.u8(0xC0);                                              // test eax,eax
      { a.u8(0x0F); a.u8(0x85); const size_t fix=a.b.size(); a.u32(0);     // jne out
        a.u8(0xFF); a.u8(0x0D); a.u32(cap); a.jcc(0x85,wait);              // dec [cap] ; jne wait
        a.u8(0xB8); a.u32(1); a.u8(0xC2); a.u8(0x04); a.u8(0x00);          // timeout: eax=1
        const int32_t r=(int32_t)a.b.size()-(int32_t)(fix+4);
        for(int k=0;k<4;k++) a.b[fix+k]=(uint8_t)(((uint32_t)r)>>(8*k)); }
      a.u8(0x31); a.u8(0xC0); a.u8(0xC2); a.u8(0x04); a.u8(0x00);
      m=a; }
    const uint32_t mainva=emit(m);
    uint32_t rr=0; const char* ff=nullptr; br.call_va(vI,{cs},rr,&ff);
    GuestThread* mt=sched.set_main(mainva, stackTop, tib);
    { gil::Guard gg; sched.create_thread(w1,0,0,false); sched.create_thread(w2,0,0,false); }
    const uint64_t t0=rt_now_us();
    sched.run();
    const uint32_t wall=(uint32_t)((rt_now_us()-t0)/1000);
    const uint32_t xv=cpu->read_u32(x);
    WxCrit* k=crit_for(cs);
    const uint32_t own=__atomic_load_n(&k->owner,__ATOMIC_SEQ_CST);
    const bool ok = mt && mt->exit_code==0 && xv==3*N && cpu->read_u32(done[0])==1 && cpu->read_u32(done[1])==1
                 && cpu->read_u32(done[2])==1 && k->count==0 && (own & ~WX86_CRIT_WAITERS)==0;
    jpline("CSSTRESS: %s (N=%u x 3 fils, compteur=%u attendu=%u, section count=%d owner=0x%x, fastsync=%s, wall=%ums, exit=0x%x)",
           ok?"PASS":"FAIL", N, xv, 3*N, k->count, own, g_csFast?"cs":"-", wall, mt?mt->exit_code:0xdead);
    return ok?0:1;
}
