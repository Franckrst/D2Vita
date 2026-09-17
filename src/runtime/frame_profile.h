// frame_profile.h -- the render-thread / per-frame diagnostic family:
// D2_LOOPWATCH (what the render thread does between two frames),
// D2_NOCAP (rate-limiter removal for in-game A/B benches), D2_FRAMEPROF
// (per-frame attribution of fps drops), D2_LAGWATCH (attributing a slow
// frame to guest code), D2_ASYNCPROF and D2_WAITPROF (async-load and
// wait-time latency), and the frame-profile part of "STAGE 2" (pc_line/
// ep_line: where per-frame time goes). Combined into one file because they
// share state directly (fp_now() reads g_lwWaitTot; lw_frame()/fp_tick()
// fold in the D2_ASYNCPROF/D2_WAITPROF counters for their report lines) --
// splitting them apart would mean extern-ing everything to everything. See
// frame_profile.cpp for the rationale of each knob.
//
// tools/rt_boot.cpp still hosts two "single registration site" observers,
// d2_sync_observe() and d2_wait_observe(), plus the Sleep/GetTickCount/
// WaitForSingleObject shims in main(), that touch several of the counters/
// functions below DIRECTLY (not just by calling a reporting function) --
// hence their extern exposure here even though nothing in this file's own
// logic reads them from outside.
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; class Bridge; }

// ---- D2_LOOPWATCH ------------------------------------------------------
extern uint64_t g_lwWfsoN;       // WFSO calls from the render thread, this frame (also bumped by d2_wait_observe)
extern uint64_t g_lwTickN;       // GetTickCount/timeGetTime calls from the render thread
extern uint64_t g_lwSimN;        // counted simulation steps (also declared for win32_shims_user32_d2.cpp)
bool lw_on();
uint32_t lw_tid();
void lw_count(uint64_t& ctr);                 // bumps ctr iff D2_LOOPWATCH armed and called from the render thread
void lw_peek_empty(d2rt::Cpu& c);             // an empty PeekMessage pump, from the render thread
void lw_sleep(d2rt::Cpu& c);                  // a Sleep call, from the render thread
void lw_frame(d2rt::Cpu& c);                  // frame boundary: closes the window, publishes every 10s

// D2_NETSLEEP / D2_GAMESLEEP / D2_FOGSLEEP: free/credited Sleep(0) paths in
// the Sleep shim.
int netsleep_n();
bool netsleep_on();
bool gamesleep_on();
bool fogsleep_on();
extern uint64_t g_gameSleepN, g_gameSleepMs;
extern uint64_t g_netSleepN, g_netSleepFull;
extern uint64_t g_fogSleepN;

// D2_ONLINE_CAP: online-only target frame rate (engine-side, no Game.exe byte touched).
uint32_t online_cap_period_us();
void online_cap_wait(uint32_t per);

// ---- D2_NOCAP: rate-limiter removal (installed once; state set at the
// installation site in main(), applied by nocap_apply()). ------------------
extern d2rt::Cpu* g_nocapCpu;
extern uint32_t g_nocapFog, g_nocapJeu, g_nocapGate;
extern uint8_t  g_nocapVal;
extern uint32_t g_nocapFrom;
extern bool     g_nocapDone;
void nocap_apply();

// ---- D2_ASYNCPROF: end-to-end async-load latency. asyncprof_on/ap_add are
// called directly by d2_sync_observe (which also calls the still-resident
// ap_signal()) and d2_wait_observe, both in tools/rt_boot.cpp. ------------
struct AsyncProf { uint64_t n, us, worst; };
bool asyncprof_on();
void ap_add(AsyncProf& a, uint64_t us);
extern AsyncProf g_apWork;         // request -> signaled (touched directly by ap_signal, still in rt_boot.cpp)
extern AsyncProf g_apSee;          // signaled -> seen (touched directly by d2_wait_observe)
extern uint64_t g_apSetN;          // SetEvent count (touched directly by ap_signal, still in rt_boot.cpp)
extern uint64_t g_apResetN;        // ResetEvent count (touched directly by d2_sync_observe)

// ---- D2_WAITPROF: requested vs actually-elapsed wait time, per thread. ---
struct WaitProf { uint64_t n, req_us, act_us, over_us; uint32_t wreq, wact, wtid; };
struct WSite { uint32_t eip, n, sig; char kind[12]; };
bool waitprof_on();
void wp_add(WaitProf& w, uint32_t req_ms, uint64_t act_us, uint32_t tid);
int wp_site(uint32_t eip, const char* kind);
extern WaitProf g_wpW;            // WFSO with a finite timeout (touched directly by d2_wait_observe)
extern WaitProf g_wpS;            // Sleep (touched directly by main()'s Sleep shim)
extern WSite g_wpSites[8];        // WFSO call sites (touched directly by d2_wait_observe)
extern uint64_t g_wpWinf_n, g_wpWinf_us;   // WFSO INFINITE (touched directly by d2_wait_observe)

// ---- D2_FRAMEPROF / D2_LAGWATCH -------------------------------------------
extern uint32_t g_lagMs;          // D2_LAGWATCH=<ms> threshold (set directly in main())
extern d2rt::Bridge* g_profBr;    // stage 2 of the frame profile (PROF_COUNTERS); set right after `Bridge br(cpu)`
void fp_tick(int frame);          // called once per presented frame
void fp_dump();                   // final report (called at teardown)
