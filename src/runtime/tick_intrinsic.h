// tick_intrinsic.h -- Tier-1 dynarec intrinsic for GetTickCount/timeGetTime,
// serviced inline in the trap dispatch without the Bridge/std::function shim
// round-trip, plus the D2_B5CLOCK "grouped trap exit" leg and the
// D2_CLOCKLOG sequence oracle. See tick_intrinsic.cpp for the rationale.
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; }

extern uint64_t g_tickIntrinHits;
extern bool g_b5clock;             // D2_B5CLOCK/D2_B5: set directly by main() once env.txt is loaded
extern uint32_t g_clockLogN;       // D2_CLOCKLOG=N: set directly by main()
extern uint64_t g_clockSeqCnt;
extern uint64_t g_clockSeqHash;
extern uint32_t g_clockSeqFirst[32];

// Registered via cpu->set_intrinsic(trapVA, &tick_intrinsic) for both
// KERNEL32!GetTickCount and WINMM!timeGetTime.
bool tick_intrinsic(d2rt::Cpu& c, uint32_t slot);
