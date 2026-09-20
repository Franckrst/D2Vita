// src/platform/vita_present.h — PS Vita presentation + platform glue for the
// rt_boot runtime. On non-Vita builds every hook is an inline no-op so the
// desktop/qemu harness is unchanged.
#pragma once
#include <cstdint>

#ifdef __vita__
// Progress path differs per build flavour so A/B VPKs installed side by side
// never clobber each other's log. Defined here (not in a .cpp) because
// multiple translation units need it; a second definition would risk drifting.
#if defined(D2VPK_TAG)
// Generic A/B flavor mechanism instead of one #ifdef per variant: D2VPK_TAG
// names the flavor (separate log + save folder), D2VPK_OFF lists knobs to
// force to 0. See tools/build_rt_boot_vpk.sh.
#define D2VITA_PROGRESS_PATH "ux0:data/d2vita/boot_progress_" D2VPK_TAG ".txt"
#else
#define D2VITA_PROGRESS_PATH "ux0:data/d2vita/boot_progress.txt"
#endif

// Set up SceDisplay double-buffered framebuffer (960x544 A8B8G8R8) once.
void d2vita_present_init();
// Convert an 8bpp palette-indexed DIB (Windows RGBQUAD palette: B,G,R,0) to
// RGBA, scale to the Vita screen, and flip. bpp==32 blits the 32bpp DIB
// directly. Called from rt_boot's frame path.
void d2vita_present(const uint8_t* pixels, int w, int h, int bpp,
                    const uint8_t* palette /*256*4 BGRA, may be null*/);
// Draws overlays (frame counter, virtual keyboard) directly into an
// already-filled 960x544 buffer. Needed on the sceGxm path, where the GPU
// wrote the frame and d2vita_present is never called (Glide emits no BitBlt).
void d2vita_overlay(uint32_t* fb);
// État du menu radial, pour que le chemin sceGxm le dessine SUR LE GPU au lieu
// de le mélanger pixel par pixel dans la CDRAM (~81 ms par image, mesuré au
// journal console). Renvoie null quand le menu est fermé. Le blitter CPU de
// radial_menu.h reste le repli quand le chemin GPU n'est pas armé.
namespace radial_menu { struct State; }
const radial_menu::State* d2vita_radial_state();
// Bake the runtime config (env + argv) before boot; returns the data dir.
const char* d2vita_platform_init();
// Append a milestone line to ux0:data/d2vita/boot_progress.txt (durable: the
// only reliable way to observe headless boot progress on Vita).
void d2vita_progress(const char* msg);
// Start a 30s heartbeat thread logging liveness counters (message-pump count,
// game frames) to the progress file — distinguishes "slow" from "hung" on
// hardware where nothing else is observable.
// fam_detections/fam_naps: anti-starvation counters (native only — coop mode
// passes null and the alive: line omits the fam= field instead of a
// misleading zero).
void d2vita_watchdog_stop();
// Clean shutdown of the pipelined presentation thread (D2_PRESENTTHREAD);
// no-op when unset. Call together with d2vita_watchdog_stop(), otherwise a
// thread still running during teardown touches pointers freed under it.
void d2vita_present_stop();
void d2vita_watchdog_start(const unsigned long long* pump_count, const int* frames, const unsigned long long* reads,
                           const unsigned int* eip, const unsigned long long* switches,
                           const unsigned long long* io_us, const unsigned long long* jit_ns,
                           const unsigned int* jit_count,
                           const unsigned int* fam_detections, const unsigned int* fam_naps);
// Monotonic process time in ms / real sleep — drive the guest clock at real
// speed on hardware (the virtual clock races at emulation speed there).
uint64_t d2vita_now_ms();
uint64_t d2vita_now_us();
void d2vita_sleep_ms(uint32_t ms);

// ---- HOST CORE ASSIGNMENT (D2_COEURS) --------------------------------------
// Three auxiliary host threads run alongside the guest runners (all pinned to
// USER_0): presentation (~2 ms/frame), the watchdog (wakes every 10 s), and
// the anti-starvation heartbeat (wakes at least every 50 ms). All three sit
// on USER_2 — the same core the fork-join's second worker lands on
// (D2_CELLPARCPU, default "12") — and sharing it was measured to cost nothing.
//
//   D2_COEURS=<3 digits>   position 0 = presentation, 1 = watchdog, 2 = heartbeat.
//                          each digit 0/1/2/3 => mask 0x10000 / 0x20000 /
//                          0x40000 / 0x80000. Default "222" matches the
//                          current placement exactly (no affinity call
//                          changes).
//                          Digit 3 (4th core) is unproven: an activeCpuMask
//                          readback on console showed four active bits, but
//                          the 0x80000 bit had never actually been requested.
//                          The 10s window's "4th core probe" settles this —
//                          don't use 3 until it reports ACCEPTED on hardware.
//
// The heartbeat must never run on USER_0: the kernel scheduler is
// run-to-block, so a same-priority thread sharing the guest runners' core
// never gets scheduled while a runner keeps running — but the heartbeat
// exists precisely for when a guest thread stops blocking. Putting it on
// USER_0 would make the safety net inert exactly when it's needed. The value
// is still accepted (the knob doesn't validate) but it's flagged in the log,
// since a silently inert safety net is worse than a rejected one.
//
// who: 0 = presentation, 1 = watchdog, 2 = anti-starvation heartbeat.
int  d2vita_core_mask(int who);
// Registers a host thread for the 10s window's "coeurs:" line: the mask it
// requested and the rc it got. The mask and actual running core are
// re-sampled every window, since a requested pin is not always an obtained
// one on hardware.
void d2vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc);
#else
static inline void d2vita_present_init() {}
static inline void d2vita_present(const uint8_t*, int, int, int, const uint8_t*) {}
static inline void d2vita_overlay(uint32_t*) {}
static inline const char* d2vita_platform_init() { return nullptr; }
static inline void d2vita_progress(const char*) {}
static inline void d2vita_watchdog_stop() {}
static inline void d2vita_present_stop() {}
static inline void d2vita_watchdog_start(const unsigned long long*, const int*, const unsigned long long*, const unsigned int*, const unsigned long long*,
                                         const unsigned long long*, const unsigned long long*, const unsigned int*,
                                         const unsigned int*, const unsigned int*) {}
static inline uint64_t d2vita_now_ms() { return 0; }
static inline uint64_t d2vita_now_us() { return 0; }
static inline void d2vita_sleep_ms(uint32_t) {}
// D2_COEURS is meaningless off-target (no cores to assign, no affinity to
// read back); these no-op stubs let call sites skip #ifdef guards.
static inline int  d2vita_core_mask(int) { return 0; }
static inline void d2vita_core_register(const char*, int, unsigned, int) {}
#endif
