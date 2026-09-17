// src/crashreport/cr_crashtest.h — D2_CRASHTEST=native|halt: deliberately,
// quickly and safely trigger one real instance of each of two main crash
// Kinds (cr_types.h) so the maintainer can prove the whole reporting
// pipeline (dialog, consent, upload, server-side dedup on a second identical
// trigger) on real hardware without having to reproduce an organic bug.
//
// Only native and halt are supported: a third value, "hang"
// (CrashTestKind::Hang, parking the native-scheduler thread forever), had no
// counterpart left once automatic hang detection itself was removed
// (cr_evidence.cpp/.h; real hardware showed the general watchdog already
// covers this need without a dedicated crash-report path). D2_CRASHTEST=hang
// is now an unrecognized value like any typo: CrashTestKind::None, behave
// exactly like a normal boot.
//
// This whole file compiles to NOTHING unless -DD2V_CRASHTEST is passed
// (tools/build_rt_boot_vpk.sh D2V_CRASHTEST=1) — a normal/shipping build
// never defines it, so this header is invisible there: zero declarations,
// zero .text. tools/tests/crashtest_kind_test.cpp defines the macro for
// itself to unit-test the parser below without touching any shared build
// flag.
//
// This header holds only the PURE, host-testable part (env value -> Kind).
// The actual trigger logic needs live Cpu/Bridge/watchdog state that only
// exists in the real boot path, so it lives in tools/rt_boot.cpp, right next
// to the other env-var-gated diagnostics (D2_EIPTRAP, D2_HALTWATCH,
// D2_CRCWATCH) whose convention it follows: getenv() read once, cheap when
// unset.
#pragma once
#ifdef D2V_CRASHTEST
#include <cstring>

namespace d2cr {

enum class CrashTestKind { None, Native, Halt };

// D2_CRASHTEST=native|halt (env.txt-settable, same convention as
// D2SCHED=native|coop: read by exact VALUE, case-sensitive, no partial
// match). Anything else — unset, empty, a typo (including the former
// "hang", now retired), trailing space — is None: behave exactly like a
// normal boot. This is the ONLY gate: callers must not re-implement the
// string comparison.
inline CrashTestKind crashtest_kind_from_env(const char* v) {
    if (!v || !*v) return CrashTestKind::None;
    if (!std::strcmp(v, "native")) return CrashTestKind::Native;
    if (!std::strcmp(v, "halt"))   return CrashTestKind::Halt;
    return CrashTestKind::None;
}

inline const char* crashtest_kind_name(CrashTestKind k) {
    switch (k) {
        case CrashTestKind::Native: return "native";
        case CrashTestKind::Halt:   return "halt";
        default:                    return "none";
    }
}

// Trigger predicate for d2crashtest_tick (tools/rt_boot.cpp): fire the FIRST
// time the observed per-frame counter reaches or passes threshold_frame,
// rather than only on an exact match. d2crashtest_tick is fed that counter
// (g_frame) from TWO independent per-frame hook sites (dumpFrame and
// d2vGlideFlush) that share it: depending on which rendering path a boot
// takes and how the two sites interleave, one specific frame VALUE can be
// observed by neither call (the counter is already past it by the time
// either site reads it), even though the counter itself is monotonically
// increasing and definitely crosses that value. A >= comparison can never
// miss a monotonic counter
// that crosses threshold_frame; a bare == can miss any single skipped value
// — see crashtest_kind_test.cpp for a contrived skip sequence that
// reproduces exactly that difference. Pure and host-testable on purpose,
// same rationale as crashtest_kind_from_env above; the caller still owns the
// separate "at most once per process" latch (rt_boot.cpp's `fired`).
inline bool crashtest_should_fire(int observed_frame, int threshold_frame) {
    return observed_frame >= threshold_frame;
}

}  // namespace d2cr
#endif  // D2V_CRASHTEST
