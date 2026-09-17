// cs_intrinsic.h -- Tier-1 dynarec intrinsics for EnterCriticalSection /
// LeaveCriticalSection / TryEnterCriticalSection (D2_CSINTRIN=1), served
// without going through Bridge::trap_handler, plus their census. See
// cs_intrinsic.cpp for the full rationale (why this does NOT repeat the two
// rejected guest-stub attempts documented in tools/rt_boot.cpp).
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; }

extern uint64_t g_csEnterIn, g_csEnterFast, g_csLeaveFast, g_csTryFast;
extern uint64_t g_csEspSaved;   // R_ESP resolutions saved (census)
extern bool     g_csIntrin;     // D2_CSINTRIN=1; default = old path; set directly by main()

// Registered via cpu->set_intrinsic() for EnterCriticalSection/
// LeaveCriticalSection/TryEnterCriticalSection.
bool cs_enter_intrinsic(d2rt::Cpu& c, uint32_t slot);
bool cs_leave_intrinsic(d2rt::Cpu& c, uint32_t slot);
bool cs_try_intrinsic(d2rt::Cpu& c, uint32_t slot);

// Prints only if the path actually served something; called at teardown.
void cs_intrin_report();
