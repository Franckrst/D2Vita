// tools/padsim_qemu.cpp — DIAGNOSTIC ONLY, qemu-arm only, never linked into
// the VPK nor into the default oracle binary (built by tools/padsim_qemu.sh
// into its own executable).
//
// Reproduces on the desktop "inputs get dropped when the framerate drops"
// (Discord, 16/10): the qemu build has no physical controls (the weak
// d2vita_input_tick is null), so this unit supplies one that models the
// console's pad driver — 60 Hz samples, timestamped, 64 kept — fed by a
// scripted wall-clock timeline of presses, and reads it the way the Vita tick
// does: same call sites (the message pump and, under -w, the per-frame body),
// same 30 Hz limiter, and either the single newest sample (PADSIM_MODE=old,
// the 0.2.2 read) or every sample through padsmp::Latch (PADSIM_MODE=new, the
// fix — the same header the Vita glue uses).
//
// The button is Triangle-as-inventory: each press injects keydown/keyup 'I'
// (0x49) through d2vita_inject, the path the Vita glue uses. What Game.exe
// DID with it is read back from the guest: the inventory panel flag
// (padst snapshot uiVars[1], read by the camera hook from [Game+0x3a27c0+4]).
// Each press toggles it; presses are spaced so that every toggle spans
// several frames.
//
// Knobs (all read once; nothing happens without PADSIM):
//   PADSIM=hold_ms:gap_ms:count  the presses, starting PADSIM_DELAY_MS
//                                (default 4000) after the 300th in-game frame
//   PADSIM_MODE=old|new          the read under test (default new)
//   PADSIM_NAP_MS=n              sleep n ms on every new in-game frame (slows
//                                the game; diagnostic only, default 0)
#include "runtime/pad_state.h"
#include "platform/pad_sampler.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace d2rt { struct Cpu; }
extern "C" void d2vita_inject(const char* act, int a, int b);

namespace {
uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Cfg { bool on = false, oldMode = false; uint64_t hold = 0, gap = 0, delay = 4000000; int count = 0, nap = 0; };
Cfg cfg() {
    static Cfg c; static bool done = false;
    if (done) return c;
    done = true;
    const char* e = getenv("PADSIM");
    unsigned h = 0, g = 0; int n = 0;
    if (!e || std::sscanf(e, "%u:%u:%d", &h, &g, &n) != 3 || n <= 0) return c;
    c.on = true; c.hold = (uint64_t)h * 1000; c.gap = (uint64_t)g * 1000; c.count = n;
    if (const char* m = getenv("PADSIM_MODE")) c.oldMode = !std::strcmp(m, "old");
    if (const char* d = getenv("PADSIM_DELAY_MS")) c.delay = (uint64_t)std::atoll(d) * 1000;
    if (const char* z = getenv("PADSIM_NAP_MS")) c.nap = std::atoi(z);
    std::printf("[padsim] arme: appui %u ms toutes les %u ms x%d, lecture=%s, sieste=%d ms/image\n",
                h, g, n, c.oldMode ? "old (1 echantillon)" : "new (historique)", c.nap);
    std::fflush(stdout);
    return c;
}
constexpr uint64_t kPeriod = 16667;                  // the driver samples at 60 Hz
uint64_t g_t0 = 0;                                   // first press, host us (0 = not started)
uint32_t hw_at(uint64_t t) {                         // the "physical" button at time t
    const Cfg& c = cfg();
    if (!g_t0 || t < g_t0) return 0;
    const uint64_t k = (t - g_t0) / c.gap;
    if (k >= (uint64_t)c.count) return 0;
    return (t - g_t0) % c.gap < c.hold ? 1u : 0u;
}
int peek(uint64_t now, padsmp::Sample* out, int count) {
    const uint64_t k = now / kPeriod;
    const uint64_t first = k + 1 >= (uint64_t)count ? k + 1 - (uint64_t)count : 0;
    int n = 0;
    for (uint64_t i = first; i <= k; ++i) out[n++] = padsmp::Sample{ i * kPeriod, hw_at(i * kPeriod) };
    return n;
}
} // namespace

