// tests/pad/pad_sampler_test.cpp — desktop tests for src/platform/pad_sampler.h
// Plain asserts, no framework: exits 0 when green.
//
// The low-fps cases model the console as it runs (see the header): the pad
// driver samples at 60 Hz into a 64-entry history, the input tick runs once
// per rendered frame (the message pump), and the game acts on a press only if
// it sees the button down at a tick. "old" = the single newest sample per tick
// (what the glue did before), "new" = the Latch over the whole history.
#include "platform/pad_sampler.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A press = [t0, t0+hold) in microseconds, on bit `bit`.
struct Press { uint64_t t0, hold; uint32_t bit; };

// The hardware: 60 Hz samples, stamp = sample time, newest last, 64 kept.
struct Pad {
    std::vector<Press> p;
    static constexpr uint64_t kPeriod = 16667;
    uint32_t at(uint64_t t) const {
        uint32_t b = 0;
        for (const Press& x : p) if (t >= x.t0 && t < x.t0 + x.hold) b |= x.bit;
        return b;
    }
    int peek(uint64_t now, padsmp::Sample* out, int count) const {
        uint64_t k = now / kPeriod;                  // index of the newest sample
        int n = 0;
        uint64_t first = k + 1 >= (uint64_t)count ? k + 1 - (uint64_t)count : 0;
        for (uint64_t i = first; i <= k; ++i) out[n++] = padsmp::Sample{ i * kPeriod + 1, at(i * kPeriod) };
        return n;
    }
};

struct Result { int sentPress = 0, oldSeen = 0, newSeen = 0; bool sameAsOld = true; int maxLagTicks = 0; };

// Runs `ticks` ticks spaced `tickUs` apart (+ jitter pattern), counts press
// edges per bit as the glue would see them.
Result run(const Pad& pad, uint64_t tickUs, int ticks, uint64_t start = 100000) {
    Result r; r.sentPress = (int)pad.p.size();
    padsmp::Latch L; uint32_t oldPrev = 0, newPrev = 0;
    padsmp::Sample buf[64];
    for (int i = 0; i < ticks; ++i) {
        const uint64_t now = start + (uint64_t)i * tickUs;
        const int n = pad.peek(now, buf, 64);
        const uint32_t oldB = buf[n - 1].bits;           // single newest sample
        const int no = padsmp::order(buf, n);
        const uint32_t newB = L.tick(buf, no, (uint32_t)i);   // one frame per tick
        for (uint32_t m = 1; m; m <<= 1) {
            if ((oldB & m) && !(oldPrev & m)) ++r.oldSeen;
            if ((newB & m) && !(newPrev & m)) ++r.newSeen;
        }
        if (oldB != newB) r.sameAsOld = false;
        oldPrev = oldB; newPrev = newB;
    }
    return r;
}

// Presses of `hold` us every `gap` us, alternating two buttons (so the
// count of presses per button is exact even if two overlap).
Pad mkPad(uint64_t hold, uint64_t gap, int n, uint64_t t0 = 300000) {
    Pad p;
    for (int i = 0; i < n; ++i) p.p.push_back(Press{ t0 + (uint64_t)i * gap, hold, (i & 1) ? 0x4000u : 0x1000u });
    return p;
}

void test_order() {
    padsmp::Sample a[3] = { {1, 0}, {2, 1}, {3, 0} };
    CHECK(padsmp::order(a, 3) == 3 && a[0].ts == 1);
    padsmp::Sample b[3] = { {3, 0}, {2, 1}, {1, 2} };
    CHECK(padsmp::order(b, 3) == 3 && b[0].ts == 1 && b[2].bits == 0);
    padsmp::Sample c[3] = { {5, 0}, {5, 1}, {5, 2} };     // copies (emulator): unusable
    CHECK(padsmp::order(c, 3) == 0);
    padsmp::Sample d[2] = { {0, 0}, {0, 1} };             // zero stamps
    CHECK(padsmp::order(d, 2) == 0);
    padsmp::Sample e[1] = { {0, 7} };
    CHECK(padsmp::order(e, 1) == 1);
}

