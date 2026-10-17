// src/platform/pad_sampler.h — every controller sample between two input
// ticks, not just the last one. Pure host logic (tests/pad/pad_sampler_test.cpp).
//
// Why: on console the input tick runs from the message pump only (the Glide
// flush does not call it), and in game D2 pumps about once per rendered frame
// (console log of 14/09: alive pump=+1266 for frames=+1109 over 62 s). One
// sceCtrlPeekBufferPositive(…, 1) per tick therefore samples the pad at the
// frame rate: at 5 fps, a press shorter than ~200 ms that falls between two
// ticks was never seen at all (Discord 16/10, "inputs get dropped when the
// framerate drops"). The pad driver keeps the last 64 samples (60 Hz,
// timestamped); reading them all recovers every edge.
//
// Contract:
//   - Per button, every transition the hardware made is replayed, in order,
//     at most ONE per tick, so the glue's edge logic (b vs was) sees each
//     press as a press and each release as a release.
//   - A transition that is AHEAD of the hardware (the button was pressed and
//     released between two ticks, so after emitting "down" a "up" is still
//     pending) is followed by its next transition only once the game has
//     moved on by one frame — or after kWaitTicks ticks on a screen the game
//     no longer redraws — so the game has at least one frame with the
//     button down.
//   - When no button changes more than once between two ticks (every press
//     longer than a tick — the whole 25 fps case), the state returned is
//     exactly the newest sample: identical to the single-sample read.
#pragma once
#include <cstdint>

namespace padsmp {

struct Sample { uint64_t ts; uint32_t bits; };

// Puts a multi-buffer read in chronological order. Returns n when the
// timestamps strictly increase, n after reversing them when they strictly
// decrease, 0 when neither (copies of one sample, zero stamps — an emulator):
// the caller then reads one current sample instead, as before.
inline int order(Sample* s, int n) {
    if (n <= 1) return n;
    bool inc = true, dec = true;
    for (int i = 1; i < n; ++i) {
        if (s[i].ts <= s[i - 1].ts) inc = false;
        if (s[i].ts >= s[i - 1].ts) dec = false;
    }
    if (inc) return n;
    if (!dec) return 0;
    for (int i = 0, j = n - 1; i < j; ++i, --j) { Sample t = s[i]; s[i] = s[j]; s[j] = t; }
    return n;
}

class Latch {
public:
    enum { MAX_PEND = 6 };          // per button: 3 presses queued at most (mashing at 3 fps)
    enum { kWaitTicks = 3 };        // a static screen does not advance the frame counter

    // Chronological samples (see order()); only those newer than the last
    // one seen are fed. The very first call (or the first after
    // tick_current) feeds only the newest: history from before is not input.
    // `bits(sample)` extracts the button mask.
    template <class S, class Bits>
    uint32_t tick(const S* s, int n, uint32_t frame, Bits bits) {
        int i0 = 0;
        if (!haveTs_) i0 = n > 0 ? n - 1 : 0;
        for (int i = i0; i < n; ++i) {
            if (haveTs_ && s[i].ts <= lastTs_) continue;
            feed(bits(s[i]));
            lastTs_ = s[i].ts; haveTs_ = true;
        }
        return step(frame);
    }
    uint32_t tick(const Sample* s, int n, uint32_t frame) {
        return tick(s, n, frame, [](const Sample& x) { return x.bits; });
    }
    // Fallback when the history is unusable: one current state, no stamps.
    uint32_t tick_current(uint32_t bits, uint32_t frame) {
        feed(bits); haveTs_ = false;
        return step(frame);
    }
    uint32_t state() const { return eff_; }    // what the last tick returned
    uint32_t hw() const { return hw_; }        // newest hardware state fed

private:
    void feed(uint32_t bits) {
        uint32_t d = bits ^ hw_;
        for (int b = 0; d; ++b, d >>= 1) {
            if (!(d & 1u)) continue;
            if (++pend_[b] > MAX_PEND) pend_[b] -= 2;   // drop one press+release, keep the parity
        }
        hw_ = bits;
    }
    uint32_t step(uint32_t frame) {
        for (int b = 0; b < 32; ++b) {
            if (!pend_[b]) continue;
            const uint32_t m = 1u << b;
            if ((early_ & m) && earlyFrame_[b] == frame && waited_[b] < kWaitTicks) { ++waited_[b]; continue; }
            eff_ ^= m; --pend_[b]; waited_[b] = 0;
            if (pend_[b]) { early_ |= m; earlyFrame_[b] = frame; }
            else early_ &= ~m;
        }
        return eff_;
    }

    uint64_t lastTs_ = 0; bool haveTs_ = false;
    uint32_t hw_ = 0, eff_ = 0, early_ = 0;
    uint32_t earlyFrame_[32] = {};
    uint8_t  pend_[32] = {}, waited_[32] = {};
};

// Front touch: the same latch on one "touching" bit, plus the position of
// the newest touching sample — a tap that began and ended between two ticks
// is reported as touching for one tick, at the spot it touched.
struct TouchSample { uint64_t ts; bool down; int x, y; };

class TouchLatch {
public:
    bool tick(const TouchSample* s, int n, uint32_t frame) {
        const uint32_t b = l_.tick(s, n, frame, [this](const TouchSample& t) {
            if (t.down) { x_ = t.x; y_ = t.y; }
            return t.down ? 1u : 0u; });
        return b != 0;
    }
    bool tick_current(bool down, int x, int y, uint32_t frame) {
        if (down) { x_ = x; y_ = y; }
        return l_.tick_current(down ? 1u : 0u, frame) != 0;
    }
    int x() const { return x_; }
    int y() const { return y_; }
private:
    Latch l_; int x_ = 0, y_ = 0;
};

} // namespace padsmp
