// cs_intrinsic.h -- Tier-1 dynarec intrinsics for EnterCriticalSection /
// LeaveCriticalSection / TryEnterCriticalSection (D2_CSINTRIN=1), served
// without going through Bridge::trap_handler, plus their census. See
// cs_intrinsic.cpp for the full rationale (why this does NOT repeat the two
// rejected guest-stub attempts documented in tools/rt_boot.cpp).
#pragma once
#include <cstdint>
#include "runtime/cpu.h"

extern uint64_t g_csEnterIn, g_csEnterFast, g_csLeaveFast, g_csTryFast;
extern uint64_t g_csEspSaved;   // R_ESP resolutions saved (census)
extern bool     g_csIntrin;     // D2_CSINTRIN=1; default = old path; set directly by main()

// Registered via cpu->set_intrinsic() for EnterCriticalSection/
// LeaveCriticalSection/TryEnterCriticalSection.
bool cs_enter_intrinsic(d2rt::Cpu& c, uint32_t slot);
bool cs_leave_intrinsic(d2rt::Cpu& c, uint32_t slot);
bool cs_try_intrinsic(d2rt::Cpu& c, uint32_t slot);

// ---- D2_FASTSYNC: lock-free Enter/Leave, served BEFORE the GIL -----------
// (cs_intrinsic.cpp has the full contract). g_csFast is set once at install.
extern bool g_csFast;
class WxCrit;
void cs_nogil_note(uint32_t cs, WxCrit* k);    // publish (called by crit_for, under the GIL)
void cs_nogil_forget(uint32_t cs);             // DeleteCriticalSection
bool cs_enter_nogil(d2rt::Cpu::IntrinFrame& f, uint32_t slot);
bool cs_leave_nogil(d2rt::Cpu::IntrinFrame& f, uint32_t slot);
extern uint64_t g_fsEnter, g_fsLeave, g_fsTick;   // calls served lock-free (statistics, racy by design)

// D2_CSBENCH=1: exclusive cost-per-call bench (does not run Game.exe).
namespace d2rt { class Bridge; class ThreadScheduler; }
int cs_bench_main(d2rt::Cpu* cpu, d2rt::Bridge& br, d2rt::ThreadScheduler& sched, uint32_t stackTop, uint32_t tib);

// Prints only if the path actually served something; called at teardown.
void cs_intrin_report();
