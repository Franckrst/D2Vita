// rt_host.h — what tools/rt_boot.cpp PROVIDES to the extracted units
// (glide_ring/gx_host.cpp, runtime/phase_hooks.cpp, runtime/native_hooks_codec.cpp,
// runtime/native_hooks_cellengine.cpp).
// Declarations only; definitions stay in rt_boot.cpp (they just lost their
// `static` there, nothing else changed).
#pragma once
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <string>
namespace d2rt { class ThreadScheduler; class CooperativeScheduler; class Waitable; struct Cpu; class Bridge; }
namespace wx86 { class GuestRegion; }
struct GMsg { uint32_t msg,wp,lp; };   // injected/posted USER32 message (see g_msgQ)

uint64_t rt_now_us();                        // monotonic host clock (us)
uint64_t rt_now_ms();                        // monotonic host clock (ms)
void jpline(const char* fmt, ...);           // stdout + d2vita_progress
void d2_crashlog(const char* fmt, ...);      // $D2WRITE/crash.log (+stderr)

// ---- for kernel32_time.cpp --------------------------------------------------
time_t rt_wall();               // sampled-once wall clock (D2_FAKEWALL-freezable)
extern bool g_realclock;        // live once the scheduler starts (real-clock mode)
extern bool g_tickvirt;         // diag: virtual tick VALUES under the real scheduler
extern uint32_t g_tickOfs;      // virtual->real tick continuity offset
int32_t detect_tzbias();        // host/Vita timezone bias (minutes); #ifdef __vita__ twin of detect_lcid()

// ---- for io_stat.cpp --------------------------------------------------------
extern std::map<uint32_t,std::string>& g_filePathByH;   // handle -> resolved host path

// ---- for scripted_input.cpp -------------------------------------------------
extern std::function<void(const char*)> g_profDump;   // PROF_COUNTERS phase dump, bound in main()

// ---- for pristine_audit.cpp --------------------------------------------------
#include <vector>
std::vector<uint8_t> slurp(const std::string& p);   // reads a whole host file into memory

// ---- for toolhelp.cpp ----------------------------------------------------
namespace d2rt { class PeImage; }
extern std::map<std::string,d2rt::PeImage*> g_modByName;   // lowercased basename -> image (for LoadLibrary)
extern uint32_t g_unhandledFilter;   // SetUnhandledExceptionFilter target: recorded, never invoked

// ---- for tick_intrinsic.cpp ---------------------------------------------------
uint32_t tick_bump(d2rt::Cpu&c, uint32_t n);   // advances/reads the guest tick page

// ---- for scheduler_backend.cpp --------------------------------------------------
extern bool g_rtwant;        // requested real-clock (Vita default / D2_REALCLOCK on desktop)
extern bool g_schedNative;   // set by the early D2SCHED parse in main(), reassigned by make_scheduler()

// ---- for tier1_intrinsics_install.cpp -------------------------------------------
extern uint32_t g_tickVA;         // guest tick page (2 dwords)

// ---- for cs_intrinsic.cpp ------------------------------------------------------
class WxCrit;
WxCrit* crit_for(uint32_t cs);   // the critical-section map + MRU cache (engine-owned)

// ---- for frame_profile.cpp ---------------------------------------------------
extern unsigned long long g_ioUs;    // cumulative host file-I/O time (us)
extern uint64_t g_readN;             // ReadFile calls (watchdog liveness)

// ---- for kernel32_files.cpp (CreateFileA/W, ReadFile/WriteFile, ...) --------
extern uint32_t g_nextH;              // next file/mapping handle to mint
// D2_IOHIST: read-length histogram + (>=2) per-call-site census. Both stay
// readable/writable from tools/rt_boot.cpp too (env parse + the periodic
// report dump).
struct RdSite { uint64_t n=0, bytes=0; };
extern uint64_t g_rdHist[24];
extern uint64_t g_rdBytes;
extern int g_ioHist;
extern std::map<uint32_t,RdSite> g_rdSite;
// Per-handle bytes-read accounting ([FILES] dump at exit + Crash.txt).
struct FRead { uint32_t h; uint64_t bytes; uint32_t reads; };
extern FRead g_frb[256];
bool is_kind(d2rt::Waitable* w, const char* kind);   // wx86_is_kind() shorthand
std::string wnarrow(d2rt::Cpu& c, uint32_t p);       // UTF-16 guest string -> ASCII (narrow)
// Crash.txt-triggered diagnostics (allocator/codec/room-guard/VA counters):
// stays defined in rt_boot.cpp -- see the comment at its definition there.
void crash_txt_diag_dump(d2rt::Cpu& c, const std::string& base);