// 25 fps (40 ms per tick) and 30 fps: presses and gaps longer than a tick
// give EXACTLY the old per-tick state, tick for tick.
void test_unchanged_at_25_and_30_fps() {
    for (uint64_t tick : { 40000ull, 33333ull }) {
        for (uint64_t hold : { 50000ull, 80000ull, 150000ull, 400000ull }) {
            Pad p = mkPad(hold, hold + 60000, 20);
            Result r = run(p, tick, 400);
            CHECK(r.sameAsOld);
            CHECK(r.newSeen == r.sentPress && r.oldSeen == r.sentPress);
        }
    }
    // stick-like long hold and nothing at all
    { Pad p; p.p.push_back(Press{ 200000, 3000000, 0x100 }); Result r = run(p, 40000, 200); CHECK(r.sameAsOld && r.newSeen == 1); }
    { Pad p; Result r = run(p, 40000, 100); CHECK(r.sameAsOld && r.newSeen == 0); }
}

// The bug: at 5 fps (200 ms per tick) and 3 fps, a short press falls between
// two ticks and the old read never sees it.
void test_low_fps_presses_are_all_seen() {
    struct Case { uint64_t tick, hold; };
    for (Case c : { Case{200000, 50000}, Case{200000, 100000}, Case{200000, 150000},
                    Case{333333, 80000}, Case{333333, 200000}, Case{100000, 40000} }) {
        Pad p = mkPad(c.hold, 713000, 30, 310000);   // 713 ms: the press phase drifts against the ticks
        Result r = run(p, c.tick, (int)(30 * 713000 / c.tick) + 20);
        std::printf("  tick %3llu ms, hold %3llu ms: %2d sent, old %2d seen, new %2d seen\n",
                    (unsigned long long)(c.tick / 1000), (unsigned long long)(c.hold / 1000),
                    r.sentPress, r.oldSeen, r.newSeen);
        CHECK(r.newSeen == r.sentPress);
        CHECK(r.oldSeen < r.sentPress);         // the old read really does drop some
    }
}

// Two presses of the same button inside one 333 ms frame: both are replayed,
// each down for one tick, in order.
void test_double_tap_inside_one_frame() {
    Pad p;
    p.p.push_back(Press{ 410000, 40000, 0x10 });
    p.p.push_back(Press{ 500000, 40000, 0x10 });
    padsmp::Latch L; padsmp::Sample buf[64];
    std::vector<uint32_t> seq;
    for (int i = 0; i < 8; ++i) {
        const uint64_t now = 400000 + (uint64_t)i * 333333;
        int n = p.peek(now, buf, 64); n = padsmp::order(buf, n);
        seq.push_back(L.tick(buf, n, (uint32_t)i) & 0x10);
    }
    // tick 0 at 400 ms: nothing yet. tick 1 (733 ms): both presses are over.
    CHECK(seq[0] == 0);
    CHECK(seq[1] == 0x10 && seq[2] == 0 && seq[3] == 0x10 && seq[4] == 0 && seq[5] == 0);
}

