// cell_frame_diag.h -- per-frame diagnostic instrumentation for the cell-blit
// loop: D2_LOOPSHAPE (is the burst of cell-blit calls contiguous, and is
// there a window left for a deferred compositing thread?), D2_CAMVEC (the
// integer camera vector, read off floor-block anchors), and D2_FRAMEUS
// (sub-timers for the per-frame body: cmd_poll / input tick / present /
// rest). See cell_frame_diag.cpp for the full rationale of each.
//
// note_caller/sh_ev/sh_loop/cv_note are also called from
// native_hooks_cellengine.cpp (see runtime/rt_host.h); sh_frame/cv_tick and
// the FuScope/FuBody RAII timers are called directly from the per-frame body
// still in tools/rt_boot.cpp. g_shOn/g_cvOn stay defined in rt_boot.cpp (the
// [soak] dump / d2rt_hot_stat() also read them) — see runtime/rt_host.h.
#pragma once
#include <cstdint>

void note_caller(uint32_t ra);        // captures the cell-blit loop's return address (D2_CALLER_N slots)
void sh_ev(int fam);                  // an intruding frame write during the burst (D2_LOOPSHAPE)
void sh_loop();                       // a call to the cell-blit loop (served or fallback)
void sh_frame();                      // frame boundary (called from dumpFrame/StretchBlt)
void cv_note(uint32_t blk, int32_t Xa, int32_t Ya);   // one cell block's screen anchor (D2_CAMVEC)
void cv_tick(int frame);              // frame boundary: votes the camera vector, logs it

// D2_FRAMEUS sub-timers: RAII guards around the per-frame body's sub-phases.
struct FuScope { uint64_t t0; uint64_t* acc;
    explicit FuScope(uint64_t* a); ~FuScope(); };
struct FuBody { uint64_t t0;
    FuBody(); ~FuBody(); };
extern uint64_t g_fuCmd, g_fuInput, g_fuPresent;   // accumulators fed via FuScope(&g_fuXxx)

// D2_LOOPSHAPE aggregate counters, read by d2rt_hot_stat() (tools/rt_boot.cpp).
extern uint64_t g_shFrames, g_shSumEv, g_shSumN, g_shSumBefore,
                g_shSumIntrus, g_shSumAfter,
                g_shSumUsBurst, g_shSumUsWindow, g_shSumUsFrame;
extern uint64_t g_shSumIntrusBlit, g_shSumIntrusOther, g_shSumIntrusNodraw;
// Cell-blit loop caller census (D2_CALLER_N open-addressed table), read by
// the top-callers diagnostic dump still in tools/rt_boot.cpp.
#define D2_CALLER_N 32
extern uint32_t g_callerKey[D2_CALLER_N];
extern uint64_t g_callerHit[D2_CALLER_N];
