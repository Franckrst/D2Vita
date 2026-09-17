// pristine_audit.h -- "guest pristine" mode integrity checkers (Warden
// fidelity tests #7/#8), called from the D2_PRISTINE_CHECK/D2_PRISTINE_AUDIT
// self-test in main(). Read-only; never feeds the anti-cheat. See
// pristine_audit.cpp for what each one reports.
#pragma once
#include <cstdint>
#include <string>
namespace d2rt { struct Cpu; }

// Executable-sections-only check (test #8). Returns the modified-byte count
// (0 = pristine, -1 = error).
int pristine_check(d2rt::Cpu& c, uint32_t base, const std::string& diskpath, const char* name);
// Full-image classifying audit (test #7): headers + every section + IAT.
// Returns D2VITA_PATCH+UNKNOWN count (0 = nothing unexplained).
int pristine_audit(d2rt::Cpu& c, uint32_t base, const std::string& diskpath, const char* name);