// ---- for kernel32_modules.cpp (GetProcAddress, GetModuleHandle*, ----------
// ---- GetModuleFileName*, LoadLibrary*) -------------------------------------
extern std::map<uint32_t,d2rt::PeImage*> g_modByBase;   // HMODULE (load base) -> image
extern std::map<uint32_t,std::string> g_sysLib;         // pseudo-handle -> dll name (lc)
extern uint32_t g_sysLibNextH;
extern const char* SYS_DLLS[];                           // NULL-terminated; see its definition for the full list
extern uint32_t g_curBase;              // base of the module currently initializing
extern d2rt::Bridge* g_bridge;          // set after commit; runtime LoadLibrary of on-disk DLLs
std::string module_path_for(uint32_t hmod);   // GetModuleFileName* path resolution
uint32_t put_cstr(d2rt::Cpu& c, const char* s);   // alias for wx86_scratch_put_cstr

// ---- for win32_import_remainder.cpp (KERNEL32/USER32/ADVAPI32/ole32/PSAPI --
// ---- leftovers from "FULL IMPORT COVERAGE") --------------------------------
extern bool g_stop;                     // controlled-stop flag (checked by the scheduler loop)
extern std::string g_stopReason;        // human-readable reason, surfaced in crash.log
uint32_t detect_lcid();                 // host/Vita system locale; #ifdef __vita__ twin of detect_tzbias()
uint32_t halloc(uint32_t n);             // bounded bump allocator over the HEAP arena (g_heapA)

// ---- for checkrevision_crypto.cpp ------------------------------------------
extern std::string g_crHashedVer;       // version string CheckRevision's CryptHashData hashed (D2CRTEST reads it too)
std::vector<uint16_t> gread_wc(d2rt::Cpu& c, uint32_t p, int len);   // reads a guest UTF-16 string

// ---- for kernel32_filemapping.cpp ------------------------------------------
extern wx86::GuestRegion g_heapA, g_vaA;   // bump allocators (HEAP / VIRTUAL arenas)
uint32_t d2_valloc(uint32_t n);            // bounded bump allocator over the VA arena (g_vaA)
extern uint32_t g_fmapN, g_unmapN;         // CreateFileMapping* / UnmapViewOfFile counts
extern uint64_t g_fmapB, g_unmapB;         // ...and the bytes mapped/unmapped

// ---- for win32_shims_advapi32_d2.cpp ---------------------------------------
uint32_t cur_tib();                          // current thread's TIB (or MAIN_TIB)
void set_lasterr(d2rt::Cpu&c, uint32_t v);   // writes TEB+0x34 (LastErrorValue)
bool env_filelog();                          // FILELOG=1, latched once
void pc_dirty(const std::string& root);      // marks the directory cache for rebuild
extern bool g_traceOn;                       // TRACE active (TRACEAFTER=<frame> or TRACE=1)
extern int g_calls;                          // traced call counter (TRACE wrapper)
std::string gread_mb(d2rt::Cpu& c, uint32_t p, int len);   // reads a guest string (CP1252/UTF-16)

// ---- for win32_shims_user32_d2.cpp -----------------------------------------
extern uint32_t g_hwnd, g_wndProc;           // D2's window + current wndproc (subclassable)
extern std::deque<GMsg> g_msgQ;              // pending injected/posted messages
extern uint64_t g_pmN, g_gmN, g_dispN, g_injN, g_coalN;
extern uint32_t g_wndCallEcx;                // ecx passed to the wndproc (end-of-hook-chain marker)
extern std::function<std::string(uint32_t)>* g_locp;   // address -> "module+off" resolver
extern std::function<void()> g_dump_fring;   // dumps the recent file-ops ring
uint32_t guest_call_stub(d2rt::Cpu& c, uint32_t proc, uint32_t hwnd, uint32_t msg,
                          uint32_t wp, uint32_t lp, uint32_t retaddr);
void gwrite_wc(d2rt::Cpu& c, uint32_t p, uint16_t w);       // writes a guest uint16 (UTF-16)
extern d2rt::ThreadScheduler* g_sched;
extern d2rt::CooperativeScheduler* g_schedCoop;   // null under native scheduling
extern uint32_t g_d2base;                    // Game.exe load base
extern const bool g_114;                     // 1.14d monolith
// D2_LOOPWATCH: definitions now live in runtime/frame_profile.cpp (see that
// file's header); g_lwSimN/lw_on/lw_tid stay declared here too since
// win32_shims_user32_d2.cpp needs them independently of frame_profile.h.
extern uint64_t g_lwSimN;                    // D2_LOOPWATCH: counted simulation steps
bool lw_on();
uint32_t lw_tid();
d2rt::Waitable* rt_never_event();           // D2_ONEDRAW=2: manual KEvent that is never signaled (sleep)
uint32_t tick_now(d2rt::Cpu& c);            // current guest tick (real-clock aware; no page advance)

