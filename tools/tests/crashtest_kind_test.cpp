// tools/tests/crashtest_kind_test.cpp — host test for the pure D2_CRASHTEST
// value parser AND the frame-threshold trigger predicate
// (src/crashreport/cr_crashtest.h). That header's entire content is gated
// behind -DD2V_CRASHTEST (a normal build never sets it); this test defines
// the macro FOR ITSELF ONLY, exactly the way a unit test turns on the one
// thing it means to exercise — it does not touch tools/build_rt_boot_vpk.sh
// or any shared build flag.
#define D2V_CRASHTEST 1
#include "crashreport/cr_crashtest.h"

#include <cstdio>
#include <cstring>

using namespace d2cr;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)
#define CHECK_NAME(k, n) CHECK(std::strcmp(crashtest_kind_name(k), n) == 0)

int main() {
    // unset / empty / unknown -> None (behave exactly like a normal boot)
    CHECK(crashtest_kind_from_env(nullptr) == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("0") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("bogus") == CrashTestKind::None);

    // the two real values
    CHECK(crashtest_kind_from_env("native") == CrashTestKind::Native);
    CHECK(crashtest_kind_from_env("halt") == CrashTestKind::Halt);
    // "hang" was a third real value until automatic hang detection was
    // removed (the general watchdog already covers a genuine freeze without
    // a dedicated crash-report path), and D2_CRASHTEST=hang along with it —
    // it is now just another unrecognized value, exactly like "bogus" above.
    CHECK(crashtest_kind_from_env("hang") == CrashTestKind::None);

    // case-sensitive, no prefix/suffix match (env.txt values are exact, like D2SCHED=native|coop)
    CHECK(crashtest_kind_from_env("Native") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("HALT") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("natives") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("nat") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env(" halt") == CrashTestKind::None);
    CHECK(crashtest_kind_from_env("halt ") == CrashTestKind::None);

    CHECK_NAME(CrashTestKind::None, "none");
    CHECK_NAME(CrashTestKind::Native, "native");
    CHECK_NAME(CrashTestKind::Halt, "halt");

    // crashtest_should_fire: threshold (>=), not exact-match — the fix for
    // the hardware-observed unreliable trigger (d2crashtest_tick fed g_frame
    // from two per-frame hook sites that share the counter; an exact frame
    // value can be skipped even though the counter itself is monotonic and
    // does cross it). Basic threshold semantics:
    CHECK(!crashtest_should_fire(19, 20));   // before threshold: no
    CHECK(crashtest_should_fire(20, 20));    // exact match: yes (unchanged from the old behavior)
    CHECK(crashtest_should_fire(21, 20));    // past threshold: yes (this is the actual fix)
    CHECK(crashtest_should_fire(2000, 20));  // long past threshold (a boot observed well past it): yes

    // Contrived reproduction of the hardware bug: a monotonically increasing
    // sequence of observed frame values that SKIPS the exact threshold value
    // entirely (18, 19, 21, 22 — 20 never appears), modeling a run where the
    // two call sites sharing g_frame interleaved such that neither ever
    // observed exactly 20. The OLD exact-match check (`frame == 20`) never
    // fires on this sequence — reproducing "ran past frame 2000+ without
    // ever triggering" from a single skipped value. The NEW threshold check
    // fires at the very next observed value (21).
    {
        const int observed[] = {18, 19, 21, 22};
        bool old_exact_match_fired = false, new_threshold_fired = false;
        int new_fired_at = 0;
        for (int f : observed) {
            if (f == 20) old_exact_match_fired = true;
            if (!new_threshold_fired && crashtest_should_fire(f, 20)) { new_threshold_fired = true; new_fired_at = f; }
        }
        CHECK(!old_exact_match_fired);              // reproduces the bug: exact 20 really was skipped
        CHECK(new_threshold_fired);                  // the fix: still fires despite the skip
        CHECK(new_fired_at == 21);                   // at the first observed value at/past threshold
    }

    if (g_fail) { std::printf("FAIL: %d check(s)\n", g_fail); return 1; }
    std::printf("OK: D2_CRASHTEST kind parsing + frame-threshold trigger\n");
    return 0;
}