// A replayed transition waits for the next frame; on a screen the game no
// longer redraws (frame counter frozen) it waits kWaitTicks ticks at most.
void test_latched_release_waits_for_a_frame() {
    padsmp::Sample s[3] = { {10, 0}, {11, 1}, {12, 0} };
    { padsmp::Latch L; padsmp::Sample z = {9, 0}; L.tick(&z, 1, 0);
      CHECK(L.tick(s, 3, 1) == 1);                  // press replayed
      CHECK(L.tick(s, 3, 1) == 1);                  // same frame: still down
      CHECK(L.tick(s, 3, 2) == 0); }                // next frame: released
    { padsmp::Latch L; padsmp::Sample z = {9, 0}; L.tick(&z, 1, 7);
      CHECK(L.tick(s, 3, 7) == 1);
      int held = 0;
      for (int i = 0; i < 10 && L.tick(s, 3, 7) == 1; ++i) ++held;
      CHECK(held == padsmp::Latch::kWaitTicks); }   // frozen frame: bounded wait
    // A press that is still held at the tick is NOT a replay: its release
    // follows the hardware at once, as before (no frame wait).
    { padsmp::Latch L; padsmp::Sample a[2] = { {1, 0}, {2, 1} }; L.tick(a, 2, 0);
      padsmp::Sample b2[1] = { {3, 1} }; CHECK(L.tick(b2, 1, 0) == 1);
      padsmp::Sample c[1] = { {4, 0} }; CHECK(L.tick(c, 1, 0) == 0); }
}

// The first tick only takes the newest sample: what happened before the
// input layer started is not input. Same after a fallback tick.
void test_history_before_start_is_ignored() {
    padsmp::Latch L;
    padsmp::Sample s[4] = { {1, 0}, {2, 8}, {3, 0}, {4, 0} };
    CHECK(L.tick(s, 4, 0) == 0);
    padsmp::Sample t[4] = { {1, 0}, {2, 8}, {3, 0}, {4, 2} };
    padsmp::Latch M;
    CHECK(M.tick(t, 4, 0) == 2);                    // a button held at start is a press, as before
    CHECK(M.tick_current(0, 1) == 0);               // fallback: plain current state
    padsmp::Sample u[3] = { {5, 1}, {6, 0}, {7, 4} };
    CHECK(M.tick(u, 3, 2) == 4);                    // after a fallback: newest only again
}

// Mashing at 3 fps: the backlog per button is bounded, parity kept (the
// button ends up released when it is released).
void test_backlog_is_bounded() {
    padsmp::Latch L; padsmp::Sample z = {1, 0}; L.tick(&z, 1, 0);
    padsmp::Sample s[20]; for (int i = 0; i < 20; ++i) s[i] = padsmp::Sample{ (uint64_t)(2 + i), (uint32_t)(i & 1) };
    // 10 presses inside one window; ends released (s[19].bits = 1? -> make it 0)
    s[19].bits = 0;
    int presses = 0; uint32_t prev = 0, cur = 0;
    for (int f = 1; f < 40; ++f) { cur = L.tick(s, 20, (uint32_t)f); if (cur && !prev) ++presses; prev = cur; }
    CHECK(presses == padsmp::Latch::MAX_PEND / 2);
    CHECK(cur == 0);
}

// Touch: a tap that began and ended between two ticks is one touching tick at
// the spot it touched; a held touch follows the newest sample as before.
void test_touch_tap_between_ticks() {
    padsmp::TouchLatch T;
    padsmp::TouchSample a[1] = { {1, false, 0, 0} };
    CHECK(!T.tick(a, 1, 0));
    padsmp::TouchSample b[4] = { {1, false, 0, 0}, {2, true, 500, 300}, {3, true, 505, 302}, {4, false, 0, 0} };
    CHECK(T.tick(b, 4, 1) && T.x() == 505 && T.y() == 302);
    CHECK(!T.tick(b, 4, 2));
    padsmp::TouchSample c[2] = { {5, true, 100, 100}, {6, true, 120, 110} };
    CHECK(T.tick(c, 2, 3) && T.x() == 120 && T.y() == 110);
    CHECK(T.tick_current(true, 130, 115, 4) && T.x() == 130);
    CHECK(!T.tick_current(false, 0, 0, 5) && T.x() == 130);
}

} // namespace

int main() {
    test_order();
    test_unchanged_at_25_and_30_fps();
    test_low_fps_presses_are_all_seen();
    test_double_tap_inside_one_frame();
    test_latched_release_waits_for_a_frame();
    test_history_before_start_is_ignored();
    test_backlog_is_bounded();
    test_touch_tap_between_ticks();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