// ---- for native_hooks_codec.cpp --------------------------------------------
extern std::string g_writeRoot;              // scratch dir for guest-created (write) files
// D2_CELWATCH: LRU evictions from the CelData cache.
extern uint32_t g_celWatch; extern uint64_t g_celEvict, g_celEvictOther;
// NATIVESCOMP (scomp_explode_114/scomp2) + native audio codecs (huff/adpcm).
extern uint64_t g_scompN, g_scompFB, g_scomp2N, g_scomp2FB;
extern uint64_t g_xvCalls, g_xvBad;
extern uint32_t g_xvFirst, g_xvG, g_xvN, g_xvSzG, g_xvSzN;
// NATIVEDCC: native DCC decoder + D2_DCCPROF.
extern uint64_t g_dccUs, g_dccN;
extern uint64_t g_dccNatN, g_dccNatFB, g_dccNatBytes, g_dccNatFrames, g_dccVerN, g_dccVerBad;
// Halt-316 room guard (D2_ROOMGUARD).
extern uint64_t g_rgIter, g_rgSkip;
// Fog raise snapshot (D2_RAISESNAP): ring of sub_6001f0, dumped to Crash.txt.
struct RaiseRec { uint32_t ra, rec, esi, edi, frame; uint8_t body[0x48]; };
extern RaiseRec g_raiseRing[8]; extern uint32_t g_raiseN;
// Bounded bump allocator (C6) shared by tools/rt_boot.cpp and the extracted
// units — see the comment at its definition in rt_boot.cpp.
uint32_t misc(uint32_t n);

// ---- for native_hooks_cellengine.cpp ---------------------------------------
// Definitions that STAY in tools/rt_boot.cpp (counters/state read or written
// by this file, but also by d2rt_hot_stat()/the [soak] dump which remain in
// rt_boot.cpp):
extern uint64_t g_blitN, g_blitFast, g_blitSlow;
extern uint64_t g_hotLightN, g_hotLightFB, g_hotBlendN, g_hotBlendFB;
extern uint64_t g_hotRleN, g_hotCollN, g_hotGridN, g_hotRleBytes;
extern uint64_t g_gridMemoHit, g_gridMemoMiss;
extern uint64_t g_cellCalls, g_cellDecl, g_cellPure;
extern uint64_t g_cellB[6];
extern bool g_shOn, g_cvOn;
extern uint32_t g_dibBits, g_dibW, g_dibH, g_dibBpp;
extern uint64_t g_oobLoop, g_oobBlit;
extern uint64_t g_loopServed, g_loopCells;
extern uint64_t g_loopFB[8];
extern uint64_t g_loopScale, g_loopScaleClip;
extern bool g_scaleForceClip;
// Definitions MOVED to native_hooks_cellengine.cpp (read by
// d2rt_hot_stat()/the [soak] dump, which stay in rt_boot.cpp):
extern uint64_t g_wrOob;
extern uint32_t g_wrOobOff, g_wrOobLen, g_wrOobSpan, g_wrOobKind;
extern uint32_t g_oobLo, g_oobHi;
extern bool g_cellStat;
extern uint64_t g_csCalls, g_csCells, g_csHP, g_csP1Band, g_csP2Band;
extern uint64_t g_csPixP1[3], g_csPixP2[3], g_csCell3[3];
extern uint64_t g_csRunP1, g_csRunP2, g_csScaleAt, g_csVecB;
extern uint64_t g_csLutHit, g_csLutMiss, g_csRowHit, g_csRowMiss;
extern uint64_t g_csBandFast, g_csBandSlow;
extern uint64_t g_csIdentPix, g_csXlatPix, g_csIdentTest;
extern bool g_ovlOn;
extern uint64_t g_ovlCalls, g_ovlWr, g_ovlHit, g_ovlDiff, g_ovlSelf, g_ovlNoMap;
extern uint64_t g_parScaleBail, g_parStraddle;
extern int g_parN, g_parLive;
extern uint32_t g_parShare;
extern uint64_t g_parCalls, g_parSeqCalls, g_parCellsW, g_parJoins;
extern uint64_t g_parWaitMax, g_parWaitSum, g_parSteals, g_parDegen;
extern uint64_t g_parPitchOdd;
extern uint64_t g_parSpanC, g_parSpanW, g_parCellsC;
extern uint64_t g_parWIdlePro, g_parWIdleGap, g_parWIdleN;
// D2_LIGHTBLOB (lightblob_apply), read by the [soak] dump.
extern uint64_t g_lbServed, g_lbVerifN, g_lbRepli, g_lbCells, g_lbSplats, g_lbVerifBad;
// Small helpers called from native_hooks_cellengine.cpp; their private state
// (burst counters, caller cache...) now lives in runtime/cell_frame_diag.cpp
// (see that file's header), not in rt_boot.cpp — only the function is
// exposed, here and there.
void note_caller(uint32_t ra);
void sh_ev(int fam);
void sh_loop();
void cv_note(uint32_t blk, int32_t Xa, int32_t Ya);
void gread_n(d2rt::Cpu& c, uint32_t va, void* dst, uint32_t n);

// ---- for select_observe.cpp ------------------------------------------------
bool env_netlog();              // D2NETLOG set: verbose network logging
