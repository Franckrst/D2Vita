// tier1_intrinsics_install.h -- installs the Tier-1 Warden-safe dynarec
// intrinsics: the GetTickCount/timeGetTime clock and the D2_CSINTRIN
// critical sections. Both point the dynarec's trap dispatch at an inline
// handler; neither writes a byte of guest memory. See
// tier1_intrinsics_install.cpp for the full rationale.
#pragma once
namespace d2rt { struct Cpu; class Bridge; }

void tier1_clock_cs_intrinsics_install(d2rt::Cpu* cpu, d2rt::Bridge& br);