extern "C" void d2vita_input_tick(d2rt::Cpu*) {
    const Cfg& c = cfg();
    if (!c.on) return;
    static uint32_t lastFrame = 0, frames = 0, invPrev = 0, toggles = 0, ticks = 0, injected = 0;
    static bool started = false, reported = false; static uint64_t firstTick = 0, lastTickUs = 0, maxGap = 0;
    padst::Snapshot s;
    const bool snap = padst::read(s) && s.inGame;
    const uint64_t now = now_us();
    uint64_t end = g_t0 + c.gap * (uint64_t)c.count + c.gap;         // press window + one gap of tail
    if (snap && s.frame != lastFrame) {
        lastFrame = s.frame;
        if (started && now >= g_t0 && now < end) ++frames;
        if (started) {
            static uint32_t ui[38]; static bool haveUi = false;
            if (haveUi) for (int i = 0; i < 38; ++i) if (ui[i] != s.uiVars[i])
                std::printf("[padsim] ui[%d] %u -> %u (image %u)\n", i, ui[i], s.uiVars[i], s.frame);
            std::memcpy(ui, s.uiVars, sizeof ui); haveUi = true;
            const uint32_t inv = s.uiVars[1] ? 1u : 0u;
            if (inv != invPrev) { ++toggles; invPrev = inv; }
        }
        if (c.nap > 0) std::this_thread::sleep_for(std::chrono::milliseconds(c.nap));
    }
    // Start once the world really runs: the camera hook fires from the very
    // first in-game frame, while the act is still loading for many seconds.
    // Also past the menu script's second Enter (rt_gameplay_arm_check.sh keeps
    // it as a char-select fallback): in the world it opens the chat line,
    // which would then swallow every 'I'. Closed here with a WM_CHAR Esc.
    if (snap && !started && s.frame >= 300) {
        if (s.uiVars[5]) { d2vita_inject("chr", 27, 0);       // WM_CHAR Esc: keydown 0x1B does not close it
                           std::printf("[padsim] ligne de chat ouverte par le script de menu: Echap\n"); }
        started = true; g_t0 = now + c.delay; invPrev = s.uiVars[1] ? 1u : 0u;
        end = g_t0 + c.gap * (uint64_t)c.count + c.gap;
        std::printf("[padsim] en jeu (image %u): premiers appuis dans %llu ms\n", s.frame,
                    (unsigned long long)(c.delay / 1000));
        std::fflush(stdout);
    }
    { static uint64_t lastSt = 0; static uint32_t nCalls = 0; ++nCalls;   // heartbeat, every 5 s
      if (!lastSt) lastSt = now;
      if (now - lastSt >= 5000000) { lastSt = now;
          std::printf("[padsim] pouls: appels=%u image_jeu=%u en_jeu=%d\n", nCalls, snap ? s.frame : 0, snap ? 1 : 0);
          std::fflush(stdout); nCalls = 0; } }
    // The Vita tick's own limiter (D2_INPUTHZ default 30), before any read.
    { static uint64_t lastUs = 0;
      if (lastUs && now - lastUs < 1000000ull / 30) return;
      lastUs = now; }
    if (!started) return;
    // Statistics only while the press window is open (plus one gap of tail).
    if (now >= g_t0 && now < end) {
        ++ticks;
        if (!firstTick) firstTick = now;
        if (lastTickUs && now - lastTickUs > maxGap) maxGap = now - lastTickUs;
        lastTickUs = now;
    }
    static padsmp::Latch L; static uint32_t prev = 0;
    padsmp::Sample buf[64];
    const int n = peek(now, buf, 64);
    uint32_t b;
    if (c.oldMode) b = buf[n - 1].bits;
    else           b = L.tick(buf, padsmp::order(buf, n), lastFrame);
    if ((b & 1u) && !(prev & 1u)) { d2vita_inject("keydown", 0x49, 0); ++injected;
        std::printf("[padsim] keydown I (image %u, t=%llu ms)\n", lastFrame, (unsigned long long)((now - g_t0) / 1000)); }
    if (!(b & 1u) && (prev & 1u)) d2vita_inject("keyup", 0x49, 0);
    prev = b;
    if (!reported && now >= end + 2000000) {         // 2 s for the last toggle to land
        reported = true;
        const double win = (double)(lastTickUs - firstTick) / 1e6;
        std::printf("[padsim] RESULTAT lecture=%s appui=%llums: envoyes=%d injectes=%u bascules_inventaire=%u | "
                    "images=%u (%.2f img/s) ticks=%u (%.1f ms/tick, max %llu ms)\n",
                    c.oldMode ? "old" : "new", (unsigned long long)(c.hold / 1000), c.count, injected, toggles,
                    frames, win > 0 ? frames / win : 0.0, ticks, ticks ? win * 1000.0 / ticks : 0.0,
                    (unsigned long long)(maxGap / 1000));
        std::fflush(stdout);
    }
}
