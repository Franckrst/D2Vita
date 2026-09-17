// tick_intrinsic.cpp -- see tick_intrinsic.h.
//
// ---- Tier-1 intrinsic: GetTickCount / timeGetTime -------------------------
// Serviced inline in the dynarec trap dispatch (see Cpu::set_intrinsic) WITHOUT
// the Bridge/std::function shim round-trip. Byte-identical to the shim body
// (K("GetTickCount")/timeGetTime = tick_bump(c,1), argc=0 stdcall): same EAX,
// same tick-page mutation, same ret. No IAT/guest-byte change — Warden-safe.
// Falls back to the slow shim when tracing (so the trace log stays complete).
#include "tick_intrinsic.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
using namespace d2rt;

uint64_t g_tickIntrinHits=0;
// ---- Grouped trap exit for the clock path ----------------------------------
// D2_B5CLOCK=1 (or D2_B5=1). DEFAULT = OLD PATH, line for line.
// The B leg replaces the trap_retaddr() + trap_epilogue() pair with the
// single trap_ret0(): ONE state resolution per thread instead of two. On the
// Vita toolchain (--disable-tls) each resolution is a cross-module
// __emutls_get_address call; that is the cost this lever removes.
// REORDERING, and why it's neutral: the legacy leg reads the return address
// BEFORE calling tick_bump, the B leg reads it after. tick_bump only touches
// the tick page (two dwords allocated by misc(), off the stack) and no
// register, so the value read off the top of the stack is the same in either
// order. This is what the SEQUENCE oracle verifies at runtime.
bool g_b5clock=false;
// SEQUENCE log (D2_CLOCKLOG=N): an error on this path doesn't show as a wrong
// pixel but as desync. So the ORDERED sequence of values served to the game
// (the first N) is hashed, and the first 32 are printed in the clear: two legs
// producing the same hash AND the same 32 values served the same clock, in
// the same order.
uint32_t g_clockLogN=0;
uint64_t g_clockSeqCnt=0;
uint64_t g_clockSeqHash=1469598103934665603ull;   // FNV-1a 64
uint32_t g_clockSeqFirst[32]={};
static inline void clock_seq_note(uint32_t v){
    if(g_clockSeqCnt<32) g_clockSeqFirst[g_clockSeqCnt]=v;
    g_clockSeqHash^= (uint64_t)v; g_clockSeqHash*=1099511628211ull;
    ++g_clockSeqCnt;
}
bool tick_intrinsic(Cpu& c, uint32_t /*slot*/){
    if(g_traceOn) return false;                 // let the shim log this call
    if(g_b5clock){
        const uint32_t eax = tick_bump(c,1);
        c.trap_ret0(eax);                       // EAX, pops the return address, EIP
        if(g_clockLogN && g_clockSeqCnt<g_clockLogN) clock_seq_note(eax);
        ++g_tickIntrinHits;
        return true;
    }
    // GROUPED ENTRY/EXIT (cpu.h): 2 state resolutions per thread and 2 virtual
    // dispatches instead of 4 and 5. The DEFAULT implementations of
    // trap_retaddr/trap_epilogue ARE exactly the old code (reg(R_ESP) +
    // read_u32, then set_reg EAX/ESP/EIP) — so the change is neutral by
    // construction; the Box86 backend overrides them to resolve the emu state
    // only once.
    // "ESP read at entry + 4" == "current ESP + 4": tick_bump only touches the
    // tick page (read_u32/write_u32), never a register.
    const uint32_t ret = c.trap_retaddr();      // return address (call just executed)
    const uint32_t eax = tick_bump(c,1);
    c.trap_epilogue(eax, 4, ret);               // EAX, pop retaddr (argc=0), EIP
    if(g_clockLogN && g_clockSeqCnt<g_clockLogN) clock_seq_note(eax);
    ++g_tickIntrinHits;
    return true;
}
