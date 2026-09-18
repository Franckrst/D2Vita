// tools/rt_boot.cpp — boot the real 1.13c DLL graph under the Unicorn oracle.
//
// Loads Storm+Fog+D2Common (+more), links inter-Blizzard imports x86->x86,
// registers a native KERNEL32 shim set (enough for the MSVC CRT startup that
// runs before DllMain), then invokes a target DLL's entry point with
// DLL_PROCESS_ATTACH. Logs the OS-shim call path and stops at the first
// unshimmed import or fault — that's the next crash to fix, one at a time.
//
//   rt_boot <dir-with-1.13c-dlls> [EntryDll]     (default EntryDll = Fog.dll)
#include <ctime>
#include "runtime/bridge.h"
#include "runtime/layout.h"
#include "runtime/cpu.h"
#include "runtime/pe_image.h"
#include "runtime/prof.h"
#include "platform/vita_present.h"
#include "platform/install_screen_vita.h"
#include "glide_ring/glide_atlas.h"
#include "glide_ring/replay60.h"
#include "platform/vita_gxm.h"
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#include "platform/vita_net.h"
#include "runtime/rt_host.h"        // what rt_boot.cpp exposes to the extracted runtime units
#include "glide_ring/gx_host.h"     // GPU rendering path
#include "runtime/phase_hooks.h"    // phase hooks
#include "runtime/native_hooks_codec.h"  // codec/DCC/Fog-raise hooks
#include "runtime/native_hooks_cellengine.h"  // cell-loop/light-grid/blend/RLE/collision hooks
#include "runtime/guest_atomics.h"           // atomics over guest memory -> winx86
#include "runtime/guest_thread_ctx.h"        // current-thread TIB + last error -> winx86
#include "runtime/guest_sync.h"              // kernel objects + handle table + observer -> winx86
#include "runtime/win32_shims_sync.h"        // event/mutex/semaphore -> winx86
#include "runtime/guest_files.h"            // file handle tables -> winx86
#include "runtime/guest_toolhelp.h"         // Toolhelp32 snapshots -> winx86
#include "runtime/win32_shims_kernel32.h"    // generic KERNEL32 -> winx86 (dependency-free shims)
#include "runtime/win32_shims_shell32.h"     // generic SHELL32 -> winx86 (SHAppBarMessage, ShellExecuteA)
#include "runtime/win32_shims_shell32_d2.h"  // D2-specific SHELL32 -> winx86 (SHGetFolderPathA)
#include "runtime/win32_shims_advapi32.h"     // generic ADVAPI32 -> winx86 (stateless security/ACL/SCM)
#include "runtime/cdkeys_file.h"
#include "runtime/lazy_seek.h"       // D2_LAZYSEEK: in-memory file-position tracking
#include "runtime/io_stat.h"         // D2_IOSTAT / D2_READAHEAD: file-read accounting
#include "runtime/scripted_input.h"  // D2SCRIPT input injection + D2CMDFILE polling
#include "crashreport/cr_boot.h"     // session/stop/exit/shutdown hooks (no-op off Vita)
#include "crashreport/cr_crashtest.h"  // D2_CRASHTEST=native|halt (no-op unless -DD2V_CRASHTEST)
#include "runtime/path_cache.h"      // host_path() + directory index (D2_PATHCACHE)
#include "runtime/d2ini.h"           // D2.ini reader (GetPrivateProfileStringA/IntA)
#include "runtime/pristine_audit.h"  // Warden-fidelity integrity checkers (tests #7/#8)
#include "runtime/cell_frame_diag.h" // D2_LOOPSHAPE / D2_CAMVEC / D2_FRAMEUS diagnostics
#include "runtime/jit_profile.h"     // D2_JITPROFILE / D2_SIGNTAG / fastmmu dynarec profiling
#include "runtime/toolhelp.h"    // Toolhelp32 enumeration + CONTEXT writer + SEH dispatch
#include "runtime/frame_profile.h"   // D2_LOOPWATCH / D2_NOCAP / D2_FRAMEPROF / D2_LAGWATCH diagnostics
#include "runtime/tick_intrinsic.h"  // Tier-1 GetTickCount/timeGetTime intrinsic + D2_B5CLOCK/D2_CLOCKLOG
#include "runtime/cs_intrinsic.h"    // Tier-1 critical-section intrinsics (D2_CSINTRIN)
#include "runtime/emutls_probe.h"    // D2VPK_TLSWRAP counter + emutls inertness self-test
#include "runtime/select_observe.h"  // select census, fed by d2_net_observe
#include "runtime/scheduler_backend.h"  // D2SCHED=native|coop scheduler backend selection
#include "runtime/eip_time_profile.h"   // D2_EIPPROF / D2_TIMEPROF address-profile reports
#include "runtime/tier1_intrinsics_install.h"  // Tier-1 clock/CS intrinsics + guest-inline hot imports
#include "runtime/kernel32_files.h"   // KERNEL32 file-open shims (CreateFileA/W + friends)
#include "runtime/kernel32_modules.h" // GetProcAddress/GetModuleHandle*/GetModuleFileName*/LoadLibrary*
#include "runtime/kernel32_time.h"    // wall-clock/calendar shims (SystemTimeToFileTime, GetLocalTime, ...)
#include "runtime/kernel32_interlocked.h"  // InterlockedCompareExchange(Pointer) + thread-id one-offs
#include "runtime/kernel32_fsinfo.h"  // GetDiskFreeSpace*/GetFullPathNameA/GetVolumeInformationA/...
#include "runtime/win32_import_remainder.h"  // KERNEL32/USER32/ADVAPI32/ole32/PSAPI leftovers
#include "runtime/checkrevision_crypto.h"    // CRYPT32/ADVAPI32 CryptoAPI + real Authenticode/VERSION wiring
#include "runtime/netguard_lock.h"    // official-server network lock (open by default; D2_LOCAL_ONLY to restrict)
#include "runtime/kernel32_w_variants.h"  // GetFileAttributesW/CreateDirectoryW/DeleteFileW/CopyFileW/FindFirstFileW
#include "runtime/kernel32_filemapping.h"  // CreateFileMappingA/W, MapViewOfFile, UnmapViewOfFile
#include "runtime/win32_shims_advapi32_d2.h"  // D2-specific ADVAPI32 -> winx86 (registry + misc()/set_lasterr())
#include "runtime/win32_shims_user32.h"        // generic USER32 -> winx86 (RECT geometry, strings, stubs)
#include "runtime/win32_shims_user32_d2.h"     // D2-specific USER32 -> winx86 (window/wndproc/messages)
#include "runtime/win32_shims_misc.h"           // DDRAW/Bink/Smacker/ijl11 -> winx86 (generic stateless stubs)
#include "runtime/win32_shims_psapi.h"           // module enumeration -> winx86 (EnumProcessModules/GetModuleInformation)
#include "runtime/win32_shims_wintrust.h"        // WINTRUST/CRYPT32 -> winx86 (real Authenticode signature verification)
#include "runtime/win32_shims_version.h"         // VERSION.dll -> winx86 (parses VS_FIXEDFILEINFO)
#include "runtime/win32_shims_gdi32.h"          // generic GDI32 -> winx86 (fake handles, palette table, screen size)
#include "runtime/win32_shims_window.h"         // generic window/cursor -> winx86
#include "runtime/win32_shims_wsock32.h"          // WSOCK32/WS2_32 -> winx86 (socket table + generic primitives)
#include "runtime/poll_gil.h"                      // GIL-free polling helpers (wx86_poll_gilfree)
#include "runtime/net_nonblock.h"                  // wx86_net_resolve / wx86_net_set_nonblock
#include "runtime/guest_scratch.h"                 // guest scratch allocator (misc() below is a thin alias for this)
#include "runtime/guest_region.h"                  // guest region allocator
#include "runtime/guest_str.h"                     // guest string helpers (gread_mb/gread_wc/gwrite_wc)
#include "runtime/host_clock.h"                     // monotonic host clock (rt_now_us)
#include "runtime/win32_shims_locale.h"            // KERNEL32 locale/strings/console -> winx86
#include "runtime/win32_shims_memory.h"            // KERNEL32 memory layout -> winx86 (heap, virtual arena, VirtualQuery)
#include "runtime/win32_shims_wait.h"              // KERNEL32 waiting -> winx86 (WFSO/WFMO/SleepEx/IOCP + Sleep core)
#include "runtime/guest_locale.h"                  // guest locale state (g_lcid/cp_ansi/cp_oem/locinfo)
#include "runtime/dcc_native.h"     // native DCC decoder (NATIVEDCC)
#include "runtime/ds_emul.h"       // DirectSound COM shim + mixer (D2_SON)
#include "runtime/audio_sink.h"    // pluggable audio sink (wav / null / vita)
#include "runtime/scomp_audio.h"   // native Storm Huffman + ADPCM (NATIVEHUFF/NATIVEADPCM)
extern "C" unsigned long long d2rt_audio_codec_n[4];   // 0=huff 1=adpcm-mono 2=adpcm-stereo 3=replis
// Prints Storm codec stats once at audio teardown. Lives here, not in the
// generic engine, because it reports Blizzard's proprietary compressed format.
// The "(audio ARMED/MUTED)" tag is a non-vacuity check: it confirms the hooks
// actually fired, not just that two A/B runs hash the same.
static void son_codec_report(){
    static bool said=false; if(said) return; said=true;
    std::printf("[son] codecs natifs: huff=%llu adpcm=%llu repli=%llu (son %s)\n",
                d2rt_audio_codec_n[0], d2rt_audio_codec_n[1]+d2rt_audio_codec_n[2],
                d2rt_audio_codec_n[3], d2rt::dsound::enabled()?"ARME":"MUET");
    std::fflush(stdout);
}
#include "dyn86_memintrin.h"        // native trap-free memcpy/memset (D2_MEMINTRIN)
#include "dyn86_intrin.h"           // intrinseques natives, table generique (D2_INTRIN)
// D2-specific side of the mechanism above (src/runtime/d2_intrin_114.cpp);
// declared here rather than in its own header since it has only two entry points.
extern "C" int d2_intrin_114_register(uint32_t d2base);
extern "C" int d2_proj_verify;
extern "C" unsigned long long d2_proj_ver_n, d2_proj_ver_bad, d2_proj_ver_skip;
extern "C" uint32_t d2_proj_verify_trap;
extern "C" {
    extern uint32_t d2rt_timeprof_on, d2rt_timeprof_base;
    extern uint32_t d2rt_lag_on;   // also declared in runtime/frame_profile.cpp (D2_LAGWATCH)
    extern uint64_t d2rt_tp_key[8192], d2rt_tp_hit[8192], d2rt_tp_bucket[8];
    extern uint64_t d2rt_tp_samples, d2rt_tp_susp, d2rt_tp_us, d2rt_tp_blocks;
}
extern "C" uint32_t d2_proj_verify_ret(void);

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <string>
#include <deque>
#include <array>
#include <set>
#include <functional>
#include <cctype>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>

using namespace d2rt;

std::vector<uint8_t> slurp(const std::string& p){
    std::vector<uint8_t> v; FILE* f=std::fopen(p.c_str(),"rb"); if(!f) return v;
    std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::fseek(f,0,SEEK_SET);
    if(n>0){v.resize(n); if(std::fread(v.data(),1,n,f)!=(size_t)n) v.clear();} std::fclose(f); return v;
}

// ---- guest allocators -------------------------------------------------------
// Fog's pointer validator rejects addresses >= 0x80000000 (Win32 user-space
// assumption), so the guest heap MUST live below 2 GiB. Carve a large heap in
// the low region [0x01000000, 0x30000000) (below the module bases at
// 0x30000000); lazily backed by Unicorn so it costs nothing until touched.
// Layout is variable so D2LAYOUT=compact can repack every region LOW: the Vita
// single-block arena must cover the whole span in device RAM, and the sparse
// default (heap 736 MiB reserved, regions up to 2 GiB) can't be committed
// there. Compact sizes track the measured high-water (heap ~7 MiB, VA ~83 MiB)
// with generous headroom; bases pack contiguously so the span is ~360 MiB.
// Defaults keep the validated sparse layout byte-for-byte.
static uint32_t HEAP_BASE=0x01000000, HEAP_SIZE=0x2E000000;   // ~736 MiB, all < 0x80000000
static uint32_t VA_BASE  =0x50000000, VA_SIZE  =0x0F000000;   // 240 MiB (up to 0x5F000000, below the 0x60xxxxxx stacks)
static uint32_t MISC_BASE=0x48000000, MISC_SIZE=0x00800000;   // strings/structs + read buffers (8 MiB)
// The allocation pointer lives in the engine (wx86_scratch_*); the range is
// granted at the mapping site (cpu->map(MISC_BASE,...)) once MISC_BASE/SIZE are
// final. The leading +0x100 offset is kept as-is: guest logs depend on the first
// allocation landing at this exact address for byte-for-byte reproducibility.
// Compact-layout knobs, filled in main() before any region is mapped.
static uint32_t SCHED_STACKS=0x70000000, SCHED_TIBS=0x74000000, MAIN_STACK_TOP=0x60200000;
// Main-thread TIB. 0 by default (linear FS base: fs:[off] reads page 0
// directly). Under the HIGH layout there is no low page, so the main TIB moves
// up with everything else and FS gets an explicit base (the Box86 backend uses
// a real per-thread segment base — see set_fs_base / fs_base_is_direct, used by
// both schedulers).
static uint32_t MAIN_TIB=0;
static void apply_compact_layout(){
    const char* l=getenv("D2LAYOUT");
    if(!l||(std::strcmp(l,"compact")&&std::strcmp(l,"haut"))) return;
    // HARDWARE-FIT PACK, 1.14d calibration: sizes derive from the measured 1.14d
    // Rogue profile (heap peak 15.6 MiB, VA peak 200.2 MiB, monolith Game.exe
    // image 5.9 MiB — relocatable, loads in the module window; misc < 2 MiB),
    // trimmed to that observed usage plus headroom (not a leak — the working
    // set here is stable). Total span 0x10F00000 = 271 MiB; D2ARENA =
    // 0x12900000 (297 MiB incl. the 16 MiB membase-rounding slack) — inside the
    // ~330 MiB real-Vita user budget (ATTRIBUTE2=12). The span above VA
    // (stacks/TIBs/trap window/D2ARENA) is byte-identical regardless of these sizes.
    HEAP_BASE=0x00500000; HEAP_SIZE=0x01400000;   // 20 MiB  -> ends 0x01900000 (= bridge next_base_)
    // modules (bridge) 0x01900000..0x02200000 (9 MiB reserved)
    // MISC window is 9 MiB: Game.exe alone needs 8 MiB, but d2vhost (2 MiB) and
    // CheckRevision.dll (0x4b000, loaded at Battle.net connect) also live here;
    // an undersized window lets CheckRevision.dll overlap MISC's callback stubs,
    // so the main thread jumps into DLL bytes and faults. The bound is also
    // enforced at the bridge (set_module_limit): overflow becomes a named
    // refusal, not a silent overwrite.
    MISC_BASE=0x02200000; MISC_SIZE=0x00200000;   // 2 MiB   -> ends 0x02400000 (= VA_BASE)
    // VirtualAlloc arena: Storm's MPQ decompressor (SCOMP) grows a doubling
    // working buffer during level load; the 1.14d Rogue Encampment run peaks
    // at 200.2 MiB of VA (measured, soak). 216 MiB leaves ~16 MiB
    // anti-fragmentation headroom. Undersizing makes SCOMP hit "error 8" and
    // the Storm I/O worker dies through a smashed frame (deterministic VA
    // exhaustion). Later acts may peak higher — re-measure past Act 1.
    // membase must be 16 MiB aligned, but the kernel only returns bases aligned
    // to 1 MiB — up to 15 MiB can be lost to alignment slop with no way to
    // reclaim it (observed anywhere from 3 to 13 MiB between runs). The guest VA
    // extent is the only knob available to compensate.
    // Without this headroom the JIT cache cannot grow; since this build has no
    // interpreter fallback (shim_impl.c: Run() sets quit=1), a refused block
    // kills the guest thread outright — this is not a soft degradation.
    // ⚠️ The same constants exist in src/runtime/bridge.cpp (stack_base_,
    // trap_base_, trap_hi_) — the two files must be kept in sync.
    VA_BASE  =0x02400000; VA_SIZE  =0x0DC00000;   // 220 MiB -> ends 0x10000000 (= bridge stack_base_)
    // main guest stack 0x10000000..0x10200000 (2 MiB, bridge stack_base_)
    MAIN_STACK_TOP=0x10200000;
    SCHED_STACKS=0x10200000; SCHED_TIBS=0x10C00000;   // worker stacks/TIBs, below the 0x10E00000 trap window
    // ---- HIGH layout (D2LAYOUT=haut): the same pack, shifted above 0x80000000,
    // membase=0. This is the byte-identical model run under qemu to validate
    // the Vita layout (src/runtime/layout.h); offset 0 for "compact" makes the
    // lines below exact no-ops.
    if(const uint32_t HI=d2rt::layout_hi()){
        HEAP_BASE+=HI; MISC_BASE+=HI; VA_BASE+=HI; MAIN_STACK_TOP+=HI;
        SCHED_STACKS+=HI; SCHED_TIBS+=HI;
        MAIN_TIB=HI;                  // first page of the block: the main TIB
    }
    // The engine owns current-TIB logic; this port only supplies where its
    // memory layout places the main TIB. Set here as soon as the layout is
    // fixed, before any shim runs.
    wx86_set_main_tib(MAIN_TIB);
    // (the guest scratch allocator is armed later, at the mapping site, once
    // MISC_BASE/MISC_SIZE are final)
    // Compact REQUIRES the relocatable main exe. The pack above reserves the
    // module window (0x01D00000) for it and starts the heap at HEAP_BASE
    // (0x00500000). A non-relocated exe at its preferred base 0x00400000 (5.9 MiB
    // image, ending ~0x009E0000) OVERLAPS that heap — heap allocations then
    // trample the exe's .data, corrupting an init sync flag and DEADLOCKING the
    // boot (all init threads wait on an event that never gets signaled; frames=0).
    // Pristine ImageBase loading (a Warden-fidelity property) is desktop/non-compact
    // only. Force reloc here unless the user overrode it.
    if(!getenv("D2_RELOC_EXE")) setenv("D2_RELOC_EXE","1",1);
}
// Real HeapAlloc/VirtualAlloc semantics: fail (return 0) when the region is
// exhausted — permissive always-succeed masks divergences (e.g. Fog probing
// memory to size its pool). Free-list allocator with coalescing: the guest
// alloc/free churn is real (Storm SMem pools grow AND shrink); a bump
// allocator leaks everything and exhausts the region (SCOMP "error 8").
// An allocation failure must reach a durable sink: the real Vita has no stderr,
// so a bare fprintf(stderr) FAIL line is invisible and leaves no trace of which
// region ran out, at what size, or how fragmented it was. Record the last
// failure in a file-scope struct that the Crash.txt branch + the shims route to
// d2vita_progress/crash.log. (d2_crashlog is defined further down.)
void d2_crashlog(const char* fmt, ...);
struct AllocFail { const char* region=nullptr; uint32_t n=0,cur=0,peak=0,largest=0,used=0; bool set=false; };
static AllocFail g_lastAllocFail;
// Codec.cpp's DCC decoder (VA 0x60bff0) sums N per-image sizes from a sprite's
// header then allocates the total; a huge allocation there means a CORRUPTED
// SIZE, not memory exhaustion. Track the identity of the last decoded sprite at
// near-zero cost so a crash here (or the "Error decompressing sprite" fault,
// which comes from the same function) can be attributed to a file.
static uint32_t g_codecFileP=0, g_codecFrames=0, g_codecStart=0, g_codecTotal=0;
static uint32_t g_codecLastSz=0, g_codecData=0, g_codecTabEnd=0;
static uint64_t g_codecCalls=0, g_codecSums=0, g_codecAbsurd=0;
static uint32_t g_codecMaxTotal=0, g_codecMaxFileP=0;
static char     g_codecFileName[96]={0}, g_codecMaxFileName[96]={0};
// Reserve-vs-commit profile of the guest's VirtualAlloc traffic. Storm reserves
// address space (MEM_RESERVE, PAGE_NOACCESS) then commits subsets, but our
// arena charges the FULL reservation; these counters show how much of an
// exhausted arena is mere reservation vs. actually committed bytes.
static uint64_t g_vaResB=0, g_vaComInB=0, g_vaComFrB=0, g_vaRelB=0, g_vaDecB=0;
static uint32_t g_vaResN=0, g_vaRelN=0, g_vaRelFailN=0;
uint32_t g_fmapN=0, g_unmapN=0; uint64_t g_fmapB=0, g_unmapB=0;   // extern: see rt_host.h (for kernel32_filemapping.cpp)
uint64_t g_dccUs=0, g_dccN=0;                              // D2_DCCPROF: time spent in the DCC decoder
#ifdef D2_EMITPROF
extern "C" void d2ep_report(void (*)(const char*));
extern "C" void d2ep_dump(const char*, int);
extern "C" void d2ep_csv(const char*);
#endif
extern "C" { extern uint32_t d2rt_eipprof_on, d2rt_eipprof_base; extern uint64_t d2rt_eipprof[8];
             extern uint64_t d2rt_eipprof_sub[32], d2rt_eipprof_sub2[32], d2rt_eipprof_fn[16], d2rt_eipprof_sub3[32], d2rt_eipprof_all[96];
             extern uint32_t d2rt_eipprof_key[8192]; extern uint64_t d2rt_eipprof_hit[8192];
             // Census of the intrinsic fast path (cpu_box86.cpp). The first four
             // counters are only incremented in a -DD2_B5CENSUS build; the last
             // three are always live and serve as a non-vacuity check (index
             // armed / window covered).
             extern unsigned long long d2rt_b5_calls, d2rt_b5_loads,
                                       d2rt_b5_hits,  d2rt_b5_emutls;
             extern unsigned int d2rt_b5_index_on, d2rt_b5_direct_n, d2rt_b5_direct_lo; }
// Fog software-raise ring (sprite-family crashes: Gfx.cpp:1632 / line-607).
// Every D2-internal software exception funnels through the raise entry VA
// 0x6001f0 (&record on the stack, 0x48 bytes). Snapshot silently (no I/O in
// the window — heisenbug rule), dump the ring at Crash.txt.
RaiseRec g_raiseRing[8]; uint32_t g_raiseN=0;
// Screen size announced to the game by the Win32 shims (GetClientRect,
// GetSystemMetrics, EnumDisplaySettings, GetMonitorInfo...). Fixed at 800x600:
// that is the surface the 1.14d monolith actually draws. Carried in winx86
// (wx86_set_screen_size); set once at startup, see the call near
// win32_shims_gdi32_install below. There is deliberately no D2_SCREEN=LxH knob:
// announcing a different resolution does not change the drawn surface —
// CreateDIBSection still renders 800x600 regardless, and so does the registry
// `Resolution` key. The monolith fixes its surface elsewhere; this is not a
// performance lever.
// Asset accounting: bytes READ per file handle, O(1) on the hot ReadFile path
// (direct-indexed slots, collision = overwrite, fine for the ~15 simultaneous
// handles). Dumped as [FILES] at exit + Crash.txt: shows which archives
// actually feed the resident set (is d2music/d2speech ever read?).
// struct FRead now declared in runtime/rt_host.h (kernel32_files.cpp's
// frb_add() writes into g_frb from ReadFile; this dump reads it back).
FRead g_frb[256];   // extern: see rt_host.h
// The region allocator itself lives in the engine
// (winx86/src/runtime/guest_region.{h,cpp}): splitting a range of GUEST
// addresses into blocks, returning and re-merging them, is an engine primitive
// like misc()/put_cstr. What stays here is what's actually port-specific: the
// two instances and the ranges they cover (set by the console's compact memory
// layout, see apply_compact_layout), plus formatting the exhaustion diagnostic
// — the engine only reports, it doesn't write to our log.
wx86::GuestRegion g_heapA, g_vaA;   // extern: see rt_host.h (for kernel32_filemapping.cpp)
// VA-arena occupancy for the Vita watchdog's "alive" heartbeat — a growth curve
// that never comes back down across a session is the signature of a leak.
extern "C" uint32_t d2rt_va_used_mb(void){ return g_vaA.used_bytes()>>20; }
// Counters for the native ports, exposed to the Vita watchdog: on console
// there is no stdout, so the "alive" line from boot_progress is the only
// channel — without them there is no way to tell whether the native ports ran at all.
//  0=grid 1=collision 2=RLE 3=lighting 4=blend 5=SCompExplode 6=total fallbacks
extern "C" unsigned long long d2rt_hot_stat(int which);
static Cpu* g_cpuForStats=nullptr;   // set once the guest image is mapped (see g_d2base)
uint32_t halloc(uint32_t n){ return g_heapA.alloc(n); }   // extern: see rt_host.h (for win32_import_remainder.cpp)
uint32_t d2_valloc(uint32_t n){ return g_vaA.alloc(n); }   // extern: see rt_host.h (for kernel32_filemapping.cpp)
#define valloc d2_valloc   // ARM32: libc valloc(size_t) would clash (size_t==uint32_t)
// misc() is now just an ALIAS: the guest scratch allocator lives in the engine
// (winx86, runtime/guest_scratch.h). It's an engine primitive, not a game one —
// any Win32 API that returns a pointer the caller never frees (GetCommandLineA,
// GetEnvironmentStrings, inet_ntoa, gethostbyname, the RTL_CRITICAL_SECTION_DEBUG
// block...) needs one, and that pointer must be a GUEST address. What stays
// d2vita-specific: the range granted (the compact memory layout is not
// universal — see wx86_scratch_init/set_limit at the MISC_BASE call sites) and
// the exhaustion diagnostic's output (the engine is silent by construction; it
// invokes a callback, and we turn that into a crash.log line).
// The single arming site, at mapping time, is guaranteed to precede every allocation.
uint32_t misc(uint32_t n){ return wx86_scratch_alloc(n); }
// Guest wndproc-call stubs, DATA-DRIVEN: the stub CODE is emitted once per
// round-robin slot and never touched again; per-call parameters (msg, wparam,
// lparam, proc, return address) live in a separate data block the stub reads
// via absolute [mem] operands. Rewriting stub *code* per dispatch (the old
// scheme) is self-modifying code: the Box86 dynarec keeps executing the stale
// translated block (linked blocks bypass the dirty check), so every dispatch
// after the 8th replayed the parameters of an old message — on the ARM path
// WM_CHAR arrived at the wndproc as a stale mouse/activate message and typed
// characters silently vanished. Unicorn retranslates on write, hiding the bug.
static uint32_t g_gcsCode[8]={0}, g_gcsData[8]={0}; static int g_gcsIx=0;
uint32_t g_wndCallEcx=0;   // ecx handed to the wndproc (hook-chain terminator; set in DispatchMessageA)
uint32_t g_d2base=0;              // Game.exe load base (VA->runtime address of D2 globals)
// D2's OWN cache accounting, read from the guest globals sized by sub_457300
// (see the GlobalMemoryStatus shim): sprite cache current/ceiling, CelData
// ceiling, and the "big machine" flag. Published so the Vita watchdog's alive
// heartbeat carries the cache curve — the direct proof of what the RAM we
// announce to the game does to its sprite cache.
// 4 = CelData CURRENT (ds:0x88db28). The decoded-cel data cache is the only one
// of the three caches whose ceiling is a hardcoded constant — 512,000 bytes
// (`push 0x7D000` at 0x4573fe) regardless of announced RAM — so it's the only
// one that can saturate and force sprites to be re-decoded. The object lives at
// ds:0x88db20 (reverse-engineered: sub_5ffd40 -> sub_609020): +4 = ceiling, +8 = usage.
extern "C" uint32_t d2rt_sprite_cache_kb(int which){   // 0=now 1=max 2=celdata-max 3=bigmachine 4=celdata-now
    if(!g_cpuForStats||!g_d2base) return 0;
    uint32_t off = which==0?0x49db68u : which==1?0x49db64u : which==2?0x48db24u
                 : which==4?0x48db28u : 0x49db58u;
    uint32_t v = g_cpuForStats->read_u32(g_d2base+off);
    return which==3? v : (v>>10); }
// Wall clock: newlib time() is 0 (epoch) under Vita3K, and SceRtc calls FAIL
// from scheduler-fiber context (observed: GetSystemTimeAsFileTime pre-sched
// got the real time, GetLocalTime on a fiber got 0). Sample the RTC ONCE on a
// real thread (warm-up call in main), then serve base + monotonic elapsed —
// no syscalls at shim time, and the clock stays self-consistent.
#ifdef __vita__
extern "C" long long d2vita_wall_unix(void);
#endif
uint64_t rt_now_us();          // fwd (defined below, platform-specific; declared in runtime/rt_host.h)
// D2_FAKEWALL=<unix seconds>: freezes the wall clock. D2 seeds its RNG from
// GetSystemTime/GetSystemTimeAsFileTime, so without freezing it two identical
// runs started a couple seconds apart diverge (ambient animations, dice rolls)
// and any frame-by-frame comparison between branches is just noise. Required
// for a pixel-exact A/B; has no effect on a normal run.
time_t rt_wall(){   // extern: see rt_host.h
    static const long long fake = getenv("D2_FAKEWALL") ? atoll(getenv("D2_FAKEWALL")) : 0;
    if(fake>0) return (time_t)fake;
    static long long base=0; static uint64_t baseUs=0;
    if(!base){
#ifdef __vita__
        long long v=d2vita_wall_unix(); base = v>0 ? v : (long long)::time(nullptr);
#else
        base=(long long)::time(nullptr);
#endif
        if(base<=0) base=1787000000ll;   // plausible fallback timestamp
        baseUs=rt_now_us();
    }
    return (time_t)(base + (long long)((rt_now_us()-baseUs)/1000000ull));
}
// civil_from_unix() (UTC civil conversion without libc) moved to
// runtime/kernel32_time.cpp -- only its clock/calendar shims call it.
uint32_t guest_call_stub(Cpu& c, uint32_t proc, uint32_t hwnd, uint32_t msg,
                                uint32_t wp, uint32_t lp, uint32_t retaddr){
    int i=g_gcsIx++&7;
    if(!g_gcsCode[i]){
        uint32_t dd=misc(0x20); g_gcsData[i]=dd;
        uint32_t st=misc(0x30); g_gcsCode[i]=st;
        std::vector<uint8_t> b;
        auto M=[&](uint8_t op2,uint32_t a){ b.push_back(0xFF); b.push_back(op2);
            for(int k=0;k<4;k++) b.push_back((uint8_t)((a>>(8*k))&0xff)); };
        // mov ecx,[dd+24]: D2's window proc is a hook-chain dispatcher that
        // does `cmp ecx,[handlerGlobal]; je skip; call [handler]`. Feeding
        // ecx = that handler makes it take the skip (default processing)
        // instead of call'ing a garbage handler pointer.
        b.push_back(0x8B); b.push_back(0x0D);                     // mov ecx,[imm32]
        for(int k=0;k<4;k++) b.push_back((uint8_t)(((dd+24)>>(8*k))&0xff));
        M(0x35,dd+0); M(0x35,dd+4); M(0x35,dd+8); M(0x35,dd+12);  // push lp,wp,msg,hwnd
        M(0x15,dd+16);                                            // call [proc]
        M(0x35,dd+20); b.push_back(0xC3);                         // push retaddr; ret
        c.write(st,b.data(),(uint32_t)b.size());
    }
    uint32_t dd=g_gcsData[i];
    c.write_u32(dd+0,lp);  c.write_u32(dd+4,wp);   c.write_u32(dd+8,msg);
    c.write_u32(dd+12,hwnd); c.write_u32(dd+16,proc); c.write_u32(dd+20,retaddr);
    c.write_u32(dd+24,g_wndCallEcx);   // ecx handed to the proc (hook-chain terminator)
    { static bool dbg=getenv("D2_DISPLOG")!=nullptr; static int n=0;
      if(dbg&&n<300){ n++; std::printf("[gcs] slot=%d proc=%08x msg=0x%04x ecx=%08x [handler]=%08x ret=%08x\n",
          i,proc,msg,g_wndCallEcx, g_d2base?c.read_u32(g_d2base+0x3d55d8):0, retaddr); std::fflush(stdout); } }
    return g_gcsCode[i];
}

// Guest stub for a CDECL call with ONE argument. guest_call_stub above always
// pushes four and cleans nothing, which would leak 16 bytes of stack per call
// for a cdecl function. Here: push arg, call, add esp,4, push retaddr, ret.
// Code is written once; only the data changes (same anti-SMC discipline as
// guest_call_stub).
static uint32_t g_gc1Code=0, g_gc1Data=0;
static uint32_t guest_call1_stub(Cpu& c, uint32_t proc, uint32_t arg, uint32_t retaddr){
    if(!g_gc1Code){
        g_gc1Data=misc(0x10);
        uint32_t st=misc(0x20); g_gc1Code=st;
        std::vector<uint8_t> b;
        auto imm=[&](uint32_t a){ for(int k=0;k<4;k++) b.push_back((uint8_t)((a>>(8*k))&0xff)); };
        b.push_back(0xFF); b.push_back(0x35); imm(g_gc1Data+0);   // push [dd+0]  (arg)
        b.push_back(0xFF); b.push_back(0x15); imm(g_gc1Data+4);   // call [dd+4]  (proc)
        b.push_back(0x83); b.push_back(0xC4); b.push_back(0x04);  // add esp,4    (cdecl)
        b.push_back(0xFF); b.push_back(0x35); imm(g_gc1Data+8);   // push [dd+8]  (retaddr)
        b.push_back(0xC3);                                        // ret
        c.write(st,b.data(),(uint32_t)b.size());
    }
    c.write_u32(g_gc1Data+0,arg);
    c.write_u32(g_gc1Data+4,proc);
    c.write_u32(g_gc1Data+8,retaddr);
    return g_gc1Code;
}

#ifdef D2V_CRASHTEST
// Guest stub for a CDECL call with THREE arguments (only D2_CRASHTEST=halt's
// call into Game.exe's own Halt reporter thunk needs this — see
// d2crashtest_tick below and the disassembly comment there). Same anti-SMC
// discipline and cdecl cleanup pattern as guest_call1_stub: push c,b,a (cdecl
// pushes right-to-left, so a ends up closest to the return address, i.e. at
// [ebp+8] inside the callee — the calling convention every real call site in
// Game.exe already relies on), call, add esp,0xC, push retaddr, ret.
static uint32_t g_gc3Code=0, g_gc3Data=0;
static uint32_t guest_call3_stub(Cpu& c, uint32_t proc, uint32_t a, uint32_t b, uint32_t c_arg, uint32_t retaddr){
    if(!g_gc3Code){
        g_gc3Data=misc(0x18);
        uint32_t st=misc(0x28); g_gc3Code=st;
        std::vector<uint8_t> buf;
        auto imm=[&](uint32_t v){ for(int k=0;k<4;k++) buf.push_back((uint8_t)((v>>(8*k))&0xff)); };
        buf.push_back(0xFF); buf.push_back(0x35); imm(g_gc3Data+8);    // push [dd+8]  (c_arg)
        buf.push_back(0xFF); buf.push_back(0x35); imm(g_gc3Data+4);    // push [dd+4]  (b)
        buf.push_back(0xFF); buf.push_back(0x35); imm(g_gc3Data+0);    // push [dd+0]  (a)
        buf.push_back(0xFF); buf.push_back(0x15); imm(g_gc3Data+12);   // call [dd+12] (proc)
        buf.push_back(0x83); buf.push_back(0xC4); buf.push_back(0x0C); // add esp,0xC  (cdecl, 3 args)
        buf.push_back(0xFF); buf.push_back(0x35); imm(g_gc3Data+16);   // push [dd+16] (retaddr)
        buf.push_back(0xC3);                                          // ret
        c.write(st,buf.data(),(uint32_t)buf.size());
    }
    c.write_u32(g_gc3Data+0,a);
    c.write_u32(g_gc3Data+4,b);
    c.write_u32(g_gc3Data+8,c_arg);
    c.write_u32(g_gc3Data+12,proc);
    c.write_u32(g_gc3Data+16,retaddr);
    return g_gc3Code;
}
#endif  // D2V_CRASHTEST

// shared guest state.
// LastError lives at TIB+0x34 and TLS slots at TIB+0xE10 — real per-thread
// Windows layout (see set_lasterr/cur_tib below, after g_sched). This lets
// GetLastError/SetLastError/TlsGetValue be guest-inlined 2-insn fs: stubs
// (profile: they were 3.9M of 8M dynarec-exit traps per 6000 frames).
uint32_t g_tickVA=0;          // guest dword pair: [g_tickVA]=tick, [+4]=0x10000+virt_ms; extern: see rt_host.h
bool g_realclock=false;       // live only once the scheduler starts (see below); extern: see rt_host.h
bool g_rtwant=false;          // requested: Vita default / D2_REALCLOCK on desktop; extern: see rt_host.h
const bool g_114=true;               // 1.14 monolith only; kept so pre-existing
                                     // "if(g_114)" guards still compile.
uint32_t g_tickOfs=0;         // virtual->real tick continuity offset; extern: see rt_host.h
unsigned long long g_ioUs=0;              // cumulative host file-I/O time (µs); extern: see rt_host.h
// Monotonic clock: ALIAS. The real implementation lives in the engine
// (runtime/host_clock.h) — a clock whose implementation depends on the
// PLATFORM rather than the game has no business living in a port. Host SLEEP
// stays here: on console it goes through the port's presentation layer.
#ifdef __vita__
static void rt_sleep_ms(uint32_t ms){ d2vita_sleep_ms(ms); }
#else
#include <unistd.h>
static void rt_sleep_ms(uint32_t ms){ if(ms) usleep(ms*1000u); }
#endif

// ---- D2_CRASHTEST=native|halt -----------------------------------------
// Compiled ONLY with -DD2V_CRASHTEST
// (tools/build_rt_boot_vpk.sh D2V_CRASHTEST=1) — absent from every normal or
// shipping build, so none of this exists there. Deliberately, quickly and
// safely triggers one real instance of each of two main crash Kinds
// (src/crashreport/cr_types.h) so the maintainer can prove the whole
// reporting pipeline (dialog, consent, upload, server-side dedup on a
// second identical trigger) on real hardware without reproducing an organic
// bug. Same convention as D2_EIPTRAP/D2_HALTWATCH/D2_CRCWATCH: getenv()
// read once, cheap (a single already-taken branch) when unset.
//
// A third leg, D2_CRASHTEST=hang (parking the native-scheduler thread
// forever to trip automatic hang detection), has no counterpart any more:
// automatic hang detection itself was removed (src/crashreport/cr_evidence.cpp)
// once real-hardware testing showed the general watchdog already covers a
// genuine freeze — a hung session is no longer classified or reported by
// this system at all.
//
// Trigger point: the first observed frame at or past kD2CrashTestFrame (see
// crashtest_should_fire, src/crashreport/cr_crashtest.h) of the SAME
// per-frame counter the watchdog publishes as "frames=" (g_frame,
// incremented by the d2vGlideFlush shim below, AND by dumpFrame's GDI
// equivalent — see d2crashtest_tick's own comment for why a >= check is used
// instead of an exact match: depending on which rendering path a boot takes,
// the two call sites sharing that counter can skip a single exact value).
// That call site is always AFTER d2cr_boot_collect() — which
// runs once, at process start inside d2vita_platform_init(), long before
// Game.exe is even loaded — and after g_d2base is set (Game.exe's module
// base, needed by the halt path below), so triggering here cannot race or
// corrupt that ordering, and cannot corrupt boot_progress.txt/session.txt in
// a way that would break evidence collection for THIS SAME crash (that
// collection already ran, for the PREVIOUS session, before this process
// existed). Empirically (tools/rt_boot_arm_check.sh), the title screen is
// reached and rendering within ~12 such frames under qemu-arm;
// kD2CrashTestFrame sits comfortably past that (the "reached the main menu"
// bar used here — no D2SCRIPT input is injected to click past the title
// screen, so "clickable main menu pixels on screen" is not independently
// confirmed, only "boot far enough to be rendering frames steadily") while
// staying fast (a couple of qemu-arm seconds), and without needing any
// savegame.
#ifdef D2V_CRASHTEST
constexpr int kD2CrashTestFrame = 20;
// "failed at (%i)" / "%08x" sentinel passed to the game's OWN real Halt()
// thunk: 8 digits, all 9s — not a plausible organic Halt id (every one on
// record is 3-4 digits: 316, 904, 1420, 1632) and safely decimal-parseable
// (cr_crashtxt_parse.cpp's dec_all caps at 9 digits, so a full 32-bit
// pattern like 0xDEADBEEF would silently fail to parse — deliberately not
// used here).
constexpr uint32_t kD2CrashTestHaltCode = 99999999u;
// Exit code D2_CRASHTEST=halt's own deliberate post-report termination uses
// (see the "haltPendingExit" block below) — deliberately NOT 2 (this file's
// and scheduler_backend.cpp's existing "FATAL: bad config" _exit(2) — reusing
// it would make a successfully-triggered Halt look like a config mistake)
// and NOT 139 (cpu_box86.cpp diag_segv's real-SIGSEGV shape — Halt is not a
// native fault, reusing it would misrepresent which Kind actually fired). No
// meaning beyond "a stable, distinctive, nonzero code a test can assert on".
constexpr int kD2CrashTestHaltExitCode = 3;

static void d2crashtest_tick(Cpu& c, Bridge& br, int frame){
    // D2_CRASHTEST=halt arms this (see the Halt case below) instead of
    // terminating immediately on its own firing tick: br.redirect_next()
    // only SCHEDULES where the guest CPU goes next — the actual guest-side
    // call into Game.exe's reporter (the one that writes the real Crash.txt)
    // does not run until AFTER this host function returns and the
    // dynarec/interpreter loop resumes. _exit()-ing on the firing tick would
    // therefore kill the process BEFORE the reporter ever ran: no Crash.txt,
    // a straight regression of the already-verified "produces a real,
    // correctly-shaped Crash.txt" behavior. So instead this waits for the
    // VERY NEXT tick (from either per-frame hook site): by then the
    // redirected call chain (guest_call3_stub -> the real Halt thunk at
    // game_base+0x8a60 -> the real reporter -> the thunk's own tail-jmp
    // chain, all a few dozen x86 instructions total) has had an entire
    // additional frame's worth of guest execution to run to completion —
    // see the Halt case for why that chain does not reliably stop the
    // process on its own. d2cr_shutdown() before _exit() matches this file's
    // existing convention for a raw process-termination call that skips C++
    // unwinding (the D2_SIGNTAG _exit(2) above, "not defensible in any mode"
    // to leave a thread alive across a hard stop) — a safe no-op here since
    // D2_CRASHTEST=halt never starts a real upload thread under qemu-arm.
    static bool haltPendingExit=false;
    if(haltPendingExit){
        char m[96]; std::snprintf(m,sizeof m,"D2_CRASHTEST=halt: arret du processus (exit=%d)",kD2CrashTestHaltExitCode);
        d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
        d2cr::d2cr_shutdown();
        // std::_Exit (ISO C++, <cstdlib>, already included above), not the
        // D2_SIGNTAG precedent's bare _exit: at this exact point in the file,
        // under the real VitaSDK/newlib compiler, unistd.h's _exit is not
        // visible (unclear why — it resolves fine for D2_SIGNTAG further down
        // this same file). _Exit is the POSIX-equivalent standard name and
        // compiles under both arm-vita-eabi-g++ and the qemu-arm toolchain.
        std::_Exit(kD2CrashTestHaltExitCode);
    }
    static const d2cr::CrashTestKind kind = d2cr::crashtest_kind_from_env(getenv("D2_CRASHTEST"));
    if(kind==d2cr::CrashTestKind::None || !d2cr::crashtest_should_fire(frame,kD2CrashTestFrame)) return;
    static bool fired=false; if(fired) return; fired=true;   // once per process
    char m[96]; std::snprintf(m,sizeof m,"D2_CRASHTEST=%s: declenchement au frame %d",
                              d2cr::crashtest_kind_name(kind), frame);
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
    switch(kind){
      case d2cr::CrashTestKind::Native: {
          // A REAL ARM-level fault, not a fabricated dump. On real Vita
          // hardware this is exactly spec §4.1's host_fault: no native fault
          // handler exists there at all (cpu_box86.cpp's diag_segv is
          // #ifndef __vita__ only — "a native fault kills the process; the
          // only trace is psp2core-*.psp2dmp"), so the OS itself produces a
          // genuine dump -> Kind::HostFault on the next boot.
          // Off Vita (this qemu-arm gate) that SAME diag_segv IS compiled in
          // and DOES catch it — confirmed empirically (qemu: "uncaught
          // target signal 11 ... core dumped", exit 139) — but it prints a
          // diagnostic to crash.log and _exit(139)s unconditionally; it does
          // NOT write boot_progress.txt's "NATIVE FAULT thr..." line (that
          // is a DIFFERENT, unrelated mechanism: winx86 sched_native.cpp's
          // own SOFTWARE bounds-check around actual guest-thread execution,
          // triggered only by EMU().error, never by a raw host SIGSEGV). And
          // boot_progress.txt itself is never written under qemu-arm at all
          // regardless — d2vita_progress()/wx86_vita_progress_c() are BOTH
          // literal no-ops off Vita (vita_present.h:100, vita_host.cpp:246;
          // rt_boot.cpp's own comment at the d2_crashlog call site: "no-op on
          // qemu, boot-progress log on Vita"). So under qemu-arm this
          // produces Kind::None (no evidence recognized at all: crash.log
          // gets an unrecognized-format "CRASH host-sig=11..." line, no
          // psp2core, no NATIVE FAULT line, session.txt stays
          // state=running). This is exactly why Kind::HostFault is
          // fundamentally real-hardware-only: nothing under qemu-arm can
          // produce the OS-level dump it requires.
          volatile uint32_t* bad=(volatile uint32_t*)(uintptr_t)0xFFFFFFF0u;   // NOT a low address: guest linear 0 is the mapped Windows TIB here (qemu-arm's "-B 0x10000"; empirically confirmed a write to 0x10 landed inside it and did NOT fault)
          *bad = 0xD2C7357u;
          break; }
      case d2cr::CrashTestKind::Halt: {
          if(!g_d2base){ d2vita_progress("D2_CRASHTEST=halt: g_d2base absent (Game.exe pas encore charge) — ignore"); break; }
          // Same idiom as CallWindowProcA/SendMessageA above: read the
          // return address this trap would otherwise resume at, then
          // redirect into the guest's OWN "Halt" thunk (Game.exe 1.14d VA
          // 0x408a60 = game_base+0x8a60 — cr_crashtxt_parse.cpp) with a
          // synthetic code. This runs the REAL reporter (VA 0x408900), which
          // writes a REAL Crash.txt through the existing kernel32 file shims
          // — exactly as an organic Halt(1420)-style call site would, just
          // with a synthetic, obviously-not-organic code. Confirmed by an
          // actual run: a real Crash.txt appears, correct shape,
          // "[Halt] (...) failed at (...)" summary line, real DBG-ADDR call
          // chain, classified Kind::Halt by build_evidence().
          //
          // ABI, read from the real disassembly (objdump -d -M intel
          // --start-address=0x408a60 --stop-address=0x408a86
          // ~/d2-vita-refs/1.14d/Game.exe), NOT guessed: the thunk is
          //   push ebp; mov ebp,esp; call 0x40d8d0 (stack-walk: this is what
          //   produces Crash.txt's real DBG-ADDR frames, no args needed);
          //   eax=[ebp+0xc]=b; ecx=[ebp+0x10]=c;
          //   push b; push 0x6cd298; push c; push 0x6cc837("" file); push 6;
          //   call 0x408900 (the common reporter — 5 args: type-selector 6
          //   ("Halt"), file="", c, a format template @0x6cd298
          //   ("Unrecoverable internal error %08x"), b).
          // A caller must push 3 cdecl args (a,b,c) — the thunk never reads
          // `a` itself (stack padding only: calling with fewer than 3 pushed
          // args, e.g. guest_call1_stub's single push, leaves b/c reading
          // STALE stack words instead, producing unrelated leftover values
          // for both — this is why guest_call3_stub exists rather than
          // reusing guest_call1_stub). c is VERIFIED to
          // land exactly in the "(%i)" slot (a real run showed "failed at
          // (99999999)", the exact chosen value) — and per
          // cr_crashtxt_parse.cpp's own parse_summary, THAT trailing decimal
          // is the only thing that becomes h->code / the claim's
          // features.code / the server dedup signature; the "(%s)"
          // description text (meant to hold b formatted as "%08x", per the
          // format template at 0x6cd298) is discarded unparsed. b is passed
          // for completeness (an organic call site would set both) but its
          // exact rendering in the description is NOT reverse-engineered
          // further here — real runs showed a plausible-looking but
          // unconfirmed stack-address-shaped value there instead of b's hex,
          // a purely cosmetic gap with no effect on evidence, the claim, or
          // classification (all keyed off c alone).
          //
          // Why this alone does NOT stop the game (seen on hardware: a real
          // Crash.txt is written, correctly classified, and the game just
          // keeps running) — traced the same way as the ABI above, by
          // disassembling further instead of guessing:
          //   408a7c: call 0x408900        (the reporter, above)
          //   408a81: add esp,0x14
          //   408a84: pop ebp
          //   408a85: jmp  0x40f960        <- NOT a ret. The thunk tail-calls
          //   this shared address instead of returning to its caller — every
          //   sibling thunk (0x408a90/0x408ac0/0x408af0/0x408a30, other
          //   report types) ends the exact same way. 0x40f960 is itself a
          //   1-instruction jmp to 0x40f8a0, whose body (also reached with a
          //   plain CALL from partway through 0x408900 itself, i.e. before
          //   Crash.txt's dialog/text are even finished) branches on a flag
          //   at VA 0x75d2ec:
          //     - flag != 0: enter/wait/leave a critical section (looks like
          //       "let pending network sends flush", capped ~1s) then a
          //       plain `ret` — which pops whatever return address was on
          //       the stack when the Halt thunk was FIRST entered. For an
          //       organic call site that is the organic caller, resuming ITS
          //       code as if Halt() had simply returned; for our injected
          //       call (guest_call3_stub's own retaddr = the original trap
          //       site) that is exactly the observed bug: execution quietly
          //       resumes normal gameplay.
          //     - flag == 0: malloc(0x1c8), a SECOND call back into this
          //       same Halt thunk (0x408a60, with only 2 explicit args — c
          //       ends up reading stack residue, a detail that does not
          //       matter here), then unconditionally `push 0xffffffff; call
          //       0x681e09` — a real CRT-shaped exit wrapper (it forwards
          //       one incoming arg into `call 0x681d27` alongside two zero
          //       args, the classic MSVC doexit(code,quick,retcaller)
          //       shape) that never returns: the REAL "process-exit" path
          //       the task asked to look for.
          //   Reaching that real exit deterministically would mean flipping
          //   the flag at 0x75d2ec directly, but what else reads that flag
          //   is not fully characterized (it also gates the malloc'd buffer
          //   + second recursive reporter call above, which could plausibly
          //   overwrite the already-verified-correct Crash.txt with a
          //   second, garbage-code report before the real exit runs) — not
          //   practically verifiable without hardware access, so this does
          //   NOT force that flag. Instead
          //   (documented fallback): let the real reporter run and finish
          //   genuinely untouched (this redirect is unchanged below), and
          //   separately guarantee termination ourselves — see
          //   "haltPendingExit" at the top of this function, armed right
          //   below. Net effect matches native's standard (a real, non-zero
          //   exit code — see kD2CrashTestHaltExitCode) without touching the
          //   not-fully-understood flag or risking a second, malformed
          //   report clobbering the first.
          const uint32_t retaddr = c.read_u32(c.reg(R_ESP));
          const uint32_t haltThunk = g_d2base + 0x8a60;
          br.redirect_next(guest_call3_stub(c, haltThunk, 0, kD2CrashTestHaltCode, kD2CrashTestHaltCode, retaddr));
          haltPendingExit=true;   // force a real exit on the NEXT tick — see top of this function
          break; }
      default: break;
    }
}
#endif  // D2V_CRASHTEST

uint64_t rt_now_us(){ return wx86_now_us(); }
uint64_t rt_now_ms(){ return wx86_now_ms(); }
// D2_RTSCALE=N: run the REAL clock N× faster (dev-iteration speedup). The guest's
// clock advances N× and the pacing sleep is divided by N, so the online flow keeps
// its protocol timing (a LOCAL server answers instantly regardless) but completes in
// ~1/N wall-clock. D2SCRIPT is frame-based, so its frame numbers stay valid. N=1
// (default) = untouched real time. Anchored so the scaled clock is continuous at the
// first read. Used by BOTH the scheduler real-clock and the guest tick page so
// GetTickCount stays consistent with virt_ms_.
static uint64_t g_rtScale = [](){ const char* e=getenv("D2_RTSCALE"); uint64_t n=e?(uint64_t)std::atoll(e):1; return n<1?1:n; }();
// Anchored, CONTINUOUS scaled clock that survives a mid-run scale change:
// g_rtVirtBase is the scaled ms at g_rtRawBase, and virtual time advances at
// g_rtScale from there. rt_set_scale() freezes the current virtual time into the
// base before switching the rate, so the guest clock never jumps. This lets the
// menu/lobby run at N× (fast dev iteration) while the IN-GAME act-load drops back
// to 1× — the load is compute-bound (not clock-bound), so at N× the client's own
// load timeout (measured in guest time) fires N× sooner in wall-clock and kills a
// load that had not finished. Dropping to 1× on the game-server connect gives the
// load the full real-time window it needs. See connect() shim (port 4000).
// Rate is a rational g_rtScale/g_rtDiv: guest_ms advances by that factor per real
// ms. Nav uses N/1 (fast). The in-game act-load is compute-bound (qemu ~1 D2GS
// packet/s) and races D2's own load timeout, which is measured in GUEST time — so
// SLOWING the guest clock (1/K) buys the load K× more real wall-clock before that
// timeout fires, letting it finish and send LOADCOMPLETE. D2_INGAME_SLOW sets K.
static uint64_t g_rtDiv = 1;
// Default 1 = in-game runs at real time (correct for hardware). >1 is a dev-only
// qemu workaround: slowing the guest clock stretches the client's guest-time load
// timeout so a slow double-emulated act-load has more real time (see docs).
static uint64_t g_ingameSlow = [](){ const char* e=getenv("D2_INGAME_SLOW"); uint64_t n=e?(uint64_t)std::atoll(e):1; return n<1?1:n; }();
static uint64_t g_rtRawBase=0, g_rtVirtBase=0; static bool g_rtBaseSet=false;
static void rt_scale_init(uint64_t raw){ if(!g_rtBaseSet){ g_rtRawBase=raw; g_rtVirtBase=raw; g_rtBaseSet=true; } }
static uint64_t rt_now_ms_scaled(){ uint64_t raw=rt_now_ms(); rt_scale_init(raw);
    return g_rtVirtBase + (raw - g_rtRawBase) * g_rtScale / g_rtDiv; }
static void rt_set_rate(uint64_t num, uint64_t den){ uint64_t raw=rt_now_ms(); rt_scale_init(raw);
    g_rtVirtBase = g_rtVirtBase + (raw - g_rtRawBase) * g_rtScale / g_rtDiv;   // freeze virtual time at the switch
    g_rtRawBase = raw; g_rtScale = num?num:1; g_rtDiv = den?den:1; }
static void rt_sleep_ms_scaled(uint32_t ms){ rt_sleep_ms((uint32_t)((uint64_t)ms * g_rtDiv / g_rtScale)); }
// OS locale + timezone detected from the REAL user's system, so SID_AUTH_INFO
// mirrors the actual environment: a French install/Vita reports French, an
// English one English, etc. Vita reads the system language + RTC timezone;
// desktop derives from the POSIX LANG/LC_* + localtime. D2_LCID / D2_TZBIAS
// override; neutral fallback is en-US / the host offset.
#ifdef __vita__
extern "C" uint32_t d2vita_sys_lcid(void);     // Vita system language -> Windows LCID
extern "C" int32_t  d2vita_sys_tzbias(void);   // Vita timezone -> minutes (UTC = local + bias)
uint32_t detect_lcid(){ return d2vita_sys_lcid(); }   // extern: see rt_host.h (for win32_import_remainder.cpp)
int32_t  detect_tzbias(){ return d2vita_sys_tzbias(); }   // extern: see rt_host.h (for kernel32_time.cpp)
#else
uint32_t detect_lcid(){ return wx86_locale_detect_posix(); }  // implementation: guest_locale.cpp; extern: see rt_host.h
int32_t detect_tzbias(){   // extern: see rt_host.h (for kernel32_time.cpp)
    time_t now=std::time(nullptr); struct tm lt; if(localtime_r(&now,&lt)) return -(int32_t)(lt.tm_gmtoff/60);
    return 0;
}
#endif
#ifdef D2_TLSCOUNT
// emutls call counters (measurement build only); defined in cpu_box86.cpp / bridge.cpp.
extern "C" { extern unsigned long long d2_tls_hits; extern unsigned long long d2_tls_traps; }
#endif
extern "C" { extern uint64_t dyn86_fill_ns; extern uint32_t dyn86_fill_count;
             extern uintptr_t dyn86_eiptrap; extern uintptr_t dyn86_eiptrap2;
             extern uint32_t dyn86_diag_regs;   // arms the arm_next.S reg-sync STM (diag builds)
             void dyn86_dump_eipring(void);
             void dyn86_set_native(int);
             int dyn86_alt_count(void); int dyn86_alt_lost(void); }   // native: g_stop is monotonic (cpu.h request_stop)
static uint32_t g_tlsN=1;            // TlsAlloc watermark (index 0 reserved, slots live in TIBs)

// host-backed file I/O (real semantics): guest HANDLE -> host FILE*.
#include <map>
std::string g_dataRoot;   // extern: see runtime/path_cache.h
std::string g_writeRoot;   // scratch dir for guest-created (write) files, e.g. Fog logs
static std::string g_crashLogPath;   // $D2WRITE/crash.log — see d2_crashlog()
// Ownership lives in the engine (guest_files.h): the four file-handle tables
// are engine state, not port state. What stays here is path RESOLUTION
// (host_path) — where data lives and under what conventions is port-specific.
// These are REFERENCES, not copies: only one table exists, owned by the engine.
static std::map<uint32_t,FILE*>& g_files = wx86_files();
// Host path per open handle + writer flush: D2 re-reads a file (VITA.d2s at
// game entry) through a SECOND handle while the writer handle is still open —
// unflushed stdio buffers make the reader see a short/empty file. Won or lost
// on timing (newlib vs glibc, realclock interleavings): the intermittent
// hardware crash/hang entering a game. Flush every open handle on the same
// host path before opening it again.
std::map<uint32_t,std::string>& g_filePathByH = wx86_file_paths();   // extern: see rt_host.h (for io_stat.cpp)
// flush_same_path() moved to runtime/kernel32_files.cpp: its only caller
// (do_create_file) lives there now, with its own g_files/g_filePathByH
// reference (the latter via extern rt_host.h; the former its own reference
// to the same engine table, exactly like this one).

// File mappings (CreateFileMapping/MapViewOfFile). These do NOT serve the
// lockdown CheckRevision DLL — disassembly confirms it imports neither
// ReadFile, CreateFileMapping nor MapViewOfFile, and hashes no game file bytes
// at all. They serve the CLASSIC CheckRevision internal to Game.exe, which
// does map executables. handle -> (source path, size).
using FMap = WxFMap;                       // engine (guest_files.h)
static std::map<uint32_t,FMap>& g_fmaps = wx86_file_maps();
#include <dirent.h>
#include <strings.h>
#include <algorithm>
static std::map<uint32_t,std::pair<std::vector<std::string>,size_t>> g_finds;  // FindFirstFile state
std::map<uint32_t,PeImage*> g_modByBase;      // HMODULE (load base) -> image; extern: see rt_host.h
std::map<std::string,PeImage*> g_modByName;   // lowercased basename -> image (for LoadLibrary); extern: see rt_host.h
d2rt::Bridge* g_bridge=nullptr;               // set after commit; runtime LoadLibrary of on-disk DLLs; extern: see rt_host.h
std::function<std::string(uint32_t)>* g_locp=nullptr;
// PROF_COUNTERS: per-phase profile dump. g_profDump is bound in main() (needs
// br/sched); phase boundaries come from PROF_PHASES="f1,f2,..." (default
// matches the 6-phase Rogue scenario).
std::function<void(const char*)> g_profDump;   // extern: see rt_host.h (for scripted_input.cpp)
static uint64_t g_profT0=0;
#ifdef PROF_COUNTERS
static void prof_phase_tick(int frame){
    static std::vector<int> b; static size_t bi=0;
    if(b.empty()){ const char* e=getenv("PROF_PHASES");
        std::string s=e?e:"1500,2820,3200,4500,6000"; size_t p=0;
        while(p<s.size()){ b.push_back(atoi(s.c_str()+p)); p=s.find(',',p);
            if(p==std::string::npos) break; ++p; } }
    if(bi<b.size() && frame>=b[bi]){ char l[24]; std::snprintf(l,sizeof l,"P%zu@frame%d",bi+1,frame);
        if(g_profDump) g_profDump(l); ++bi; }
}
#endif
std::function<void()> g_dump_fring;   // dump recent file ops (set once shims exist)

uint32_t g_hwnd=0, g_wndProc=0;   // the game's window + current wndproc (subclassable)

// Dynamically-loaded SYSTEM dlls (shimmed, no PE image): pseudo-handles let
// LoadLibraryA/GetModuleHandleA + GetProcAddress resolve to shim traps.
std::map<uint32_t,std::string> g_sysLib;   // pseudo-handle -> dll name (lc); extern: see rt_host.h
uint32_t g_sysLibNextH=0x7E000001u;   // extern: see rt_host.h
// d2.lng: resource-only module (localized strings/accelerators), absent from
// our refs. D2Client only needs LoadLibraryExA("D2.LNG",0,DATAFILE) to succeed
// and then pulls resources via LoadAcceleratorsA/LoadStringA (shimmed) — a
// pseudo-handle avoids "Unable to start LNG manager".
const char* SYS_DLLS[]={"kernel32.dll","user32.dll","gdi32.dll","advapi32.dll",   // extern: see rt_host.h
    "wsock32.dll","ws2_32.dll","winmm.dll","dsound.dll","ddraw.dll","imm32.dll","version.dll",
    "d2.lng",
    // d2vhost.dll: FAKE system DLL through which our x86 glide3x.dll obtains its
    // three host entry points (init, flush, texture upload). The game never
    // requests it directly.
    "d2vhost.dll",
    // glide3x.dll is intentionally absent here: loadlib() skips this entry and
    // loads the REAL x86 DLL from disk instead (see the comment at that site),
    // so listing it here would have no effect anyway.
    nullptr};
// Real GPU counters live in g_gr*/gr_line() (src/glide_ring/gx_host.cpp).
std::deque<GMsg> g_msgQ;
uint64_t g_pmN=0, g_gmN=0, g_dispN=0, g_injN=0, g_coalN=0;
uint64_t g_readN=0;   // ReadFile calls (watchdog liveness); extern: see rt_host.h
// D2_IOHIST: histogram of ReadFile LENGTHS. The seek+read pair is 1:1 on
// Storm's path, so the read count IS the pair count — and a seek costs ~4.8 ms
// on the memory card versus ~30 us for the read. This histogram turns "cut the
// pair count by eight" from a claim into a measurement.
uint64_t g_rdHist[24]={0};   // index = floor(log2(length)), 23 = >=8 MiB; extern: see rt_host.h
uint64_t g_rdBytes=0;        // extern: see rt_host.h
// struct RdSite now declared in runtime/rt_host.h (kernel32_files.cpp's
// ReadFile populates this map under D2_IOHIST>=2).
std::map<uint32_t,RdSite> g_rdSite;   // guest return address -> reads; extern: see rt_host.h
int g_ioHist=0;   // extern: see rt_host.h
// D2_CELWATCH: tracks the CelData cache (usage/ceiling + LRU evictions).
static uint32_t g_celPeak=0;       // maximum usage observed (bytes)
uint32_t g_celWatch=0;      // period (frames) of the [CELDATA] line; 0 = off
static uint32_t g_celCap=0;        // D2_CELCAP: CelData ceiling overridden from our side; 0 = untouched
uint64_t g_celEvict=0;      // LRU evictions from the CelData cache (real saturation)
uint64_t g_celEvictOther=0; // same for the other caches (control/reference)
bool g_traceOn=false;   // switched on at TRACEAFTER=<frame> (see frameTick)
uint32_t g_nextH=0x81000000;   // extern: see rt_host.h (for kernel32_files.cpp)
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cerrno>


// ---- real host sockets for Battle.net (D2NET=1) -----------------------------
// The Winsock ordinal shims below dispatch here when wx86_net_enabled().
// ONE code path, host and console: the #ifndef __vita__ guards are gone
// because every real function already tests the network flag on its first
// line. Two offline stubs maintained in parallel means two behaviors to
// diagnose; the runtime flag alone is enough as a switch. The two primitives
// the VitaSDK libc doesn't make reliable (switching to non-blocking, name
// resolution) now live in winx86 (net_nonblock.h) — the Vita specificity was
// never D2's concern.
// Guest SOCKETs are small ints 1..N mapped to host fds. Host fds are kept
// NON-BLOCKING so no single call ever freezes the cooperative scheduler; the
// guest's own non-blocking + select loop (D2Net's model) drives retries, and
// blocking-mode guest sockets get a bounded yield-poll.
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
// <netdb.h> is no longer included here: no getaddrinfo remains in this file,
// resolution goes through vita_net. The VitaSDK header declares getnameinfo
// with `restrict`, a C keyword C++ does not recognize, so including it would
// break the console build for no benefit.
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
// MSG_NOSIGNAL doesn't exist in the VitaSDK's newlib, and isn't needed: with
// no SIGPIPE, send just returns an error. Same fallback as
// src/platform/vita_net.cpp; otherwise the console build fails here.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
// Version string ACTUALLY hashed by CheckRevision (":1.14.3.71:" expected;
// ":0.0.0.0:" means host_path failed). Captured at the CryptHashData entry —
// in runtime/checkrevision_crypto.cpp now (g_crAuthByte moved there with it;
// only that file's CryptHashData shim uses it). g_crHashedVer stays here:
// the D2CRTEST self-test dump later in main() also reads it.
std::string g_crHashedVer;   // extern: see rt_host.h
// g_netOn/GSock/g_socks/g_sockNext/g_wsaErr/gsock_fd/gsock_port live in winx86
// (wx86_net_enabled/set_enabled, WsockHandle, wx86_sock_*,
// wx86_net_last_error/set_last_error — win32_shims_wsock32.h): the socket
// table and network flag were never D2-specific. Only the connect/recv/send/
// select logic that uses them (below) stays here.

// ---- passive network OBSERVER (D2_NETWATCH=1) -------------------------------
// OBSERVE ONLY, per user spec: frame the BNCS (0xFF,id,len16) and MCP (len16,id)
// streams and log every packet id, EXPLICITLY flagging SID_WARDEN 0x5E (BNCS).
// The D2GS stream (port 4000) is Huffman-compressed, so we do NOT mis-frame it:
// we log the raw server->client payload (timestamp + size) for offline decode and
// note any literal 0xAE byte as "verify by decode". Absolute rule: this NEVER
// fabricates a reply (no forged 0x66), NEVER simulates Warden, and NEVER hides a
// packet — an unknown/compressed packet is logged raw, always visible.
// FUNCTION-scope static, not file-scope. A file-scope initializer runs BEFORE
// main, i.e. before d2vita_platform_init reads ux0:data/d2vita/env.txt: on
// console the flag would then always be false and the observer impossible to
// arm, precisely on the platform with no other way to see packets go by. Same
// shape as env_netlog().
#include <cstdarg>
static bool g_netwatch_on(){ static const bool v=getenv("D2_NETWATCH")!=nullptr; return v; }

// Network log goes TO A FILE. On console there is no standard output: anything
// netwatch and the network shims print there vanishes, making the observer
// mute exactly on the platform where it is the ONLY way to see packets go by.
// The file lives next to the save files, so it's retrievable over FTP.
static FILE* netlog_file(){
    static FILE* f = nullptr; static bool tried = false;
    if(!tried){ tried = true;
        if(g_netwatch_on() || getenv("D2NETLOG") || getenv("D2_SYNCLOG")){
            std::string p = (g_writeRoot.empty()? std::string("/tmp") : g_writeRoot) + "/netlog.txt";
            f = std::fopen(p.c_str(), "w");
            pc_dirty(g_writeRoot);              // the log file CREATES a directory entry
            if(f) std::fprintf(f, "=== journal reseau D2Vita ===\n"); } }
    return f; }
// Writes to both stdout (host) and the file (console). fflush on every line:
// a killed run or a frozen console must still leave a COMPLETE log behind,
// otherwise exactly the ending that matters is lost.
static void nlog(const char* fmt, ...){
    va_list ap; va_start(ap, fmt); std::vprintf(fmt, ap); va_end(ap);
    if(FILE* f = netlog_file()){ va_list ap2; va_start(ap2, fmt);
        std::vfprintf(f, fmt, ap2); va_end(ap2); std::fflush(f); } }
struct NetAcc { std::vector<uint8_t> buf; };
static std::map<uint64_t,NetAcc> g_nw;      // key = (fd<<1)|(s2c?1:0)
static std::map<int,uint8_t> g_nwSel;       // fd -> protocol selector (0x01 game / 0x02 BNFTP / 0x03 chat), reset on connect
static const char* nw_phase(uint16_t p){ return p==6112?"BNCS":p==6113?"MCP":p==4000?"D2GS":"?"; }
static void nw_reset(int fd){ g_nwSel.erase(fd); g_nw.erase(((uint64_t)(unsigned)fd<<1)); g_nw.erase(((uint64_t)(unsigned)fd<<1)|1u); }
static void netwatch(int fd, uint16_t port, bool s2c, const uint8_t* data, int n){
    if(!g_netwatch_on() || n<=0) return;
    const char* dir = s2c ? "S->C" : "C->S";
    unsigned long long ts = (unsigned long long)rt_now_ms();
    if(port==4000){                          // D2GS: compressed — log raw, never mis-frame
        bool ae=false,r66=false; for(int k=0;k<n;k++){ if(data[k]==0xAE)ae=true; if(data[k]==0x66)r66=true; }
        nlog("    [netwatch] D2GS(4000) %s t=%llu %dB raw:",dir,ts,n);
        for(int k=0;k<n&&k<24;k++) nlog(" %02x",data[k]);
        nlog("%s [compressed]%s%s\n", n>24?" ...":"",
            (s2c&&ae)?"  <-- 0xAE byte present (Warden? decode to confirm)":"",
            (!s2c&&r66)?"  <-- 0x66 byte OUTBOUND (would-be Warden reply; verify)":"");
        return;
    }
    // The first C->S byte on a fresh BNCS/MCP connection is the protocol SELECTOR
    // (0x01 game, 0x02 BNFTP file transfer, 0x03 chat) — NOT part of the framing.
    // Record it once and strip it, so BNFTP/chat aren't mis-framed as 0xFF packets.
    if(!s2c && !g_nwSel.count(fd)){
        uint8_t sel=data[0]; g_nwSel[fd]=sel;
        nlog("    [netwatch] %s(%u) C->S selector=0x%02x fd=%d\n", nw_phase(port),port,sel,fd);
        data++; n--; if(n<=0) return;
    }
    uint8_t sel = g_nwSel.count(fd)? g_nwSel[fd] : 0x01;      // assume game if a S->C beats the selector
    if(sel==0x02 || sel==0x03){              // BNFTP / chat: not 0xFF-framed — log raw, never hide
        nlog("    [netwatch] %s(%u,sel=0x%02x) %s t=%llu %dB raw:", sel==0x02?"BNFTP":"CHAT", port, sel, dir, ts, n);
        for(int k=0;k<n&&k<24;k++) nlog(" %02x",data[k]); nlog("%s\n", n>24?" ...":"");
        return;
    }
    bool mcp = (port==6113);                 // MCP realm server: len16,id framing (BNCS: FF,id,len16)
    NetAcc& s=g_nw[((uint64_t)(unsigned)fd<<1)|(s2c?1u:0u)];
    s.buf.insert(s.buf.end(),data,data+n);
    for(;;){
        uint32_t hdr; uint16_t len; uint8_t id;
        if(!mcp){                            // BNCS: FF id len(LE16), len covers the 4-byte header
            if(s.buf.size()<4) break;
            if(s.buf[0]!=0xFF){ nlog("    [netwatch] BNCS DESYNC fd=%d %s byte=0x%02x (resync)\n",fd,dir,s.buf[0]); s.buf.erase(s.buf.begin()); continue; }
            len=(uint16_t)(s.buf[2]|(s.buf[3]<<8)); id=s.buf[1]; hdr=4;
        } else {                             // MCP: len(LE16) id ...
            if(s.buf.size()<3) break;
            len=(uint16_t)(s.buf[0]|(s.buf[1]<<8)); id=s.buf[2]; hdr=3;
        }
        if(len<hdr){ s.buf.erase(s.buf.begin()); continue; }   // bad frame: resync
        if(s.buf.size()<len) break;                           // wait for the rest
        const char* flag="";
        if(!mcp && id==0x5E) flag = s2c ? "   <-- SID_WARDEN 0x5E OBSERVED (server->client)"
                                        : "   <-- SID_WARDEN 0x5E SENT (client->server)";
        nlog("    [netwatch] %s(%u) %s t=%llu id=0x%02x len=%u%s\n", nw_phase(port),port,dir,ts,id,len,flag);
        s.buf.erase(s.buf.begin(), s.buf.begin()+len);
    }
}
// wsa_from_errno lives in winx86 (wx86_wsa_from_errno,
// win32_shims_wsock32.h) — a pure errno->WSA* table, nothing D2-specific.

// ---------------------------------------------------------------------------
// build_verinfo() (VS_VERSIONINFO/VS_FIXEDFILEINFO parsing) lives in winx86
// (win32_shims_version.cpp) — a pure PE parser, zero D2 content. Why this
// shim exists (checkrevision "3a" hashes the version string, not the raw
// bytes) is documented there.

// ---- guest threading: cooperative scheduler + kernel sync objects -----------
#include "runtime/guest_thread.h"
#include "runtime/sched_cooperative.h"
#include "runtime/sched_native.h"
#include "runtime/prof_map.h"
#include "runtime/gil.h"
#include <cstring>
ThreadScheduler* g_sched=nullptr;
// Coop-only diagnostics (switch counter, wake attribution, thread/switch-ring
// dumps): deliberately NOT in the ThreadScheduler interface. Concrete alias,
// set alongside g_sched at the construction sites; a non-coop backend leaves
// it null and these diagnostic paths skip (same null-when-native pattern as
// the plan's Task 3 `coop` pointer).
CooperativeScheduler* g_schedCoop=nullptr;
// g_casCalls (InterlockedCompareExchange spin-break counter) moved to
// runtime/kernel32_interlocked.cpp -- only that file's shim uses it.

// ---- Interlocked*: real atomic operations ----------------------------------
//
// WHAT WAS WRONG. The ten Interlocked* shims did a BARE read-modify-write, and
// their comment assumed "cooperative scheduler = no real races". That was
// true for COOP and already false for native, for a reason the GIL does not
// cover: the GIL serializes shim bodies against each other, but TRANSLATED
// code does NOT take it. The dynarec emits real LDREX/STREX + DMB for
// lock-prefixed x86 instructions (third_party/box86-dynarec/dynarec/
// dynarec_arm_00.c:1022-1074, dynarec_arm_66f0.c), so a translated "lock xadd"
// and an InterlockedIncrement shim can target the SAME guest address at the
// SAME instant with no lock separating them. A lost reference count is a
// premature free.
//
// WHY THIS IS SIMPLE TO FIX. Guest memory is a FLAT host mapping
// (CpuBox86::hostptr returns the host address for a guest VA), and translated
// code writes that same memory through that same mapping. A host atomic RMW
// is therefore atomic from the guest's point of view too, including against
// translated code. No allocation, no lock, no signature change.
//
// THE FALLBACK IS A HARDWARE NECESSITY, NOT LAZINESS. x86 tolerates an
// Interlocked op on an UNALIGNED address; ARM does not — an unaligned LDREX
// FAULTS. When the address isn't 4-aligned, or the host can't resolve it (VA
// outside the arena), it falls back to the old non-atomic path, exactly the
// prior behavior. This fallback is COUNTED: an atomicity fix that silently
// fell back would read as "this is atomic" when it isn't — the "diagnostic
// that lies" family of bug.
extern "C" int dyn86_protectdb(void);   // src/dynarec86/dyn86.c — 0 by default
// Atomics over guest memory: the implementation lives in the engine
// (third_party/winx86/src/runtime/guest_atomics.{h,cpp}) — the code was
// word-for-word identical in both ports, i.e. duplicated engine logic. The
// short names stay as local aliases so the dozens of call sites in this file
// don't need to change.
// ilk_ptr/ilk_cas moved to runtime/kernel32_interlocked.cpp (the only shims
// that still call them: InterlockedCompareExchange/-Pointer). The other
// aliases below are kept as-is (unused here now, but not this refactor's
// call to remove -- see the "dozens of call sites" note above; the generic
// Interlocked* shims that used them moved to the engine some time ago).
static inline uint32_t ilk_add_fetch(uint32_t* h, uint32_t v){ return wx86_ilk_add_fetch(h,v); }
static inline uint32_t ilk_sub_fetch(uint32_t* h, uint32_t v){ return wx86_ilk_sub_fetch(h,v); }
static inline uint32_t ilk_exchange (uint32_t* h, uint32_t v){ return wx86_ilk_exchange (h,v); }
static inline uint32_t ilk_fetch_add(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_add(h,v); }
static inline uint32_t ilk_fetch_or (uint32_t* h, uint32_t v){ return wx86_ilk_fetch_or (h,v); }
static inline uint32_t ilk_fetch_and(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_and(h,v); }
static inline uint32_t ilk_fetch_xor(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_xor(h,v); }
// Trap VA that clears the scheduler's loader-lock pin (set_no_preempt(false)).
// The DllMain redirect stub `call`s it right after DllMain returns, so the
// checkrevision thread runs DllMain non-preemptibly. Resolved lazily (first
// LoadLibrary DISK) once the trap window exists.
// (g_dllmainDoneTrap itself, and loadlib() that uses it, now live in
// runtime/kernel32_modules.cpp -- only that file needs it.)
// SHA-1 (streaming), Sha1/g_cryptHashes/g_cryptHashNext, and the CryptoAPI
// shims that use them all moved to runtime/checkrevision_crypto.cpp -- see
// that file for the full rationale.
// Active OS locale reported to Battle.net (SID_AUTH_INFO). DETECTED from the real
// system (Vita system language / host POSIX locale; see detect_lcid), so it's
// correct for any user — French install/Vita -> French, English -> English.
// D2_LCID overrides. Kept coherent across GetUserDefaultLangID/LCID/GetLocaleInfo
// so the client's locale fingerprint matches a real Windows client.
// LCID lives in the engine (guest_locale.h): the LCID -> codepage / language
// name / separators mapping is Windows knowledge, not game knowledge, and the
// other port carried the same copy. What stays HERE is the one piece that's
// actually consumer-specific: WHICH locale is active on THIS machine (the
// console's system language, or LC_ALL/LANG), and the name of the variable
// that forces it.
static const uint32_t g_lcid_init = [](){ const char* e=getenv("D2_LCID");
    wx86_locale_set_lcid(e?(uint32_t)std::strtoul(e,nullptr,16):detect_lcid()); return 1u; }();
// Timezone bias (g_tzbias, minutes; Windows sense: UTC = localtime + Bias) now
// lives in runtime/kernel32_time.cpp -- only GetLocalTime/GetTimeZoneInformation
// (there) read it. detect_tzbias() itself stays here (see above): it's the
// #ifdef __vita__ twin of detect_lcid(), which stays too.
uint64_t g_blitN = 0;   // native palette-blit shim invocations (proves the hook/alternate fires)
uint64_t g_blitFast=0, g_blitSlow=0;   // validated global extent / virtual-path fallback
uint64_t g_scompN = 0, g_scompFB = 0;   // native PKWARE explode: served / guest-fallback
// NATIVEDCC=1: native DCC decoder (src/runtime/dcc_native.cpp) over
// Game+0x20bff0. Counters: decodes served / fallbacks (reason) / bytes produced.
uint64_t g_dccNatN=0, g_dccNatFB=0, g_dccNatBytes=0, g_dccNatFrames=0, g_dccVerN=0, g_dccVerBad=0;
uint64_t g_scomp2N = 0, g_scomp2FB = 0; // same, for SCompExplode (2nd entry point)
// Native "hot spot" ports: a served counter + a faithful-fallback counter per
// target — a rising fallback count means the fast-path condition isn't
// holding and the gain isn't materializing.
uint64_t g_hotLightN=0, g_hotLightFB=0;   // 0x4ddef0 8x8 light grid
uint64_t g_hotBlendN=0, g_hotBlendFB=0;   // 0x4f6bb0 15-band "blend" blit
uint64_t g_hotRleN=0;                     // 0x606d40 cell RLE blit
uint64_t g_hotCollN=0;                    // 0x64cb30 collision lookup
// NO fallback counter for these two: the RLE path always completes (slow
// c.read/c.write path if the host view is missing), and collision has had no
// fallback since the slow path at 0x463740 was ported. A permanent "/0" would
// have implied a fallback existed when it doesn't.
uint64_t g_hotGridN=0;                    // 0x4756d0 grid reconstruction
uint64_t g_gridMemoHit=0, g_gridMemoMiss=0;   // per-room memo (48x48 tiles)
uint64_t g_hotRleBytes=0;                 // bytes copied by the RLE (= guest memcpy avoided)
// ---- CENSUS of the cell-blit loop Game+0xf8b80 (D2_CELLCENSUS) ------------
// 0x4f8b80 is the ONLY path to the cell blit: the chain
// 0x4f8b80 -> 0x4f8810 -> 0x4f69b0 has a single call site at each stage
// (disassembly-verified on Game.exe 1.14d). g_blitN is therefore exactly the
// number of FLAT UNCLIPPED cells drawn by this loop, a large fraction of all
// GIL acquisitions. What was missing is the fallback-rate DENOMINATOR: how
// many CALLS to the loop, and how many cells per call.
// g_cellPure counts calls where ALL cells are servable: since the port is
// all-or-nothing per call (a mid-loop fallback would leave already-drawn cells
// that the guest body would redraw), this is the real fallback rate, not the
// per-cell proportion.
uint64_t g_cellCalls=0, g_cellDecl=0, g_cellPure=0;
uint64_t g_cellB[6]={0,0,0,0,0,0};   // 0 ignored 1 rejected 2 flat/interior
                                            // 3 flat/clipped 4 bit2 (scaled) 5 degraded
bool     g_shOn=false;
bool     g_cvOn=false;

// Cell-blit body, extracted into a FREE function: the shim calls it directly.
// No line of the validated body changed — only its wrapper.
// ---- FRAME GUARD ------------------------------------------------------------
// hostptr() only validates the ARENA (297 MiB): a wrong destination extent
// resolves fine there, and the write then lands in GAME memory — the sprite
// cache sits right next to it. The symptom would not be a wrong pixel: it
// would be a Halt MUCH LATER, on animation data that has become incoherent
// (this is the same failure signature as the nLine 1420 Halt). So, at zero
// cost, this counts how often our writes fall outside the captured DIB.
// NOTHING CHANGES BY DEFAULT. The game may legitimately render into a
// DIFFERENT surface than the one CreateDIBSection sees; clamping unconditionally
// would break that case and manufacture a regression to chase a different one.
// D2_DIBGUARD=1 arms the clamp, for A/B testing.
uint32_t g_dibBits=0, g_dibW=0, g_dibH=0, g_dibBpp=0;
namespace d2rt { uint32_t scomp_flat_threads(); uint32_t scomp_flat_maxout();
                 uint32_t scomp_flat_calls(); }
uint64_t g_oobLoop=0, g_oobBlit=0;
// Mirrors of the decompressor's cross-verifier (D2_EXPLVERIFY). The original
// counters are local to a lambda and published via printf — mute on console,
// where only d2vita_progress reaches the log.
uint64_t g_xvCalls=0, g_xvBad=0;
uint32_t g_xvFirst=0, g_xvG=0, g_xvN=0, g_xvSzG=0, g_xvSzN=0;

// ---- Loop port (NATIVECELLLOOP=1) ------------------------------------------
// served / fallback PER PATTERN: a rising fallback means the fast-path
// condition isn't holding and the gain isn't there.
// WARNING: Game+0xf7ea0 / 0xf6f60 are NOT function entry points — the hot
// loops (0xf7f41, 0xf6f80) are reached by BRANCH, not CALL. A standard
// transparent entry hook placed there undercounts silently (looks armed, only
// fires once) instead of failing loudly. Find the real entry via the call
// graph before hooking either address.
uint64_t g_loopServed=0, g_loopCells=0;
// Fallbacks PER PATTERN. Pattern 0 used to mix DEGRADED and SCALED: two
// different leaves to port, hence two different decisions. Split apart.
uint64_t g_loopFB[8]={0,0,0,0,0,0,0,0};  // 0 degraded 1 host view 2 too-big 3 tables 4 scaled 5 scaled-corrupt 6 OUT OF FRAME
                                            // 4 CLIPPED scaled (not ported) 5 corrupt scaled stream
uint64_t g_loopScale=0;              // scaled cells served natively
// Broken out PER LEAF: the CLIPPED leaf's arithmetic was the one not covered
// by any oracle. A mixed total wouldn't say whether it's exercised at all,
// and "covered" would be indistinguishable from "never reached".
uint64_t g_loopScaleClip=0;
bool g_scaleForceClip=false;         // D2_SCALEFORCECLIP: cross-test of both leaves
static uint64_t g_fbHash=1469598103934665603ull, g_fbHashN=0;   // FBHASH: pixel oracle for the A/B
// D2_DIBWATCH=1 — DOES THE GAME REWRITE THE DIB BETWEEN TWO StretchBlt CALLS?
// This decides whether presentation can take a POINTER (zero-copy) or needs an
// 8-bit copy. Method: snapshot the DIB at every blit, compare it to the NEXT
// blit before overwriting it. The result is a byte count, not an impression.
// COST: two 480 KiB passes per frame — a measurement instrument, never a
// default. Works under qemu as well as on console (no platform calls).
static uint64_t g_dwFrames=0, g_dwChanged=0, g_dwBytes=0, g_dwTotal=0;
static uint32_t g_dwMaxPct=0, g_dwMinPct=1000;   // in thousandths of the DIB's bytes
extern "C" unsigned long long d2rt_hot_stat(int which){
    switch(which){
        case 0: return g_hotGridN;   case 1: return g_hotCollN;  case 2: return g_hotRleN;
        case 3: return g_hotLightN;  case 4: return g_hotBlendN; case 5: return g_scomp2N;
        case 6: return g_hotLightFB+g_hotBlendFB+g_scomp2FB+g_scompFB;  // total
        // Fallbacks PER TARGET (7..10): a single combined total would force
        // guessing which target it came from, needing correlation with other
        // counters to attribute it.
        case 7: return g_hotLightFB;   case 8: return g_hotBlendFB;
        case 9: return g_scomp2FB;     case 10: return g_scompFB;
        // 11..13: the cell blit. g_blitN used to only be published at shutdown,
        // so never in a bench run that doesn't exit cleanly — yet it's one of
        // the largest measured items in the frame budget. Without a periodic
        // readout there's no way to form "calls per frame", the missing
        // denominator for every rendering-side lever: prior estimates ranged
        // from 500 to 1500 cells/frame, a 3x spread.
        // A COUNT, not a comparison: valid with no noise floor to worry about.
        case 11: return g_blitN;       case 12: return g_blitFast;
        case 13: return g_blitSlow;
        // 14..21: cell-blit loop census (D2_CELLCENSUS).
        case 14: return g_cellCalls;   case 15: return g_cellDecl;
        case 16: return g_cellPure;
        case 17: return g_cellB[0];    case 18: return g_cellB[1];
        case 19: return g_cellB[2];    case 20: return g_cellB[3];
        case 21: return g_cellB[4]+g_cellB[5];   // not servable: scaled + degraded
        // 22..24: loop port (NATIVECELLLOOP).
        case 22: return g_loopServed;  case 23: return g_loopCells;
        case 24: return g_loopFB[0]+g_loopFB[1]+g_loopFB[2]+g_loopFB[3]+g_loopFB[4]+g_loopFB[5];
        case 25: return g_loopFB[0];   case 26: return g_loopFB[1];
        case 27: return g_loopFB[2];   case 28: return g_loopFB[3];
        case 29: return g_loopFB[4];  case 46: return g_loopScale;
        case 47: return g_loopFB[5];
        // 30..38: frame shape (D2_LOOPSHAPE).
        case 30: return g_shFrames;    case 31: return g_shSumEv;
        case 32: return g_shSumN;      case 33: return g_shSumBefore;
        case 34: return g_shSumIntrus; case 35: return g_shSumAfter;
        case 36: return g_shSumUsBurst; case 37: return g_shSumUsWindow;
        case 38: return g_shSumUsFrame;
        case 39: return g_shSumIntrusBlit;  case 40: return g_shSumIntrusOther;
        case 41: return g_shSumIntrusNodraw;
        case 55: return g_oobLoop;     case 56: return g_oobBlit;
        case 57: return g_oobLo;       case 58: return g_oobHi;
        case 59: return g_dibBits;     case 60: return (uint64_t)g_dibW*g_dibH*(g_dibBpp?g_dibBpp/8u:1u);
        case 61: return g_loopFB[6];  case 62: return g_loopScaleClip;
        case 63: return g_xvCalls;    case 64: return g_xvBad;
        case 67: return g_wrOob;      case 68: return g_wrOobOff;
        case 69: return g_wrOobLen;   case 70: return g_wrOobSpan;
        case 71: return g_wrOobKind;
        // 75..85: cell-blit loop fork-join (D2_CELLPAR).
        case 75: return g_parCalls;    case 76: return g_parCellsW;
        case 77: return g_parJoins;    case 78: return g_parWaitMax;
        case 79: return g_parWaitSum;  case 80: return g_parSteals;
        case 81: return g_parStraddle; case 82: return g_parScaleBail;
        case 83: return g_parDegen;    case 84: return g_parSeqCalls;
        case 85: return (uint64_t)g_parLive;
        case 92: return g_parPitchOdd;
        // 93..99: fork-join load balancing (D2_CELLPARSHARE) — effective share
        // and the TWO wait times that must both trend to zero together.
        case 93: return g_parCellsC;   case 94: return g_parSpanW;
        case 95: return g_parSpanC;    case 96: return g_parWIdlePro;
        case 97: return g_parWIdleGap; case 98: return g_parWIdleN;
        case 99: return (uint64_t)g_parShare;
        // 86..91: overlap census (D2_CELLOVL).
        case 86: return g_ovlCalls;    case 87: return g_ovlWr;
        case 88: return g_ovlHit;      case 89: return g_ovlDiff;
        case 90: return g_ovlSelf;     case 91: return g_ovlNoMap;
        case 72: return d2rt::scomp_flat_calls();     // flat wrapper 0x415240
        case 73: return d2rt::scomp_flat_threads();   // >1 = broken residual chain
        case 74: return d2rt::scomp_flat_maxout();
        default: return 0; } }

#include <cstdarg>
// KEvent is now WxEvent, an engine type (guest_sync.h, included at the top):
// no more forward declaration to maintain here.
static void ap_signal(WxEvent* e);
// --- transcribed "room / collision" primitives, SINGLE SOURCE ---------------
// These serve the collision shim (Game+0x24cb30) AND the light-grid
// reconstruction (Game+0x756d0), which calls both thousands of times per
// frame. A single transcription: two copies that drifted apart would make any
// audit worthless.

// 0x463740 — the room containing (x,y), starting from `room`. Fast path: the
// point is inside `room`. Otherwise scan its neighbors: list at [room+0x00],
// count at [room+0x24] (all that 0x619790 does, a pure accessor). SIGNED
// comparisons as in the original, unsigned iteration.
// GROUPED read: a single bounds check for N contiguous fields, instead of one
// virtual call per field. That's the whole point here — collision is called
// millions of times per run, and reading four bounds in four calls instead of
// one cost ~17s by itself on a 5000-frame run.
void gread_n(d2rt::Cpu& c, uint32_t va, void* dst, uint32_t n){
    if(const void* h=c.hostptr(va,n)) std::memcpy(dst,h,n);
    else c.read(va,dst,n);                     // met a zero si hors arene
}
// Diagnostic ring of the last sprite decompressions (dumped at a Fog Halt so a
// sprite-decompression-error names its culprit: native-served vs fell-back).
// Halt-316 room-guard counters (D2_ROOMGUARD): file-scope so the Crash.txt
// hook and the [soak] stats can report them (statics inside the hook block
// were invisible — cost a blind validation run).
static int g_rgDepth=0; static uint32_t g_rgRA[16];
uint64_t g_rgIter=0, g_rgSkip=0; static uint64_t g_rgDispCalls=0, g_rgDispS2=0;
namespace d2rt { uint32_t scomp_explode_pkware_trunc(const uint8_t* in, uint32_t in_len,
                                                     uint8_t* out, uint32_t out_max);
                 uint32_t scomp_explode_pkware(const uint8_t* in, uint32_t in_len,
                                               uint8_t* out, uint32_t out_max);
                 }
// Per-thread Windows TIB fields (real layout). Pre-scheduler the live TIB is
// mapped at MAIN_TIB (linear 0, except under the HIGH layout), so
// cur_tib()==MAIN_TIB is correct there too.
// The current-thread TIB and last error live in the engine
// (guest_thread_ctx.{h,cpp}): the LOGIC there is generic, this port only
// supplies the two DATA points that are its own — which scheduler is live
// (wx86_set_scheduler) and where its memory layout placed the main TIB
// (wx86_set_main_tib). Short names kept as aliases: dozens of call sites in
// this file.
uint32_t cur_tib(){ return wx86_cur_tib(); }
void set_lasterr(Cpu&c,uint32_t v){ wx86_set_lasterr(c,v); }
static uint32_t get_lasterr(Cpu&c){ return wx86_get_lasterr(c); }
// Zero a guest range through the host view when available. c.write()'s only
// barrier is `if(isprotectedDB) unprotectDB` — invalidate_code() is that exact
// barrier (cpu_box86.cpp), so hostptr+memset is STRICTLY equivalent to the old
// 4 KiB c.write() loops, minus one virtual call + memcpy per chunk (the town
// load zeroes ~200 MB through those loops). Fallback keeps the old path.
// gzero() (ALIAS: the implementation lives in the engine, win32_shims_memory.h
// wx86_gzero) moved to runtime/kernel32_filemapping.cpp -- only MapViewOfFile
// calls it now.
// Cached boolean env probes for per-call hot-path logging knobs (getenv walks
// the environ array on EVERY call; these sit in shims called 10k-1M times).
// env.txt / baked setenv all happen before the first shim call, so a
// once-latched value is exact.
static bool env_waitlog(){ static const bool v=getenv("WAITLOG")!=nullptr; return v; }
bool env_filelog(){ static const bool v=getenv("FILELOG")!=nullptr; return v; }
bool env_netlog(){  static const bool v=getenv("D2NETLOG")!=nullptr; return v; }   // extern: see rt_host.h
static bool env_wlog(){    static const bool v=getenv("WLOG")!=nullptr; return v; }
// Guest tick page ([g_tickVA]=counter, [+4]=0x10000+virt_ms): the ONE clock
// read+bumped by C++ shims and the guest-inlined GetTickCount stub alike.
// Real-mode guest tick with bounded catch-up: the tick may advance at most
// g_tickCap ms between two consecutive guest reads (0 = uncapped). On a
// machine ~10x slower than any PC D2 shipped on, a level load takes several
// REAL seconds — guest logic that ages by tick delta (D2CMP sprite-cache LRU)
// then evicts entries still being composed and asserts on the freed header
// (Fog line 1454, "Unrecoverable internal error"). No real PC ever showed D2
// multi-second deltas mid-load; capping restores that invariant while normal
// gameplay (a read every frame, delta ~40 ms < cap) stays real-time.
static uint32_t g_tickCap=0;
bool g_tickvirt=false;   // diag: virtual tick VALUES under the real scheduler; extern: see rt_host.h
static uint32_t tick_real(){
    // Perf: rt_now_ms() is a host syscall and D2 polls the clock ~26k/s
    // (GetTickCount 1.8M calls / 70 s in-game). Serve a cached value with
    // bounded staleness: refresh every 32nd call or on any scheduler switch
    // (d2rt_sw_seq). Max ~1-2 ms stale — far FINER than the 10-15 ms
    // granularity of real Windows GetTickCount, monotone by construction, and
    // a spin on the clock still advances (the 32-call throttle guarantees
    // forward progress without a switch).
    static uint32_t ccount=0, cseq=0, cval=0; static bool cinit=false;
    if(cinit && ((++ccount)&31) && cseq==d2rt_sw_seq) return cval;
    cseq=d2rt_sw_seq;
    uint32_t real=g_tickOfs+(uint32_t)rt_now_ms();
    if(!g_tickCap){ cval=real; cinit=true; return real; }
    static uint32_t last=0; static bool init=false;
    if(!init){ last=real; init=true; cval=real; cinit=true; return real; }
    uint32_t dt=real-last;
    if(dt>g_tickCap){ g_tickOfs-=dt-g_tickCap; real=last+g_tickCap; }  // future reads stay continuous
    last=real; cval=real; cinit=true; return real; }
// Networking clock: with D2NET the guest keeps the VIRTUAL scheduler pacing
// (which drives the menu-input path correctly), but the clock D2 reads must be
// REAL wall-clock so its Battle.net protocol timers match real network latency
// — else the select-spin fast-forwards virtual time and every handshake step
// times out. tick source becomes real ms; scheduler pacing is untouched.
static bool g_netClock=false;
static bool g_bnetConnected=false;   // set once D2 connects to the BNCS gateway (port 6112)
#include <cstdarg>
// D2_SYNCLOG: targeted Win32-sync trace (armed only AFTER g_bnetConnected, so the
// boot's sync noise is cut). Logs thread id + caller EIP for every wait/signal/thread
// op, to find who waits on what and who should signal it. A wait ENTER with no
// matching EXIT = that thread is stuck there (the online game-load stall).
static void slog(Cpu& c, const char* fmt, ...){
    static int en=-1; if(en<0) en=getenv("D2_SYNCLOG")?1:0;
    if(!en || !g_bnetConnected) return;
    uint32_t tid=(g_sched&&g_sched->current())?g_sched->current()->id:0;
    uint32_t caller=c.read_u32(c.reg(R_ESP));
    char body[256]; va_list ap; va_start(ap,fmt); vsnprintf(body,sizeof body,fmt,ap); va_end(ap);
    // stderr doesn't exist on console: without also routing to the file log,
    // the sync trace — the ONLY tool able to say which thread is waiting on
    // which signal — would be mute on the device.
    std::fprintf(stderr,"[sync] tid=%-2u caller=0x%08x %s\n", tid, caller, body);
    nlog("[sync] tid=%-2u caller=0x%08x %s\n", tid, caller, body);
}
// Crash / fatal-event log. Appends one durable line to $D2WRITE/crash.log,
// fopen+fclose per call so the write survives a hard crash and is durable on
// Vita (writes reach the vfs only on fclose). Also mirrored to stderr and the
// Vita boot-progress log. The host-SIGSEGV path (qemu) writes via dyn86_crash_fd
// instead (async-signal-safe). A run whose crash.log has no "CLEAN EXIT" line
// crashed or hung; the last line names how far it got.
void d2_crashlog(const char* fmt, ...){
    char body[512]; va_list ap; va_start(ap,fmt); std::vsnprintf(body,sizeof body,fmt,ap); va_end(ap);
    std::fprintf(stderr,"[crashlog] %s\n", body);
    d2vita_progress(body);   // no-op on qemu, boot-progress log on Vita
    if(g_crashLogPath.empty()) return;
    if(FILE* f=std::fopen(g_crashLogPath.c_str(),"a")){ std::fprintf(f,"%s\n",body); std::fclose(f); }
}
uint32_t tick_bump(Cpu&c,uint32_t n){   // extern: see rt_host.h (for tick_intrinsic.cpp)
    if((g_realclock||g_netClock)&&!g_tickvirt) return tick_real();
    uint32_t t=c.read_u32(g_tickVA)+n;
    c.write_u32(g_tickVA,t); return t+c.read_u32(g_tickVA+4); }
uint32_t tick_now(Cpu&c){   // extern: see rt_host.h (for win32_shims_user32_d2.cpp)
    if((g_realclock||g_netClock)&&!g_tickvirt) return tick_real();
    return c.read_u32(g_tickVA)+c.read_u32(g_tickVA+4); }
// The handle table AND its counter live in the engine (guest_sync.h). Only
// ONE must exist: the engine already had one, unused, and two tables for the
// same object is exactly the kind of accident defused for critical sections —
// invisible at compile time, paid for in a deadlock. Handles specific to this
// port that are NOT waitable objects (the process snapshot) take their number
// from the SAME counter, via wx86_handle_next_id().
uint32_t g_unhandledFilter=0;   // SetUnhandledExceptionFilter target (SEH dispatcher); extern: see rt_host.h
// D2_ASYNCPROF — END-TO-END LATENCY of an asynchronous load.
// Findings: the game polls ~12 waiting objects per frame when it's chugging
// (vs 1-2 when smooth), polling itself costs a small fraction of a frame, the
// handler is a tight 134-byte body with no loop, and the game thread executes
// LESS code when it's slow — it's blocked, not busy. Total I/O time is even
// LOWER in the slow windows. So this is not a throughput problem but a
// LATENCY one. It's split into two halves observable from our shims:
//   request -> signaled  (ResetEvent .. SetEvent)  = the worker
//   signaled -> seen     (SetEvent .. WFSO returns 0) = our own scheduling
// The second half is ENTIRELY our responsibility.
// The object itself now belongs to the engine (guest_sync.h): its semantics
// are pure Win32, and both ports carried the same body. What stayed here —
// the two timestamps for async-load profiling — isn't semantics but
// OBSERVATION: it lives in a side table of this port, fed by the sync
// observer, exactly like BNCS decoding lives outside the network shims.
using KEvent = WxEvent;
struct ApTimes { uint64_t t_reset=0, t_set=0; };
static std::map<KEvent*,ApTimes> g_apT;
// D2_ONEDRAW=2 (src/runtime/phase_hooks.cpp): the manual event that is NEVER
// signaled, on which the game thread naps. Created once, on first use — this
// was the drawing hook's local `static KEvent* never` before extraction.
// Marks a signal: closes the "request->signaled" interval and opens
// "signaled->seen". Called from SetEvent AND from an OVERLAPPED ReadFile
// completion — the latter is what carries asynchronous loads.
static void ap_signal(KEvent* e){
    if(!asyncprof_on() || !e) return;
    const uint64_t now=rt_now_us(); ++g_apSetN;
    ApTimes& at=g_apT[e];
    if(at.t_reset && now>at.t_reset) ap_add(g_apWork, now-at.t_reset);
    at.t_set=now;
}
// Sync observer: the ONE registration site for this port (same rule as
// d2_net_observe for networking — a registry that silently accepts a second
// hook is exactly what caused a connection regression before). The engine
// NARRATES what happens to kernel objects; everything specific to this port —
// here, async-load profiling — lives on this side and decides nothing.
static void d2_sync_observe(const WxSyncEvent& e){
    Cpu* c=e.cpu;
    switch(e.kind){
    case WX86_SYNC_OBJ_CREATE:
        if(c) slog(*c,"CreateObj  h=0x%08x kind=%s", e.handle, e.obj?e.obj->kind():"?");
        break;
    case WX86_SYNC_EVENT_SET:
        if(c) slog(*c,"SetEvent   h=0x%08x", e.handle);
        ap_signal(static_cast<KEvent*>(e.obj));
        if(env_waitlog()) std::fprintf(stderr,"    [setevent] tid=%u h=0x%08x\n",
            g_sched&&g_sched->current()?g_sched->current()->id:0, e.handle);
        break;
    case WX86_SYNC_EVENT_RESET:
        if(c) slog(*c,"ResetEvent h=0x%08x", e.handle);
        if(asyncprof_on()){ ++g_apResetN;
            ApTimes& at=g_apT[static_cast<KEvent*>(e.obj)];
            at.t_reset=rt_now_us(); at.t_set=0; }   // reset OPENS the interval, set CLOSES it
        break;
    default: break;
    }
}

Waitable* rt_never_event(){ static KEvent* never=nullptr; if(!never){ never=new KEvent(); never->manual=true; never->signaled=false; } return never; }
// Counting semaphore (real count, unlike modeling it as a binary event): a wait
// decrements, ReleaseSemaphore adds. Honest for count>1 (a Warden worker pool).
using KSemaphore = WxSemaphore;   // engine (guest_sync.h)
// Completion port and multi-wait: both types live in the engine
// (guest_sync.h). This file used to keep a copy of them; it keeps none now —
// a generic type duplicated in two places is exactly what this split is meant
// to eliminate.
using KIocp = WxIocp;             // engine (guest_sync.h)
using KThread = WxThread;         // engine (guest_sync.h)
// NOTE (fidelity, audit T1): GetCurrentThreadId returns the scheduler id
// (1,2,3…), but TEB ClientId.UniqueThread at TIB+0x24 is INTENTIONALLY left 0
// for every thread — see sched_cooperative.cpp thread-init. Writing the real id
// there regressed Storm's SMem pool sizing (its per-thread pools are keyed off
// the TEB id -> tripled -> 48 MiB heap overflow). So fs:[0x24] and
// GetCurrentThreadId deliberately DISAGREE; a fully honest fix must first solve
// the SMem pool sizing (docs/FIDELITY_TODO.md), not just write the id.
static Cpu* g_cpu=nullptr;   // set in main() right after the Cpu is created
// CS state lives in C++ (owner/count), INVISIBLE to the guest. This was tried
// guest-backed (writing OwningThread/RecursionCount into the real structure)
// and REVERTED: with visible state the level load livelocks — Blizzard code
// inspects the RTL fields inline, and a nonzero OwningThread flips it onto
// paths our shim-based Enter/Leave never balance (heap fills at 48 MiB, Fog
// aborts). Keeping the fields zero preserves the validated behaviour.
using KCrit = WxCrit;             // engine (guest_sync.h)
// The critical-section map AND its MRU cache live in the engine. Only ONE
// must exist: otherwise the shim, the dynarec fast path, and this file would
// operate on two distinct tables — the kind of divergence that's invisible at
// compile time and paid for in a deadlock.
KCrit* crit_for(uint32_t cs){ return wx86_crit_for(cs); }   // extern: see rt_host.h (for cs_intrinsic.cpp)
bool is_kind(Waitable* w,const char* k){ return wx86_is_kind(w,k); }   // extern: see rt_host.h (for kernel32_files.cpp)

// Wait observer: the ONE registration site for this port (same rule as
// d2_sync_observe, d2_mem_observe and d2_net_observe). The engine NARRATES
// what it waits for and what it gets; the sync trace, the wait log, the
// timing, and async-load profiling live here and decide nothing.
//
// The timer must connect EXIT to ENTRY. It does so via two arrays indexed by
// guest thread id, not a thread_local variable: this project's toolchain is
// built without native TLS, and its emutls is expensive under the native
// scheduler. Each thread writes only its own slot, so no lock is needed. A
// thread beyond slot 64 falls back to slot 0: the timer loses precision
// there, but corrupts nothing.
static uint64_t g_wpT0[64]={0};   // 0 = timer off for this thread
static int      g_wpIx[64]={0};   // site index, READ only if g_wpT0 is armed
static void d2_wait_observe(const WxWaitEvent& e){
    Cpu* c=e.cpu;
    const uint32_t tid = (g_sched&&g_sched->current())?g_sched->current()->id:0u;
    const uint32_t slot = tid<64?tid:0;
    switch(e.kind){
    case WX86_WAIT_SINGLE_UNKNOWN:
        // An UNKNOWN handle is treated as immediately "signaled", WITHOUT
        // waiting. If the async sprite loader ever waits on such a handle,
        // the renderer never blocks on it — a possible race (see the Halt
        // 607/1420 family). Logged for visibility.
        lw_count(g_lwWfsoN);
        { static int n=0; if(n<8){ ++n; char m[140];
            std::snprintf(m,sizeof m,"WFSO handle INCONNU 0x%08x -> signale immediat sans attente (tid=%u to=%d)",
                e.handle,tid,(int)e.timeout); d2vita_progress(m); } }
        break;
    case WX86_WAIT_SINGLE_ENTER:
        lw_count(g_lwWfsoN);
        if(env_waitlog()) std::fprintf(stderr,"    [wait] tid=%u h=0x%08x kind=%s timeout=0x%08x\n",
            tid, e.handle, e.obj->kind(), e.timeout);
        if(c) slog(*c,"WFSO ENTER h=0x%08x kind=%s to=%d", e.handle, e.obj->kind(), (int)e.timeout);
        g_wpT0[slot] = waitprof_on() ? rt_now_us() : 0;
        g_wpIx[slot] = (g_wpT0[slot] && c) ? wp_site(c->read_u32(c->reg(R_ESP)), e.obj->kind()) : -1;
        break;
    case WX86_WAIT_SINGLE_EXIT:
        if(g_wpT0[slot] && g_wpIx[slot]>=0 && e.ret==0) ++g_wpSites[g_wpIx[slot]].sig;   // WAIT_OBJECT_0 = work is ready
        if(e.ret==0 && asyncprof_on() && is_kind(e.obj,"event")){
            auto* ev=static_cast<KEvent*>(e.obj);
            ApTimes& at=g_apT[ev]; if(at.t_set){ const uint64_t now=rt_now_us(); if(now>at.t_set) ap_add(g_apSee, now-at.t_set); at.t_set=0; } }
        if(g_wpT0[slot]){ const uint64_t dt = rt_now_us()-g_wpT0[slot];
            if(e.timeout==0xFFFFFFFFu){ ++g_wpWinf_n; g_wpWinf_us += dt; }
            else wp_add(g_wpW, e.timeout, dt, tid); }
        if(c) slog(*c,"WFSO EXIT  h=0x%08x ret=0x%x", e.handle, e.ret);
        break;
    case WX86_WAIT_MULTI_ENTER:
        if(env_waitlog() && c){ std::fprintf(stderr,"    [waitmulti] tid=%u n=%u all=%u to=0x%x handles:",
                tid, e.count, e.all, e.timeout);
            for(uint32_t i=0;i<e.count;i++){ uint32_t h=c->read_u32(e.handles+4*i);
                bool known=(wx86_handle_find(h)!=nullptr);
                std::fprintf(stderr," [%u]0x%08x%s", i, h, known?"":"(UNKNOWN)"); }
            std::fprintf(stderr,"\n"); }
        if(c){ char hb[128]={0}; int q=0;
            for(uint32_t i=0;i<e.count&&q<100;i++) q+=snprintf(hb+q,sizeof hb-q," 0x%08x",c->read_u32(e.handles+4*i));
            slog(*c,"WFMO ENTER n=%u all=%u to=%d h:%s", e.count, e.all, (int)e.timeout, hb); }
        break;
    case WX86_WAIT_MULTI_EXIT:
        if(c) slog(*c,"WFMO EXIT  ret=0x%x", e.ret);
        break;
    default: break;
    }
}

// call-path log
int g_calls=0;
bool g_stop=false;   // extern: see rt_host.h
std::string g_stopReason;   // extern: see rt_host.h
uint32_t g_curBase=0;   // base of the module currently initializing; extern: see rt_host.h

// Honest region introspection for VirtualQuery (Warden's #1 memory-scan
// primitive). Reports the real committed regions — loaded modules (MEM_IMAGE),
// the guest heap / VA arena / misc / main stack (MEM_PRIVATE) — and MEM_FREE
// for the gaps, sized up to the next known region so a region-by-region walk
// terminates. Limitation: per-thread worker stacks/TIBs (SCHED_* ranges) are
// reported MEM_FREE (their exact extents live in the scheduler); the primary
// scan targets (process image + heaps + main stack) are accurate. Returns
// false only for kernel space (>= layout_user_hi(), 0x7FFF0000 by default —
// under the HIGH layout the ceiling moves up with the regions, otherwise ALL
// of guest space would be declared "kernel space" and VirtualQuery would fail
// everywhere).
static bool vquery_region(uint32_t addr, uint32_t& base, uint32_t& size,
                          uint32_t& state, uint32_t& type, uint32_t& protect,
                          uint32_t& allocbase){
    if(addr>=d2rt::layout_user_hi()) return false;
    // 1) loaded modules -> committed image
    for(auto&p:g_modByBase){ uint32_t b=p.first, e=b+p.second->image_size();
        if(addr>=b && addr<e){ base=b; size=(e-b+0xFFFu)&~0xFFFu; state=0x1000u;   // MEM_COMMIT
            type=0x1000000u; protect=0x40u; allocbase=b; return true; } }         // MEM_IMAGE, PAGE_EXECUTE_READWRITE
    // 2) committed private arenas / main stack
    struct R{ uint32_t b,s,prot; } regs[]={
        {HEAP_BASE,HEAP_SIZE,0x04u},                              // heap: PAGE_READWRITE
        {MISC_BASE,MISC_SIZE,0x40u},                              // misc stubs: RWX
        {VA_BASE,VA_SIZE,0x40u},                                  // VA arena: RWX
        {MAIN_STACK_TOP-0x200000u,0x200000u,0x04u},               // main stack: RW
    };
    for(auto&r:regs){ if(addr>=r.b && addr<r.b+r.s){ base=r.b; size=r.s; state=0x1000u;
        type=0x20000u; protect=r.prot; allocbase=r.b; return true; } }            // MEM_PRIVATE
    // 3) free gap -> size up to the next known region start
    uint32_t next=d2rt::layout_user_hi();
    auto lo=[&](uint32_t s){ if(s>addr && s<next) next=s; };
    lo(HEAP_BASE); lo(MISC_BASE); lo(VA_BASE); lo(MAIN_STACK_TOP-0x200000u);
    lo(SCHED_STACKS); lo(SCHED_TIBS); for(auto&p:g_modByBase) lo(p.first);
    base=addr&~0xFFFu; size=next-base; state=0x10000u; type=0u; protect=0x01u; allocbase=0u;  // MEM_FREE
    return true;
}

// Memory-layout observer: the ONE registration site for this port (same rule
// as d2_sync_observe and d2_net_observe). The engine NARRATES what the heap
// and the arena do; the counters, the log lines, and naming the offending
// site stay here — they never decide anything.
//
// The VirtualFree/MEM_RELEASE report stays capped at the first four
// occurrences, as before. What changes: the engine now reads the return
// address on EVERY occurrence, because the counter that used to bound that
// read lives on this side. This is a rare error path and one stack read.
static void d2_mem_observe(const WxMemEvent& e){
    auto site=[&]{ return g_locp?(*g_locp)(e.ra).c_str():"?"; };
    switch(e.kind){
    case WX86_MEM_VA_COMMIT_IN:    g_vaComInB+=e.size; break;
    case WX86_MEM_VA_RESERVE:      g_vaResB+=e.size; ++g_vaResN; break;
    case WX86_MEM_VA_COMMIT_FRESH: g_vaComFrB+=e.size; break;
    case WX86_MEM_VA_BIG:
        std::fprintf(stderr,"[VirtualAlloc %u KB type=0x%x prot=0x%x from %s, va used %u MB]\n",
                     e.size>>10,e.type,e.protect,site(),g_vaA.used_bytes()>>20);
        break;
    // A huge request is padding from Fog's size check, which ends in a Halt at
    // SFILE.CPP:898 on hardware. Name it before the failure.
    case WX86_MEM_VA_GARBAGE:
        d2_crashlog("VirtualAlloc GARBAGE size=0x%08x from %s",e.size,site());
        break;
    case WX86_MEM_VA_FAIL:         // durably name the site that fails
        d2_crashlog("VirtualAlloc FAIL sz=%u KB type=0x%x from %s",e.size>>10,e.type,site());
        break;
    case WX86_MEM_VA_RELEASE:
        g_vaRelB+=e.size; ++g_vaRelN;
        if(e.size>=0x100000) std::fprintf(stderr,"[VirtualFree RELEASE %u KB, va used %u MB]\n",
                                          e.size>>10,g_vaA.used_bytes()>>20);
        break;
    case WX86_MEM_VA_RELEASE_MISS:  // a LOST free = an arena leak
        ++g_vaRelFailN;
        if(g_vaRelFailN<=4) d2_crashlog("VirtualFree RELEASE MISS addr=0x%08x from %s",e.addr,site());
        break;
    case WX86_MEM_VA_DECOMMIT:     g_vaDecB+=(uint64_t)e.size; break;
    default: break;
    }
}

// Path for a module handle (GetModuleFileName): honor hModule so a runtime
// module (CheckRevision.dll) reports its OWN path, coherent with
// GetModuleHandle. hModule==0 -> the exe. Unknown handle -> the exe.
std::string module_path_for(uint32_t hmod){   // extern: see rt_host.h
    if(!hmod) return std::string("C:\\Diablo II\\game.exe");        // NULL = the executable
    for(auto&p:g_modByName) if(p.second->load_base()==hmod)
        return std::string("C:\\Diablo II\\")+p.first;
    // An UNKNOWN handle — chiefly the 0x7E000001+ pseudo-handles for
    // kernel32/user32/gdi32 — used to return "C:\Diablo II\game.exe". A
    // doubly-inconsistent lie: GetModuleHandle("kernel32") returns a handle,
    // and GetModuleFileName on THAT handle would return the EXE's path. There
    // is no real kernel32 image to name, and fabricating a system path would
    // be exactly what this project's honesty rule forbids. The honest answer
    // is therefore FAILURE (empty string -> 0 + ERROR_INVALID_HANDLE for the
    // caller). The "nonexistent system modules" gap is accepted and
    // documented (docs/FIDELITY_TODO.md).
    if(wx86_fid_avant()) return std::string("C:\\Diablo II\\game.exe");   // the OLD lie
    return std::string();
}

// Alias, like misc(): the implementation lives in the engine (wx86_scratch_put_cstr).
uint32_t put_cstr(Cpu& c, const char* s){ return wx86_scratch_put_cstr(c,s); }   // extern: see rt_host.h

// ---- guest strings: ALIAS, like misc()/put_cstr above.
// The implementations live in the engine (guest_str.h): any Win32 API that
// takes or returns a string must read it at a GUEST address, which isn't
// specific to any one game — the other port carried the same copy,
// word-for-word. The ~43 call sites in this file don't change; only one
// implementation exists.
std::string gread_mb(Cpu& c, uint32_t p, int len){ return wx86_gread_mb(c,p,len); }
std::vector<uint16_t> gread_wc(Cpu& c, uint32_t p, int len){ return wx86_gread_wc(c,p,len); }   // extern: see rt_host.h
void gwrite_wc(Cpu& c, uint32_t p, uint16_t w){ wx86_gwrite_wc(c,p,w); }

// Honest Toolhelp thread/process snapshot: capture the REAL scheduler state at
// snapshot time (never a fabricated "clean" view). Per-handle cursor so
// Thread32/Process32 First/Next walk it. A live process ALWAYS has itself +
// >=1 thread, so returning an empty snapshot (the old stub) was DISHONEST.
// Ownership lives in the engine (guest_toolhelp.h). A Toolhelp32 snapshot —
// the frozen list of threads and processes, and the cursor that walks it — is
// a system notion, not a game one. Both ports had the same structure and the
// same table. REFERENCE, not a copy: only one table exists, owned by the engine.
// The MODULE list (g_mod32list, above) does NOT move: each port builds it its
// own way from its own module table.
using SnapState = WxSnapState;
static std::map<uint32_t,SnapState>& g_snaps = wx86_snapshots();

// Minimal wsprintf: handles %[flags][width][.prec][l/h]{s,d,i,u,x,X,c,p,%}.
// `next()` yields successive dword varargs. Returns chars written (excl. null).

bool g_schedNative=false;   // set by the early parse (main), then reassigned by make_scheduler; extern: see rt_host.h

// d2rt_poll_gilfree/_n live in winx86 (d2rt::wx86_poll_gilfree/_n,
// runtime/poll_gil.h) — nothing D2-specific ever lived here. See that file
// for the exact body.

// ---- Diablo-II-specific network observer -----------------------------------
// The ONE function registered on winx86's generic socket-layer observation
// point (wx86_net_set_observer). Everything D2-specific lives HERE: BNCS/MCP/
// D2GS protocol decoding (netwatch), the gateway-connection flag that drives
// D2_AUTOBNET/D2_SYNCLOG, the two clock switches, and the game-load thread
// log. The connect/recv/send shims themselves are generic and live in
// winx86: they know no port or protocol.
//
// Ports 6112 (BNCS gateway), 6113 (MCP realm) and 4000 (game server) are D2
// PROTOCOL constants, not engine ones — which is exactly why they belong on
// this side of the boundary.
//
// A SINGLE registration site, deliberately: a registry that silently accepts
// a second hook on the same key is what caused this project to lose time to
// a login regression before (the WinVerifyTrust incident).
static void d2_net_observe(const WsockEvent& e){
    switch(e.kind){
    case WX86_NET_CONNECT: {
        const uint32_t dip=e.ip; const uint16_t port=e.port;
        // ENTRY trace: without it, a connect that never returns is
        // indistinguishable from a connect that was never called.
        nlog("    [net] connect ENTER guest=%u fd=%d -> %u.%u.%u.%u:%u\n",
             e.handle, e.fd, dip&0xff,(dip>>8)&0xff,(dip>>16)&0xff,(dip>>24)&0xff, port);
        if(port==6112) g_bnetConnected=true;   // stops the D2_AUTOBNET auto-click
        nw_reset(e.fd);                        // netwatch: fresh stream on this fd
        // Entering a GAME (D2GS/d2ingress on :4000): drop the guest clock to
        // 1/K so the act load, bounded by a timeout measured in GUEST time,
        // gets the full real-time window. Without D2_RTSCALE or
        // D2_INGAME_SLOW this is a no-op (1/1 -> 1/1); with D2_RTSCALE=N this
        // brings the fast-navigation clock back to 1x.
        if(port==4000 && g_rtDiv==1){ rt_set_rate(1,g_ingameSlow);
            std::printf("    [clock] game connect (:4000) -> guest clock 1/%llu for the act-load\n",
                        (unsigned long long)g_ingameSlow); }
        // Print the ACTUAL redirect value: hardcoding the text "127.0.0.1" here
        // made a redirect to a local-network address display as a loopback.
        // The route itself is applied by winx86.
        if(e.route_ip!=dip)
            nlog("    [net] BNCS redirect %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u\n",
                dip&0xff,(dip>>8)&0xff,(dip>>16)&0xff,(dip>>24)&0xff,port,
                e.route_ip&0xff,(e.route_ip>>8)&0xff,(e.route_ip>>16)&0xff,(e.route_ip>>24)&0xff,port);
        // Virtual -> real clock switch on connecting to the REAL gateway
        // (the ORIGINAL destination, before redirection). This switch only
        // fires in the D2NET=1 + D2_VIRTCLOCK=1 combination (the
        // deterministic bench). In normal game config, D2NET already sets
        // g_rtwant (see the "D2_REALCLOCK || D2NET" rule above), so
        // g_realclock is already true before the guest even starts and the
        // condition is false: a control run without D2_VIRTCLOCK reaches the
        // gateway with ZERO occurrences; a run with it gets exactly one. So
        // this only matters for the deterministic bench (menu on the virtual
        // clock so the scripted click always lands at the same spot, real
        // time afterward for protocol timeouts) and nowhere else.
        const uint8_t hi=dip&0xff;
        const bool remoteGw = hi!=127 && hi!=0 && dip!=0xffffffffu && port==6112;
        if(remoteGw && !g_netClock && !g_realclock && !g_tickvirt && e.cpu){
            uint32_t vt=tick_now(*e.cpu); g_tickOfs = vt - (uint32_t)rt_now_ms();
            g_netClock=true;
            nlog("    [net] clock -> real @gateway-connect (orig %u.%u.%u.%u:%u) (tick %u)\n",
                 dip&0xff,(dip>>8)&0xff,(dip>>16)&0xff,(dip>>24)&0xff,port,vt); }
        break; }
    case WX86_NET_CONNECT_DONE:
        if(e.result==1)      nlog("    [net] connect fd=%d COMPLETED (sync)\n",e.fd);
        else if(e.result<0)  nlog("    [net] connect fd=%d failed/timeout wsa=%u\n",e.fd,e.wsa_err);
        break;
    case WX86_NET_RESOLVE: {
        // gethostbyname lives in winx86; since the engine is silent, this is
        // where it becomes a trace. A name that fails to resolve is THE first
        // expected failure on console: without this line, the screen stays
        // silent and there's nothing for a diagnosis to go on.
        const std::string host(e.data?(const char*)e.data:"", e.len>0?(size_t)e.len:0);
        if(e.result<0){ char rm[96];
            // A LOCK refusal (no request even sent) or a real DNS failure? The
            // engine's counter is incremented BEFORE the notification.
            static unsigned long long refusVus=0;
            const unsigned long long refus=wx86_net_resolves_refused();
            if(refus!=refusVus){ refusVus=refus;
                std::snprintf(rm,sizeof rm,"netguard: REFUS resolution %s (verrou arme, aucune requete DNS emise)",host.c_str()); }
            else
                std::snprintf(rm,sizeof rm,"reseau: resolution KO %s",host.c_str());
            d2vita_progress(rm); nlog("    [net] gethostbyname %s -> ECHEC\n",host.c_str()); }
        else
            nlog("    [net] gethostbyname %s -> %u.%u.%u.%u\n",host.c_str(),
                 e.ip&0xff,(e.ip>>8)&0xff,(e.ip>>16)&0xff,(e.ip>>24)&0xff);
        break; }
    case WX86_NET_RECV:
        if(e.result>0){
            netwatch(e.fd, e.port, true, e.data, e.len);            // observation passive (D2_NETWATCH)
            if(e.port==4000 && e.cpu)
                slog(*e.cpu,"recv D2GS %d o, first id=0x%02x", e.result, e.len>0?e.data[0]:0);
            if(env_netlog()){ nlog("    [net] recv fd=%d n=%d:",e.fd,e.result);
                for(int k=0;k<e.len&&k<16;k++) std::printf(" %02x",e.data[k]); std::printf("\n"); }
        } else if(e.result==0){
            if(env_netlog()) nlog("    [net] recv fd=%d GRACEFUL CLOSE\n",e.fd);
        } else {
            if(env_netlog()){ static int re=0; if(re++<40) nlog("    [net] recv fd=%d ERR wsa=%u\n",e.fd,e.wsa_err); }
        }
        break;
    case WX86_NET_SEND:
        netwatch(e.fd, e.port, false, e.data, e.len);                // observation passive (D2_NETWATCH)
        if(env_netlog()){ nlog("    [net] send fd=%d len=%d n=%d:",e.fd,e.len,e.result);
            for(int k=0;k<e.len&&k<16;k++) std::printf(" %02x",e.data[k]); std::printf("\n"); }
        break;
    case WX86_NET_SELECT:
        sel_observe(e);
        break;
    case WX86_NET_REFUSED: {
        // The engine WITHHELD a packet: this is where it gets announced
        // loudly. On Vita a printf proves nothing, so the line ALSO goes
        // through d2vita_progress.
        const char* quoi = e.result==WX86_NET_CONNECT ? "connect"
                         : e.result==WX86_NET_SEND    ? "sendto"
                         : e.result==WX86_NET_RECV    ? "recvfrom" : "sortie";
        nlog("    [netguard] REFUS %s vers %u.%u.%u.%u:%u (hors liste blanche locale) — WSAEACCES\n",
             quoi,e.ip&0xff,(e.ip>>8)&0xff,(e.ip>>16)&0xff,(e.ip>>24)&0xff,e.port);
        std::fprintf(stderr,"[netguard] REFUS %s -> %u.%u.%u.%u:%u (regle maison : aucun serveur officiel)\n",
             quoi,e.ip&0xff,(e.ip>>8)&0xff,(e.ip>>16)&0xff,(e.ip>>24)&0xff,e.port);
        { char m[128]; std::snprintf(m,sizeof m,"netguard: REFUS %s %u.%u.%u.%u:%u",
             quoi,e.ip&0xff,(e.ip>>8)&0xff,(e.ip>>16)&0xff,(e.ip>>24)&0xff,e.port);
          d2vita_progress(m); }
        break; }
    default: break;
    }
}

// Room-graph dump (map-seed divergence diagnosis): print EVERY ghAct room's
// coordinate block + room2 chain, identically on qemu (stdout) and Vita
// (boot_progress), so one seed's generated map can be diffed across platforms.
// Triggers: Crash.txt open (D2_CRASHDUMP=1) and frame D2_ROOMDUMP_FRAME=<n>.
// (File scope: called both from crash_txt_diag_dump() below and directly from
// main()'s D2_ROOMDUMP_FRAME frame-trigger check.)
static auto dump_ghact_rooms=[](Cpu& c, const char* tag){
    // Bounds and address RELATIVE to the layout: under D2LAYOUT=haut the
    // ghAct pointer no longer lives at 0x020a0634 and the low guards
    // would reject everything. g_d2base is the base WHERE the monolith
    // was loaded; the old constant 0x020a0634 equaled g_d2base + 0x7a0634
    // under the compact layout (0x01900000), so the expression below
    // yields the SAME address it used to at 0x020a0634.
    const uint32_t RLO=0x400u+d2rt::layout_hi(), RHI=0x13000000u+d2rt::layout_hi();
    auto RD=[&](uint32_t a)->uint32_t{ return (a>=RLO && a<RHI)?c.read_u32(a):0xBADBAD00u; };
    uint32_t ghAct=RD(g_d2base+0x7a0634u); char m[200];
    std::snprintf(m,sizeof m,"[ROOMS/%s] ghAct=%08x",tag,ghAct); d2vita_progress(m); std::printf("%s\n",m);
    int rn=0;
    for(uint32_t r=RD(ghAct+0x10); r && r<RHI && rn<80; r=RD(r+0x7c), rn++){
        uint32_t L1=RD(r+0x10), room2=RD(L1+0x5c);
        std::snprintf(m,sizeof m,"[ROOMS/%s] #%02d r=%08x c=%x,%x,%x,%x L1=%08x r2=%08x r2x8=%x",
            tag,rn,r,RD(r+0x4c),RD(r+0x50),RD(r+0x54),RD(r+0x58),L1,room2,RD(room2+0x8));
        d2vita_progress(m); std::printf("%s\n",m); }
    std::snprintf(m,sizeof m,"[ROOMS/%s] total=%d",tag,rn); d2vita_progress(m); std::printf("%s\n",m);
};
// 1.14d's save-path code (hit at world load) walks the filesystem through
// the W APIs; unshimmed they ESP-drift and the game thread rets into
// stack garbage (the deterministic eip=0x117a4 derail on Vita — desktop
// escaped only because its write dirs pre-exist, so the W create/probe
// branch never ran). Narrow UTF-16 to ASCII and mirror the A logic.
// (File scope, non-static: both main()'s remaining KERNEL32 W-variants and
// kernel32_files.cpp's CreateFileW need it -- see runtime/rt_host.h.)
std::string wnarrow(Cpu&c,uint32_t p){ std::string r;
    if(p) for(uint16_t w:gread_wc(c,p,-1)) r.push_back((char)(w&0xff)); return r; }
// Crash.txt-triggered diagnostics: D2 opens Crash.txt (via do_create_file, now
// in kernel32_files.cpp) exactly at a Fog Halt. This reaches so far into
// rt_boot.cpp's own state (allocator/codec/room-guard/VA counters that other
// not-yet-extracted code also reads) that it stays here as a named function,
// called verbatim from do_create_file at the same point the inline blocks
// used to sit -- moving the CALLER doesn't require dragging this along too.
void crash_txt_diag_dump(Cpu& c, const std::string& base){
    // ALWAYS at a Fog Halt (D2 opens Crash.txt): dump the last sprite
    // decompressions. A "sprite decompression error" (Gfx.cpp:1632) is
    // then attributable — n>0 = native decoder served (my bug if D2 then
    // rejects it); n=0 = fell back to D2's own decoder (input already bad
    // => upstream corruption, not the decoder). Zero cost until a Halt.
    if(base=="Crash.txt"){
        // FIDELITY of the flat wrapper at 0x415240. Storm serializes ALL
        // threads onto ONE global buffer there (critical section
        // 0x779080): more than one thread with a per-thread buffer
        // would break the residual chain. This wrapper has NEVER been
        // verified on hardware — D2_EXPLVERIFY only exists in the
        // 0x41e000 hook.
        { char m[200]; std::snprintf(m,sizeof m,
            "[SCOMPFLAT] appels=%u fils-distincts=%u sortie-max=%u o%s",
            d2rt::scomp_flat_calls(), d2rt::scomp_flat_threads(),
            d2rt::scomp_flat_maxout(),
            d2rt::scomp_flat_threads()>1?"  ATTENTION PLUSIEURS CHAINES DE RESIDU":"");
            std::printf("  %s\n",m); d2vita_progress(m); }
        // Allocator state at the Halt — tells a capacity/fragmentation
        // failure (SFILE.CPP:898: large used/peak, small
        // largest-free) apart from a one-off garbage size
        // (g_lastAllocFail.n huge).
        { char m[176]; std::snprintf(m,sizeof m,
            "[ALLOC] heap now/peak=%u/%u KB va now/peak=%u/%u KB heap-free-max=%u KB va-free-max=%u KB misc=%u/%u KB",
            g_heapA.cur()>>10,g_heapA.peak()>>10,g_vaA.cur()>>10,g_vaA.peak()>>10,
            g_heapA.largest_free()>>10,g_vaA.largest_free()>>10,
            // Scratch occupancy: the engine counts from ITS OWN base
            // (MISC_BASE+0x100); the leading +0x100 is added back so
            // the figure stays the one from before, counted from MISC_BASE.
            (wx86_scratch_base()+wx86_scratch_used()-MISC_BASE)>>10,MISC_SIZE>>10); d2vita_progress(m); }
        if(g_lastAllocFail.set){ char m[176]; std::snprintf(m,sizeof m,
            "[ALLOC] LAST-FAIL region=%s req=%u KB used=%u KB largest-free=%u KB",
            g_lastAllocFail.region?g_lastAllocFail.region:"?",g_lastAllocFail.n>>10,
            g_lastAllocFail.used>>10,g_lastAllocFail.largest>>10); d2vita_progress(m); }
        // Codec.cpp: WHICH sprite was being decoded, and what size its
        // header summed to. An absurd total here explains a "Pool
        // Blocks overflowed" Halt without needing the hook — and
        // names the file.
        if(g_codecCalls){ char m[240]; std::snprintf(m,sizeof m,
            "[CODEC] appels=%llu sommes=%llu absurdes=%llu | DERNIER: « %s » "
            "images=%u (depart=%u) total=%u o (derniere=%u o) donnees=%08x",
            (unsigned long long)g_codecCalls,(unsigned long long)g_codecSums,
            (unsigned long long)g_codecAbsurd,
            g_codecFileName[0]?g_codecFileName:"<?>",
            g_codecFrames,g_codecStart,g_codecTotal,g_codecLastSz,g_codecData);
            std::printf("  %s\n",m); d2vita_progress(m);
            if(g_codecMaxTotal){ std::snprintf(m,sizeof m,
                "[CODEC] PLUS GROS TOTAL vu = %u o sur « %s »",
                g_codecMaxTotal,g_codecMaxFileName[0]?g_codecMaxFileName:"<?>");
                std::printf("  %s\n",m); d2vita_progress(m); } }
        // Reserve-vs-commit traffic + the largest live VA blocks. Tells a
        // reservation-charged arena (res >> commit) apart from a real
        // resident hog, and WHAT holds the arena when SFILE.CPP:898 fires.
        { char m[280]; std::snprintf(m,sizeof m,
            "[VASTATS] reserves=%u (%llu MB) commit-in=%llu MB commit-fresh=%llu MB releases=%u (%llu MB) rel-miss=%u decommit=%llu MB map=%u (%llu MB) unmap=%u (%llu MB)",
            g_vaResN,(unsigned long long)(g_vaResB>>20),(unsigned long long)(g_vaComInB>>20),
            (unsigned long long)(g_vaComFrB>>20),g_vaRelN,(unsigned long long)(g_vaRelB>>20),
            g_vaRelFailN,(unsigned long long)(g_vaDecB>>20),
            g_fmapN,(unsigned long long)(g_fmapB>>20),g_unmapN,(unsigned long long)(g_unmapB>>20)); d2vita_progress(m); }
        { uint32_t ta[8]={0},ts[8]={0};                     // top-8 live blocks, no <algorithm> needed
          for(auto&p:g_vaA.used_map()){ uint32_t a=p.first,s=p.second;
              for(int i=0;i<8;i++) if(s>ts[i]){ for(int j=7;j>i;j--){ts[j]=ts[j-1];ta[j]=ta[j-1];}
                  ts[i]=s; ta[i]=a; break; } }
          char m[220]; int off=std::snprintf(m,sizeof m,"[VATOP] n=%u top:",(unsigned)g_vaA.used_map().size());
          for(int i=0;i<8&&ts[i];i++) off+=std::snprintf(m+off,(size_t)(sizeof m-off)," %uK@%08x",ts[i]>>10,ta[i]);
          d2vita_progress(m); }
        // Fog raise ring: the LAST raise is the one that killed us — its
        // retaddr names the raise site, its record names the cel/component.
        for(uint32_t k=0;k<8 && k<g_raiseN;k++){ RaiseRec& r=g_raiseRing[(g_raiseN-1-k)&7];
            char m[256]; int off=std::snprintf(m,sizeof m,
                "[RAISE-%u] ra=%08x rec=%08x esi=%08x edi=%08x frame=%u body:",k,r.ra,r.rec,r.esi,r.edi,r.frame);
            for(int i=0;i<0x48&&off<(int)sizeof m-10;i+=4){ uint32_t v; std::memcpy(&v,r.body+i,4);
                off+=std::snprintf(m+off,(size_t)(sizeof m-off)," %08x",v); }
            d2vita_progress(m); std::printf("%s\n",m); }   // printf: d2vita_progress is a no-op under qemu
        // D2's OWN cache accounting (RE sub_457300): ceiling + current
        // of the sprite cache (ds:0x89db64 / 0x89db68) and the CelData
        // ceiling (ds:0x88db24) — proves what the announced RAM actually does.
        { uint32_t GB=g_d2base;
          char m[160]; std::snprintf(m,sizeof m,
            "[D2CACHE] sprite now/max=%u/%u KB celdata-max=%u KB bigmachine=%u",
            c.read_u32(GB+0x49db68)>>10, c.read_u32(GB+0x49db64)>>10,
            c.read_u32(GB+0x48db24)>>10, c.read_u32(GB+0x49db58));
          d2vita_progress(m); std::printf("%s\n",m); }
        // Asset read accounting: which archives feed the resident set.
        for(int i=0;i<256;i++){ if(!g_frb[i].h || g_frb[i].bytes<(64u<<10)) continue;
            auto pit=g_filePathByH.find(g_frb[i].h);
            const char* nm=pit!=g_filePathByH.end()?pit->second.c_str():"(closed)";
            const char* b=std::strrchr(nm,'/'); if(b) nm=b+1;
            char m[160]; std::snprintf(m,sizeof m,"[FILES] h=%08x reads=%u bytes=%lluK %s",
                g_frb[i].h,g_frb[i].reads,(unsigned long long)(g_frb[i].bytes>>10),nm);
            d2vita_progress(m); std::printf("%s\n",m); }
        // Halt-316 race diagnosis: which threads exist and where they sit at
        // the exact moment D2 reports. A worker mid-act-load while the main
        // thread is in FindRoomForUnit = the smoking gun for the wake-timing
        // race. Routed to the durable sink too (Vita has no stdout).
        if(g_sched){ std::printf("[HALT] thread states at Crash.txt open:\n");
            if(g_schedCoop) g_schedCoop->dump_threads();
            g_sched->dump_threads_to(d2vita_progress); }
        // Silent eiptrap ring (diag builds): the last 32 chains to the
        // D2_EIPTRAP address, recorded without I/O so the race is not
        // perturbed. The LAST entry is the halting call.
        dyn86_dump_eipring();
        { char m[112]; std::snprintf(m,sizeof m,"[ROOMGUARD] iter-calls=%llu deferred=%llu depth-now=%d disp-calls=%llu disp-s2=%llu",
              (unsigned long long)g_rgIter,(unsigned long long)g_rgSkip,g_rgDepth,(unsigned long long)g_rgDispCalls,(unsigned long long)g_rgDispS2);
          std::printf("%s\n",m); d2vita_progress(m); }
        if(g_schedCoop) g_schedCoop->dump_switch_ring();
        // Fog deferred-free queue (async free worker, proc RVA 0x101c0):
        // head @RVA 0x35e870, count/bytes @0x35d140/44. If the unit
        // nodes main was iterating sit in this queue, the Halt is a
        // genuine D2 use-after-free exposed by slow loads.
        { uint32_t rel=g_d2base-0x400000;
          uint32_t head=c.read_u32(0x75e870+rel);
          char m[160]; std::snprintf(m,sizeof m,"[FREEQ] head=%08x count=%u bytes=%u flag830=%u",
              head,c.read_u32(0x75d140+rel),c.read_u32(0x75d144+rel),c.read_u32(0x75e830+rel));
          d2vita_progress(m); std::printf("%s\n",m);
          uint32_t p=head;
          for(int k=0;k<8 && p>=0x1000 && p<0x12900000u; k++){
              char mm[176]; std::snprintf(mm,sizeof mm,"[FREEQ] n%d @%08x: %08x %08x %08x %08x",
                  k,p,c.read_u32(p),c.read_u32(p+4),c.read_u32(p+8),c.read_u32(p+12));
              d2vita_progress(mm); std::printf("%s\n",mm);
              p=c.read_u32(p); }
        }
    }
    // DIAG (D2_CRASHDUMP): D2 opens Crash.txt exactly at the Fog Halt. Walk
    // ghAct's persistent room graph and report any coordinate field holding a
    // guest-stack address (0x10bf.. — the leaked value in the (316) halt).
    // Tells us if the corruption is PERSISTENT (visible here) or transient.
    if(base=="Crash.txt" && getenv("D2_CRASHDUMP")){
        // Bounds / addresses RELATIVE to the layout (see dump_ghact_rooms).
        const uint32_t RLO=0x400u+d2rt::layout_hi(), RHI=0x13000000u+d2rt::layout_hi();
        auto RD=[&](uint32_t a)->uint32_t{ return (a>=RLO && a<RHI)?c.read_u32(a):0xBADBAD00u; };
        auto isstk=[](uint32_t v){ const uint32_t H=d2rt::layout_hi();
                                   return v>=0x10b00000u+H && v<0x10c00000u+H; };
        uint32_t ghAct=RD(g_d2base+0x7a0634u);
        { char m[128]; std::snprintf(m,sizeof m,"[CDUMP] ghAct=%08x room0=%08x",ghAct,RD(ghAct+0x10)); d2vita_progress(m); }
        int rn=0, hits=0;
        for(uint32_t r=RD(ghAct+0x10); r && r<RHI && rn<300; r=RD(r+0x7c), rn++){
            uint32_t off=RD(r+0x4c), L1=RD(r+0x10), room2=RD(L1+0x5c), x8=RD(room2+0x8);
            if(rn==0){ char m[200]; std::snprintf(m,sizeof m,"[CDUMP] r#0 raw: r=%08x [r+4c]=%08x [r+10](L1)=%08x [L1+5c](room2)=%08x [room2+8]=%08x sum=%08x",r,off,L1,room2,x8,off+x8); d2vita_progress(m); }
            if(isstk(off)||isstk(x8)||isstk(off+x8)){ hits++;
                char m[200]; std::snprintf(m,sizeof m,"[CDUMP] STACK-COORD r#%d room=%08x [r+4c]=%08x room2=%08x [room2+8]=%08x sum=%08x %s%s%s",
                  rn,r,off,room2,x8,off+x8, isstk(off)?"OFF ":"", isstk(x8)?"X8 ":"", isstk(off+x8)?"SUM":"");
                d2vita_progress(m); }
        }
        { char m[96]; std::snprintf(m,sizeof m,"[CDUMP] walked %d ghAct rooms, %d stack-coord hits",rn,hits); d2vita_progress(m); }
        dump_ghact_rooms(c,"crash");   // full per-room dump for the cross-platform map diff
    }
}

int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IOLBF,65536);   // keep progress visible when a run is killed
#ifdef __vita__
    // Vita has no shell/env/argv: bake config, mkdir via sceIo, drive the boot
    // with the Rogue Encampment input script. d2vita_platform_init returns the
    // genuine 1.14d PE-set dir under ux0:.
    const char* vdir = d2vita_platform_init();
    // MODULE LOAD BASE. The module is NOT loaded at a fixed address: two
    // dumps of the same eboot showed all their PCs shifted by exactly
    // 0xC000. Any symbol resolution that assumes a fixed base is therefore
    // WRONG — it can misname a thread sleeping in a kernel stub as a
    // completely unrelated function, or a PROCESS_INFO field as if it were a PC.
    //
    // So the RUNTIME address of a known symbol is published. The offset is
    // computed offline: bias = value_below - nm(d2vita.elf|main), and
    // any dump becomes readable, including one from a future crash.
    { char mb[160];
      std::snprintf(mb,sizeof mb,"module: main a l'execution=%p (biais = cette valeur - nm(main))",
                    (void*)(uintptr_t)&main);
      d2vita_progress(mb); std::printf("[%s]\n",mb); std::fflush(stdout); }
    // Install preflight: the #1 support request is "the game won't start"
    // with zero clue why. A missing Game.exe is caught again below (at
    // module load) but that check only reaches printf, invisible on
    // console; a missing MPQ isn't caught by us AT ALL otherwise until D2's
    // own guest code stumbles on it, deep into boot, with no message either
    // (do_create_file's read-miss is silent by default). Check the whole
    // required set HERE, once, so boot_progress.txt always names precisely
    // what's absent and exactly where it was expected -- patch_d2.mpq is
    // deliberately not in this list (recommended, not required; see
    // docs-site/installation.md).
    { static const char* kRequired[] = {
          "Game.exe","d2data.mpq","d2exp.mpq","d2char.mpq","d2sfx.mpq",
          "d2music.mpq","d2speech.mpq","d2video.mpq",
          "d2xmusic.mpq","d2xtalk.mpq","d2xvideo.mpq", nullptr };
      int nmiss=0; std::vector<std::string> missingNames;
      for(const char** f=kRequired; *f; ++f){
          std::string p=std::string(vdir)+"/"+*f;
          struct stat st{};   // zero-initialized: a stat() that fails without
                              // touching st must not leave st_size looking
                              // like a plausible (nonzero) size by accident.
          long rc=::stat(p.c_str(),&st);
          if(rc!=0 || st.st_size==0){
              char m[192]; std::snprintf(m,sizeof m,"install: MANQUANT %s (attendu: %s)",*f,p.c_str());
              d2vita_progress(m); std::printf("[%s]\n",m); ++nmiss; missingNames.push_back(*f); } }
      if(nmiss){ char m[96]; std::snprintf(m,sizeof m,"install: %d fichier(s) manquant(s) -- voir ci-dessus",nmiss);
          d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
          // Real on-screen message, not just a log line a player has to know
          // to go find: shown BEFORE Game.exe is even opened, using the same
          // sceGxm-free hand-drawn framebuffer as the crash-report consent
          // dialog (see install_screen_vita.cpp for why).
          d2vita_show_missing_files_screen(vdir, missingNames);
          // A required file missing is always fatal in practice -- D2 does
          // not degrade gracefully just because the ABSENT file happens not
          // to be Game.exe itself, so there is nothing to gain and a much
          // worse (later, less clear) failure to risk by letting the boot
          // limp forward. Stop here, the same way the Game.exe-missing case
          // below does.
          return 1; } }
    // Hardware runs at real speed — but only once the scheduler starts: the
    // DllMain init phase depends on per-call tick advance (poll loops with
    // timeouts, workers not yet running) and crawls under a real clock.
    if(!getenv("D2_VIRTCLOCK")) g_rtwant=true;
    // argc==2 (dir only): the GAMEEXE=1 + D2ARGS path drives Game.exe via the
    // cooperative scheduler. An entry arg (argc>2) would take the single-DLL
    // init branch and return early.
    const char* vargv[] = {"rt_boot", vdir};
    argc = 2; argv = (char**)vargv;

#endif
    // d2vita_platform_init() above (Vita-only) is where
    // d2cr_after_present() may already have started the d2cr_upload thread
    // (and the network) for a report consented in an earlier boot — long
    // before any of GAMEEXE/D2_RUNEXE/the self-test blocks below run. From
    // here to the end of main(), EVERY plain `return` (a missing module, a
    // failed br.commit()/br.link(), a self-test's own exit code, the
    // D2_RUNEXE torture-test branch, ...) must still stop and join that
    // thread before the process ends — "a thread alive during teardown isn't
    // defensible in any mode" (cr_boot.h) applies just as much to these bail-
    // outs as to the ordered teardown block at the bottom of the normal
    // game-boot path. A single scope guard covers all of them via ordinary
    // C++ stack unwinding (d2cr_shutdown() is a safe no-op when no thread was
    // ever started, so this costs nothing on host/qemu-arm builds, where
    // d2cr_after_present() has no real implementation at all, or on a run
    // with nothing pending). The one exit this destructor CANNOT catch is a
    // raw process-termination call that skips unwinding entirely — this file
    // has exactly two: the D2_SIGNTAG _exit(2) below (which gets its own
    // explicit d2cr_shutdown() call right before it) and the
    // sceKernelExitProcess(0) inside the ordered teardown block itself (which
    // already has its own explicit call, for the same reason).
    struct D2crShutdownGuard{ ~D2crShutdownGuard(){ d2cr::d2cr_shutdown(); } } d2cr_shutdown_guard;
    // Real-time mode: opt-in for qemu repro (D2_REALCLOCK) OR auto whenever
    // networking is on (D2NET) — the BNCS/MCP/D2GS protocol timers need the real
    // clock; under the virtual clock they expire and D2 stalls on SID_NULL without
    // ever sending the selector/SID_AUTH_INFO. (Vita already defaults g_rtwant on
    // above unless D2_VIRTCLOCK.) D2_VIRTCLOCK forces it off for diagnostics.
    // D2NET defaults to ON here too (same default as d2_boot_config.cpp, for
    // host/qemu launches that don't go through env.txt): D2NET=0 opts out.
    if(!getenv("D2NET")) setenv("D2NET","1",1);
    if(const char* n=getenv("D2NET")){ if(!*n || !strcmp(n,"0") || !strcasecmp(n,"non") || !strcasecmp(n,"off") || !strcasecmp(n,"false")) unsetenv("D2NET"); }
    if((getenv("D2_REALCLOCK") || getenv("D2NET")) && !getenv("D2_VIRTCLOCK")) g_rtwant=true;
    // EARLY parse of D2SCHED: the guest-inline gate and the TICKCAP knob run
    // BEFORE make_scheduler (called at the game site). make_scheduler
    // re-parses, validates (native|coop), and remains the ONLY assignment
    // point for g_sched/g_schedCoop. Native = real-clock by construction (the
    // D2_VIRTCLOCK+native combo is FATAL in make_scheduler).
    // The default is NATIVE: this rule must stay IDENTICAL to
    // make_scheduler's, otherwise the guest-inline gate would decide for one
    // backend while the game actually ran on the other.
    { const char* sm=getenv("D2SCHED");
      g_schedNative = !(sm && std::strcmp(sm,"coop")==0);
      if(g_schedNative) g_rtwant=true; }
    // D2_FAMINETEST lives at the scheduler site (GAMEEXE branch, after
    // make_scheduler): without GAMEEXE=1 it would be skipped SILENTLY — the
    // same trap as D2_ALT_SELFTEST, called out here rather than left unsaid.
    { const char* ft=getenv("D2_FAMINETEST");
      if(ft && *ft=='1' && !getenv("GAMEEXE"))
          std::fprintf(stderr,"faminetest: ignore (GAMEEXE=1 requis — le bloc vit au site du scheduler)\n"); }
    if(g_rtwant){ const char* tc=getenv("D2_TICKCAP");
        if(tc) g_tickCap=(uint32_t)std::atoi(tc); }   // diagnostic knob; default uncapped (validated behavior)
    if(const char* et=getenv("D2_EIPTRAP")) dyn86_eiptrap=(uintptr_t)strtoul(et,nullptr,16);
    if(const char* et2=getenv("D2_EIPTRAP2")) dyn86_eiptrap2=(uintptr_t)strtoul(et2,nullptr,16);
    // arm_next.S syncs the live regs (STM) only when a diag knob needs them —
    // nominal runs skip the 10-word store on every arm_next transition.
    if(dyn86_eiptrap || dyn86_eiptrap2 || getenv("D2_XFERTRACE")) dyn86_diag_regs=1;
    if(getenv("D2_TICKVIRT")) g_tickvirt=true;   // diag: virtual tick values, real scheduler
    if(argc<2){ std::printf("usage: rt_boot <dir> [EntryDll]\n"); return 2; }
    std::string dir=argv[1];
    g_dataRoot = dir;
    { const char* wr=getenv("D2WRITE"); g_writeRoot = wr?wr:"/tmp/d2vita_write";
#ifndef __vita__
      std::string mk="mkdir -p '"+g_writeRoot+"/Save'"; if(std::system(mk.c_str())){}
#endif
    }
    // Crash log: fresh file per run under $D2WRITE. A run whose crash.log ends
    // without "CLEAN EXIT" crashed or hung; the last line names how far it got.
    // The host-SIGSEGV handler (qemu) writes via dyn86_crash_fd (signal-safe).
    g_crashLogPath = g_writeRoot + "/crash.log";
    if(FILE* f=std::fopen(g_crashLogPath.c_str(),"w")){
        std::fprintf(f,"=== D2Vita boot (build %s %s) ===\n", __DATE__, __TIME__); std::fclose(f); }
    // The write directory has just been created (mkdir Save) and received
    // crash.log: any index already built on it is now stale. Later
    // d2_crashlog APPENDs add no directory entries — nothing further to
    // invalidate here.
    pc_dirty(g_writeRoot);
    // First index line of the run, in either state of the knob: only here,
    // not earlier, does crash.log exist AND has env.txt been read by
    // d2vita_platform_init (the same trap as D2_NETWATCH, see above).
    pc_announce();
#ifndef __vita__
    { extern int dyn86_crash_fd;
      dyn86_crash_fd = ::open(g_crashLogPath.c_str(), O_WRONLY|O_APPEND|O_CREAT, 0666); }
#endif
    // ---- CHECKING AN ALREADY-PRODUCED WAV (D2_SONCHECK=<path>) -----------
    // The same checker as the self-test, applied to an arbitrary file: this
    // is what the audio oracle calls on the game's own WAV. "The thread runs"
    // and "the file carries sound" do not produce the same log.
    if(const char* wc=getenv("D2_SONCHECK")){
        d2rt::audio::WavStats st; char rep[512];
        const bool ok=d2rt::audio::wav_check(wc,&st,rep,sizeof rep,22050);
        std::printf("=== SONCHECK %s | %s | %s\n", ok?"OK":"ECHEC", wc, rep);
        return ok?0:1;
    }

    // ---- AUDIO SINK self-test (D2_SONTEST=1) -----------------------------
    //
    // No DirectSound, no game, no thread: a 440 Hz sine wave pushed into the
    // WAV sink, then the file is READ BACK and verified (duration, frequency,
    // peak amplitude, silence ratio, clipping). This self-test permanently
    // separates "the sink works" from "DirectSound emulation works" —
    // without it, a silent WAV and a dead COM stub produce the same log. It
    // ends with a return code; it never boots the game.
    if(getenv("D2_SONTEST")){
        std::string wav = getenv("D2_SONDUMP") ? getenv("D2_SONDUMP") : (g_writeRoot + "/d2_sontest.wav");
        const int ms = getenv("D2_SONTESTMS") ? atoi(getenv("D2_SONTESTMS")) : 2000;
        std::printf("\n=== [SONTEST] auto-test du puits audio -> %s ===\n", wav.c_str());
        const int rc = d2rt::dsound::selftest(wav.c_str(), ms, 22050);
        d2_crashlog("CLEAN EXIT (auto-test D2_SONTEST — le jeu n'a PAS ete boote)");
        return rc;
    }

    // ---- Directory-index self-test (D2_PATHCACHETEST=1) --------------------
    //
    // Why here: the index is a PATH-RESOLUTION fix, not a rendering one; it
    // can be proven with no guest, no dynarec, and no clock, hence
    // DETERMINISTIC — and it runs on the real ARM binary, not a host mockup.
    // The block ends with a return code; it never boots the game.
    //
    // The SAME resolution assertions run with the index active and with
    // D2_PATHCACHE=0: that's the only way to prove the escape hatch changes
    // nothing observable. The COUNTER assertions, on the other hand, only
    // make sense with the index active and are announced as such (a missing
    // line must be information, never an artifact).
    if(getenv("D2_PATHCACHETEST")){
        std::printf("\n=== [PATHCACHETEST] auto-test de l'index de repertoire (index %s) ===\n",
                    pc_on()?"ACTIF":"DESACTIVE (D2_PATHCACHE=0)");
        std::string arene = g_writeRoot + "/pctest";
        std::string W = arene + "/w", D = arene + "/d";
        // Test arena built without going through a shell: std::system isn't
        // usefully available on Vita, and this test must run everywhere.
        mkdir(arene.c_str(),0755); mkdir(W.c_str(),0755); mkdir(D.c_str(),0755);
        for(const char* f : {"DATA.MPQ","Partage.txt","Realms.bin","VITA.d2s",
                             "Oublie.bin","Tardif.bin","Introuvable.bin","Nulle.part",
                             "BnetLog.txt","Aveugle.bin"}){
            ::unlink((W+"/"+f).c_str()); ::unlink((D+"/"+f).c_str()); }
        // The roots are MOVED onto the test arena: the real data directory
        // (the Blizzard files) is neither read nor written by this test.
        g_writeRoot=W; g_dataRoot=D;
        { PcLock lk; g_pcDirs.clear(); }
        auto touche=[](const std::string& p, const char* quoi){
            FILE* f=std::fopen(p.c_str(),"wb"); if(f){ std::fwrite(quoi,1,std::strlen(quoi),f); std::fclose(f); } };
        int echecs=0, n=0;
        auto verifie=[&](const char* nom, const std::string& got, const std::string& want){
            ++n; bool ok = (got==want);
            std::printf("  %-42s %s\n    obtenu=%s\n    attendu=%s\n", nom, ok?"OK":"ECHEC",
                        got.c_str(), want.c_str());
            if(!ok){ ++echecs; d2_crashlog("PATHCACHETEST ECHEC %s : obtenu=%s attendu=%s", nom, got.c_str(), want.c_str()); } };
        auto compte=[&](const char* nom, bool cond, const char* detail){
            ++n; if(!cond){ ++echecs; d2_crashlog("PATHCACHETEST ECHEC %s (%s)", nom, detail); }
            std::printf("  %-42s %s   (%s)\n", nom, cond?"OK":"ECHEC", detail); };

        touche(D+"/DATA.MPQ","donnees");
        touche(D+"/Partage.txt","cote-donnees");
        touche(W+"/Partage.txt","cote-ecriture");

        // T1/T2: Win32 semantics — exact name, then case-insensitive.
        verifie("T1 nom exact (dossier de donnees)",
                host_path("c:\\Diablo II\\DATA.MPQ"), D+"/DATA.MPQ");
        verifie("T2 casse differente",  host_path("data.mpq"), D+"/DATA.MPQ");
        // T3: ORDER. g_writeRoot is checked BEFORE g_dataRoot — the constraint
        // that stops the game from looping to rebuild what it just wrote.
        verifie("T3 ordre ecriture AVANT donnees",
                host_path("c:\\Diablo II\\Partage.txt"), W+"/Partage.txt");

        uint32_t b0,r0,s0,sc0;
        { PcLock lk; b0=g_pc.builds; r0=g_pc.fallbacks; s0=g_pc.stale; sc0=g_pc.scans; }
        // T4: one readdir per DIRECTORY, not per file. Twenty resolutions of
        // the same already-indexed file build NOTHING, and only scan for the
        // periodic re-verification that comes due (one every kMemoRescan
        // "absent" answers from the write directory — DATA.MPQ is absent
        // there, served from the data directory). This is the bound to
        // verify: readdir is no longer paid per FILE, it's paid per WINDOW.
        for(int i=0;i<20;i++) host_path("DATA.MPQ");
        { PcLock lk;
          compte("T4 20 resolutions -> 0 construction, <=1 parcours",
                 !pc_on() || (g_pc.builds==b0 && (g_pc.scans-sc0) <= 20u/kMemoRescan + 1u),
                 pc_on()?"index actif : le readdir est paye par dossier et par fenetre, jamais par fichier"
                        :"index desactive : assertion de compteur SANS OBJET, resolution seule verifiee"); }

        // T5: an "absent" answer costs NO scan. It comes from the build's
        // readdir, which already answered for the whole directory: redoing it
        // name by name would be the same scan for the same answer.
        { PcLock lk; r0=g_pc.fallbacks; sc0=g_pc.scans; }
        for(int i=0;i<3;i++) host_path("Introuvable.bin");
        { PcLock lk;
          compte("T5 3 sondes d'un absent -> 0 parcours", !pc_on() || (g_pc.scans==sc0 && g_pc.fallbacks==r0),
                 pc_on()?"le readdir de la construction fait foi pour tout le dossier":"sans objet"); }

        // T6: INVALIDATION — the Realms.bin / .d2s scenario, the only one
        // that can BREAK the game. First probe (the "absent" window opens),
        // then create the way the CreateFileA write shim does (fopen in the
        // write directory + pc_dirty), then reread. If invalidation failed to
        // clear the window, the read would return the DATA directory, the
        // fopen would fail, and the game would loop rebuilding the file.
        verifie("T6a Realms.bin absent -> dossier de donnees",
                host_path("Realms.bin"), D+"/Realms.bin");
        touche(W+"/Realms.bin","ecrit-par-le-jeu");
        pc_dirty(g_writeRoot);                 // what the shim does at the same site
        { PcLock lk; s0=g_pc.stale; }
        verifie("T6b Realms.bin apres creation+invalidation",
                host_path("Realms.bin"), W+"/Realms.bin");
        { PcLock lk;
          compte("T6c retrouve par l'INDEX, pas par le repli", !pc_on() || g_pc.stale==s0,
                 pc_on()?"aucun repli bruyant : l'invalidation a suffi":"sans objet"); }
        // The same scenario with a character save file.
        verifie("T6d VITA.d2s absent -> dossier de donnees",
                host_path("c:\\Diablo II\\Save\\VITA.d2s"), D+"/VITA.d2s");
        touche(W+"/VITA.d2s","sauvegarde");
        pc_dirty(g_writeRoot);
        verifie("T6e VITA.d2s apres creation+invalidation",
                host_path("c:\\Diablo II\\Save\\VITA.d2s"), W+"/VITA.d2s");

        // T7: THE NOISY FALLBACK. A file is created BEHIND the runtime's back
        // (no invalidation): this simulates a write site that was forgotten
        // and never catalogued. The index believes it absent; periodic
        // re-verification must find it WITHIN THE BOUNDED WINDOW, SAY SO, and
        // rebuild. A stale index must stay a SLOWNESS problem — never a
        // silent failure and never a permanent one.
        //
        // Both legs of the knob produce the SAME final resolution here:
        // without the index the historical scan finds it right away, with the
        // index after kMemoRescan. What differs is the number of silent
        // lookups (T7b), and it is reported as a counter, not as a resolution.
        { PcLock lk; s0=g_pc.stale; }
        touche(W+"/Oublie.bin","cree-sans-invalidation");
        std::string got; uint32_t k=0;
        for(; k<kMemoRescan+2; k++){ got=host_path("Oublie.bin"); if(got==W+"/Oublie.bin") break; }
        verifie("T7a repli bruyant : trouve malgre l'index perime", got, W+"/Oublie.bin");
        compte("T7b ... dans la fenetre bornee", pc_on() ? k<=kMemoRescan : k==0,
               pc_on()?"consultations muettes <= kMemoRescan":"index desactive : trouve du premier coup");
        std::printf("       (mesure : %u consultation(s) muette(s), borne kMemoRescan=%u)\n", k, kMemoRescan);
        { PcLock lk;
          compte("T7c le repli l'a DIT (compteur perime +1)", !pc_on() || g_pc.stale==s0+1,
                 pc_on()?"une ligne « INDEX PERIME » a ete ecrite dans crash.log":"sans objet"); }
        // T7d: and after the fallback, the index has been rebuilt — the next
        // resolution must not scan anything at all.
        { PcLock lk; sc0=g_pc.scans; }
        verifie("T7e apres reconstruction : toujours trouve",
                host_path("Oublie.bin"), W+"/Oublie.bin");
        { PcLock lk;
          compte("T7f ... et sans nouveau parcours", !pc_on() || g_pc.scans==sc0,
                 pc_on()?"le dossier a bien ete reconstruit par le repli":"sans objet"); }

        // T9: PRECEDENCE UNDER THE SAFETY NET — the case that could stay
        // SILENT. A name that legitimately exists in the DATA directory
        // (BnetLog.txt is exactly this case in the 1.14d refs) is probed
        // first: the write directory answers "absent". The game then creates
        // it on the write side WITHOUT invalidation. If the data-side hit
        // excused re-checking the write directory, precedence would flip
        // silently and permanently. Instead it must re-establish itself
        // within the window, and say so.
        touche(D+"/BnetLog.txt","cote-donnees");
        verifie("T9a d'abord servi par le dossier de donnees",
                host_path("BnetLog.txt"), D+"/BnetLog.txt");
        { PcLock lk; s0=g_pc.stale; }
        touche(W+"/BnetLog.txt","cree-sans-invalidation");
        k=0; for(; k<kMemoRescan+2; k++){ got=host_path("BnetLog.txt"); if(got==W+"/BnetLog.txt") break; }
        verifie("T9b la precedence ECRITURE se retablit", got, W+"/BnetLog.txt");
        compte("T9c ... dans la fenetre bornee, et en le disant", pc_on() ? (k<=kMemoRescan && g_pc.stale==s0+1) : k==0,
               pc_on()?"la trouvaille cote donnees ne dispense pas de re-verifier le dossier d'ecriture":"sans objet");

        // T8: not found anywhere -> data directory, as before.
        verifie("T8 introuvable partout -> dossier de donnees",
                host_path("Nulle.part"), D+"/Nulle.part");

        // T10: BLIND DIRECTORY. A failing opendir does not mean an empty
        // directory, it means NO information at all: the runtime must fall
        // back to the historical scan for this directory, still serve the
        // exact name, and not spin declaring itself stale. Mode 0311 allows
        // opening a known name but forbids listing — exactly this case.
        {
            std::string AV = arene + "/aveugle";
            mkdir(AV.c_str(),0755); touche(AV+"/Aveugle.bin","dossier-illisible");
            std::string savedW=g_writeRoot;
            if(geteuid()!=0 && chmod(AV.c_str(),0311)==0){
                g_writeRoot=AV; { PcLock lk; g_pcDirs.clear(); }
                uint32_t st0; { PcLock lk; st0=g_pc.stale; }
                verifie("T10a dossier illisible : nom exact servi",
                        host_path("Aveugle.bin"), AV+"/Aveugle.bin");
                host_path("Aveugle.bin");
                { PcLock lk;
                  compte("T10b marque AVEUGLE, et jamais « perime »", !pc_on() || (g_pc.blind>0 && g_pc.stale==st0),
                         pc_on()?"pas de tourniquet reconstruction/repli sur un dossier illisible":"sans objet"); }
                chmod(AV.c_str(),0755);
            } else {
                std::printf("  %-42s SANS OBJET   (%s)\n","T10 dossier aveugle",
                            geteuid()==0?"lance en root : chmod n'interdit rien":"chmod refuse");
            }
            ::unlink((AV+"/Aveugle.bin").c_str());
            g_writeRoot=savedW;   // the BLIND entry stays in the index: it's the PROOF, in the final summary
        }

        pc_report(true);
        std::printf("=== [PATHCACHETEST] %s : %d/%d verifications, %d echec(s) ===\n",
                    echecs?"ECHEC":"PASS", n-echecs, n, echecs);
        // The verdict ALSO goes to crash.log: on console std::printf goes
        // nowhere, and a self-test run must be readable the same way as any
        // other. The CLEAN EXIT line explicitly says the game was not booted
        // — without it, a D2_PATHCACHETEST left on in env.txt would read as a
        // crash (crash.log without CLEAN EXIT = crashed or hung).
        d2_crashlog("PATHCACHETEST %s : %d/%d verifications, %d echec(s)",
                    echecs?"ECHEC":"PASS", n-echecs, n, echecs);
        d2_crashlog("CLEAN EXIT (auto-test D2_PATHCACHETEST — le jeu n'a PAS ete boote)");
        return echecs?1:0;
    }
    rt_wall();   // warm up the wall-clock base on the real main thread (see rt_wall)
    // D2NET enables the real network layer on both platforms. On console,
    // d2vita_platform_init already brought the Sony stack up (and, if that
    // itself failed, already set D2NET_FAILED) before the arena was
    // entered; the CONNECTION wait it kicked off in the background is
    // joined here instead of back there -- by now several seconds of arena/
    // DllMain/Authenticode/GXM setup have usually already covered it, so
    // this rarely actually blocks. If it isn't up: this stays offline, the
    // game shows its own "cannot connect" message, and solo mode is
    // unaffected.
    if(getenv("D2NET") && !getenv("D2NET_FAILED")){
        int nr = d2vita_net_join();
        d2vita_progress(d2vita_net_status());
        if(nr != 0){
            char m[64]; std::snprintf(m, sizeof m, "reseau: init KO (code %d) -> hors-ligne", nr);
            d2vita_progress(m);
            setenv("D2NET_FAILED", "1", 1);
        }
    }
    if(getenv("D2NET") && !getenv("D2NET_FAILED")){
        wx86_net_set_enabled(true); std::printf("[net] sockets reelles ACTIVEES (D2NET)\n"); }
    else if(getenv("D2NET")){
        // stdout doesn't exist on console: this verdict must go to the boot
        // log, otherwise an offline run is indistinguishable from a silent
        // online one.
        std::printf("[net] D2NET demande mais la pile n est pas montee -- hors-ligne\n");
        d2vita_progress("reseau: D2NET demande, pile absente -> hors-ligne"); }
    if(getenv("D2SCRIPT")){ inj_parse(getenv("D2SCRIPT"));
        std::printf("[inj] %zu scripted input events\n",(size_t)inj_count()); }
    std::string entryDll = argc>2 ? argv[2] : "Fog.dll";
    apply_compact_layout();   // repack regions LOW under D2LAYOUT=compact (Vita RAM fit)

#ifdef D2RT_CPU_BOX86
    Cpu* cpu = make_cpu_box86();   // ARM: Box86-derived dynarec backend
    // Arena reach probe (D2LAYOUT=compact): touch a ladder of guest addresses
    // up to the packed span ceiling and log each rung. On Vita3K the world
    // load faulted writing guest 0x10bf95a4 (main stack) even though the
    // 0x12900000-byte block alloc "succeeded" — this shows where the real
    // committed block ends. Cheap (8 writes), runs on every compact boot.
    // Under the HIGH layout there is NO arena yet: nothing is mapped at this
    // point (regions are mapped further down in main), so the ladder would
    // write into nothing. The probe stays WORD FOR WORD the same for
    // "compact"; it is simply skipped under the HIGH layout.
    if(getenv("D2LAYOUT") && !d2rt::layout_hi()){
        static const uint32_t rungs[]={0x01000000,0x0F000000,0x10000000,0x10800000,
                                       0x10BF0000,0x11000000,0x11400000,0x118FF000};
        for(uint32_t a:rungs){ uint32_t v=0xD2A0BEEF; cpu->write(a,&v,4);
            char m[64]; std::snprintf(m,sizeof m,"arena probe ok: 0x%08x",a);
            d2vita_progress(m); }
        std::printf("arena probe: all rungs writable\n");
    }
#else
#error "Box86 is the only CPU backend: build with -DD2RT_CPU_BOX86 (the Unicorn backend is retired)"
#endif
    g_cpu=cpu;
    // D2_CALLRET=1: dynarec return prediction. Halves the number of
    // translated block entries while producing an IDENTICAL image, pixel for
    // pixel (FBHASH oracle, deterministic bench). No speed gain under qemu —
    // expected: what the prediction saves is the FIXED cost of a block entry
    // on real hardware, which emulation doesn't model. Verdict is
    // console-only. This is now the default (validated in env.txt, +6.8% on
    // console) — no more environment read, the old "inactive" leg is removed.
    { extern int box86_dynarec_callret;
      box86_dynarec_callret = 1;
      std::printf("dynarec: prediction de retour ACTIVE (defaut)\n"); }
    // D2_BUDGETTAIL=1: the block-budget expiry check moved to the END of a
    // block instead of the prologue (same code, different placement). What
    // this targets: profiling attributes a meaningful share of all emitted
    // ARM bytes to a dead path that the BGT branch skips over on EVERY block
    // entry. Read AT TRANSLATION TIME by the generator — one binary carries
    // both legs, so it must be set HERE, before the first translation.
    // ⚠️ NEVER MEASURED ALONE ON CONSOLE: it only ran as part of a combined
    // pass with other levers since removed. Should be re-verified against
    // controls on its own.
    // This is now the default (validated in env.txt) despite the caveat
    // above — no more environment read.
    { extern int box86_dynarec_budgettail;
      box86_dynarec_budgettail = 1;
      std::printf("dynarec: chemin d'expiration du budget EN QUEUE de bloc (defaut)\n"); }
    // D2_FORWARD=<n>: size of the gap a block may bridge forward (default
    // 256 = original box86). Larger = longer blocks, hence fewer block
    // transitions (~283 cycles each on console), but more translated code and
    // more I-cache/jump-table pressure. The SIGN of the result isn't
    // predictable from qemu: this is a console A/B, worth trying at 512 then
    // 1024. Clamped at 0: a negative value would make the comparison
    // `(next-addr) < box86_dynarec_forward` (unsigned on the left) always
    // true, so a gap of any size would be bridged.
    // D2_SIGNTAG=1: neutralizes the Blizzard idiom "complemented pointer,
    // discriminated by the sign bit" (312 sites found by disassembling
    // Game.exe 1.14d). The discriminant moves from bit 31 to bit 30. This is
    // NOT a fix: it covers only one pattern among several (the TERNARY test
    // at 0x44f8e5 escapes it, and that's the one that kills the HIGH layout).
    // It's a MEASUREMENT INSTRUMENT, and it refuses to arm outside
    // D2LAYOUT=haut rather than allow a silent A/B.
    { extern int dyn86_signtag;
      if(const char* e=getenv("D2_SIGNTAG")){
          int v=atoi(e);
          if(v && !d2rt::layout_hi()){
              d2_crashlog("FATAL: D2_SIGNTAG exige D2LAYOUT=haut (le discriminant bit 30 "
                          "s'inverse sur un plan invite sous 0x40000000)");
              // _exit() skips C++ stack unwinding, so the scope
              // guard above never runs for this exit — same rule ("not
              // defensible in any mode"), explicit call since RAII can't
              // reach here. No-op if no upload thread was ever started.
              d2cr::d2cr_shutdown(); _exit(2); }
          dyn86_signtag = v;
          jpline("dynarec: discriminant de pointeur complemente sur le BIT 30 %s (D2_SIGNTAG=%d)",
                 dyn86_signtag?"ACTIF":"inactif", dyn86_signtag); } }
    // D2_MMUFOLD=1 / D2_MMUSTACK=1: another lever on the same profile — the
    // fastmmu base ADD, present before EVERY guest memory access once the
    // arena exists (membase != 0), and which sits ON the critical path for
    // loading. MMUFOLD folds the base into the constant of ABSOLUTE
    // addressing; MMUSTACK chains consecutive PUSH/POP r32 onto the working
    // register the previous one already left translated. BOTH are INERT
    // without an arena: a bench without D2ARENA proves nothing about them.
    // Read by the GENERATOR at translation time (one binary carries every
    // leg), so set HERE before any translation. Contract and proofs:
    // third_party/box86-dynarec/dynarec/dynarec_arm_mmu.h.
    // This is now the default (validated in env.txt) — no more environment read.
    { extern int dyn86_mmufold, dyn86_mmustack;
      dyn86_mmufold = 1; dyn86_mmustack = 3;
      jpline("dynarec: pliage fastmmu des absolus ACTIF (D2_MMUFOLD=%d, defaut)", dyn86_mmufold);
      jpline("dynarec: chaine fastmmu POP=ACTIF PUSH=ACTIF (D2_MMUSTACK=%d, defaut)", dyn86_mmustack); }
    { extern int dyn86_nopend;
      if(const char* e=getenv("D2_NOPEND")){
          int v=atoi(e); if(v!=1 && v!=3) v=0; dyn86_nopend=v;
          char m[192]; std::snprintf(m,sizeof m,
              (v==3) ? "nopend: mode=3 — X_PEND JAMAIS force. INCORRECT PAR "
                       "CONSTRUCTION, mesure de PLAFOND uniquement : verifier "
                       "que ring: (dessins/sommets par image) n'a pas bouge"
                     : "nopend: mode=%d", v);
          std::printf("%s\n",m); d2vita_progress(m); } }
    { extern int box86_dynarec_forward;
      if(const char* e=getenv("D2_FORWARD")){ int v=atoi(e); if(v<0) v=0;
          box86_dynarec_forward = v;
          std::printf("dynarec: forward=%d octets (D2_FORWARD, defaut 256)\n", v); } }
    Bridge br(cpu);
    br.set_module_limit(MISC_BASE); // modules auto-placed below MISC, in both memory layouts
    g_profBr = &br;                 // stage 2 of the frame profile (PROF_COUNTERS)
    // 1.14 (monolith): all former gameplay DLLs are statically linked into a
    // single Game.exe. We load ONE module and let its CRT entry drive the whole
    // boot. (1.13c, with its 14 separate DLLs, is retired — see git history.)
    std::string err;
    // POINT-9 generic x86 torture-test: D2_RUNEXE=<path> loads an ARBITRARY 32-bit
    // Win32 EXE at its ImageBase and drives its entry through the SAME shim+dynarec+
    // scheduler path as Game.exe (imports resolve via K()/U(); a missing import trips
    // the default-shim CONTROLLED STOP). Fully gated: unset => byte-identical boot.
    const char* g_runexe = getenv("D2_RUNEXE");
    std::string exeLogical = "Game.exe";
    if(g_runexe){
        std::string p=g_runexe; size_t s=p.find_last_of("/\\");
        exeLogical = (s==std::string::npos)?p:p.substr(s+1);      // basename = module name
        auto b=slurp(g_runexe);
        if(b.empty()){ std::printf("missing D2_RUNEXE %s\n",g_runexe); return 1; }
        std::printf("=== D2_RUNEXE: %s @ its ImageBase ===\n", exeLogical.c_str());
        if(!br.add_module(exeLogical,b,err)){ std::printf("add %s: %s\n",exeLogical.c_str(),err.c_str()); return 1; }
    } else {
        std::vector<const char*> mods = {"Game.exe"};
        std::printf("=== 1.14 monolith: single Game.exe ===\n"); d2vita_progress("mode: 1.14 monolith");
        for(auto m:mods){ auto b=slurp(dir+"/"+m); if(b.empty()){
            char em[192]; std::snprintf(em,sizeof em,"install: %s introuvable ou vide (attendu: %s/%s)",m,dir.c_str(),m);
            d2vita_progress(em); std::printf("[%s]\n",em); return 1;}
            if(!br.add_module(m,b,err)){ std::printf("add %s: %s\n",m,err.c_str()); return 1; } }
    }
    if(!br.commit(err)){ std::printf("commit: %s\n",err.c_str()); d2vita_progress("commit FAILED"); return 1; }
    // Version guard (monolith path only). The install preflight above only
    // checks that Game.exe and the MPQs EXIST, not their VERSION -- so a 1.13c
    // install (which has a Game.exe + the MPQs) sails through it, then a few
    // seconds later calls an unshimmed Storm.dll ordinal and stops with a
    // cryptic "unshimmed import" the player can't act on. Close that gap here:
    // a genuine 1.14d Game.exe is a monolith and imports NONE of the D2 split
    // DLLs; a Game.exe that imports Storm/Fog/D2Win/... is 1.13c (or otherwise
    // split), which this build does not support. Name it on screen and in the
    // log, and stop cleanly.
    if(!g_runexe){ if(PeImage* gpi=br.module("Game.exe")){
        static const char* kSplit[]={"storm.dll","fog.dll","d2win.dll","d2client.dll","d2common.dll","d2gfx.dll",nullptr};
        std::string badDll;
        for(const auto& ir : gpi->imports()){
            std::string dl=ir.dll; for(auto&ch:dl) ch=(char)std::tolower((unsigned char)ch);
            for(const char** s=kSplit; *s; ++s) if(dl==*s){ badDll=ir.dll; break; }
            if(!badDll.empty()) break; }
        if(!badDll.empty()){
            char m[192]; std::snprintf(m,sizeof m,
                "install: Game.exe importe %s -> version 1.13c/splittee, D2Vita exige la 1.14d monolithe",badDll.c_str());
            d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
            d2vita_show_version_error_screen(dir);
            return 1; } } }
    // Purely informational: note (do NOT stop, do NOT alarm) any leftover 1.13c
    // split DLLs / extra launchers sitting next to the 1.14d monolith.
    // kernel32_modules IGNORES those DLLs at load time (see its kD2Split list),
    // so their presence is HARMLESS -- the game behaves exactly as with a clean
    // install. This one line only exists so a crash report / the log shows a
    // messy install for what it is, should some subtler symptom ever be traced
    // back to it. A clean 1.14d install prints nothing here.
    if(!g_runexe){
        static const char* kLeftover[]={"Storm.dll","Fog.dll","D2Win.dll","D2Client.dll",
            "D2Common.dll","D2gfx.dll","D2Game.dll","Diablo II.exe","BNUpdate.exe","SystemSurvey.exe",nullptr};
        std::string found; int nf=0;
        for(const char** f=kLeftover; *f; ++f){ struct stat st{};
            if(::stat((dir+"/"+*f).c_str(),&st)==0){ if(nf<6){ if(nf) found+=", "; found+=*f; } ++nf; } }
        if(nf){ char m[224]; std::snprintf(m,sizeof m,
            "install: %d fichier(s) PC en trop presents (%s%s) -- ignores, sans effet (seuls Game.exe + MPQ comptent)",
            nf, found.c_str(), nf>6?", ...":"");
            d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout); } }
    // Index the loaded module by base + name so LoadLibraryA/GetProcAddress resolve
    // to the real image (Game.exe dynamically loads D2 DLLs and calls exports).
    { PeImage* pi=br.module(exeLogical); if(pi){ g_modByBase[pi->load_base()]=pi;
        std::string ln=exeLogical; for(auto&ch:ln) ch=(char)std::tolower((unsigned char)ch); g_modByName[ln]=pi; } }
    g_bridge=&br;   // enable runtime LoadLibrary of on-disk DLLs (lockdown CheckRevision.dll)
    uint32_t entryBase = br.module_base(g_runexe?exeLogical.c_str():(g_114?"Game.exe":(argc>2?argv[2]:"Fog.dll")));
    g_d2base = br.module_base(exeLogical);
    d2cr::d2cr_session_game_loaded(g_d2base);   // crash addresses as module+offset later
    d2_prof_map();   // the engine knows no subsystems: we describe our own
    g_cpuForStats = cpu;            // arms d2rt_sprite_cache_kb (watchdog cache curve)
    if(getenv("D2_CELWATCH")){ long v=strtol(getenv("D2_CELWATCH"),nullptr,0); g_celWatch=(uint32_t)(v>0?v:120); }
    if(getenv("D2_IOHIST")) g_ioHist=atoi(getenv("D2_IOHIST"))?atoi(getenv("D2_IOHIST")):1;
    io_init();                       // D2_IOSTAT / D2_READAHEAD
    if(getenv("D2_CELCAP")){ uint32_t v=(uint32_t)strtoul(getenv("D2_CELCAP"),nullptr,0);
        if(v>=0x4000u && v<=0x2000000u){ g_celCap=v; if(!g_celWatch) g_celWatch=120; }   // the reset goes through the CELWATCH loop
        else std::printf("celdata: D2_CELCAP=%u REFUSE (16 Ko .. 32 Mo) — plafond inchange\n",v); }
    // D2_TIMEPROF=<us>: TIME profile of guest code (default 250 us if the
    // value is missing or absurd). Unlike D2_EIPPROF, the block budget is
    // SERVO-CONTROLLED so intervals are equal in TIME: the sample count at an
    // address is then proportional to the TIME spent there. This instrument
    // exists because D2_EIPPROF over-represents short, heavily-called
    // functions by roughly a factor of 10.
    if(const char* tp = getenv("D2_TIMEPROF")){
        long us = strtol(tp, nullptr, 10);
        if(us < 20 || us > 100000) us = 250;
        d2rt_timeprof_base = g_d2base;
        d2rt_timeprof_on = (uint32_t)us;
        char m[160]; std::snprintf(m,sizeof m,
            "timeprof: ARME cible=%ld us par echantillon (budget initial 2048 blocs)", us);
        std::printf("%s\n",m); d2vita_progress(m);
    }
    // ---- D2_LAGWATCH=<ms>: attribute one-off freezes in-game ---------------
    // For a GAME session, not a bench. Arms three things at once:
    //   * the frame profile (D2_FRAMEPROF), which already knows WHEN and
    //     attributes HOST causes (jit / sync / reads / scomp / sw);
    //   * the EIP sampler, at a COARSE interval (2000 us by default,
    //     overridable via D2_TIMEPROF) — a small fraction of overhead instead
    //     of the much higher cost measured at 250 us;
    //   * the attribution ring, reread ONLY when a frame exceeds the
    //     threshold. A session with no freeze only pays for the sampler.
    // The "slow#NN" lines are published PERIODICALLY (not only at the end): a
    // play session often ends with the app being killed outright.
    if(const char* lw = getenv("D2_LAGWATCH")){
        long ms = strtol(lw, nullptr, 10);
        if(ms < 20 || ms > 60000) ms = 80;
        g_lagMs = (uint32_t)ms;
        d2rt_lag_on = 1;
        d2rt_timeprof_base = g_d2base;
        // ⚠️ THE SAMPLER IS THE ONLY EXPENSIVE PART. It forces a FINITE block
        // budget, but under D2SCHED=native the budget is normally INFINITE:
        // every expiry goes back through the scheduler and the GIL.
        // cpu_box86.cpp's own comment says so explicitly ("don't enable
        // D2_EIPPROF + native without rethinking this"). Default set to
        // 10 ms: attributing a freeze needs no finer grain (a 200 ms freeze
        // still yields plenty of samples), and it's 5x fewer preemptions than
        // at 2 ms.
        // D2_LAGWATCH_NOSAMP=1: cuts the sampler. All HOST-side attribution
        // (jit / sync / reads / scomp / sw / look) is kept, which already
        // suffices to separate loading, decompression, and guest code — at
        // ZERO extra cost beyond D2_FRAMEPROF.
        if(getenv("D2_LAGWATCH_NOSAMP")){
            d2rt_lag_on = 0;
        } else if(!d2rt_timeprof_on){             // D2_TIMEPROF keeps control
            d2rt_timeprof_on = 10000;             // us
            cpu->set_run_limit(4096ull*8ull);     // amorcage, l'asservissement reprend
        }
        char m[176]; std::snprintf(m,sizeof m,
            "lagwatch: ARME seuil=%ld ms | echantillonnage=%s | "
            "lire les lignes « lente#NN ... | invite ech=... »",
            ms, d2rt_lag_on ? "actif" : "COUPE (D2_LAGWATCH_NOSAMP)");
        std::printf("%s\n",m); d2vita_progress(m);
    }
    if(getenv("D2_EIPPROF")){ d2rt_eipprof_base = g_d2base; d2rt_eipprof_on = 1;
        // WITHOUT A BLOCK BUDGET, THE PROFILE IS SILENT UNDER D2SCHED=native.
        // The sample is taken on block-budget EXPIRY (cpu_box86.cpp:818):
        // that's what makes it unbiased, a cut at a random point in
        // execution. coop arms one per time slice (QUANTUM); native arms
        // NONE — budget 0x7FFFFFFF, expiry never reached. So a
        // MEASUREMENT-ONLY budget is armed here. On the native side, expiry
        // falls back to the existing "spurious resume" path (unchanged
        // starvation epoch => immediate resume), so scheduling is untouched;
        // on the coop side the value is overwritten every slice by QUANTUM,
        // so nothing changes there either.
        // set_run_limit divides by 8 (cpu_box86.cpp:517): x8 to get blocks.
        // 20,000 blocks ~= 7 samples per frame at 150,000 blocks/frame.
        unsigned long nb = 20000;
        if(const char* e=getenv("D2_EIPPROF_BUDGET")) nb = strtoul(e,nullptr,10);
        if(nb) cpu->set_run_limit((uint64_t)nb*8ull);
        std::printf("EIPPROF: budget d'echantillonnage = %lu blocs\n", nb); }
    // SAME NEED FOR D2_TIMEPROF, and it's essential: under D2SCHED=native the
    // budget is armed INFINITE and budget expiry — the only moment emu->ip is
    // up to date, hence the only possible sampling point — NEVER FIRES.
    // Without this line the instrument silently reports "samples=0". The
    // value set here is only a SEED: from the first sample onward, the
    // feedback loop (cpu_box86.cpp, timeprof_sample) takes over and adjusts
    // the per-thread budget to hit the microsecond target.
    if(d2rt_timeprof_on) cpu->set_run_limit(2048ull*8ull);
    // guest heap / VA / misc regions
    cpu->map(HEAP_BASE,HEAP_SIZE,nullptr,P_RW);
    cpu->map(VA_BASE,  VA_SIZE,  nullptr,P_RWX);
    // The engine REPORTS exhaustion, it does NOT write to our log (it knows
    // neither d2_crashlog nor our line format). The callback is armed BEFORE
    // the first init: a region that refused right at creation would still
    // report it. Output byte-identical to the old RegionAlloc::record_fail.
    wx86::region_set_fail_handler([](const wx86::RegionFail& f){
        g_lastAllocFail = { f.name, f.want, f.cur, f.peak, f.largest, f.used, true };
        std::fprintf(stderr,"[RegionAlloc %s 0x%08x FAIL %u bytes, used %u MB, largest-free %u MB]\n",
            f.name, f.base, f.want, f.used>>20, f.largest>>20);
        d2_crashlog("ALLOC FAIL region=%s req=%u KB used=%u KB peak=%u KB largest-free=%u KB",
            f.name, f.want>>10, f.used>>10, f.peak>>10, f.largest>>10);
    });
    g_heapA.init(HEAP_BASE+0x1000, HEAP_SIZE-0x1000, 16, "heap");
    // FIDELITY (root cause of the Halt 904/1420/253/124/607 family): Windows
    // returns VirtualAlloc bases aligned to dwAllocationGranularity (64 KiB),
    // and Fog relies on this: it rounds its pointers down to that granularity
    // (and eax,~([0x776cb8]-1), Game+0x11ac3..0x11acb) to find its
    // allocator's page header and for VirtualFree. With bases at xxxx1000,
    // that rounding lands in the LAST page of the PRECEDING region:
    // counters get written at +0x14/+0x18/+0x20/+0x24/+0x38 of a foreign
    // block (a sprite header -> its [1],[2],[7] table gets overwritten) and
    // the wrong region gets freed. D2_VA4K=1 restores the old alignment (A/B).
    if(std::getenv("D2_VA4K")) g_vaA.init(VA_BASE+0x1000, VA_SIZE-0x1000, 0x1000, "va");
    else g_vaA.init(VA_BASE+0x10000, VA_SIZE-0x10000, 0x10000, "va");
    cpu->map(MISC_BASE,MISC_SIZE,nullptr,P_RWX);   // RWX: hosts guest-callback stubs (wndproc dispatch)
    // Granting the scratch range to the engine: pointer starts at
    // MISC_BASE+0x100 (the historical first-allocation address, unchanged)
    // and the upper bound is MISC_BASE+MISC_SIZE (now enforced in the
    // engine). This is the single arming site: MISC_BASE/MISC_SIZE are final
    // here, apply_compact_layout having already had the chance to redefine
    // them well before this point.
    wx86_scratch_init(MISC_BASE+0x100, MISC_SIZE-0x100);
    wx86_scratch_set_oom_handler([](uint32_t at,uint32_t want,uint32_t limit){
        d2_crashlog("MISC EXHAUSTED at 0x%08x (+%u) limit 0x%08x — leaking per-call alloc suspected",at,want,limit); });
    // Guest tick page (see tick_bump): counter starts at 0x10000 like the old
    // g_tick so timestamp magnitudes are unchanged; base carries virt_ms.
    g_tickVA=misc(8); cpu->write_u32(g_tickVA,0x10000); cpu->write_u32(g_tickVA+4,0x10000);

    // Windows TIB/TEB. Unicorn's FS base is 0 by default and reliable segment
    // reprogramming is fiddly, so we map the TIB at linear 0 — then fs:[off]
    // (base 0) reads/writes the TIB directly. The MSVC CRT startup touches
    // fs:[0] (SEH chain head) before any KERNEL32 call.
    // (HIGH layout: MAIN_TIB != 0, and FS gets its base explicitly — without
    // that, fs:[0] at CRT startup would read page 0, which no longer exists.)
    cpu->map(MAIN_TIB,0x1000,nullptr,P_RW);
    cpu->write_u32(MAIN_TIB+0x00,0xFFFFFFFF);          // ExceptionList = end-of-chain
    cpu->write_u32(MAIN_TIB+0x04,MAIN_STACK_TOP);      // StackBase (top of 2 MiB guest stack)
    cpu->write_u32(MAIN_TIB+0x08,MAIN_STACK_TOP-0x200000); // StackLimit
    cpu->write_u32(MAIN_TIB+0x18,MAIN_TIB);            // Self
    if(MAIN_TIB) cpu->set_fs_base(MAIN_TIB);

    // ---- KERNEL32 shim set (argc = dword args, stdcall cleanup) ------------
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // Internal (never imported by D2): clears the scheduler loader-lock pin.
    // The DllMain redirect stub calls its trap VA after DllMain returns.
    K("__d2rt_dllmain_done",0,[](Cpu&){ if(g_sched) g_sched->set_no_preempt(false); return 0u; });
    K("GetVersion",0,[](Cpu&){ return 0x0A280105u; });                     // 5.1 build 2600
    win32_shims_kernel32_install(br);   // 46 generic KERNEL32 shims -> winx86
    // Criterion: body IDENTICAL to the other port (same engine, different
    // game) AND no external dependency.
    //
    // CURRENT STATE. Of the 252 KERNEL32 keys, the engine serves 137 and this
    // file 115. Synchronization, locale, memory layout, and waiting have all
    // moved. What remains here comes down to five dependencies, all on this
    // port, NONE on Diablo II:
    //   * host files (paths, handle table, directory index, write
    //     directory) — 31 keys;
    //   * module mapping (g_modByBase/g_modByName, the "C:\Diablo II\..."
    //     paths returned by GetModuleFileName) — 26 keys;
    //   * the GUEST clock (tick page, D2_VIRTCLOCK, time bias) — 10;
    //   * threads and the TIB (g_sched, cur_tib, TLS slots) — 15;
    //   * critical sections, which have an intrinsic fast path in the
    //     dynarec and cannot move without it — 6.
    // The rest (controlled shutdown, .ini readers, Sleep prelude) is
    // genuinely game-specific.
    //
    // MEASURED, NOT IMPRESSION: all 115 remaining keys are ALSO implemented
    // by the other port. The duplication debt is therefore not resolved — it
    // is only blocked by state this port owns, not by game specificity.
    
    // ---- THE MEMORY LAYOUT -> winx86 --------------------------------------
    // Seventeen shims move: heap (Heap/Local/Global), virtual address arena
    // (Virtual*), region introspection, and machine description. Their
    // bodies were character-identical in the other port — checked one by
    // one, the only difference was the NAME of an allocation alias.
    //
    // What stays HERE, and this isn't caution: the LAYOUT itself. The two
    // region INSTANCES and their ranges (dictated by the console's compact
    // layout), the region description for VirtualQuery (it names our
    // HEAP_BASE/VA_BASE/MISC_BASE constants and our module map), and all the
    // INSTRUMENTATION — occupancy counters, log lines, naming the offending
    // site in crash.log. The latter decides nothing: it plugs into
    // d2_mem_observe(), the single observer.
    //
    // The RAM announced to the game is a decision made by THIS port: 256 MiB.
    // D2 sizes its sprite cache from dwTotalPhys (reverse-engineered,
    // sub_457300): U = T - 11 MiB, cache = clamp((U-S-8)/3, 24 MiB, 64 MiB),
    // and U >= 64 MiB arms an extra caching path. Announcing 64 MiB would
    // hit the 24 MiB floor and disarm that path (~40 MiB less resident) at
    // the cost of more re-decompression; this stays at the value measured
    // and settled on.
    wx86_mem_set_total_phys_mb(256);
    wx86_mem_set_observer(d2_mem_observe);
    { Wx86MemoryPlan mp; mp.heap=&g_heapA; mp.va=&g_vaA; mp.vquery=vquery_region;
      win32_shims_memory_install(br,mp); }
    kernel32_modules_install(br);
    
    checkrevision_crypto_install(br);
    // KERNEL32 imports the lockdown CheckRevision.dll pulls in that Game.exe's
    // own CRT didn't (correct argc = no ESP drift). EncodePointer/DecodePointer
    // are identity (no per-process pointer obfuscation needed here).
    
    
    
    
    K("GetCommandLineA",0,[](Cpu&c){ return put_cstr(c, getenv("D2ARGS")?getenv("D2ARGS"):"game.exe"); });
    // 36 LOCALE, STRING and CONSOLE shims -> winx86. They used to be spread
    // across FOUR places in this file; they are now registered here, in one
    // spot. This move therefore changes registration ORDER — not the
    // effective table: each of the 36 keys had only one registration site
    // (checked key by key against the shim_seq record), so no winning body
    // changes. See tools/shim_seq.allow. What used to block them — the guest
    // scratch allocator and guest strings — belongs to the engine.
    win32_shims_locale_install(br);
    // TLS slots live at TIB+0xE10 (real Windows layout, per thread — a fresh
    // thread's slots read 0, exactly like Windows). TlsGetValue additionally
    // gets a guest-inlined fs: stub after link (see inline_hot_imports).
    K("TlsAlloc",0,[](Cpu&c){ uint32_t i=g_tlsN++; if(i<64) c.write_u32(cur_tib()+0xE10+4*i,0); return i; });
    K("TlsSetValue",2,[](Cpu&c){ uint32_t i=c.arg(0); if(i>=64) return 0u;
        c.write_u32(cur_tib()+0xE10+4*i,c.arg(1)); return 1u; });
    K("TlsGetValue",1,[](Cpu&c){ uint32_t i=c.arg(0); if(i>=64) return 0u;
        set_lasterr(c,0); return c.read_u32(cur_tib()+0xE10+4*i); });
    
    // Critical sections with REAL semantics (a holder blocks other threads).
    // Build a real CRITICAL_SECTION + RTL_CRITICAL_SECTION_DEBUG in guest memory:
    // Fog validates that DebugInfo!=0 and DebugInfo.CriticalSection points back to
    // the CS (a self-check that the CS was properly initialized).
    auto initCS=[](Cpu&c,uint32_t cs){ KCrit* k=crit_for(cs);
        k->owner=0; k->count=0;                  // re-Init frees a stale lock (see DeleteCriticalSection)
        if(g_sched) g_sched->notify(k);
        uint32_t dbg=misc(0x20);                 // RTL_CRITICAL_SECTION_DEBUG (in MISC, <0x80000000)
        c.write_u32(cs+0x00, dbg);               // DebugInfo
        c.write_u32(cs+0x04, 0xFFFFFFFFu);       // LockCount = -1 (unlocked)
        c.write_u32(cs+0x08, 0);                 // RecursionCount
        c.write_u32(cs+0x0c, 0);                 // OwningThread
        c.write_u32(cs+0x10, 0);                 // LockSemaphore
        c.write_u32(cs+0x14, 0);                 // SpinCount
        c.write_u32(dbg+0x00, 0);                // Type / CreatorBackTraceIndex
        c.write_u32(dbg+0x04, cs);               // CriticalSection -> back-pointer to CS
        c.write_u32(dbg+0x08, dbg+0x08);         // ProcessLocksList.Flink = self
        c.write_u32(dbg+0x0c, dbg+0x08);         // ProcessLocksList.Blink = self
        c.write_u32(dbg+0x10, 0); c.write_u32(dbg+0x14, 0);
    };
    K("InitializeCriticalSection",1,[initCS](Cpu&c){ initCS(c,c.arg(0)); return 0u; });
    K("InitializeCriticalSectionAndSpinCount",2,[initCS](Cpu&c){ initCS(c,c.arg(0)); return 1u; });
    // CRITLOG=<cs-hex>: trace enter/leave of ONE critsec with caller retaddr —
    // finds the Enter-without-Leave leak path (early error return upstream).
    static uint32_t g_critlog = getenv("CRITLOG")?(uint32_t)strtoul(getenv("CRITLOG"),nullptr,16):0;
    auto clog=[](Cpu&c,const char* op,uint32_t cs,KCrit* k){
        if(g_critlog && cs==g_critlog)
            std::fprintf(stderr,"[crit %s] tid=%d ra=0x%08x owner=%u count=%d\n",
                op, g_sched?g_sched->current_id():-1, c.read_u32(c.reg(R_ESP)), k->owner, k->count); };
    // Enter here is the SLOW path only once the guest fast-path stub is live
    // (contended case, or GetProcAddress callers): the stub takes the free /
    // recursive cases entirely in guest code.
    // arg() = ONE R_ESP read (emutls chain) + one read_u32: done ONCE per
    // shim, never two or three times. Same value either way — nothing here
    // moves ESP or the guest stack between the original reads.
    K("EnterCriticalSection",1,[clog](Cpu&c){ const uint32_t cs=c.arg(0);
        KCrit* k=crit_for(cs);
#ifdef PROF_COUNTERS
        prof::cs_enter++;
        if(g_sched && k->count>0 && g_sched->current() && k->owner!=(uint32_t)g_sched->current()->id)
            prof::cs_contended++;   // held by another thread -> this Enter blocks
#endif
        // Pre-scheduler Enter is a NOP (single thread; DllMain-held locks must
        // NOT stay marked owned into the scheduler phase — the historical
        // behaviour the whole boot was validated against).
        if(g_sched) g_sched->wait(k,0xFFFFFFFFu);
        clog(c,"ENTER",cs,k); return 0u; });
    K("TryEnterCriticalSection",1,[clog](Cpu&c){ if(!g_sched) return 1u; const uint32_t cs=c.arg(0);
        KCrit* k=crit_for(cs);
        bool got=k->try_acquire(g_sched->current()); clog(c,got?"TRY+ ":"TRY- ",cs,k); return got?1u:0u; });
    // Core SHARED with the dynarec fast path (see cs_leave_intrinsic): both
    // call wx86_crit_leave, so they can no longer diverge.
    K("LeaveCriticalSection",1,[clog](Cpu&c){ const uint32_t cs=c.arg(0);
        wx86_crit_leave(cs, g_sched);
        if(KCrit* k=wx86_crit_lookup(cs)) clog(c,"LEAVE",cs,k);
        return 0u; });
    // Delete/Initialize must RESET our kernel object: D2CMP's cache shutdown
    // does Enter -> DeleteCriticalSection (no Leave — legal Win32), then the
    // game re-Initializes the SAME address for the in-game cache. A stale
    // owner/count here deadlocks the sprite-decoder thread on game entry.
    K("DeleteCriticalSection",1,[](Cpu&c){ const uint32_t cs=c.arg(0);
        if(KCrit* k=wx86_crit_lookup(cs)){ k->owner=0; k->count=0;
            if(g_sched) g_sched->notify(k); }
        wx86_crit_forget(cs);   // clears the map AND invalidates the MRU cache (engine)
        return 0u; });
    // Threads + events via the scheduler.
    K("CreateThread",6,[](Cpu&c){ if(!g_sched) return 0u; std::fprintf(stderr,"[CreateThread proc=0x%08x]\n",c.arg(2)); GuestThread* t=g_sched->create_thread(c.arg(2),c.arg(3),c.arg(1),(c.arg(4)&0x4u)!=0);
        if(!t) return 0u;   // Win32: NULL en echec (cap natif atteint / pthread_create / teardown)
        if(c.arg(5)) c.write_u32(c.arg(5),wx86_win_tid(t->id));   // GUEST view (multiple of 4)
        KThread* k=new KThread(); k->gt=t; uint32_t h=wx86_handle_add(k); slog(c,"CreateThread proc=0x%08x -> h=0x%08x newtid=%u", c.arg(2), h, t->id); return h; });
    // ExitThread(dwExitCode): the CRT's _endthreadex tail-calls this (Game+0x68803b)
    // to end a guest thread. UNSHIMMED it hit the default shim (argc unknown ->
    // the stdcall arg is left on the stack, +4 ESP drift) AND never actually
    // terminated the coop thread, so the Battle.net net-worker's frame got
    // corrupted and its epilogue `ret` popped 0 -> EIP=0 (the crash blamed on a
    // "garbage network struct pointer": callee-saved regs poisoned by the drifted
    // frame fabricated the fake pointer). Fix: end the current cooperative thread
    // via the scheduler sentinel with EAX=exit code so the tested reap path
    // (state=Finished) runs; never return into the guest.
    K("ExitThread",1,[&br](Cpu&c)->uint32_t{ uint32_t code=c.arg(0);
        slog(c,"ExitThread code=0x%x", code);
        br.redirect_next(br.sentinel()); return code; });
    // TerminateThread(hThread,exitCode): correct argc (was UNSHIMMED -> +8 drift).
    // Mark the target thread Finished so the scheduler stops running it.
    K("TerminateThread",2,[](Cpu&c){ Waitable* itw=wx86_handle_find(c.arg(0));
        if(itw&&is_kind(itw,"thread")){ auto* k=static_cast<KThread*>(itw);
            if(k->gt){ k->gt->exit_code=c.arg(1); k->gt->state=GuestThread::State::Finished; if(g_sched) g_sched->notify(itw); }
            return 1u; } return 0u; });
    // SetFileTime(hFile,pCreate,pAccess,pWrite): the BNFTP CheckRevision download
    // stamps the saved file's timestamps. UNSHIMMED = 4 args left on the stack
    // (+16 ESP drift) mid-net-worker -> same frame corruption. Accept + succeed.
    
    // CompareFileTime(pft1,pft2): Storm calls this to compare a CACHED
    // CheckRevision.mpq's timestamp against the server's filetime (decides
    // download-vs-reuse). UNSHIMMED = 2 args left on the stack -> ESP drift ->
    // the very next file op reads a GARBAGE filename and the whole checkrevision
    // path breaks (the game reports it cannot identify its version). This is why a FIRST
    // run (no cache -> no CompareFileTime) reached checkrevision but later runs
    // did not. Compare the two FILETIMEs as u64 (Win32 contract: -1/0/1).
    K("CompareFileTime",2,[](Cpu&c)->uint32_t{
        uint32_t a=c.arg(0), b=c.arg(1);
        uint64_t t1 = a ? ((uint64_t)c.read_u32(a) | ((uint64_t)c.read_u32(a+4)<<32)) : 0;
        uint64_t t2 = b ? ((uint64_t)c.read_u32(b) | ((uint64_t)c.read_u32(b+4)<<32)) : 0;
        return t1<t2 ? 0xFFFFFFFFu : (t1>t2 ? 1u : 0u); });
    K("ResumeThread",1,[](Cpu&c){ Waitable* itw=wx86_handle_find(c.arg(0)); if(itw&&is_kind(itw,"thread")) g_sched->resume(static_cast<KThread*>(itw)->gt); return 0u; });
    // ---- WAITING -> winx86 -------------------------------------------------
    // WaitForSingleObject / WaitForMultipleObjects / SleepEx / SwitchToThread
    // and the three completion-port shims. The other port carried the SAME
    // body — WaitForMultipleObjects is character-identical, comments
    // included. What differed between the two wasn't semantics but the
    // INSTRUMENTATION wrapped around it; that stays here and plugs into
    // d2_wait_observe(), the single observer.
    //
    // Sleep does NOT move, and this isn't caution: its body starts with
    // shortcuts tied to GAME ADDRESSES (the two packet-loop sites
    // 0x4c715/0x4c744, the Fog site 0xfa67a, the two render-period words).
    // These are literals specific to Diablo II. What IS generic in Sleep —
    // yielding, or sleeping for a delay — is now wx86_wait_sleep(), which
    // this body calls at the end.
    wx86_wait_set_observer(d2_wait_observe);
    win32_shims_wait_install(br);
    
    win32_shims_sync_install(br);   // event/mutex/semaphore -> winx86
    // With ownership of the state (handle table, kernel objects, id counter)
    // already on the engine side, the bodies can follow without leaving a
    // twin behind. THIS port's instrumentation — sync trace, wait log,
    // profiling counters — does NOT move with them: it plugs into
    // d2_sync_observe(), the single observer. WaitForSingleObject,
    // WaitForMultipleObjects, Sleep, CloseHandle, CreateThread, and the
    // snapshot stay here: they touch state this port still owns (file
    // tables, the anti-starvation net, game addresses), which is a reason,
    // not caution.
    
    // --- 1.14 monolith extra KERNEL32 imports (absent from the 1.13c surface) -
    
    
    toolhelp_install(br);
    // I/O completion ports (Fog job system). Unshimmed these hit the default
    // shim with unknown argc -> ESP drift -> the Fog worker died at boot and
    // async jobs (incl. game-server creation) were never executed.
    // Single-instance probe: no other instance exists -> NULL + ERROR_FILE_NOT_FOUND.
    
    
    // Mutex modeled as an auto-reset event: bInitialOwner=FALSE -> available
    // (signaled), TRUE -> owned by creator (unavailable). ReleaseMutex re-signals.
    // (Real recursive/abandoned ownership is a dedicated KMutex, out of scope.)

    
    // The counting semaphore: creation AND release now both live in the
    // engine (win32_shims_sync.cpp). ReleaseSemaphore had stayed here while
    // CreateSemaphore had already moved — the engine knew how to create a
    // semaphore but not how to release it.

    // MemoryBarrier: a REAL barrier. The old body was "return 0" under the
    // "single-host-thread" assumption — already wrong under D2SCHED=native
    // (one host thread per guest thread), and with no nuance across multiple
    // cores. When the game explicitly asks for a barrier, refusing it is a
    // bug. Emits a DMB ISH on ARM; on a single core that's a few cycles, and
    // MemoryBarrier isn't a hot-loop call.
    
    // PulseEvent — a SANCTIONED infidelity: wakes NOBODY, leaves the event
    // unsignaled. Under coop the historical notify() was a no-op (pick_ready
    // re-polled on every switch), so the oracle never observed a wake-up;
    // under native that same notify() WOULD wake waiters and silently turn
    // the accepted infidelity into an accidental one (a coop/native
    // divergence). Hence: no notify.
    
    
    
    
    kernel32_interlocked_install(br);
    
    
    // Tie the tick clock to the scheduler's virtual time so frame-delta timing
    // is consistent with when timed waits actually fire; nudge forward each call.
    // Pre-scheduler +16 preserves the historical CRT/DllMain pacing (the old
    // g_tick+=1 then +=15 path); once the scheduler runs, +1 matches the
    // guest-inlined stub exactly.
    K("GetTickCount",0,[](Cpu&c){ lw_count(g_lwTickN); return tick_bump(c,1); });
    kernel32_time_install(br);
    
    
    
    // D2 SIZES ITS SPRITE CACHE FROM dwTotalPhys (reverse-engineered, sub_457300):
    //   U = T - 11 MiB ; S = clamp(U/10, 3, 5) MiB (audio budget)
    //   sprite_cache = clamp((U - S - 6 - 2)/3, 24 MiB, 64 MiB)
    //   and U >= 64 MiB sets the "big machine" flag (ds:0x89db58) that arms an
    //   EXTRA caching path. T < 22.5 MiB = fatal abort ("not enough physical
    //   memory"). Reporting 256 MiB (the old constant) lands D2 on the MAXIMUM
    //   64 MiB sprite cache + the extra path — on a console whose whole guest
    //   arena is 230 MiB. Declaring 64 MiB instead yields the 24 MiB floor and
    //   disarms the extra path: ~40 MiB less resident, using D2's OWN scaling
    //   (it shipped on 32-128 MiB machines). Trade-off: a smaller cache evicts
    //   more, so sprites get re-decompressed more often. This stays at 256 MiB,
    //   the value measured and settled on; environment-based tuning was removed,
    //   it had never been used in actual play.
    
    
    
    // SetUnhandledExceptionFilter now registered by toolhelp_install() above
    // (it records g_unhandledFilter; nothing ever invokes it — documented gap).

    K("OutputDebugStringA",1,[](Cpu&c){ char buf[256]={0}; c.read(c.arg(0),buf,255); std::printf("    [OutputDebugString] %s\n",buf); return 0u; });
    // Sleep must actually BLOCK the guest thread (timed, virtual clock): a
    // no-op turned Fog's worker idle loop (Sleep(252) between flag polls)
    // into a monopolising spin that also froze virtual time.
    // D2_FOGSLEEP=1: Fog's main loop (0x4fa590, menus/loading) calls Sleep(0)
    // at 0x4fa674 while the window is active, and re-pumps until its EDI
    // credit (ms remaining before the next frame) is consumed: on Windows a
    // turn costs a few us; here each Sleep(0) costs the better part of a
    // millisecond of kernel time + traps, dropping the menu to a handful of
    // fps. When the caller is THIS site and the credit is positive, the
    // credit is slept in one call (capped at 20 ms, Fog's own bound) instead
    // of spinning. EDI is callee-saved across the call, so it's read as-is.
    // No other caller is touched.
    K("Sleep",1,[](Cpu&c){ uint32_t ms=c.arg(0); lw_sleep(c);
        // times the WHOLE shim: this is the time the game loses, whatever the
        // internal path (GAMESLEEP credit, kernel-free yield, real sleep)
        const uint64_t wp_s0 = waitprof_on() ? rt_now_us() : 0;
        struct WpGuard { uint64_t t0; uint32_t ms; ~WpGuard(){ if(t0){
            wp_add(g_wpS, ms, rt_now_us()-t0, g_sched&&g_sched->current()?g_sched->current()->id:0u); } } }
            wp_guard{wp_s0, ms};
        // D2_NETSLEEP=1: the packet-receive loop at 0x44c6e0 does
        //   while ((p = recv())) { process(p); Sleep(0); }
        // — one Sleep(0) PER PACKET (sites 0x4c715 and 0x4c744). On Vita,
        // even with D2_YIELD=1 (a short DelayThread) each call costs a
        // significant fraction of a millisecond of kernel time, which adds
        // up to several ms per frame in the "other" bucket. This yields
        // control WITHOUT a kernel call: it releases the GIL long enough for
        // another thread to take it, which is the Windows semantics of
        // Sleep(0) when nothing else is ready — and on Vita guest threads
        // are preemptive anyway. Limited to this loop's two sites: the rest
        // of the game keeps the real yield (the anti-starvation net showed
        // the cost of a global Sleep(0) with no yielding).
        // D2_GAMESLEEP=1: SLEEP THE REMAINING CREDIT INSTEAD OF POLLING.
        // In-game, loop 0x44efa0 spins 7 to 51 times per DRAWN frame. These
        // turns are not work: the game is waiting for the render period
        // ([0x70ef1c] = 40 ms) to elapse since the last draw ([0x7a0490]). On
        // a PC a turn costs a few us; here it costs a few tenths of a ms
        // (pump, packets, traps), so polling far exceeds the time it was
        // meant to fill. The remaining credit is computed and slept in ONE
        // call, bringing the turns down to 1-2. This NEVER sleeps if the
        // game is running late (credit <= 0), never more than the cap
        // (8 ms), and never more than the credit minus 1 ms: drawing stays
        // triggered by the game itself.
        if(!ms && gamesleep_on() && g_d2base){
            const uint32_t ra0=c.read_u32(c.reg(R_ESP));
            if(ra0==g_d2base+0x4c715u || ra0==g_d2base+0x4c744u){
                const uint32_t per =c.read_u32(g_d2base+0x0030ef1cu);   // render period (ms)
                const uint32_t last=c.read_u32(g_d2base+0x003a0490u);   // tick of the last draw
                const uint32_t now =tick_now(c);
                if(per>=10 && per<=200){
                    const int32_t ecoule=(int32_t)(now-last);
                    const int32_t reste =(int32_t)per-ecoule;
                    if(ecoule>=0 && reste>=3){
                        uint32_t d=(uint32_t)(reste-1); if(d>8) d=8;
                        ++g_gameSleepN; g_gameSleepMs+=d;
                        if(g_sched){ static KEvent* nv2=new KEvent(); nv2->manual=true; nv2->signaled=false;
                            g_sched->wait(nv2,d); }
                        return 0u; } } } }
        if(!ms && netsleep_on() && g_d2base){
            const uint32_t ra=c.read_u32(c.reg(R_ESP));
            if(ra==g_d2base+0x4c715u || ra==g_d2base+0x4c744u){
                // Budget recharged by TIME SLICE (20 ms ~ half a render period)
                // rather than by frame: main()'s frame counter isn't visible
                // here, and the bound must track real time anyway — it's the
                // loop's rate that's being protected.
                static uint64_t slotStart=0; static int budget=0;
                const uint64_t now=rt_now_us();
                if(now-slotStart>=20000ull){ slotStart=now; budget=netsleep_n(); }
                if(budget>0){ --budget; ++g_netSleepN; { d2rt::gil::Release r; } return 0u; }
                ++g_netSleepFull; } }
        if(!ms && fogsleep_on() && g_d2base && c.read_u32(c.reg(R_ESP))==g_d2base+0xfa67au){
            const int32_t credit=(int32_t)c.reg(R_EDI);
            if(credit>0){ ms=(uint32_t)(credit>20?20:credit); ++g_fogSleepN; } }
        wx86_wait_sleep(ms);   // generic core: engine (win32_shims_wait.h)
        return 0u; });
    // file enumeration (init-time probes) — report "no files" cleanly
    // Real directory enumeration against the data root (so install/MPQ scans work).
    K("FindFirstFileA",2,[](Cpu&c){ std::string pat=gread_mb(c,c.arg(0),-1);
        size_t s=pat.find_last_of("\\/"); std::string wild=s==std::string::npos?pat:pat.substr(s+1);
        std::vector<std::string> names;
        for(const std::string& root:{g_dataRoot,g_writeRoot}){ DIR* d=opendir(root.c_str()); if(!d) continue;
            struct dirent* e; while((e=readdir(d))){ std::string fn=e->d_name; if(fn=="."||fn=="..") continue;
                if(wild_match(wild.c_str(),fn.c_str())) names.push_back(fn); }
            closedir(d); }
        if(names.empty()){ set_lasterr(c,2); if(env_filelog()) std::fprintf(stderr,"    [find MISS] %s\n",pat.c_str()); return 0xFFFFFFFFu; }
        uint32_t h=g_nextH++; fill_finddata(c,c.arg(1),names[0],host_filesize(g_dataRoot+"/"+names[0]));
        g_finds[h]={names,1}; return h; });
    K("FindNextFileA",2,[](Cpu&c){ auto it=g_finds.find(c.arg(0)); if(it==g_finds.end()){set_lasterr(c,18);return 0u;}
        auto& fs=it->second; if(fs.second>=fs.first.size()){ set_lasterr(c,18); return 0u; }
        fill_finddata(c,c.arg(1),fs.first[fs.second],host_filesize(g_dataRoot+"/"+fs.first[fs.second])); fs.second++; return 1u; });
    K("FindClose",1,[](Cpu&c){ g_finds.erase(c.arg(0)); return 1u; });
    kernel32_files_install(br);
    
    kernel32_w_variants_install(br);
    
    // EnumProcessModules/K32EnumProcessModules/GetModuleInformation/
    // K32GetModuleInformation -> winx86, generic: they already only read the
    // Bridge's module table (base/size), via Bridge::loaded_modules().
    // GetModuleFileNameEx*/GetModuleFileName* stay here: they resolve the
    // full "C:\Diablo II\..." path, which is specific to d2vita (module_path_for).
    win32_shims_psapi_install(br);
    K("GetModuleFileNameExW",4,[](Cpu&c)->uint32_t{ std::string path=module_path_for(c.arg(1));
        if(path.empty()){ set_lasterr(c,6); return 0u; }
        uint32_t buf=c.arg(2),sz=c.arg(3);
        const char* p=path.c_str(); uint32_t n=(uint32_t)path.size();
        if(buf&&sz){ uint32_t w=n<sz?n:sz-1; for(uint32_t i=0;i<w;i++) gwrite_wc(c,buf+2*i,(uint16_t)(uint8_t)p[i]);
            if(w<sz) gwrite_wc(c,buf+2*w,0); return w; } return 0u; });
    K("GetModuleFileNameExA",4,[](Cpu&c)->uint32_t{ std::string path=module_path_for(c.arg(1));
        if(path.empty()){ set_lasterr(c,6); return 0u; }
        uint32_t b=c.arg(2),n=c.arg(3); if(!b||!n) return 0u; uint32_t len=(uint32_t)path.size(); if(len>=n) len=n-1;
        c.write(b,path.c_str(),len); uint8_t z=0; c.write(b+len,&z,1); return len; });
    
    
    kernel32_fsinfo_install(br);
    // WinVerifyTrust: registered by win32_shims_wintrust_install() (real
    // Authenticode verification, winx86). No more "return 0" here.
    kernel32_filemapping_install(br);
    // locale / codepage. A real Windows client fills SID_AUTH_INFO's locale block
    // (country abbrev/name, language, MPQ locale, user lang id) from these APIs;
    // the old stubs were INCONSISTENT (LangID=French 0x040C but LCID/LocaleInfo
    // =US 0x0409) and returned "1"/"" for the country — a distinct fingerprint vs
    // a real client. One coherent locale, from D2_LCID (default French 0x040C to
    // match the user's frFR install; set 0x0409 for a US-English client) — the LCID
    // is file-scope (below) so GetUserDefaultLangID, declared earlier, shares it.
    // The LCID -> values table, and the six shims that read it, all moved to
    // the engine (win32_shims_locale.cpp). The .ini readers (GetPrivateProfile
    // StringA/IntA, querying a REAL file via ini_lookup()) now live in
    // runtime/win32_import_remainder.cpp, alongside ExitProcess/TerminateProcess/
    // FormatMessageA -- none of the four had a natural home of their own.

    win32_shims_advapi32_d2_install(br);
    win32_shims_advapi32_install(br);

    win32_shims_shell32_d2_install(br);
    win32_shims_shell32_install(br);

    // --- network: every WSOCK32/WS2_32 ordinal is served by winx86 -----------
    // (win32_shims_wsock32_install). D2 policy reaches the socket layer through
    // ONE observer (d2_net_observe: BNCS/D2GS decoding, select census) and ONE
    // route.
    //
    // D2BNCS_LOCAL feeds winx86's generic route (every non-loopback
    // destination is rewritten to this address, port kept as-is).
    //
    // This is a debugging safety net, NOT the normal way to point D2 at a
    // private server: the PC-faithful way is what the game actually uses —
    // its own gateway list in the registry ("Diablo II Battle.net gateways"
    // key, see tools/registry_console.txt). With that list reduced to a single
    // 127.0.0.1 entry and D2BNCS_LOCAL absent, D2 connects to 127.0.0.1:6112 on
    // its own, with no rewrite needed, and the BNFTP handshake proceeds
    // normally. The route now only catches what isn't in that list — in
    // practice the port-7 latency probe, which targets a literal address that
    // must never be allowed to reach Blizzard.
    if(const char* loc=getenv("D2BNCS_LOCAL")){
        uint32_t rip=(std::strchr(loc,'.')? inet_addr(loc):inet_addr("127.0.0.1"));
        if(rip==INADDR_NONE) rip=inet_addr("127.0.0.1");
        wx86_net_set_redirect(rip); }
    netguard_lock_install();
    wx86_net_set_observer(&d2_net_observe);
    wx86_sync_set_observer(&d2_sync_observe);   // only site (see d2_sync_observe)
    win32_shims_wsock32_install(br);

    win32_shims_user32_d2_install(br);
    win32_shims_user32_install(br);
    // Screen size + virtual cursor: D2's values (see the comment near their
    // former g_scrW/g_scrH declaration), set ONCE before any generic winx86
    // shim reads them (GetDeviceCaps, GetClientRect, GetSystemMetrics,
    // GetCursorPos...).
    wx86_set_screen_size(800,600);
    wx86_set_cursor(400,300);
    win32_shims_gdi32_install(br);
    win32_shims_window_install(br);
    // GDI32 basics (window-class brushes, DC caps, gamma).
    auto GD=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("GDI32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))
            s.fn=[fn,tag,trace](Cpu&c){ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("GDI32.dll",name,s);
    };
    // (g_dibBits/W/H/Bpp are file-scope: the image guard reads them)
    static uint8_t  g_palette[256*4]={0}; static bool g_havePalette=false;   // captured RGBQUAD table
    GD("CreateDIBSection",6,[](Cpu&c){ uint32_t pbmi=c.arg(1), pb=c.arg(3);
        int32_t w=(int32_t)c.read_u32(pbmi+4), h=(int32_t)c.read_u32(pbmi+8);
        uint32_t bpp=c.read_u32(pbmi+12)>>16;   // biPlanes:16|biBitCount:16
        uint32_t sz=(uint32_t)((w<0?-w:w)*(h<0?-h:h))*(bpp?bpp:32)/8; if(!sz) sz=800*600*4;
        uint32_t bits=valloc(sz); if(pb) c.write_u32(pb,bits);
        g_dibBits=bits; g_dibW=(uint32_t)(w<0?-w:w); g_dibH=(uint32_t)(h<0?-h:h); g_dibBpp=bpp?bpp:32;
        std::printf("    [gdi] CreateDIBSection %dx%d %ubpp bits=0x%08x\n", w, h, g_dibBpp, bits);
        return 0x88000010u; });
    // Frame capture: on BitBlt (menu blits the DIB to the window DC), dump the
    // DIB pixels — proof of the first rendered screen.
    // FBDUMP=N: dump the first 5 frames, then every Nth (default: first 5 only).
    // Blit count always tracked; reported at scheduler stop.
    static int g_frame=0;
    // D2V_CRASHTEST changes ONLY the capture list (adds &br) — see the
    // matching comment at d2vGlideFlush below (the Glide-ring per-frame
    // tick): with the define absent this is byte-for-byte the original
    // lambda. This GDI path is the one actually reached under "-w" (the
    // rendering mode this whole project's qemu-arm gate and D2ARGS use) —
    // d2vGlideFlush is the Glide-ring equivalent for when D2_GLIDERING is
    // the active backend instead.
#ifdef D2V_CRASHTEST
    auto dumpFrame=[&br](Cpu&c){ if(!g_dibBits) return;
#else
    auto dumpFrame=[](Cpu&c){ if(!g_dibBits) return;
#endif
        FuBody _fuBody;             // D2_FRAMEUS: total per-frame body cost (all exits included)
        sh_frame();                 // frame boundary (D2_LOOPSHAPE)
        ++g_frame;
        cv_tick(g_frame);           // D2_CAMVEC: this frame's camera vector (no-op without the knob)
        // Drive scripted-input injection HERE, co-located with the reliable
        // per-frame body (the same path the D2_AUTOLOGIN state machine below runs
        // on). frameTick()'s separate inj_tick() call — reached only via the
        // GDI BitBlt/StretchBlt shims — stalls on static screens (e.g. the realm
        // lobby "Welcome to the realm") where the game stops issuing those blits,
        // freezing D2SCRIPT even though g_frame keeps advancing. Calling it from
        // dumpFrame makes injection track g_frame unconditionally. Idempotent
        // (g_injIx is monotonic), so the frameTick() call remains a harmless no-op.
        inj_tick(g_frame);
        jp_tick(g_frame);   // D2_JITPROFILE: Rogue boundary + window end (no-op without the knob)
        fp_tick(g_frame);   // D2_FRAMEPROF: per-frame attribution (no-op without the knob)
        lw_frame(c);        // D2_LOOPWATCH: render-thread wait (no-op without the knob)
        pp_frame();         // D2_PHASEPROF: frame boundary (no-op without the knob)
        if(g_nocapFrom && !g_nocapDone && (uint32_t)g_frame>=g_nocapFrom) nocap_apply();
        ep_tick(g_frame);   // D2_EIPPROF_FROM/TO: profiling window (no-op without the knobs)
        { FuScope _s(&g_fuCmd);   cmd_poll(); }      // vision-driven runtime injection ($D2CMDFILE)
        { FuScope _s(&g_fuInput); if(d2vita_input_tick) d2vita_input_tick(); }   // physical controls (Vita)
#ifdef D2V_CRASHTEST
        d2crashtest_tick(c, br, g_frame);
#endif
        // D2_ROOMDUMP_FRAME=<n>: one-shot full room-graph dump at frame n (qemu
        // trigger for the cross-platform map diff; the Vita side uses the
        // Crash.txt trigger since it halts before reaching a stable frame).
        { static uint32_t rdf=getenv("D2_ROOMDUMP_FRAME")?(uint32_t)strtoul(getenv("D2_ROOMDUMP_FRAME"),nullptr,10):0u;
          static bool rdone=false;
          if(rdf && !rdone && (uint32_t)g_frame>=rdf){ rdone=true; dump_ghact_rooms(c,"frame"); } }
        // D2_AUTOBNET: VISUALLY detect the BATTLE.NET button (D2's beige button
        // fill ~rgb(224,196,148) in the y~358 row) and click it ONCE when it's
        // actually on screen — robust vs the real-clock menu timing, and SAFE
        // (never a blind click on an unknown screen). Retries after a cooldown if
        // the click didn't take (g_bnetConnected stops it on the gateway connect).
        if(g_dibBpp==8 && !g_bnetConnected && getenv("D2_AUTOBNET")){
            static int cs=0, ct=0;
            auto beige=[&](int x,int y)->bool{ uint8_t idx=0; c.read(g_dibBits+(uint32_t)y*g_dibW+x,&idx,1);
                uint8_t b=g_palette[idx*4], g=g_palette[idx*4+1], r=g_palette[idx*4+2];
                return r>180 && g>150 && b>110 && r>=g && g>=b; };
            int nb=0; for(int x=300;x<=500;x+=8) for(int y=350;y<=366;y+=3) if(beige(x,y)) nb++;
            bool menuUp = nb>=6;
            // Don't click the very instant the menu paints — D2's background init
            // (and the net stack) isn't fully settled yet, and connecting too early
            // faults the checkrevision worker. Give it a beat (matches the frame
            // where a manual click reliably worked).
            if(cs==0 && menuUp && g_frame>=600){ inj_queue("move",400,358); cs=1; ct=g_frame;
                if(env_filelog()) std::fprintf(stderr,"    [autobnet] BATTLE.NET détecté (beige=%d) frame %d -> clic\n",nb,g_frame); }
            else if(cs==1 && g_frame>=ct+6){ inj_queue("ldown",400,358); cs=2; }
            else if(cs==2 && g_frame>=ct+12){ inj_queue("lup",400,358); cs=3; ct=g_frame; }
            else if(cs==3 && g_frame>=ct+90){ cs=0; }   // retry if not yet connected
        }
        // D2_AUTOLOGIN: after the Battle.net connect the login form appears
        // (account + password fields + CONNEXION). Auto-fill it: click the account
        // field, type the account, click the password field, type it, click
        // CONNEXION. UI automation ONLY — D2 itself builds & sends the real
        // SID_LOGONRESPONSE2 (no protocol forging). Coords from the 800x600 login
        // screen; account/password can be anything (local server accepts all).
        // D2_ACCOUNT / D2_PASSWORD override the defaults.
        if(g_dibBpp==8 && g_bnetConnected && getenv("D2_AUTOLOGIN")){
            static int ls=0, lt=0, li=0, tries=0;
            static std::string acct = getenv("D2_ACCOUNT")?getenv("D2_ACCOUNT"):"";
            static std::string pass = getenv("D2_PASSWORD")?getenv("D2_PASSWORD"):"";
            if(acct.empty()||pass.empty()) return;   // credentials come from env/env.txt only — never baked in source
            if(ls==0){ lt=g_frame; ls=1; }                                                             // arm at connect frame
            else if(ls==1 && g_frame>=lt+50){ inj_queue("move",400,333); inj_queue("ldown",400,333); inj_queue("lup",400,333); ls=2; lt=g_frame; li=0; }   // focus account
            else if(ls==2 && g_frame>=lt+3){ if(li<(int)acct.size()){ inj_queue("chr",(unsigned char)acct[li],0); li++; lt=g_frame; } else { ls=3; lt=g_frame; } }  // type account
            else if(ls==3 && g_frame>=lt+8){ inj_queue("move",400,388); inj_queue("ldown",400,388); inj_queue("lup",400,388); ls=4; lt=g_frame; li=0; }   // focus password
            else if(ls==4 && g_frame>=lt+3){ if(li<(int)pass.size()){ inj_queue("chr",(unsigned char)pass[li],0); li++; lt=g_frame; } else { ls=5; lt=g_frame; } }  // type password
            else if(ls==5 && g_frame>=lt+8){ inj_queue("move",400,468); inj_queue("ldown",400,468); inj_queue("lup",400,468); ls=6; lt=g_frame;             // click CONNEXION
                if(env_filelog()) std::fprintf(stderr,"    [autologin] submitted account='%s' frame %d (try %d)\n",acct.c_str(),g_frame,tries+1); }
            else if(ls==6 && g_frame>=lt+150){ if(++tries<3){ ls=1; lt=g_frame; } else ls=99; }        // bounded retry, then stop
        }
        // D2_CELWATCH=<N frames>: CelData cache usage and cap, published in
        // the periodic status line. On console printf reaches no log —
        // d2vita_progress is the only readable channel; under qemu it's the
        // opposite, hence both. The cache is HARDCODED capped at 512000
        // bytes (0x4573fe): it's the only one of the game's three caches
        // that can saturate and force sprite re-decoding, and it had never
        // been measured. Cost: two guest memory reads per frame.
        if(g_celWatch && g_cpuForStats && g_d2base){
            uint32_t use=g_cpuForStats->read_u32(g_d2base+0x48db28);
            uint32_t cap=g_cpuForStats->read_u32(g_d2base+0x48db24);
            // D2_CELCAP=<bytes>: RESETS the cap, once, after sub_5ffd40 has
            // built the cache. It's .data (ds:0x88db24), NOT .text — and the
            // cap is only a byte accounting limit: sub_609020 only allocates
            // the bucket table, sized from ds:0x741ef8, independent of the
            // cap. Also serves as a SCALE TEST for the usage counter: cap
            // divided => evictions.
            if(g_celCap && cap && cap!=g_celCap){
                g_cpuForStats->write_u32(g_d2base+0x48db24,g_celCap);
                char m[128]; std::snprintf(m,sizeof m,"celdata: plafond %uK -> %uK (ds:0x88db24, .text intact)",cap>>10,g_celCap>>10);
                d2vita_progress(m); std::printf("  %s\n",m); cap=g_celCap; }
            if(use>g_celPeak) g_celPeak=use;
            if(cap && g_frame%(int)g_celWatch==0){
                char m[192]; std::snprintf(m,sizeof m,
                    "[CELDATA] image=%d usage=%uK/%uK (%u%%) pic=%uK evict=%llu (autres=%llu)",
                    g_frame,use>>10,cap>>10,(unsigned)((uint64_t)use*100/cap),g_celPeak>>10,
                    (unsigned long long)g_celEvict,(unsigned long long)g_celEvictOther);
                d2vita_progress(m); std::printf("  %s\n",m); } }
        if(g_realclock && g_frame%100==0 && g_schedCoop)
            std::printf("  [frame] %d  t=%llums sw=%llu wake sig=%llu to=%llu pm=%llu\n", g_frame,
                (unsigned long long)rt_now_ms(),(unsigned long long)g_schedCoop->switches(),
                (unsigned long long)g_schedCoop->wake_signal_,(unsigned long long)g_schedCoop->wake_timeout_,
                (unsigned long long)g_pmN);
#ifdef PROF_COUNTERS
        prof_phase_tick(g_frame);
#endif
        // Vita: present every frame to SceDisplay (D2VITA_PRESENT). Read the DIB
        // once here and reuse the buffer for both present and the optional dump.
        static std::vector<uint8_t> px;
        uint32_t sz=g_dibW*g_dibH*(g_dibBpp/8);
        static const bool present = getenv("D2VITA_PRESENT")!=nullptr;   // set before frame 1 (baked/env.txt)
        static const char* fb=getenv("FBDUMP");
        // D2_FBWIN=a:b — dumps EVERY frame in window [a,b]. FBDUMP=N can't do
        // CONSECUTIVE frames (it falls back to "the first 5" whenever the
        // step is < 2); reprojection needs n and n+1.
        static const char* fbw=getenv("D2_FBWIN");
        static int fbwA=0, fbwB=-1;
        static bool fbwInit=false;
        if(!fbwInit){ fbwInit=true; if(fbw) std::sscanf(fbw,"%d:%d",&fbwA,&fbwB); }
        const bool inWin = (fbw && g_frame>=fbwA && g_frame<=fbwB);
        bool snap=g_snap; if(snap) g_snap=false;
        // FBHASH=N: framebuffer fingerprint every Nth frame, folded into a
        // running value printed at exit. This is the pixel-exactness oracle
        // for an A/B: two branches that display the SAME image over the
        // whole run produce the same fingerprint, without depending on /tmp
        // dumps (a path shared between parallel runs) or a file comparison.
        static const uint32_t fbh = getenv("FBHASH")?(uint32_t)strtoul(getenv("FBHASH"),nullptr,10):0u;
        bool wantHash = fbh && (g_frame % (int)fbh)==0;
        bool wantDump = fb || snap || inWin;
        static const bool wantWatch = getenv("D2_DIBWATCH")!=nullptr;   // see g_dwFrames
        if(!present && !wantDump && !wantHash && !wantWatch) return;
        // Perf: hand the host view straight to the present layer (it memcpys
        // into its own staging slot) — saves a 300-480 KB c.read every frame.
        const uint8_t* pd;
        if(const void* hp=c.hostptr(g_dibBits,sz)) pd=(const uint8_t*)hp;
        else { px.resize(sz); c.read(g_dibBits,px.data(),sz); pd=px.data(); }
        // D2_DIBWATCH: comparison BEFORE any presentation (the snapshot is
        // from the previous blit, so whatever differs was written by the
        // game between the two blits).
        if(wantWatch){ static std::vector<uint8_t> prev; static uint32_t prevSz=0;
            if(prevSz==sz && sz){ uint32_t diff=0;
                for(uint32_t i=0;i<sz;i++) if(prev[i]!=pd[i]) ++diff;
                ++g_dwFrames; g_dwBytes+=diff; g_dwTotal+=sz;
                if(diff){ ++g_dwChanged;
                    uint32_t pm=(uint32_t)((uint64_t)diff*1000ull/sz);
                    if(pm>g_dwMaxPct) g_dwMaxPct=pm;
                    if(pm<g_dwMinPct) g_dwMinPct=pm; } }
            prev.resize(sz); std::memcpy(prev.data(),pd,sz); prevSz=sz; }
        if(present){ FuScope _s(&g_fuPresent);
            d2vita_present(pd,(int)g_dibW,(int)g_dibH,(int)g_dibBpp, g_havePalette?g_palette:nullptr); }
        if(wantHash){ uint64_t h=g_fbHash;                       // FNV-1a 64, whole frame
            for(uint32_t i=0;i<sz;i++){ h^=pd[i]; h*=1099511628211ull; }
            h^=(uint64_t)g_frame; h*=1099511628211ull;           // the frame number counts too
            g_fbHash=h; ++g_fbHashN; }
        if(!wantDump) return;
        int every=fb?atoi(fb):0;
        if(!snap && !inWin && g_frame>5 && (every<2 || g_frame%every)) return;
        // Captures used to hardcode /tmp — which does NOT exist on Vita: on
        // console the feature was therefore dead, precisely where seeing the
        // screen matters most. Now written into the game's write directory,
        // which exists everywhere.
        const char* froot = g_writeRoot.empty() ? "/tmp" : g_writeRoot.c_str();
        char fn[160]; std::snprintf(fn,sizeof fn,"%s/d2_frame_%d_%ux%ux%u.raw",froot,g_frame,g_dibW,g_dibH,g_dibBpp);
        FILE* f=std::fopen(fn,"wb"); if(f){ std::fwrite(pd,1,sz,f); std::fclose(f); std::printf("    [gdi] FRAME %d DUMPED -> %s\n",g_frame,fn); std::fflush(stdout); }
        // Palette AS OF THIS FRAME, paired with the dump (menu vs act palettes
        // differ; a single latest-palette file miscolors earlier frames).
        if(g_havePalette){
            char pn[160]; std::snprintf(pn,sizeof pn,"%s/d2_frame_%d_%ux%ux%u.pal",froot,g_frame,g_dibW,g_dibH,g_dibBpp);
            FILE* pf=std::fopen(pn,"wb"); if(pf){ std::fwrite(g_palette,1,256*4,pf); std::fclose(pf); }
            { char qn[160]; std::snprintf(qn,sizeof qn,"%s/d2_palette.raw",froot); pf=std::fopen(qn,"wb"); } if(pf){ std::fwrite(g_palette,1,256*4,pf); std::fclose(pf); } }
        // Capturing CREATES entries in the write directory, which is what
        // bloats the path-cache index over time. A single invalidation after
        // .raw + .pal keeps the index in sync without changing dump logic —
        // only the index learns the directory has changed.
        pc_dirty(g_writeRoot); };
    static int g_maxFrames = getenv("MAXFRAMES")?atoi(getenv("MAXFRAMES")):0;
    static int g_traceAfter = getenv("TRACEAFTER")?atoi(getenv("TRACEAFTER")):0;
    auto frameTick=[](Cpu& c){ inj_tick(g_frame);
        // THE HOST AUDIO SINK IS PULLED HERE. On every presented frame,
        // produce exactly the frames for the GUEST timeline elapsed so far:
        // zero threads, zero pinning, zero GIL release, and a WAV duration
        // paced on GAME time even when qemu runs at a tenth of real time.
        // No-op without D2_SON and under a self-paced sink (console).
        d2rt::dsound::frame_pump(c);
        if(g_traceAfter && !g_traceOn && g_frame>=g_traceAfter){ g_traceOn=true;
            std::printf("  [trace ON at frame %d]\n",g_frame); std::fflush(stdout); }
        if(g_maxFrames && g_frame>=g_maxFrames && g_sched) g_sched->request_shutdown(); };
    // The Fog purge is triggered HERE: at the frame boundary, outside of any
    // in-flight Storm allocation (0x409b80 takes the pool locks — triggering
    // this from the VirtualAlloc shim would self-deadlock). `ra` is the
    // shim's return address: the guest stub resumes there once the purge is
    // done.
    GD("BitBlt",9,[dumpFrame,frameTick](Cpu&c){ uint32_t ra=c.read_u32(c.reg(R_ESP));
        pp_flush_begin(); dumpFrame(c); pp_flush_end(); frameTick(c); return 1u; });   // D2_PHASEPROF: flush = presentation body
    GD("StretchBlt",11,[dumpFrame,frameTick](Cpu&c){ uint32_t ra=c.read_u32(c.reg(R_ESP));
        pp_flush_begin(); dumpFrame(c); pp_flush_end(); frameTick(c); return 1u; });
    GD("TextOutA",5,[](Cpu&c){ if(env_filelog()) std::fprintf(stderr,"    [text] '%s'\n",gread_mb(c,c.arg(3),(int)c.arg(4)).c_str()); return 1u; });
    GD("SetDIBColorTable",4,[](Cpu&c){ uint32_t st=c.arg(1),n=c.arg(2),p=c.arg(3);
        // Windows contract: entries land at uStartIndex. D2 updates partial
        // ranges (act palettes, gamma flashes); writing them at 0 shifted the
        // whole in-game palette (blue/gray ground). File writes stay OUT of
        // this path (369 us/call profile artifact) — dumpFrame persists the
        // palette lazily, per dumped frame.
        if(p&&n&&st<256){ uint32_t cnt=n; if(st+cnt>256) cnt=256-st;
            c.read(p,g_palette+st*4,cnt*4); g_havePalette=true;
            return cnt; }
        return 0u; });

    // DSOUND — THE COM SCAFFOLDING, AND THE DEFAULT THAT NEVER MOVES.
    // Game.exe imports only TWO ordinals from DSOUND.dll: everything else
    // goes through COM vtables, which therefore have to be FABRICATED
    // (src/runtime/ds_emul.cpp). install() UNCONDITIONALLY registers all 32
    // methods plus the two exports and FORCES their slot allocation —
    // alloc_trap advances by 16 bytes per slot, so a registration conditioned
    // on the knob would make an A/B differ by more than just the knob
    // (bridge.h:60-66). Allocating a slot touches NO guest byte regardless of
    // the knob's value. D2_SON is armed by default; with D2_SON=0,
    // DirectSoundCreate returns DSERR_NODRIVER (0x88780078): no thread, no
    // library, no vtable set up.
    // The DirectSound scaffolding itself lives in the engine now — that's
    // Win32 emulation, not Diablo. What stays here is only what's genuinely
    // ours: the arena the PCM buffers live in, the GUEST clock, the write
    // directory, our log, and the Storm codec counters (a Blizzard format,
    // which has no place in a generic engine).
    { d2rt::dsound::set_logger([](const char* l){ jpline("%s", l); });
      d2rt::dsound::set_extra_stat([](char* out, unsigned n)->int{
          int r = std::snprintf(out, n, " codec=h%llu/a%llu/repli%llu",
                                d2rt_audio_codec_n[0],
                                d2rt_audio_codec_n[1] + d2rt_audio_codec_n[2],
                                d2rt_audio_codec_n[3]);
          return (r < 0 || (unsigned)r >= n) ? 0 : r; });
      // WHAT THE PLAYER WILL SEE in Options > Sound, logged ONCE, at the point
      // the cause is established. The engine reports the generic fact (no 3D
      // hardware buffers); only this code can name D2's own menu entries.
      //
      // Verified by disassembling Game.exe 1.14d: the SoundOptions page has
      // SEVEN entries, each with an "enabled" predicate at +0x110 (paint
      // 0x47e4c2: predicate ? color 5 : color 1; hover 0x47d520: -1 if
      // false). The THREE that depend on dwMaxHw3DAllBuffers are 3D Sound
      // (0x4df940, [0x88176c]), EAX / environmental sound (0x4df960,
      // [0x881770]) and 3D Bias (0x4df980, Sound Mixer in {1,2}). The other
      // four — effects volume, music volume, NPC speech, Previous — depend
      // only on [0x7c8c78] ("sound is initialized"), which becomes 1 as soon
      // as InitDS's 2D bar passes. If one of THOSE is greyed out on console,
      // that's a NEW symptom.
      d2rt::dsound::set_caps_observer([](){
          jpline("[son] Options>Son : 3D Sound, Son environnemental et Biais 3D seront GRISES"
                 " (normal, comme un PC sans tampons 3D materiels) ; volume effets, volume"
                 " musique, parole PNJ et Precedent doivent rester ACTIFS."); });
      d2rt::dsound::HostOps ops;
      ops.va_alloc = &d2_valloc;     // GUEST PCM buffers: the VA arena, never scratch
      ops.tick_ms= &tick_now;      // GUEST clock (keeps the WAV paced to game time)
      ops.write_root = g_writeRoot.c_str();
      // Every D2 1.14d WAV is 22050 Hz (exhaustively true across the audio
      // MPQs' files — no other rate exists), and the Vita's BGM sink accepts
      // 22050: no resampling on the default path. This constant is specific
      // to THIS game, not a generic engine default, which is why it's set
      // here rather than hardcoded in the engine's mixer.
      ops.mix_rate = 22050;
      d2rt::dsound::install(br,*cpu,ops); }

    // Real window semantics (RegisterClassA/CreateWindowExA/Peek-Get-DispatchMessageA/
    // GetKeyState family) now registered by win32_shims_user32_d2_install() above.
    // (U() itself moved out too: the "USER32: real clipboard + remainder" part
    // of FULL IMPORT COVERAGE that used to be its only remaining consumer here
    // now lives in runtime/win32_import_remainder.cpp, with its own local U().)

    // ================== FULL IMPORT COVERAGE ==================================
    // Every unshimmed Game.exe import is a latent controlled-halt (e.g.
    // USER32!CopyRect at camp exit). This whole block is faithful Win32
    // implementations or clean failures — never a bypass.
    {
      auto REG=[&](const char* dll,const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
          Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string(dll)+"!"+name; s.fn=std::move(fn);
          br.register_shim(dll,name,s); };
      auto REGORD=[&](const char* dll,uint32_t ord,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
          Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string(dll)+"!#"+std::to_string(ord); s.fn=std::move(fn);
          br.register_shim_ordinal(dll,ord,s); };

      win32_import_remainder_install(br);
      // GDI32: remainder -> win32_shims_gdi32_install() (winx86, generic:
      // stateless fake handles).

      // IMM32/DDRAW/Bink/Smacker/ijl11 -> win32_shims_misc_install() (winx86,
      // generic); the full comment on the DirectDraw "-w" guard
      // (0x405cb3..0x405cea, D2ARGS/vita_present.cpp) now lives in that
      // winx86 file.
      win32_shims_misc_install(br);

      // ================== glide3x.dll — no shims here ==========================
      // D2_GLIDERING (the default) loads the real x86 glide3x.dll from disk
      // (build-glide/, MinGW); GetProcAddress then resolves "_grXxx" against
      // that real module's export table and never falls back to a shim. All
      // actual GPU traversal happens exclusively through the d2vhost.dll ring
      // just below.

      // ============ d2vhost.dll — THE ONLY THREE RING CROSSINGS ================
      // Our x86 glide3x.dll (src/glide_ring/) resolves them via
      // LoadLibraryA("d2vhost.dll") + GetProcAddress. They replace the ~970
      // Glide crossings per frame: one init per game session, one flush per
      // frame, one call per real texture upload (a handful per frame). The
      // game doesn't know this DLL exists; without D2_GLIDERING it is never
      // loaded and this block is never reached.
      REG("d2vhost.dll","d2vGlideInit",2,[](Cpu&c)->uint32_t{
          const uint32_t hdr=c.arg(0), cfg=c.arg(1);
          const uint32_t magic=c.read_u32(hdr), ver=c.read_u32(hdr+4);
          char m[220];
          if(magic!=D2GR_MAGIC_H){
              std::snprintf(m,sizeof m,"ring: REFUS — magie 0x%08x a 0x%08x (attendu 0x%08x)",magic,hdr,D2GR_MAGIC_H);
              d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout); return 0u; }
          uint32_t grDataVA=0, grSize=0; gr_ring_arm(c,hdr,&grDataVA,&grSize);
          // Configuration answers read by the guest DLL: announced TMU memory
          // (D2_GLTEXMEM) and gamma table (fields identified by disassembly
          // at 0x5090f5..0x509144).
          uint32_t texmax=0x400000u;
          if(const char* e=getenv("D2_GLTEXMEM")){ unsigned mo=atoi(e); if(mo>=1&&mo<=1024) texmax=mo<<20; }
          c.write_u32(cfg+0,0u); c.write_u32(cfg+4,texmax);
          c.write_u32(cfg+8,256u); c.write_u32(cfg+12,8u);
          { uint32_t bench=0; if(const char* e=getenv("D2_GLIDEBENCH")){ long v=atol(e); if(v>0&&v<100000000L) bench=(uint32_t)v; }
            c.write_u32(cfg+16,bench); }
          // D2_GRDEDUP (default 1): the DLL skips writing a ring entry whose
          // state is identical to the last one recorded for that function
          // (per-function cache, invalidated every frame and on every upload —
          // glide3x_ring.c). 0 = old behavior (every call goes through the
          // ring). An older DLL ignores this field.
          const bool dedup = !(getenv("D2_GRDEDUP") && !strcmp(getenv("D2_GRDEDUP"),"0"));
          c.write_u32(cfg+20,dedup?1u:0u);
          std::snprintf(m,sizeof m,"ring: ARME — en-tete 0x%08x donnees 0x%08x taille %u Ko, TMU annoncee %u Mio (v%u) dedup-DLL=%s",
                        hdr,grDataVA,grSize>>10,texmax>>20,ver,dedup?"oui":"NON (D2_GRDEDUP=0)");
          d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
          return 1u; });
      // D2V_CRASHTEST changes ONLY the capture list (adds &br, needed to
      // reach d2crashtest_tick's br.redirect_next) and inserts one call at
      // the end of the body: with the define absent, this is BYTE-FOR-BYTE
      // the original lambda — zero .text, zero behavior change.
#ifdef D2V_CRASHTEST
      REG("d2vhost.dll","d2vGlideFlush",2,[frameTick,&br](Cpu&c)->uint32_t{
#else
      REG("d2vhost.dll","d2vGlideFlush",2,[frameTick](Cpu&c)->uint32_t{
#endif
          gr_flush(c,c.arg(0),pp_flush_begin);   // D2_GRPROF benchmark, flush timing, pp_flush_begin (D2_PHASEPROF), gr_replay, tail/stalls/dropped
          pp_flush_end();
          online_cap_wait(online_cap_period_us());   // D2_ONLINE_CAP: see the comment at its definition site
          ++g_frame; lw_frame(c); pp_frame(); fp_tick(g_frame); ep_tick(g_frame); cmd_poll(); frameTick(c);   // lw_frame: D2_LOOPWATCH also sees the ring path
#ifdef D2V_CRASHTEST
          d2crashtest_tick(c, br, g_frame);
#endif
          return 1u; });
      // Crossing benchmark: n no-op calls, timestamped from the 1st to the last.
      REG("d2vhost.dll","d2vGlideNop",1,[](Cpu&c)->uint32_t{
          static uint64_t t0=0,n=0;
          const uint32_t a=c.arg(0);
          if(a&0x80000000u){                       // end-of-loop marker WITHOUT a crossing
              const uint64_t t2=rt_now_us();
              char m[200];
              std::snprintf(m,sizeof m,"traversee: %llu appels en %llu us => %.3f us/traversee"
                            " | meme boucle sans traversee: %llu us",
                            (unsigned long long)n,(unsigned long long)(g_gbT1-t0),
                            n? (double)(g_gbT1-t0)/(double)n : 0.0,
                            (unsigned long long)(t2-g_gbT1));
              d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
              t0=0; n=0; return 0u; }
          if(!n) t0=rt_now_us();
          ++n; g_gbT1=rt_now_us();
          return 0u; });
      REG("d2vhost.dll","d2vGlideTexUpload",4,[](Cpu&c)->uint32_t{
          const uint64_t nb=gl_texbytes(c,c.arg(3));
          gr_tex_upload_count(nb);
          if(grgx_on()) gx_tex_upload(c,c.arg(1),c.arg(3));
          // The HOST-side texture cache hooks in here: hash the content,
          // return a stable handle, only recopy if the fingerprint is new.
          // Under qemu we settle for measuring only (D2_GLTEXHASH).
          return 1u; });


      // Bink/Smacker/ijl11: see win32_shims_misc_install() above (BinkOpen/
      // SmackOpen NULL = "video unavailable"; D2 handles this failure and
      // skips the cutscene, no corruption, no hang).
    }

    // Default shim: log the first unshimmed OS import and force a CONTROLLED
    // STOP. We can't know an unknown stdcall's argc, so the caller's frame is
    // already drifted the instant we're here — but we must never let the guest
    // execute a single instruction on that drifted ESP (that turns a nameable
    // "missing import X" into an unrelated C0000005 at a misleading EIP later).
    // redirect_next(sentinel) makes the very next fetch end the run cleanly at
    // the sentinel; request_shutdown() makes the scheduler loop exit instead of
    // rescheduling. Net: honest "arret controle" that NAMES the missing import.
    br.set_default_shim([&](Cpu&, const std::string& tag)->uint32_t{
        if(!g_stop){ g_stop=true; g_stopReason="UNSHIMMED "+tag; }
        static std::set<std::string> seen;
        if(seen.insert(tag).second){
            std::fprintf(stderr,"[UNSHIMMED] %s -> CONTROLLED STOP (unknown argc; refusing to run on a drifted frame)\n",tag.c_str());
            char m[160]; std::snprintf(m,sizeof m,"UNSHIMMED %s -> arret controle (import manquant)",tag.c_str());
            d2vita_progress(m);   // on console, stderr doesn't exist: the reason MUST reach boot_progress
        }
        if(g_sched) g_sched->request_shutdown();
        br.redirect_next(br.sentinel());
        return 0;
    });

    if(!br.link(err)){ std::printf("link: %s\n",err.c_str()); return 1; }
    std::printf("linked: %u imports still unresolved (routed to default shim)\n", br.unresolved_count());

    tier1_clock_cs_intrinsics_install(cpu, br);

    // POINT-9 torture-test drive: run the arbitrary EXE's entry through the SAME
    // scheduler as Game.exe, then return — bypassing every D2-specific hook below
    // (inline-hot patches, native blit, CheckRevision). Imports resolve via the
    // same K()/U() table; a missing one trips the default-shim CONTROLLED STOP.
    if(g_runexe){
        PeImage* g=br.module(exeLogical);
        if(!g){ std::printf("D2_RUNEXE not loaded\n"); return 1; }
        std::printf("=== D2_RUNEXE drive: %s entry @0x%08x ===\n", exeLogical.c_str(), g->entry_va());
        g_curBase=g->load_base();                         // GetModuleHandle(NULL) = this exe
        ThreadScheduler* schedp = make_scheduler(cpu,&br, SCHED_STACKS, 0x100000, SCHED_TIBS);
        ThreadScheduler& sched = *schedp;
        sched.set_fault_dispatcher([cpu](GuestThread* t,uint32_t code,uint32_t){ return fault_note_seh_chain(*cpu, t, code); });
        GuestThread* mainT=sched.set_main(g->entry_va(), MAIN_STACK_TOP, MAIN_TIB);
        if(g_schedNative){
            // T10 pre-arming: native IGNORES set_time_sink (no virtual tick)
            // — without the real-clock switch, GetTickCount would serve the
            // CALL counter (tick_bump per trap) and any torture CHECK
            // measuring a duration would diverge from the golden result for a
            // reason unrelated to the backend. g_realclock routes
            // tick_now/Sleep onto tick_real; offset = continuity with the
            // current virtual tick (rt_now_ms unscaled: that's what
            // tick_real consumes).
            g_tickOfs = tick_now(*cpu) - (uint32_t)rt_now_ms();
            g_realclock = true;
        }
        { static Cpu* TC=cpu; sched.set_time_sink([](uint64_t ms){
              TC->write_u32(g_tickVA+4, 0x10000+(uint32_t)ms); }); }   // virtual clock (monotone; native: ignored)
        sched.run();
        std::printf("=== D2_RUNEXE stopped: %s  main exit=0x%08x ===\n", sched.stop_reason(), mainT->exit_code);
        d2_crashlog("D2_RUNEXE stopped: %s main-exit=0x%08x", sched.stop_reason(), mainT->exit_code);
        pc_report(true);   // forced summary of the directory index
        signtag_dump();    // D2_SIGNTAG: "complemented pointer" patterns seen / rewritten
        mmu_dump();        // fastmmu: folded absolutes / chained stack links
        d2_crashlog("CLEAN EXIT (runexe)");
        return 0;
    }


    // ---- D2_LOOPWATCH (no simulation) / D2_PHASEPROF / D2_ONEDRAW / D2_PHASEPROF_X
    // and D2_REPLAY60 / D2_RINGTAG / D2_RINGPHASE_X: phase and ring-tagging
    // hooks, extracted into src/runtime/phase_hooks.cpp (logic unchanged).
    phase_hooks_install(cpu,br);
    ringtag_hooks_install(cpu,br);
    // ---- keys.txt: CD keys in cleartext, encoded in the installer's format
    // and substituted into the buffer read from the MPQ (src/runtime/cdkeys_file.cpp).
    cdkeys_hooks_install(cpu,br);

    // ---- D2_NOCAP: lift the monolith's frame-rate caps ---------------------
    // Game.exe 1.14d, ImageBase 0x400000. The game has THREE 25 Hz clocks.
    // They must be told apart, or you think you're lifting a cap and you're
    // actually speeding up the game.
    //
    // (1) OUT OF GAME (menus, loading, cutscenes) — Fog's main loop at
    //     0x4fa590, 27 call sites, a "one frame" callback argument. Credit in
    //     milliseconds in EDI:
    //
    //       4fa606  call GetTickCount     ; elapsed = now - previous
    //       4fa60e  sub  esi,ebx          ;   (capped at 1000 ms)
    //       4fa625  sub  edi,esi          ; credit -= elapsed
    //       4fa627  test edi,edi
    //       4fa629  jg   4fa650           ; credit>0: no frame, pump again
    //       4fa62b  add  edi,0x28         ; <<< 40 ms = 1000/25 = 25 fps
    //       4fa638  call [ebp+8]          ; the "one frame" callback
    //       4fa64b  call 4f98e0           ; goes from D2Win drawing -> StretchBlt
    //       4fa674  call Sleep            ; 0 while the window is active
    //
    // (2) IN GAME — game loop at 0x44ef.., the render period is a DATA value:
    //
    //       44f0b8  mov ecx,[0x70ef1c]    ; <<< render period = 40 ms
    //       44f0c0  sub esi,[0x7a0490]    ; esi = elapsed since last draw
    //       44f0c6  cmp esi,ecx
    //       44f0c8  jb  44f12b            ; too soon: NO draw this pass
    //       44f0f3  mov [0x7a0704],edi(0) ; late enough: draw is allowed
    //       ...
    //       44f278  cmp [0x7a0704],0
    //       44f27e  jne 44f2a4            ; skip the draw
    //       44f28b  call [0x7a0484]       ; THE DRAW
    //
    //     [0x70ef1c] holds 0x28 in initialized data and is never WRITTEN (4
    //     references in the binary, all reads, all in this function).
    //
    // (3) THE SIMULATION — gated by 0x52fc20 (timeGetTime), period
    //     [0x883d60] = 1000/[0x731014], and [0x731014] = 0x19 = 25. It calls
    //     the game-step function 0x52fd90. THIS is the game's actual speed,
    //     and D2_NOCAP DOES NOT TOUCH IT. D2_LOOPWATCH's "sim=" counter
    //     verifies this at runtime.
    //
    // Under caps (1) and (2), every millisecond saved by an optimization
    // turns into waiting and does NOT show up in frames per second.
    //
    // MECHANISM: one .text byte for (1), one .data dword for (2).
    // D2_NOCAP=1 => one frame every 1 ms (nominal cap 1000 fps, i.e. no
    // reachable cap); D2_NOCAP=<n>, 1<=n<=40, gives an intermediate cap
    // (D2_NOCAP=20 => 50 fps). ABSENT => not a single byte touched.
    // By default the write happens BEFORE the dynarec has translated
    // anything. With D2_NOCAP_FROM=<frame> it happens at the requested frame
    // boundary, via Cpu::write, which lifts the translated block's
    // protection and marks it for re-verification (the same barrier as for
    // a guest self-modifying write): the translation is redone next pass.
    //
    // WHAT THIS BREAKS: these are modified game bytes — the ONLY thing in the
    // tree that writes any, and only when this knob is explicitly set. It is
    // therefore incompatible with any memory-integrity check an anti-cheat
    // could run, and must never be armed for online play. Absent, the guest
    // image stays byte-identical to disk (D2_PRISTINE_CHECK verifies it).
    if(getenv("D2_NOCAP") && g_114 && g_d2base){
        unsigned ms=(unsigned)strtoul(getenv("D2_NOCAP"),nullptr,0);
        if(ms<1u||ms>40u) ms=1u;
        g_nocapCpu=cpu; g_nocapVal=(uint8_t)ms;
        g_nocapFog=g_d2base+0x000fa62du;    // .text: the Fog loop's immediate
        g_nocapJeu=g_d2base+0x0030ef1cu;    // .data: in-game render period
        g_nocapGate=g_d2base+0x0004f27eu;   // .text: the in-game draw-lock jump
        g_nocapFrom=getenv("D2_NOCAP_FROM")?(uint32_t)strtoul(getenv("D2_NOCAP_FROM"),nullptr,10):0u;
        if(!g_nocapFrom) nocap_apply();
        else { char m[160]; std::snprintf(m,sizeof m,"nocap: arme a l'image %u (%u ms/image)",g_nocapFrom,ms);
               d2vita_progress(m); std::printf("%s\n",m); }
    }

    native_hooks_codec_install_celwatch(cpu,br);

    native_hooks_cellengine_install_blit(cpu,br);

    native_hooks_codec_install_codecs(cpu,br);

    native_hooks_cellengine_install_rest(cpu,br);

    native_hooks_codec_install_post(cpu,br,&g_frame);

    // ALTERNATES CENSUS. The dynarec's redirect table (the primitive all our
    // native ports hook through) has no bound anymore, but a LOST counter
    // remains for the one possible failure (allocation refused): read here,
    // once, after all registrations. An explicit "lost=0" is worth more than
    // silence that could mean either "nothing lost" or "nobody's watching".
    { char m[96]; std::snprintf(m,sizeof m,"alternates: poses=%d perdus=%d",
                                dyn86_alt_count(), dyn86_alt_lost());
      jpline("%s",m);
      if(dyn86_alt_lost()) d2_crashlog("%s <<< DES CROCHETS NATIFS MANQUENT",m); }

    // Address -> "module+rva" resolver for diagnostics (fault reports, prof
    // dumps, MessageBoxA caller attribution). Exposed via g_locp.
    struct Mod2{ std::string name; uint32_t base,size; };
    std::vector<Mod2> mmap;
    { PeImage* im=br.module(exeLogical); if(im) mmap.push_back({exeLogical,im->load_base(),im->image_size()}); }
    auto loc=[&](uint32_t a)->std::string{ for(auto&m:mmap) if(a>=m.base&&a<m.base+m.size){ char b[64]; std::snprintf(b,sizeof b,"%s+0x%05x",m.name.c_str(),a-m.base); return b; } char b[16]; std::snprintf(b,sizeof b,"0x%08x",a); return b; };
    static std::function<std::string(uint32_t)> locFn = loc; g_locp=&locFn;

    (void)entryBase;
    auto faultReport=[&](const char* fault){
        std::printf("    FAULT: %s data@0x%08x  EIP=0x%08x  at %s\n", fault?fault:"?", cpu->fault_addr(), cpu->reg(R_EIP), loc(cpu->reg(R_EIP)).c_str());
        std::printf("    EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
            cpu->reg(R_EAX),cpu->reg(R_ECX),cpu->reg(R_EDX),cpu->reg(R_EBX),cpu->reg(R_ESP),cpu->reg(R_EBP),cpu->reg(R_ESI),cpu->reg(R_EDI));
    };
    // Run one DLL's entry (DllMain CRT startup, DLL_PROCESS_ATTACH). Returns
    // EAX (>=0), or a negative code on stop/fault. State persists across calls.
    auto runInit=[&](const std::string& name)->long{
        PeImage* im=br.module(name); if(!im){ std::printf("  %-13s (not loaded)\n",name.c_str()); return -1; }
        g_curBase=im->load_base(); g_stop=false; g_stopReason.clear();
        uint32_t ret=0; const char* fault=nullptr;
        bool ok=br.call_va(im->entry_va(), { im->load_base(),1u,0u }, ret,&fault);
        if(g_stop){ std::printf("  %-13s STOPPED: %s\n",name.c_str(),g_stopReason.c_str()); return -2; }
        if(!ok){ std::printf("  %-13s FAULT\n",name.c_str()); faultReport(fault); return -3; }
        std::printf("  %-13s DllMain -> 0x%08x %s\n",name.c_str(),ret, ret==1?"(TRUE)":ret==0?"(FALSE)":"");
        return (long)ret;
    };

    if(argc>2){ std::printf("\n=== single DLL init: %s ===\n",entryDll.c_str()); runInit(entryDll); std::printf("heap used=%u KB  va used=%u KB\n",g_heapA.used_bytes()/1024,g_vaA.used_bytes()/1024); return 0; }

    // 1.14 monolith: no separate DLLs to init — Game.exe's own CRT entry (run
    // via the scheduler below) performs all startup. Skip the 1.13c DllMain
    // chain entirely.


    // ---- exercise phase: real post-init export calls (x86↔x86 + shims) -----
    auto callOrd=[&](const std::string& mod,uint32_t ord,std::vector<uint32_t> args,bool& okOut)->uint32_t{
        okOut=false; PeImage* im=br.module(mod); if(!im||!im->export_va_ordinal(ord)){ std::printf("    %s@%u: no such export\n",mod.c_str(),ord); return 0; }
        g_curBase=im->load_base(); g_stop=false; g_stopReason.clear(); set_lasterr(*cpu,0);
        uint32_t ret=0; const char* fault=nullptr;
        bool ok=br.call_export_ordinal(mod,ord,args,ret,&fault);
        if(g_stop){ std::printf("    %s@%u STOPPED: %s\n",mod.c_str(),ord,g_stopReason.c_str()); return 0; }
        if(!ok){ std::printf("    %s@%u FAULT\n",mod.c_str(),ord); faultReport(fault); return 0; }
        okOut=true; return ret;
    };
    auto isGuest=[&](uint32_t a){ return (a>=HEAP_BASE&&a<HEAP_BASE+HEAP_SIZE)||(a>=VA_BASE&&a<VA_BASE+VA_SIZE); };



    // Run the REAL Game.exe entry point — let it orchestrate the boot
    // (Fog/Storm init, MPQ mount, dynamic-load D2Client/D2Game/D2Common, data
    // tables). Crash-by-crash: add a shim only when this real path demands it.
    if(getenv("GAMEEXE")){
        PeImage* g=br.module("Game.exe");
        if(g){ g_curBase=g->load_base(); g_stop=false; g_stopReason.clear();
            uint32_t iatva=g->load_base()+0x916c;
            std::printf("  [diag] IAT[+0x916c] = 0x%08x  (should be a shim trap 0x7f0xxxxx)\n", cpu->read_u32(iatva));
            uint32_t opva=g->load_base()+0x1250; uint32_t opnd=0; cpu->read(opva,&opnd,4);
            std::printf("  [diag] call operand @entry+0x22 = 0x%08x  (should be 0x%08x)\n", opnd, iatva);
            // --- Runtime fidelity self-test (D2FIDTEST=1) ----------------------
            // Deterministic (no game flow) validation of the audit quick-wins:
            // dynamic-code invalidation (D2/D3), VirtualQuery regions (M1), and
            // module enumeration (M4). See docs/FIDELITY_TODO.md. Exit 0 = all
            // passed, 1 = a regression. The residual SMC gap (D1: guest self-write
            // WITHOUT Flush/VirtualProtect) is exercised too and documented FAIL.
            // --- KEYSTORE self-test (D2KEYSTORETEST=write|read) -----------
            // The CD key must NOT go through registry.txt. The test writes a
            // SYNTHETIC value (a marker, not a real key) under
            // HKCU\Software\Battle.net\BLIZZARDKEY through the REAL registry
            // shims, then verifies ON DISK:
            //   write  registry.txt contains neither the name nor the bytes,
            //          the keystore contains both, and a NON-secret value
            //          still lands in registry.txt (a scale witness — without
            //          it, "absent from registry.txt" would be trivially true
            //          because the file would be empty);
            //   read   in a FRESH process, the value comes back byte for byte.
            if(const char* kst=getenv("D2KEYSTORETEST")){
                std::printf("\n=== [KEYSTORE] cle hors de registry.txt (%s) ===\n", kst);
                int pass=0, fail=0;
                auto CHECK=[&](const char* what, bool ok){ std::printf("  [%s] %s\n", ok?"PASS":"FAIL", what); if(ok) pass++; else fail++; };
                static const uint8_t MARK[8]={0xC0,0xFF,0xEE,0x01,0x02,0x03,0x04,0x05};
                const char* MARKHEX="c0ffee0102030405";
                auto AVA=[&](const char* n){ return br.shim_trap("ADVAPI32.dll", n); };
                auto call=[&](uint32_t trap, std::vector<uint32_t> a)->uint32_t{ uint32_t rr=0; const char* f=nullptr; br.call_va(trap,a,rr,&f); return rr; };
                auto slurpTxt=[&](const char* fp)->std::string{
                    FILE* f=std::fopen(fp,"rb"); if(!f) return std::string();
                    std::string o; char b[4096]; size_t n; while((n=std::fread(b,1,sizeof b,f))>0) o.append(b,n);
                    std::fclose(f); return o; };
                uint32_t ph=misc(4);
                uint32_t sub=put_cstr(*cpu,"Software\\Battle.net");
                uint32_t vn =put_cstr(*cpu,"BLIZZARDKEY");
                CHECK("RegCreateKeyExA(HKCU\\Software\\Battle.net) -> 0",
                      call(AVA("RegCreateKeyExA"),{0x80000001u,sub,0,0,0,0,0,ph,0})==0u);
                uint32_t hk=cpu->read_u32(ph);
                if(std::string(kst)=="write"){
                    uint32_t data=misc(16); cpu->write(data,MARK,8);
                    CHECK("RegSetValueExA(BLIZZARDKEY, REG_BINARY, 8 o) -> 0",
                          call(AVA("RegSetValueExA"),{hk,vn,0,3u,data,8u})==0u);
                    std::string reg=slurpTxt(d2_registry_file()), ks=slurpTxt(d2_keystore_file());
                    CHECK("registry.txt ne contient PAS le nom blizzardkey",
                          reg.find("blizzardkey")==std::string::npos);
                    CHECK("registry.txt ne contient PAS le blob (aucun fragment)",
                          reg.find(MARKHEX)==std::string::npos);
                    CHECK("keystore.bin existe et porte la valeur", !ks.empty()
                          && ks.find("blizzardkey")!=std::string::npos && ks.find(MARKHEX)!=std::string::npos);
                    uint32_t vn2=put_cstr(*cpu,"Gateway");
                    uint32_t d2v=misc(8); cpu->write(d2v,"LOCAL",6);
                    call(AVA("RegSetValueExA"),{hk,vn2,0,1u,d2v,6u});
                    reg=slurpTxt(d2_registry_file());
                    CHECK("temoin : une valeur NON secrete reste dans registry.txt",
                          reg.find("gateway")!=std::string::npos);
                    std::printf("  fichiers : %s | %s\n", d2_registry_file(), d2_keystore_file());
                } else {
                    uint32_t buf=misc(64), cb=misc(4), ty=misc(4); cpu->write_u32(cb,64);
                    uint32_t r=call(AVA("RegQueryValueExA"),{hk,vn,0,ty,buf,cb});
                    uint8_t got[8]={0}; cpu->read(buf,got,8);
                    CHECK("aller-retour : la valeur revient octet pour octet apres redemarrage",
                          r==0u && cpu->read_u32(cb)==8u && std::memcmp(got,MARK,8)==0);
                    std::string reg=slurpTxt(d2_registry_file());
                    CHECK("registry.txt reste propre apres relecture",
                          reg.find("blizzardkey")==std::string::npos && reg.find(MARKHEX)==std::string::npos);
                }
                std::printf("=== [KEYSTORE] %d passed, %d failed ===\n", pass, fail);
                return fail==0?0:1;
            }
            // --- NETWORK LOCK and UDP shims self-test (D2NETGUARDTEST=1)
            // Entirely OFFLINE: the two "official" destinations named below
            // never receive a single byte — that is exactly what the test
            // proves. The UDP bench itself talks to a host socket on
            // 127.0.0.1 created by the test.
            if(getenv("D2NETGUARDTEST")){
                std::printf("\n=== [NETGUARD] verrou reseau + shims UDP ===\n");
                int pass=0, fail=0;
                auto CHECK=[&](const char* what, bool ok){ std::printf("  [%s] %s\n", ok?"PASS":"FAIL", what); if(ok) pass++; else fail++; };
                const bool prevNet=wx86_net_enabled(); wx86_net_set_enabled(true);
                const bool prevGuard=wx86_net_private_only(); wx86_net_set_private_only(true);
                uint32_t tSock =br.shim_trap("WSOCK32.dll","#23");
                uint32_t tConn =br.shim_trap("WSOCK32.dll","#4");
                uint32_t tSend =br.shim_trap("WSOCK32.dll","#20");
                uint32_t tRecv =br.shim_trap("WSOCK32.dll","#17");
                uint32_t tClose=br.shim_trap("WSOCK32.dll","#3");
                CHECK("ordinaux #23/#4/#20/#17/#3 tous enregistres (plus de shim par defaut)",
                      tSock&&tConn&&tSend&&tRecv&&tClose);
                // ESP-DRIFT measurement. call_va starts from a FIXED stack and
                // pushes args + return address; a stdcall shim whose argc is
                // CORRECT therefore returns ESP to the SAME value regardless of
                // argument count. A wrong argc lands elsewhere — and shifts the
                // stack on EVERY call in the real game. We compare EXIT ESPs
                // against each other, not against the entry ESP.
                std::set<uint32_t> espOut;
                auto callw=[&](uint32_t trap, std::vector<uint32_t> a)->uint32_t{
                    uint32_t rr=0; const char* f=nullptr;
                    br.call_va(trap,a,rr,&f); espOut.insert(cpu->reg(R_ESP)); return rr; };
                auto mksa=[&](const char* ip, uint16_t port)->uint32_t{
                    uint32_t p=misc(16); uint8_t sa[16]={0}; sa[0]=2; sa[1]=0;
                    sa[2]=(uint8_t)(port>>8); sa[3]=(uint8_t)(port&0xff);
                    uint32_t a=inet_addr(ip); std::memcpy(sa+4,&a,4);
                    cpu->write(p,sa,16); return p; };
                // 1) TCP to an official gateway: REFUSED, no handshake
                uint32_t s1=callw(tSock,{2,1,0});
                uint32_t r1=callw(tConn,{s1,mksa("37.244.28.156",6112),16});
                uint32_t w1=wx86_net_last_error();
                CHECK("connect 37.244.28.156:6112 -> SOCKET_ERROR", r1==0xFFFFFFFFu);
                CHECK("  ... avec WSAEACCES (10013), pas un echec reseau quelconque", w1==10013u);
                { int fd=wx86_sock_fd(s1); uint8_t sa[16]; socklen_t sl=16;
                  CHECK("  ... et AUCUNE connexion etablie (getpeername echoue)",
                        fd<0 || ::getpeername(fd,(sockaddr*)sa,&sl)<0); }
                // 2) the startup port-7 probe
                uint32_t r2=callw(tConn,{s1,mksa("24.105.29.30",7),16});
                CHECK("connect 24.105.29.30:7 -> refus WSAEACCES", r2==0xFFFFFFFFu && wx86_net_last_error()==10013u);
                // 3) SCALE TEST: the whitelist lets local traffic through.
                //    Without this witness, a lock that refuses EVERYTHING
                //    would make the two lines above true for the wrong reason.
                unsigned long long nRef0=wx86_net_refused();
                uint32_t s3=callw(tSock,{2,1,0});
                uint32_t r3=callw(tConn,{s3,mksa("127.0.0.1",9),16});
                CHECK("temoin : 127.0.0.1:9 traverse le verrou (echec reseau, PAS 10013)",
                      wx86_net_refused()==nRef0 && (r3==0u || wx86_net_last_error()!=10013u));
                callw(tClose,{s3});
                // 4) Real UDP: sendto/recvfrom against a host echo on 127.0.0.1
                {   int hfd=::socket(AF_INET,SOCK_DGRAM,0);
                    sockaddr_in ha{}; ha.sin_family=AF_INET; ha.sin_port=0;
                    ha.sin_addr.s_addr=inet_addr("127.0.0.1");
                    bool bound = hfd>=0 && ::bind(hfd,(sockaddr*)&ha,sizeof ha)==0;
                    socklen_t hl=sizeof ha; if(bound) ::getsockname(hfd,(sockaddr*)&ha,&hl);
                    uint16_t hport=ntohs(ha.sin_port);
                    CHECK("banc UDP : socket hote d'echo prete sur 127.0.0.1", bound && hport!=0);
                    uint32_t su=callw(tSock,{2,2,0});                        // SOCK_DGRAM
                    const char msg[]="\x08\x14PING";
                    uint32_t pbuf=misc(64); cpu->write(pbuf,msg,6);
                    uint32_t sent=callw(tSend,{su,pbuf,6,0,mksa("127.0.0.1",hport),16});
                    CHECK("sendto 6 octets vers l'echo local -> 6", sent==6u);
                    uint8_t got[64]={0}; sockaddr_in from{}; socklen_t fl=sizeof from;
                    ssize_t n = bound? ::recvfrom(hfd,got,sizeof got,0,(sockaddr*)&from,&fl) : -1;
                    CHECK("l'hote a bien recu les 6 octets EXACTS", n==6 && std::memcmp(got,msg,6)==0);
                    if(n>0) ::sendto(hfd,got,(size_t)n,0,(sockaddr*)&from,fl);   // echo
                    uint32_t rbuf=misc(64), pfrom=misc(16), pflen=misc(4);
                    cpu->write_u32(pflen,16);
                    uint32_t rn=callw(tRecv,{su,rbuf,64,0,pfrom,pflen});
                    uint8_t back[8]={0}; if(rn==6) cpu->read(rbuf,back,6);
                    CHECK("recvfrom rend les 6 memes octets", rn==6u && std::memcmp(back,msg,6)==0);
                    CHECK("recvfrom renseigne l'adresse source (AF_INET + 127.0.0.1)",
                          cpu->read_u32(pfrom+4)==(uint32_t)inet_addr("127.0.0.1"));
                    // 5) sendto to an official gateway: REFUSED
                    unsigned long long nRef1=wx86_net_refused();
                    uint32_t bad=callw(tSend,{su,pbuf,6,0,mksa("37.244.28.156",6112),16});
                    CHECK("sendto 37.244.28.156:6112 -> refus WSAEACCES, rien n'est emis",
                          bad==0xFFFFFFFFu && wx86_net_last_error()==10013u && wx86_net_refused()==nRef1+1);
                    callw(tClose,{su});
                    if(hfd>=0) ::close(hfd); }
                callw(tClose,{s1});
                // 6) correct argc = stable ESP. An incorrectly-sized stdcall
                //    shim drifts by the difference on EVERY call (the bug that
                //    once sent Fog's init off to execute .rdata).
                { std::printf("       ESP de sortie observes :"); for(uint32_t e:espOut) std::printf(" 0x%08x",e); std::printf("\n"); }
                CHECK("ESP identique en sortie de TOUS les shims (argc stdcall juste)", espOut.size()==1);
                std::printf("  refus=%llu  passages=%llu\n",
                            (unsigned long long)wx86_net_refused(),(unsigned long long)wx86_net_allowed());
                wx86_net_set_enabled(prevNet); wx86_net_set_private_only(prevGuard);
                std::printf("=== [NETGUARD] %d passed, %d failed ===\n", pass, fail);
                return fail==0?0:1;
            }
            if(getenv("D2FIDTEST")){
                std::printf("\n=== [FIDTEST] runtime fidelity self-test ===\n");
                int pass=0, fail=0;
                auto CHECK=[&](const char* what, bool ok){ std::printf("  [%s] %s\n", ok?"PASS":"FAIL", what); if(ok) pass++; else fail++; };
                uint32_t code=d2_valloc(0x100);                                  // a VirtualAlloc-style RWX region
                auto emit=[&](uint32_t imm){ uint8_t s[6]={0xB8,(uint8_t)imm,(uint8_t)(imm>>8),(uint8_t)(imm>>16),(uint8_t)(imm>>24),0xC3}; cpu->write(code,s,6); }; // mov eax,imm; ret
                auto poke=[&](uint32_t imm){ uint8_t* hp=(uint8_t*)cpu->hostptr(code,5); if(hp){ hp[1]=(uint8_t)imm; hp[2]=(uint8_t)(imm>>8); hp[3]=(uint8_t)(imm>>16); hp[4]=(uint8_t)(imm>>24); return true; } return false; };
                auto call=[&](uint32_t& r)->bool{ const char* f=nullptr; r=0; return br.call_va(code,{},r,&f); };
                uint32_t r=0;
                // 1) emit + execute freshly-written dynamic code
                emit(0x11); CHECK("dyn-code exec (mov eax,0x11 -> 0x11)", call(r) && r==0x11);
                // 2) host-write SMC invalidation (write barrier): re-run sees new bytes
                emit(0x22); CHECK("host-write SMC invalidation (->0x22)", call(r) && r==0x22);
                // 3) invalidate_code hook (D2 VirtualProtect / D3 FlushInstructionCache):
                //    modify via hostptr (bypasses the write barrier) then invalidate_code
                bool poked=poke(0x33); cpu->invalidate_code(code,6);
                CHECK("invalidate_code hook (D2/D3, ->0x33)", poked && call(r) && r==0x33);
                // 4) residual guest-SMC gap (D1): modify WITHOUT invalidation -> stale
                //    translation replays. Documented FAIL (needs a fault handler; see (d)).
                poke(0x44); bool d1ok = call(r) && r==0x44;
                std::printf("  [%s] guest-SMC WITHOUT flush picks up change (D1 residual: FAIL expected)\n", d1ok?"PASS":"FAIL(expected)");
                // 4b) POINT-3 FULL CYCLE through the ACTUAL guest Win32 APIs (not the
                //     C++ shortcuts above): VirtualAlloc -> write -> VirtualProtect RX
                //     -> exec -> hostptr-poke (bypass barrier) -> guest VirtualProtect /
                //     FlushInstructionCache invalidate -> retranslate -> exec new value.
                {
                  auto GVA=[&](const char* n){ return br.shim_trap("KERNEL32.dll", n); };
                  auto callk=[&](uint32_t trap, std::vector<uint32_t> a)->uint32_t{ uint32_t rr=0; const char* f=nullptr; br.call_va(trap,a,rr,&f); return rr; };
                  auto emitAt=[&](uint32_t at, uint32_t imm){ uint8_t s[6]={0xB8,(uint8_t)imm,(uint8_t)(imm>>8),(uint8_t)(imm>>16),(uint8_t)(imm>>24),0xC3}; cpu->write(at,s,6); };
                  uint32_t oldp=misc(4);
                  // real VirtualAlloc(NULL,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE)
                  uint32_t vbuf=callk(GVA("VirtualAlloc"), {0u,0x1000u,0x3000u,0x40u});
                  CHECK("guest VirtualAlloc commit (non-null, zeroed)", vbuf && cpu->read_u32(vbuf)==0);
                  if(vbuf){
                    emitAt(vbuf,0x1111);
                    callk(GVA("VirtualProtect"), {vbuf,6u,0x20u,oldp});               // -> PAGE_EXECUTE_READ
                    CHECK("guest VirtualProtect wrote lpflOldProtect=RWX(0x40)", cpu->read_u32(oldp)==0x40u);
                    { uint32_t rr=0; const char* f=nullptr; bool ok=br.call_va(vbuf,{},rr,&f);
                      CHECK("exec RX buffer -> 0x1111", ok && rr==0x1111); }
                    // CENTREPIECE: poke via hostptr (bypasses write barrier) so the ONLY
                    // thing that can retranslate is the guest API call.
                    { uint8_t* hp=(uint8_t*)cpu->hostptr(vbuf,5); if(hp){ hp[1]=0x22; hp[2]=hp[3]=hp[4]=0; }
                      callk(GVA("VirtualProtect"), {vbuf,6u,0x40u,oldp});             // RW  (invalidate)
                      callk(GVA("VirtualProtect"), {vbuf,6u,0x20u,oldp});             // RX  (invalidate)
                      uint32_t rr=0; const char* f=nullptr; bool ok=br.call_va(vbuf,{},rr,&f);
                      CHECK("guest VirtualProtect drives retranslate -> 0x22", ok && rr==0x22); }
                    { uint8_t* hp=(uint8_t*)cpu->hostptr(vbuf,5); if(hp){ hp[1]=0x33; hp[2]=hp[3]=hp[4]=0; }
                      callk(GVA("FlushInstructionCache"), {0xFFFFFFFFu, vbuf, 6u});
                      uint32_t rr=0; const char* f=nullptr; bool ok=br.call_va(vbuf,{},rr,&f);
                      CHECK("guest FlushInstructionCache drives retranslate -> 0x33", ok && rr==0x33); }
                  }
                }
                // 5) VirtualQuery (M1): module image / heap private / free gap
                // ⚠️ THESE FOUR CHECKS DO NOT EXERCISE THE KERNEL32!VirtualQuery
                // SHIM. They call vquery_region() DIRECTLY — this port's region
                // descriptor — not the path the game actually takes. Fault
                // injection confirms it: sabotaging the shim (always
                // returning 0) leaves all four lines GREEN, while sabotaging
                // VirtualProtect does make its own line fail. What actually
                // covers the shim is the engine's Win32 fidelity bench
                // (tools/torture), where a real PE calls the import. Do not
                // conclude "VirtualQuery is tested" from these four lines.
                { uint32_t b=0,s=0,st=0,ty=0,pr=0,ab=0; bool ok=vquery_region(g_d2base,b,s,st,ty,pr,ab);
                  CHECK("VirtualQuery module -> MEM_COMMIT+MEM_IMAGE", ok && st==0x1000u && ty==0x1000000u); }
                { uint32_t b=0,s=0,st=0,ty=0,pr=0,ab=0; bool ok=vquery_region(HEAP_BASE+0x2000,b,s,st,ty,pr,ab);
                  CHECK("VirtualQuery heap -> MEM_COMMIT+MEM_PRIVATE", ok && st==0x1000u && ty==0x20000u); }
                { uint32_t b=0,s=0,st=0,ty=0,pr=0,ab=0; bool ok=vquery_region(d2rt::layout_user_hi()-0xFF0000u,b,s,st,ty,pr,ab);
                  CHECK("VirtualQuery free gap -> MEM_FREE", ok && st==0x10000u); }
                { uint32_t b=0,s=0,st=0,ty=0,pr=0,ab=0; bool ok=vquery_region(d2rt::layout_user_hi()+0xF00u,b,s,st,ty,pr,ab);
                  CHECK("VirtualQuery kernel space -> fail (like Windows)", !ok); }
                // 6) Module enum (M4): snapshot non-empty + contains game.exe
                { mod32_snapshot(); bool has=false; for(auto&m:g_mod32list) if(m.name=="game.exe") has=true;
                  CHECK("Module32 snapshot non-empty + has game.exe", !g_mod32list.empty() && has); }
                // 7) ExitProcess never returns: a stub `call ExitProcess; mov [flag],0xDEAD`
                //    must NOT run the poison store (redirect_next(sentinel)). g_sched is
                //    null here so request_shutdown() is skipped; restore g_stop after.
                { uint32_t flag=misc(4); cpu->write_u32(flag,0);
                  uint32_t exVA=br.shim_trap("KERNEL32.dll","ExitProcess");
                  uint32_t stub=d2_valloc(0x40); uint8_t s[32]; int k=0;
                  s[k++]=0x6A; s[k++]=0x00;                                   // push 0 (exit code)
                  s[k++]=0xB8; std::memcpy(s+k,&exVA,4); k+=4;                // mov eax, exVA
                  s[k++]=0xFF; s[k++]=0xD0;                                   // call eax  (-> ExitProcess trap)
                  s[k++]=0xC7; s[k++]=0x05; std::memcpy(s+k,&flag,4); k+=4;   // mov dword[flag], 0xDEAD  (POISON)
                  uint32_t poison=0x0000DEADu; std::memcpy(s+k,&poison,4); k+=4;
                  s[k++]=0xC3;                                                // ret
                  cpu->write(stub,s,(uint32_t)k);
                  bool gstop0=g_stop; uint32_t r=0; const char* f=nullptr; br.call_va(stub,{},r,&f);
                  CHECK("ExitProcess never returns (no guest instr after)", cpu->read_u32(flag)!=0x0000DEADu);
                  g_stop=gstop0; }
                // 8) INTRINSIC state parity (Tier-1 GetTickCount): the dynarec
                //    fast path must be byte-identical to the Bridge shim. This
                //    self-test runs in VIRTUAL-clock mode, where tick_bump bumps
                //    the guest tick page by exactly 1/call — so an intrinsic call
                //    then a forced slow-path call must yield CONSECUTIVE ticks and
                //    the SAME page mutation. Proves fast-path == slow-path state.
                {
                  uint32_t tickTrap=br.shim_trap("KERNEL32.dll","GetTickCount");
                  bool virt = !g_realclock && !g_netClock && !g_tickvirt;
                  uint32_t base=cpu->read_u32(g_tickVA+4);
                  uint32_t c0=cpu->read_u32(g_tickVA);
                  cpu->set_intrinsics_enabled(true);
                  uint32_t ri=0; const char* ff=nullptr; bool oki=br.call_va(tickTrap,{},ri,&ff);
                  uint32_t c1=cpu->read_u32(g_tickVA);
                  cpu->set_intrinsics_enabled(false);
                  uint32_t rs=0; bool oks=br.call_va(tickTrap,{},rs,&ff);
                  uint32_t c2=cpu->read_u32(g_tickVA);
                  cpu->set_intrinsics_enabled(!getenv("D2_DISABLE_INTRINSICS"));
                  if(virt) CHECK("GetTickCount intrinsic == slow-path (virtual: consecutive ticks + same page bump)",
                      oki && oks && c1==c0+1 && c2==c0+2 && ri==c0+1+base && rs==c0+2+base && rs==ri+1);
                  else CHECK("GetTickCount intrinsic monotone vs slow-path (real clock)",
                      oki && oks && rs>=ri);
                }
                // --- process/thread identity, and the CPU seen from both sides ---
                // Windows never allocates PID 1 and hands out PID/TID in steps
                // of 4. This returned 1 and 1,2,3... — two constants a scanner
                // could read for free.
                {
                  auto GVA=[&](const char* n){ return br.shim_trap("KERNEL32.dll", n); };
                  auto callk=[&](uint32_t trap, std::vector<uint32_t> a)->uint32_t{ uint32_t rr=0; const char* f=nullptr; br.call_va(trap,a,rr,&f); return rr; };
                  uint32_t pid=callk(GVA("GetCurrentProcessId"),{});
                  CHECK("GetCurrentProcessId : != 0, != 1, multiple de 4", pid>1 && (pid&3)==0);
                  uint32_t tid=callk(GVA("GetCurrentThreadId"),{});
                  CHECK("GetCurrentThreadId : != 0, != 1, multiple de 4", tid>1 && (tid&3)==0);
                  // IsProcessorFeaturePresent MUST agree with CPUID. Execute a
                  // REAL guest cpuid — so the dynarec, not a C++ constant —
                  // and compare.
                  uint32_t cp=misc(8); cpu->write_u32(cp,0); cpu->write_u32(cp+4,0);
                  uint32_t stub=d2_valloc(0x40); uint8_t q[32]; int k=0;
                  q[k++]=0x53;                                            // push ebx
                  q[k++]=0xB8; { uint32_t one=1; std::memcpy(q+k,&one,4); } k+=4;   // mov eax,1
                  q[k++]=0x0F; q[k++]=0xA2;                               // cpuid
                  q[k++]=0x89; q[k++]=0x15; std::memcpy(q+k,&cp,4); k+=4; // mov [cp],edx
                  uint32_t cp4=cp+4;
                  q[k++]=0x89; q[k++]=0x0D; std::memcpy(q+k,&cp4,4); k+=4;// mov [cp+4],ecx
                  q[k++]=0x5B;                                            // pop ebx
                  q[k++]=0xC3;                                            // ret
                  cpu->write(stub,q,(uint32_t)k);
                  { uint32_t rr=0; const char* f=nullptr; br.call_va(stub,{},rr,&f); }
                  uint32_t edx=cpu->read_u32(cp), ecx=cpu->read_u32(cp+4);
                  bool mmx=(edx>>23)&1, sse=(edx>>25)&1, sse2=(edx>>26)&1, sse3=ecx&1, fpu=edx&1;
                  uint32_t ipf3 =callk(GVA("IsProcessorFeaturePresent"),{3u});
                  uint32_t ipf6 =callk(GVA("IsProcessorFeaturePresent"),{6u});
                  uint32_t ipf10=callk(GVA("IsProcessorFeaturePresent"),{10u});
                  uint32_t ipf13=callk(GVA("IsProcessorFeaturePresent"),{13u});
                  uint32_t ipf1 =callk(GVA("IsProcessorFeaturePresent"),{1u});
                  std::printf("       cpuid.1 edx=0x%08x ecx=0x%08x | PF 3/6/10/13/1 = %u/%u/%u/%u/%u\n",
                              edx,ecx,ipf3,ipf6,ipf10,ipf13,ipf1);
                  CHECK("IsProcessorFeaturePresent(MMX) == cpuid.MMX",   (ipf3!=0)==mmx);
                  CHECK("IsProcessorFeaturePresent(SSE) == cpuid.SSE",   (ipf6!=0)==sse);
                  CHECK("IsProcessorFeaturePresent(SSE2) == cpuid.SSE2", (ipf10!=0)==sse2);
                  CHECK("IsProcessorFeaturePresent(SSE3) == cpuid.SSE3", (ipf13!=0)==sse3);
                  CHECK("PF_FLOATING_POINT_EMULATED == 0 quand cpuid.FPU==1", fpu && ipf1==0);
                  // GetModuleFileName on a system handle used to return
                  // "C:\Diablo II\game.exe". An unknown handle must FAIL.
                  uint32_t nb=misc(300);
                  uint32_t nExe=callk(GVA("GetModuleFileNameA"),{0u,nb,260u});
                  std::string sExe; for(uint32_t i=0;i<nExe&&i<260;i++){ uint8_t ch=0; cpu->read(nb+i,&ch,1); sExe+=(char)ch; }
                  CHECK("GetModuleFileNameA(NULL) rend toujours le chemin de l'exe",
                        nExe>0 && sExe.find("game.exe")!=std::string::npos);
                  uint32_t hK=callk(GVA("GetModuleHandleA"),{put_cstr(*cpu,"kernel32.dll")});
                  uint32_t nSys=callk(GVA("GetModuleFileNameA"),{hK,nb,260u});
                  uint32_t le=callk(GVA("GetLastError"),{});
                  std::printf("       GetModuleHandleA(kernel32)=0x%08x -> GetModuleFileNameA=%u lasterr=%u\n",hK,nSys,le);
                  CHECK("GetModuleFileNameA(hKernel32) ECHOUE (plus de faux chemin d'exe)",
                        hK==0 || (nSys==0 && le==6u));
                }
                std::printf("=== [FIDTEST] %d passed, %d failed (D1 residual not counted) ===\n", pass, fail);
                return fail==0?0:1;
            }
            // --- CheckRevision.dll loader self-test (D2CRTEST=1 or =<path>) ------
            // Load the lockdown DLL directly and call its "CheckRevision" export,
            // WITHOUT the flaky real-clock Battle.net flow. Validates the runtime
            // PE loader: map + import-link + export resolution + dynarec exec.
            if(const char* crt=getenv("D2CRTEST")){
                std::string path = (crt[0] && std::strcmp(crt,"1")!=0) ? crt : (dir+"/CheckRevision.dll");
                std::vector<uint8_t> b=slurp(path);
                std::printf("\n=== [CRTEST] load %s (%zu bytes) ===\n", path.c_str(), b.size());
                if(b.size()<0x40 || b[0]!='M' || b[1]!='Z'){ std::printf("  not a PE\n"); return 0; }
                std::string lerr; PeImage* pi=br.load_library_runtime("checkrevision.dll",b,lerr);
                if(!pi){ std::printf("  load FAIL: %s\n", lerr.c_str()); return 0; }
                g_modByBase[pi->load_base()]=pi; g_modByName["checkrevision.dll"]=pi;
                uint32_t ev=pi->export_va("CheckRevision");
                std::printf("  loaded @0x%08x size 0x%x, %zu imports; export CheckRevision @0x%08x\n",
                    pi->load_base(), pi->image_size(), pi->imports().size(), ev);
                if(!ev){ std::printf("  export NOT FOUND\n"); return 0; }
                // Run DllMain(DLL_PROCESS_ATTACH) first: the CRT startup
                // (__security_init_cookie, _CRT_INIT) must initialize before the
                // export runs, else the default security cookie (0xBB40E64E) is
                // used as a live pointer and faults.
                { uint32_t dm=pi->entry_va(); const char* f2=nullptr; uint32_t r2=0;
                  std::printf("  DllMain @0x%08x (DLL_PROCESS_ATTACH)...\n", dm);
                  bool ok2=br.call_va(dm, {pi->load_base(),1u,0u}, r2, &f2);
                  std::printf("  DllMain -> ok=%d ret=0x%x fault=%s\n", ok2?1:0, r2, f2?f2:"-"); }
                // Best-effort call: real arg semantics come from D2's call site; here
                // we just exercise the code path with valid writable buffers.
                uint32_t buf1=misc(0x400), buf2=misc(0x400), buf3=misc(0x400);
                uint32_t seed=put_cstr(*cpu, getenv("D2CRTEST_SEED")?getenv("D2CRTEST_SEED"):"PlK71AAA");
                std::vector<uint32_t> args={0,0,0, seed, buf1, buf2, buf3};
                const char* fault=nullptr; uint32_t ret=0;
                std::printf("  call CheckRevision(0,0,0,seed=0x%x,0x%x,0x%x,0x%x)...\n",seed,buf1,buf2,buf3);
                uint32_t esp0=cpu->reg(R_ESP);
                bool ok=br.call_va(ev, args, ret, &fault);
                uint32_t esp1=cpu->reg(R_ESP);
                // out3 is `info`: a STRING (24 base64 characters), not a u32.
                // Printing it as hex made the output incomparable to the
                // oracle — nothing could be concluded from the bench.
                std::string info; { for(int i=0;i<64;i++){ uint8_t ch=0; cpu->read(buf3+i,&ch,1);
                        if(!ch) break; info += (ch=='\n')? std::string("\\n") : std::string(1,(char)ch); } }
                uint32_t version=cpu->read_u32(buf1), checksum=cpu->read_u32(buf2);
                std::printf("  -> ok=%d ret=0x%08x fault=%s\n", ok?1:0, ret, fault?fault:"-");
                std::printf("  version  = 0x%08x\n", version);
                std::printf("  checksum = 0x%08x\n", checksum);
                std::printf("  info     = \"%s\"\n", info.c_str());
                std::printf("  chaine hachee = %s\n", g_crHashedVer.empty()?"(non observee)":g_crHashedVer.c_str());
                std::printf("  esp avant/apres = 0x%08x / 0x%08x %s\n", esp0, esp1, esp0==esp1?"(stable)":"(DERIVE !)");
                // MACHINE-readable line, for the tools/oracle_checkrevision.sh
                // comparator: this is what gets checked against
                // tools/checkrevision_ref.py.
                std::printf("CRTEST-RESULT seed=%s version=0x%08x checksum=0x%08x info=%s ver=%s\n",
                    getenv("D2CRTEST_SEED")?getenv("D2CRTEST_SEED"):"PlK71AAA",
                    version, checksum, info.c_str(), g_crHashedVer.empty()?"?":g_crHashedVer.c_str());
                std::printf("=== [CRTEST] done ===\n");
                return 0;
            }
            // Pristine integrity check (D2_PRISTINE_CHECK): after all load-time
            // setup, verify no Blizzard code byte was modified (beyond relocations).
            if(getenv("D2_PRISTINE_CHECK")){
                std::printf("=== [pristine] code-integrity check ===\n");
                pristine_check(*cpu, g_d2base, dir+"/Game.exe", "Game.exe");
                for(const char* m : {"Storm.dll","D2Win.dll","D2Client.dll","D2Common.dll","D2gfx.dll","Fog.dll"}){
                    uint32_t b=br.module_base(m); if(b) pristine_check(*cpu, b, dir+"/"+m, m); }
                if(getenv("D2_PRISTINE_AUDIT")){   // full-image audit (headers+all sections+IAT), test #7
                    std::printf("=== [audit] full-image classification (base 0x%08x) ===\n", g_d2base);
                    pristine_audit(*cpu, g_d2base, dir+"/Game.exe", "Game.exe");
                    for(const char* m : {"Storm.dll","D2Win.dll","D2Client.dll","D2Common.dll","D2gfx.dll","Fog.dll"}){
                        uint32_t b=br.module_base(m); if(b) pristine_audit(*cpu, b, dir+"/"+m, m); }
                }
                std::printf("=== [pristine] check done ===\n");
                if(getenv("D2_PRISTINE_CHECK_ONLY")) return 0;
            }
            // Blit-hook self-test (D2_ALT_SELFTEST): execute a TRANSLATED `call
            // 0xf69b0` and see whether the native-blit shim fires. Proves the
            // translation-time redirect (pristine/alternate) or the .text patch
            // (normal) without needing to reach in-game. At load the per-band
            // tables are zero (BSS) so every band is skipped — no memory touched.
            // GAMEEXE=1 REQUIRED: this block lives in the Game.exe branch (the
            // `if(getenv("GAMEEXE"))` above) and depends on g_d2base; without
            // GAMEEXE=1, D2_ALT_SELFTEST is skipped SILENTLY — no output, no
            // error.
            if(getenv("D2_ALT_SELFTEST") && g_d2base){
                uint32_t blit=g_d2base+0xf69b0;
                uint32_t lut=misc(256); { uint8_t id[256]; for(int i=0;i<256;i++) id[i]=(uint8_t)i; cpu->write(lut,id,256); }
                uint32_t stub=misc(16); uint8_t s[16]; int k=0;
                s[k++]=0x68; std::memcpy(s+k,&lut,4); k+=4;                              // push lut
                s[k++]=0xE8; uint32_t rel=blit-(stub+k+4); std::memcpy(s+k,&rel,4); k+=4; // call 0xf69b0
                s[k++]=0xC3;                                                             // ret
                cpu->write(stub,s,(uint32_t)k);
                cpu->set_reg(R_ECX,0); cpu->set_reg(R_EDX,0);
                uint64_t before=g_blitN; uint32_t r=0; const char* f=nullptr;
                bool ok=br.call_va(stub,{},r,&f);
                std::printf("=== [alt-selftest] translated 'call 0x%08x': ok=%d fault=%s  blitN %llu->%llu  => %s ===\n",
                    blit, ok?1:0, f?f:"-", (unsigned long long)before,(unsigned long long)g_blitN,
                    g_blitN>before ? "HOOK FIRED (alternate, .text untouched)"
                                   : "genuine blit ran (no hook)");
                if(getenv("D2_PRISTINE_CHECK_ONLY")) return 0;
            }
            std::printf("\n=== running Game.exe via cooperative scheduler (multi-guest-thread) ===\n");
            // stacks at 0x70000000 (1 MiB each), TIBs at 0x74000000; main uses the
            // bridge stack (top 0x60200000).
            ThreadScheduler* schedp = make_scheduler(cpu,&br, SCHED_STACKS, 0x100000, SCHED_TIBS);
            ThreadScheduler& sched = *schedp;
            CooperativeScheduler* coop = g_schedCoop;   // null under a non-coop backend
            NativeScheduler* nat = coop ? nullptr
                : static_cast<NativeScheduler*>(schedp);   // non-null ssi backend natif
            // On a real fault, record whether the game had an SEH chain installed
            // before the thread is killed (it is never dispatched to). Only fires
            // on a fault, so the fault-free boot/menu path is untouched.
            sched.set_fault_dispatcher([cpu](GuestThread* t,uint32_t code,uint32_t){ return fault_note_seh_chain(*cpu, t, code); });
#ifdef __vita__
            // Stall diagnostics: when the frame counter freezes, the Vita
            // watchdog dumps every guest thread's state into boot_progress.
            { extern void (*d2vita_wd_threads)(void);
              d2vita_wd_threads=[](){ if(g_sched) g_sched->dump_threads_to(d2vita_progress); }; }
            // Automatic profiling (D2_EIPAUTO=N): same output as the eipdump
            // command, but triggered by the watchdog rather than the command
            // channel, which isn't reliable over long runs.
            { extern void (*d2vita_wd_eipdump)(void);
              d2vita_wd_eipdump=[](){ inj_queue("eipdump",24,0); }; }
            // GIL observability: the gil= field on the alive: line. Wired up
            // ONLY under native — under coop the hook stays null and the
            // field doesn't exist (same discipline as the fam= field).
            if(nat){ extern int (*d2vita_wd_gil)(char*,unsigned);
              d2vita_wd_gil=[](char* b,unsigned n)->int{
                  return d2rt::g_native_sched ? d2rt::g_native_sched->gil_field(b,n) : 0; }; }
            // PER-RUNNER liveness (the run= field on the alive: line): same
            // discipline, native only. This is the field that tells
            // starvation (a counter moves) apart from deadlock (none move)
            // during a freeze — otherwise indistinguishable on console.
            if(nat){ extern int (*d2vita_wd_run)(char*,unsigned);
              d2vita_wd_run=[](char* b,unsigned n)->int{
                  return d2rt::g_native_sched ? d2rt::g_native_sched->vivacity_field(b,n) : 0; }; }
#endif
            // Anti-starvation net: ARMED BY DEFAULT under native, on all
            // three targets (D2_FAMINE absent = armed, convention '1', kill
            // switch D2_FAMINE=0 — remote disabling via env.txt must work).
            // arm_famine() only remembers the config: the heartbeat starts in
            // sched.run() and stops before the runners' bounded join. Placed
            // BEFORE set_main so the D2_FAMINETEST self-test inherits the
            // knobs. Stamps boot_progress + crash.log — never a silent arm or
            // disarm. Coop: none of this exists, nothing is printed.
            bool famArmed=false;   // arming requested at boot (vs fam_armed() AFTER run: a heartbeat failure would clear it)
            if(nat){
                const char* fe=getenv("D2_FAMINE");
                if(fe && *fe=='0')
                    d2_crashlog("famine: filet DESACTIVE (D2_FAMINE=0)");
                else {
                    uint32_t wms=kFamineWindowMsDefault, nus=kFamineNapUsDefault;
                    if(const char* e=getenv("D2_FAMINE_WINDOW_MS")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) wms=v; }
                    if(const char* e=getenv("D2_FAMINE_NAP_US")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) nus=v; }
                    nat->arm_famine(&g_frame, wms, nus);
                    d2_crashlog("famine: filet arme (fenetre=%ums sieste=%uus)", wms, nus);
                    famArmed=true;
                }
            }
            // ---- Anti-starvation-net self-test (D2_FAMINETEST=1) -----
            // EXCLUSIVE branch (same as CRTEST/ALT_SELFTEST): does NOT run
            // Game.exe. Native is guaranteed here (coop => FATAL in
            // make_scheduler); the D2_FAMINE* knobs are inherited from the
            // arming block above. Three x86 actors synthesized in guest
            // memory, start order S before W (the T12 duel order):
            //   S (spinner):  S: call check ; test eax,eax ; jz S ;
            //                 mov [flag2],1 ; ret   with check: mov eax,[flag];
            //                 ret — the call/ret forces a dynablock exit on
            //                 EVERY iteration: the loop crosses seams
            //                 (otherwise this would measure the intra-dynablock
            //                 gap, not the net). [flag2] set before the ret
            //                 proves S itself exited its loop (unstarved).
            //   W (setter):   push 50 ; call [slot Sleep] ; mov [flag],1 ; ret
            //                 (50 = default of the D2_FAMINETEST_WSLEEP_MS
            //                 knob) — sleeps 50 ms (a REAL block) then sets
            //                 the flag S polls. Under run-to-block (console):
            //                 while sleeping, S holds the core; at expiry W is
            //                 ready-but-never-scheduled — exactly the
            //                 starvation this net exists to catch.
            //   guest main:   a bounded loop of 50 x Sleep(200) testing
            //                 [flag2], verdict at 10 s max — TWO victims
            //                 validated: a timed one (main), one set by a
            //                 third party (W -> S -> flag2). ret eax=0 (seen)
            //                 / eax=1 (timeout).
            // The verdict is ALWAYS accompanied by the net's counters (never a
            // silent PASS): on console, detections>0 is REQUIRED (and the
            // negative control D2_FAMINE=0 must FAIL there); on qemu/Vita3K
            // the host preempts / round-robins, so detections~0 is possible
            // and a PASS is still expected — the line states that explicitly.
            // BENCH RULE: assert detections>0, NEVER the nap count — the nap
            // count is non-deterministic BY CONSTRUCTION (a detection pokes
            // up to N Running threads, and which one reaches its seam first
            // varies run to run, all of them healthy). rc: 0=PASS, 1=FAIL
            // (FIDTEST convention); lines go to stdout AND
            // crash.log/boot_progress.
            { const char* ft=getenv("D2_FAMINETEST");
              if(ft && *ft=='1'){
                std::printf("\n=== [FAMINETEST] auto-test du filet anti-famine (natif, filet %s) ===\n",
                            famArmed?"arme":"DESARME");
                // Data cells (absolute [mem] operands for the stubs).
                uint32_t flag=misc(4), flag2=misc(4), count=misc(4), slot=misc(4);
                cpu->write_u32(flag,0); cpu->write_u32(flag2,0);
                cpu->write_u32(count,50);                       // 50 x Sleep(200) = 10 s
                uint32_t sleepTrap=br.shim_trap("KERNEL32.dll","Sleep");
                if(!sleepTrap){                                 // shim_trap returns 0 if the import is absent:
                    d2_crashlog("FAMINETEST: FAIL (shim KERNEL32!Sleep introuvable — slot nul)");
                    return 1; }                                 // FAIL is logged, not a call through a null slot
                cpu->write_u32(slot,sleepTrap);                 // synthetic import slot (call [mem], IAT-style)
                // S — layout : 0:call check(+20) 5:test 7:jz S(-9) 9:mov[flag2],1 19:ret 20:check 25:ret
                uint32_t S=misc(32); { uint8_t s[32]; int k=0; uint32_t one=1;
                    s[k++]=0xE8; { uint32_t rel=20-5; std::memcpy(s+k,&rel,4); k+=4; }   // call check
                    s[k++]=0x85; s[k++]=0xC0;                                            // test eax,eax
                    s[k++]=0x74; s[k++]=0xF7;                                            // jz S (-9)
                    s[k++]=0xC7; s[k++]=0x05; std::memcpy(s+k,&flag2,4); k+=4;           // mov dword[flag2],1
                    std::memcpy(s+k,&one,4); k+=4;
                    s[k++]=0xC3;                                                         // ret
                    s[k++]=0xA1; std::memcpy(s+k,&flag,4); k+=4;                         // check: mov eax,[flag]
                    s[k++]=0xC3;                                                         // ret
                    cpu->write(S,s,(uint32_t)k); }
                // W — push <wsleep> ; call [slot] ; mov dword[flag],1 ; ret
                // D2_FAMINETEST_WSLEEP_MS (default 50, unchanged for
                // console): the qemu bench sets 250 to make main's 2nd
                // iteration (the dec/jnz L path) DETERMINISTIC — without this
                // knob, the re-loop and the 10 s bound would be code that
                // never actually executes.
                uint32_t wsleep=50;
                if(const char* e=getenv("D2_FAMINETEST_WSLEEP_MS")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) wsleep=v; }
                uint32_t W=misc(32); { uint8_t s[32]; int k=0; uint32_t one=1;
                    s[k++]=0x68; std::memcpy(s+k,&wsleep,4); k+=4;                       // push wsleep (imm32)
                    s[k++]=0xFF; s[k++]=0x15; std::memcpy(s+k,&slot,4); k+=4;            // call [slot Sleep]
                    s[k++]=0xC7; s[k++]=0x05; std::memcpy(s+k,&flag,4); k+=4;            // mov dword[flag],1
                    std::memcpy(s+k,&one,4); k+=4;
                    s[k++]=0xC3;                                                         // ret
                    cpu->write(W,s,(uint32_t)k); }
                // main — L: push 200 ; call [slot] ; mov eax,[flag2] ; test ;
                //        jnz done ; dec dword[count] ; jnz L ; mov eax,1 ; ret ;
                //        done: mov eax,0 ; ret
                uint32_t M=misc(48); { uint8_t s[48]; int k=0;
                    { uint32_t v=200; s[k++]=0x68; std::memcpy(s+k,&v,4); k+=4; }        // push 200
                    s[k++]=0xFF; s[k++]=0x15; std::memcpy(s+k,&slot,4); k+=4;            // call [slot Sleep]
                    s[k++]=0xA1; std::memcpy(s+k,&flag2,4); k+=4;                        // mov eax,[flag2]
                    s[k++]=0x85; s[k++]=0xC0;                                            // test eax,eax
                    s[k++]=0x75; s[k++]=0x0E;                                            // jnz done (rel8 = 34-(18+2) = +14)
                    s[k++]=0xFF; s[k++]=0x0D; std::memcpy(s+k,&count,4); k+=4;           // dec dword[count]
                    s[k++]=0x75; s[k++]=0xE4;                                            // jnz L (rel8 = 0-(26+2) = -28 -> off 0; verify against objdump)
                    { uint32_t v=1; s[k++]=0xB8; std::memcpy(s+k,&v,4); k+=4; }          // mov eax,1 (timeout)
                    s[k++]=0xC3;
                    { uint32_t v=0; s[k++]=0xB8; std::memcpy(s+k,&v,4); k+=4; }          // done: mov eax,0
                    s[k++]=0xC3;
                    cpu->write(M,s,(uint32_t)k); }
                GuestThread* mainFT=sched.set_main(M, MAIN_STACK_TOP, MAIN_TIB);
                GuestThread *St=nullptr,*Wt=nullptr;
                { gil::Guard gg;                                 // create_thread's contract (gil::assert_held)
                  St=sched.create_thread(S,0,0,false);           // S BEFORE W (the T12 duel order)
                  Wt=sched.create_thread(W,0,0,false); }
                if(!St||!Wt){
                    d2_crashlog("FAMINETEST: FAIL (create_thread S=%p W=%p)",(void*)St,(void*)Wt);
                    return 1; }
                uint64_t t0=rt_now_us();
                d2vita_progress("scheduler.run() entered (FAMINETEST)");
                sched.run();
                uint32_t wall_ms=(uint32_t)((rt_now_us()-t0)/1000ull);
                uint32_t det=nat->fam_detections(), naps=nat->fam_naps();
                uint32_t f1=cpu->read_u32(flag), f2=cpu->read_u32(flag2);
                // PASS = BOTH victims unstarved: main exited via the "seen"
                // branch (exit 0) AND flag2 set by S (S exited its loop). An
                // exit_code != 0/1 (0xC0000005...) = a stub fault, FAIL — the
                // diag line below carries the raw code.
                bool pass = (mainFT->exit_code==0) && f2==1;
                char line[192];
                if(pass)
                    std::snprintf(line,sizeof line,
                        "FAMINETEST: PASS (flag en %ums, detections=%u siestes=%u)%s",
                        wall_ms, det, naps,
                        det==0?" [vacuite: aucun desaffamage exerce — attendu qemu/Vita3K, MENSONGE sur console]":"");
                else if(mainFT->exit_code==1)
                    // eip-spin: S's ctx.eip, best-effort — the last
                    // save_context snapshot (finish_thread at teardown): the
                    // hot loop if S was still spinning at the timeout.
                    std::snprintf(line,sizeof line,
                        "FAMINETEST: FAIL (timeout 10s, detections=%u siestes=%u eip-spin=%08x)",
                        det, naps, St->ctx.eip);
                else
                    // main-exit neither 0 nor 1 = not the timeout branch: a
                    // stub fault (0xC0000005...) or an abnormal exit — name
                    // the real cause rather than lying "timeout".
                    std::snprintf(line,sizeof line,
                        "FAMINETEST: FAIL (sortie anormale main-exit=0x%x, detections=%u siestes=%u eip-spin=%08x)",
                        mainFT->exit_code, det, naps, St->ctx.eip);
                std::printf("  %s\n", line);
                d2_crashlog("%s", line);
                { char d[160]; std::snprintf(d,sizeof d,
                      "faminetest: flag=%u flag2=%u main-exit=0x%x restant=%u sautees-gil=%u wall=%ums wsleep=%ums",
                      f1, f2, mainFT->exit_code, cpu->read_u32(count), nat->fam_gil_skips(), wall_ms, wsleep);
                  std::printf("  %s\n", d); d2_crashlog("%s", d); }
                d2_crashlog("CLEAN EXIT (FAMINETEST)");
                return pass?0:1;
              } }
            // ---- select self-test (D2_SELECTTEST=1, needs D2NET=1) ------------
            // Checks winx86's select against the Winsock contract on REAL host
            // sockets (loopback only): timeouts honored with the GIL released,
            // {0,0} as a poll, ready sockets reported, WSAEINVAL / WSAENOTSOCK /
            // network-down errors returned late rather than at once, a reset
            // connection reported in readfds only, exceptfds-only waits that do
            // not return early. EXCLUSIVE branch (like FAMINETEST): does NOT run
            // Game.exe. Guest actors:
            //   V: walks a row table, calls select(row) then WSAGetLastError,
            //      stores both in the row, then [done]=1
            //   S: spinner crossing a seam each lap; its laps while V waits are
            //      printed, never asserted (host scheduling under qemu)
            //   main: Sleep(50) loop until [done] (verdict at 10 s)
            // Durations come from the WX86_NET_SELECT events (host clock inside
            // the shim), so they are exact lower bounds, not guest tick samples.
            { const char* st=getenv("D2_SELECTTEST");
              if(st && *st=='1'){
                std::printf("\n=== [SELECTTEST] select Winsock contract ===\n");
                if(!wx86_net_enabled()){
                    d2_crashlog("SELECTTEST: FAIL (D2NET=1 requis)");
                    return 1; }
                uint32_t selTrap=br.shim_trap("WS2_32.dll","#18");
                uint32_t errTrap=br.shim_trap("WS2_32.dll","#111");
                uint32_t sleepTrap=br.shim_trap("KERNEL32.dll","Sleep");
                if(!selTrap||!errTrap||!sleepTrap){
                    d2_crashlog("SELECTTEST: FAIL (trap introuvable select=%08x WSAGetLastError=%08x Sleep=%08x)",selTrap,errTrap,sleepTrap);
                    return 1; }
                // Host sockets, loopback only.
                auto bound=[](int type)->int{
                    int fd=::socket(AF_INET,type,0); if(fd<0) return -1;
                    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                    if(::bind(fd,(sockaddr*)&a,sizeof a)!=0){ ::close(fd); return -1; }
                    return fd; };
                auto addr_of=[](int fd)->sockaddr_in{ sockaddr_in a{}; socklen_t l=sizeof a; ::getsockname(fd,(sockaddr*)&a,&l); return a; };
                int fIdle=bound(SOCK_DGRAM), fReady=bound(SOCK_DGRAM), fTx=bound(SOCK_DGRAM), fL=bound(SOCK_STREAM);
                int fRst=::socket(AF_INET,SOCK_STREAM,0), fWr=::socket(AF_INET,SOCK_STREAM,0);
                bool sockOk = fIdle>=0 && fReady>=0 && fTx>=0 && fL>=0 && fRst>=0 && fWr>=0 && ::listen(fL,4)==0;
                int fAccRst=-1, fAccWr=-1;
                if(sockOk){
                    sockaddr_in la=addr_of(fL), ra=addr_of(fReady);
                    sockOk = ::sendto(fTx,"x",1,0,(sockaddr*)&ra,sizeof ra)==1
                          && ::connect(fRst,(sockaddr*)&la,sizeof la)==0
                          && ::connect(fWr,(sockaddr*)&la,sizeof la)==0
                          && (fAccRst=::accept(fL,nullptr,nullptr))>=0
                          && (fAccWr=::accept(fL,nullptr,nullptr))>=0;
                    // Close with linger 0: the peer receives an RST, not a FIN.
                    struct { int onoff, secs; } lg{1,0};
                    if(sockOk){ ::setsockopt(fAccRst,SOL_SOCKET,SO_LINGER,&lg,sizeof lg); ::close(fAccRst); fAccRst=-1; }
                }
                if(!sockOk){ d2_crashlog("SELECTTEST: FAIL (sockets loopback impossibles, errno=%d)",errno); return 1; }
                const uint32_t hIdle=wx86_sock_alloc(fIdle), hReady=wx86_sock_alloc(fReady),
                               hRst=wx86_sock_alloc(fRst), hWr=wx86_sock_alloc(fWr);
                const uint32_t hBad=0xBAD0BAD0u;
                auto tvbuf=[&](int32_t sec,int32_t usec){ uint32_t t=misc(8); cpu->write_u32(t,(uint32_t)sec); cpu->write_u32(t+4,(uint32_t)usec); return t; };
                auto fdset=[&](std::initializer_list<uint32_t> hs){ uint32_t f=misc(4+4*(uint32_t)hs.size()); uint32_t i=0;
                    cpu->write_u32(f,(uint32_t)hs.size()); for(uint32_t h:hs) cpu->write_u32(f+4+4*i++,h); return f; };
                const uint32_t tv100=tvbuf(0,100000), tv0=tvbuf(0,0), tv1s=tvbuf(1,0), tvNeg=tvbuf(-1,0);
                struct Row { uint32_t rd,wr,ex,tv; int32_t eax; uint32_t err; uint32_t minMs; };
                // err = expected WSAGetLastError when eax is -1 (10093 once the
                // network is off: that is what WSAGetLastError reports then).
                std::vector<Row> rows;
                for(int i=0;i<5;i++) rows.push_back({fdset({hIdle}),0,0,tv100, 0,0,100});   // 0-4 timeout, full wait
                rows.push_back({fdset({hIdle}),0,0,tv0,          0,0,0});                  // 5  {0,0} poll
                rows.push_back({fdset({hReady}),0,0,tv1s,        1,0,0});                  // 6  pending datagram
                rows.push_back({0,0,0,tv100,                    -1,10022,100});            // 7  all sets NULL
                rows.push_back({fdset({}),0,0,tv100,            -1,10022,100});            // 8  only empty sets
                rows.push_back({fdset({hBad}),0,0,tv100,        -1,10038,100});            // 9  not a socket
                rows.push_back({fdset({hIdle}),0,0,tvNeg,       -1,10022,1000});           // 10 invalid timeval
                rows.push_back({fdset({hRst}),0,fdset({hRst}),tv1s, 1,0,0});               // 11 reset: readfds only
                rows.push_back({0,0,fdset({hIdle}),tv100,        0,0,100});                // 12 exceptfds only
                rows.push_back({0,fdset({hWr}),0,tv1s,           1,0,0});                  // 13 connected: writable
                rows.push_back({fdset({hIdle}),0,0,tv100,       -1,10093,100});            // 14 network switched off
                const uint32_t kRowBytes=28, nRows=(uint32_t)rows.size();
                uint32_t table=misc(kRowBytes*nRows);
                for(uint32_t i=0;i<nRows;i++){ uint32_t r=table+i*kRowBytes;
                    cpu->write_u32(r,0); cpu->write_u32(r+4,rows[i].rd); cpu->write_u32(r+8,rows[i].wr);
                    cpu->write_u32(r+12,rows[i].ex); cpu->write_u32(r+16,rows[i].tv);
                    cpu->write_u32(r+20,0x5A5A5A5Au); cpu->write_u32(r+24,0x5A5A5A5Au); }
                // Events in call order (only V calls select). After row 13 the
                // network is switched off so row 14 exercises that path.
                static std::vector<WsockEvent> s_selEv; s_selEv.clear();
                wx86_net_set_observer([](const WsockEvent& e){
                    if(e.kind==WX86_NET_SELECT){ s_selEv.push_back(e); if(s_selEv.size()==14) wx86_net_set_enabled(false); }
                    d2_net_observe(e); });
                uint32_t done=misc(4), spins=misc(4), cnt=misc(4), cntM=misc(4), slotS=misc(4), slotE=misc(4), slotZ=misc(4);
                cpu->write_u32(done,0); cpu->write_u32(spins,0); cpu->write_u32(cnt,nRows); cpu->write_u32(cntM,200);
                cpu->write_u32(slotS,selTrap); cpu->write_u32(slotE,errTrap); cpu->write_u32(slotZ,sleepTrap);
                uint32_t V=misc(64); { uint8_t b[64]; int k=0; uint32_t one=1;
                    b[k++]=0xBE; std::memcpy(b+k,&table,4); k+=4;                 // mov esi, table
                    const int L=k;
                    b[k++]=0xFF; b[k++]=0x76; b[k++]=0x10;                        // push [esi+16] timeout
                    b[k++]=0xFF; b[k++]=0x76; b[k++]=0x0C;                        // push [esi+12] exceptfds
                    b[k++]=0xFF; b[k++]=0x76; b[k++]=0x08;                        // push [esi+8]  writefds
                    b[k++]=0xFF; b[k++]=0x76; b[k++]=0x04;                        // push [esi+4]  readfds
                    b[k++]=0xFF; b[k++]=0x36;                                     // push [esi]    nfds
                    b[k++]=0xFF; b[k++]=0x15; std::memcpy(b+k,&slotS,4); k+=4;    // call [select] (stdcall)
                    b[k++]=0x89; b[k++]=0x46; b[k++]=0x14;                        // mov [esi+20],eax
                    b[k++]=0xFF; b[k++]=0x15; std::memcpy(b+k,&slotE,4); k+=4;    // call [WSAGetLastError]
                    b[k++]=0x89; b[k++]=0x46; b[k++]=0x18;                        // mov [esi+24],eax
                    b[k++]=0x83; b[k++]=0xC6; b[k++]=(uint8_t)kRowBytes;          // add esi,28
                    b[k++]=0xFF; b[k++]=0x0D; std::memcpy(b+k,&cnt,4); k+=4;      // dec dword[cnt]
                    b[k++]=0x75; b[k]=(uint8_t)(int8_t)(L-(k+1)); k++;            // jnz L
                    b[k++]=0xC7; b[k++]=0x05; std::memcpy(b+k,&done,4); k+=4;     // mov dword[done],1
                    std::memcpy(b+k,&one,4); k+=4;
                    b[k++]=0xC3;
                    cpu->write(V,b,(uint32_t)k); }
                // S — 0:inc[spins] 6:call check(+16) 11:test 13:jz S(-15) 15:ret 16:check mov eax,[done] 21:ret
                uint32_t S2=misc(32); { uint8_t b[32]; int k=0;
                    b[k++]=0xFF; b[k++]=0x05; std::memcpy(b+k,&spins,4); k+=4;    // inc dword[spins]
                    b[k++]=0xE8; { uint32_t rel=5; std::memcpy(b+k,&rel,4); k+=4; } // call check
                    b[k++]=0x85; b[k++]=0xC0;                                     // test eax,eax
                    b[k++]=0x74; b[k++]=0xF1;                                     // jz S (-15)
                    b[k++]=0xC3;                                                  // ret
                    b[k++]=0xA1; std::memcpy(b+k,&done,4); k+=4;                  // check: mov eax,[done]
                    b[k++]=0xC3;
                    cpu->write(S2,b,(uint32_t)k); }
                // main — same layout as FAMINETEST (offsets verified there)
                uint32_t M2=misc(48); { uint8_t b[48]; int k=0;
                    { uint32_t v=50; b[k++]=0x68; std::memcpy(b+k,&v,4); k+=4; }
                    b[k++]=0xFF; b[k++]=0x15; std::memcpy(b+k,&slotZ,4); k+=4;
                    b[k++]=0xA1; std::memcpy(b+k,&done,4); k+=4;
                    b[k++]=0x85; b[k++]=0xC0;
                    b[k++]=0x75; b[k++]=0x0E;
                    b[k++]=0xFF; b[k++]=0x0D; std::memcpy(b+k,&cntM,4); k+=4;
                    b[k++]=0x75; b[k++]=0xE4;
                    { uint32_t v=1; b[k++]=0xB8; std::memcpy(b+k,&v,4); k+=4; }
                    b[k++]=0xC3;
                    { uint32_t v=0; b[k++]=0xB8; std::memcpy(b+k,&v,4); k+=4; }
                    b[k++]=0xC3;
                    cpu->write(M2,b,(uint32_t)k); }
                GuestThread* mainST=sched.set_main(M2, MAIN_STACK_TOP, MAIN_TIB);
                GuestThread *Vt=nullptr,*Sp=nullptr;
                { gil::Guard gg;
                  Vt=sched.create_thread(V,0,0,false);
                  Sp=sched.create_thread(S2,0,0,false); }
                if(!Vt||!Sp){ d2_crashlog("SELECTTEST: FAIL (create_thread V=%p S=%p)",(void*)Vt,(void*)Sp); return 1; }
                uint64_t t0=rt_now_us();
                d2vita_progress("scheduler.run() entered (SELECTTEST)");
                sched.run();
                uint32_t wall=(uint32_t)((rt_now_us()-t0)/1000ull);
                sel_report(true);
                int fails=0;
                auto chk=[&](bool ok, uint32_t row, const char* what, uint32_t got){
                    if(ok) return;
                    ++fails; char m[160]; std::snprintf(m,sizeof m,"SELECTTEST: ligne %u : %s (obtenu 0x%x)",row,what,got);
                    std::printf("  %s\n",m); d2_crashlog("%s",m); };
                chk(mainST->exit_code==0 && cpu->read_u32(done)==1, 0, "V n'a pas fini dans les 10 s", mainST->exit_code);
                chk(s_selEv.size()==nRows, 0, "nombre d'evenements WX86_NET_SELECT", (uint32_t)s_selEv.size());
                chk(g_sel.calls==nRows, 0, "recensement g_sel.calls", g_sel.calls);
                for(uint32_t i=0;i<nRows && i<s_selEv.size();i++){
                    const uint32_t r=table+i*kRowBytes; const Row& x=rows[i];
                    const uint32_t eax=cpu->read_u32(r+20), err=cpu->read_u32(r+24);
                    chk(eax==(uint32_t)x.eax, i, "valeur de retour", eax);
                    if(x.eax<0) chk(err==x.err, i, "WSAGetLastError", err);
                    chk(s_selEv[i].waited_ms>=x.minMs, i, "duree minimale (ms)", s_selEv[i].waited_ms);
                }
                // Set contents after the call.
                auto cntOf=[&](uint32_t f){ return cpu->read_u32(f); };
                auto first=[&](uint32_t f){ return cpu->read_u32(f+4); };
                for(uint32_t i=0;i<=5;i++) chk(cntOf(rows[i].rd)==0, i, "readfds non vide apres expiration", cntOf(rows[i].rd));
                chk(cntOf(rows[6].rd)==1 && first(rows[6].rd)==hReady, 6, "readfds du datagramme", cntOf(rows[6].rd));
                chk(cntOf(rows[11].rd)==1 && first(rows[11].rd)==hRst, 11, "RST absent de readfds", cntOf(rows[11].rd));
                chk(cntOf(rows[11].ex)==0, 11, "RST signale dans exceptfds", cntOf(rows[11].ex));
                chk(cntOf(rows[12].ex)==0, 12, "exceptfds non vide apres expiration", cntOf(rows[12].ex));
                chk(cntOf(rows[13].wr)==1 && first(rows[13].wr)==hWr, 13, "writefds du socket connecte", cntOf(rows[13].wr));
                wx86_net_set_enabled(true);
                for(uint32_t h:{hIdle,hReady,hRst,hWr}) wx86_sock_erase(h);
                for(int fd:{fIdle,fReady,fTx,fL,fRst,fWr,fAccWr}) if(fd>=0) ::close(fd);
                char line[200];
                std::snprintf(line,sizeof line,"SELECTTEST: %s (%u appels, %d echec(s), dans-select=%llums wall=%ums tours-spinner=%u main-exit=0x%x)",
                    fails?"FAIL":"PASS", nRows, fails, (unsigned long long)g_sel.waitedMs, wall, cpu->read_u32(spins), mainST->exit_code);
                std::printf("  %s\n", line); d2_crashlog("%s", line);
                d2_crashlog("CLEAN EXIT (SELECTTEST)");
                return fails?1:0;
              } }
            // ---- Critical-section CONCURRENCY self-test (D2_CSTEST=1)
            // WHY THIS BENCH EXISTS: a critical section is a SYNCHRONIZATION
            // object. The FBHASH pixel fingerprint proves the image doesn't
            // change; it proves NOTHING about mutual exclusion or
            // re-entrancy, because the game's measured contention is
            // 0.0003% — a BROKEN exclusion path would still pass the pixel
            // oracle. This bench puts THREE guest threads in contention over
            // ONE section and checks, from guest code itself, two Win32
            // invariants:
            //   (1) EXCLUSION: an occupancy counter incremented inside the
            //       section must NEVER exceed 1, including during a Sleep(1)
            //       that forces a mid-section switch;
            //   (2) RECURSION: Enter/Enter then a SINGLE Leave leaves the
            //       section HELD (recursion count 2 -> 1). The bench sleeps
            //       again at that point: if the first Leave had released it,
            //       another thread would enter and occupancy would reach 2.
            //       That's violrec.
            // Plus a DETERMINISTIC host-side sequence, before any thread, on
            // TryEnterCriticalSection: free success, RECURSIVE success, then
            // two Leaves — read DIRECTLY from KCrit (count/owner), independent
            // of any print output.
            // EXCLUSIVE branch (same as FAMINETEST/SELECTTEST): does NOT run
            // Game.exe. BOTH legs of the A/B (knob absent, then
            // D2_CSINTRIN=1) must produce the SAME guest numbers.
            { const char* ct=getenv("D2_CSTEST");
              if(ct && *ct=='1'){
                uint32_t N=200;   // iterations per thread
                if(const char* e=getenv("D2_CSTEST_N")){ uint32_t v=(uint32_t)strtoul(e,nullptr,10); if(v) N=v; }
                std::printf("\n=== [CSTEST] auto-test de concurrence des sections critiques"
                            " (intrinseque %s, %u iterations x 3 fils) ===\n",
                            g_csIntrin?"ARME":"absent", N);
                uint32_t slotE=misc(4), slotL=misc(4), slotT=misc(4), slotZ=misc(4);
                uint32_t vE=br.shim_trap("KERNEL32.dll","EnterCriticalSection");
                uint32_t vL=br.shim_trap("KERNEL32.dll","LeaveCriticalSection");
                uint32_t vT=br.shim_trap("KERNEL32.dll","TryEnterCriticalSection");
                uint32_t vI=br.shim_trap("KERNEL32.dll","InitializeCriticalSection");
                uint32_t vZ=br.shim_trap("KERNEL32.dll","Sleep");
                if(!vE||!vL||!vT||!vI||!vZ){
                    d2_crashlog("CSTEST: FAIL (trap introuvable E=%08x L=%08x T=%08x I=%08x Z=%08x)",vE,vL,vT,vI,vZ);
                    return 1; }
                cpu->write_u32(slotE,vE); cpu->write_u32(slotL,vL);
                cpu->write_u32(slotT,vT); cpu->write_u32(slotZ,vZ);
                // TryEnterCriticalSection is not imported by 1.14d: its slot
                // was JUST allocated, so the general arming pass
                // (shim_trap_existing) never saw it. Armed here instead,
                // otherwise this bench would only exercise TryEnter's shim
                // path.
                if(g_csIntrin && vT) cpu->set_intrinsic(vT,&cs_try_intrinsic);
                uint32_t csva=misc(0x20);
                uint32_t inside=misc(4), viol=misc(4), violrec=misc(4);
                uint32_t tryok=misc(4), tryfail=misc(4);
                uint32_t cnt1=misc(4), cnt2=misc(4), cnt3=misc(4);
                uint32_t done1=misc(4), done2=misc(4), done3=misc(4), cntM=misc(4);
                for(uint32_t a : {inside,viol,violrec,tryok,tryfail,done1,done2,done3}) cpu->write_u32(a,0);
                cpu->write_u32(cnt1,N); cpu->write_u32(cnt2,N); cpu->write_u32(cnt3,N);
                cpu->write_u32(cntM,400);                      // 400 x Sleep(50) = 20 s cap
                // --- tiny labeled x86 assembler (offsets patched, not
                // hand-counted: a miscounted rel8 is a false FAIL)
                struct Asm {
                  std::vector<uint8_t> b;
                  void u8(uint8_t v){ b.push_back(v); }
                  void u32(uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)(v>>(8*i))); }
                  void push_imm(uint32_t v){ u8(0x68); u32(v); }
                  void push_1(){ u8(0x6A); u8(0x01); }
                  void push_b(uint8_t v){ u8(0x6A); u8(v); }
                  void call_ind(uint32_t s){ u8(0xFF); u8(0x15); u32(s); }
                  void inc_m(uint32_t a){ u8(0xFF); u8(0x05); u32(a); }
                  void dec_m(uint32_t a){ u8(0xFF); u8(0x0D); u32(a); }
                  void mov_eax_m(uint32_t a){ u8(0xA1); u32(a); }
                  void and_eax_m(uint32_t a){ u8(0x23); u8(0x05); u32(a); }
                  void cmp_eax_1(){ u8(0x83); u8(0xF8); u8(0x01); }
                  void test_eax(){ u8(0x85); u8(0xC0); }
                  void mov_eax_imm(uint32_t v){ u8(0xB8); u32(v); }
                  void mov_m_imm(uint32_t a,uint32_t v){ u8(0xC7); u8(0x05); u32(a); u32(v); }
                  void ret(){ u8(0xC3); }
                  size_t je32(){ u8(0x0F); u8(0x84); u32(0); return b.size(); }
                  size_t jne32(){ u8(0x0F); u8(0x85); u32(0); return b.size(); }
                  size_t jmp32(){ u8(0xE9); u32(0); return b.size(); }
                  void patch(size_t end,size_t tgt){ int32_t r=(int32_t)((long)tgt-(long)end);
                      for(int i=0;i<4;i++) b[end-4+i]=(uint8_t)(((uint32_t)r)>>(8*i)); }
                };
                auto emit_stub=[&](Asm& a)->uint32_t{
                    uint32_t va=misc((uint32_t)a.b.size());
                    cpu->write(va,a.b.data(),(uint32_t)a.b.size()); return va; };
                // W(cnt,done): Enter / Enter / occupy / Sleep / check /
                //              Leave / Sleep / RECURSIVE check / Leave
                auto build_W=[&](uint32_t cnt,uint32_t done)->uint32_t{
                    Asm a; const size_t L=0;
                    a.push_imm(csva); a.call_ind(slotE);            // Enter          (recursion 0->1)
                    a.push_imm(csva); a.call_ind(slotE);            // Enter RECURSIVE (1->2)
                    a.inc_m(inside);
                    a.push_1(); a.call_ind(slotZ);                  // Sleep(1) mid-section
                    a.mov_eax_m(inside); a.cmp_eax_1();
                    { size_t j=a.je32(); a.inc_m(viol); a.patch(j,a.b.size()); }
                    a.push_imm(csva); a.call_ind(slotL);            // Leave (2->1): must NOT release
                    a.push_1(); a.call_ind(slotZ);                  // Sleep(1) with recursion=1
                    a.mov_eax_m(inside); a.cmp_eax_1();
                    { size_t j=a.je32(); a.inc_m(violrec); a.patch(j,a.b.size()); }
                    a.dec_m(inside);
                    a.push_imm(csva); a.call_ind(slotL);            // Leave (1->0): releases
                    // Rest LONGER than T's step: without it the two W threads
                    // hand the section back and forth without a gap and
                    // TryEnter fails 200 times out of 200 — the "TryEnter
                    // SUCCEEDS under concurrency" path would then never be
                    // exercised.
                    a.push_b(5); a.call_ind(slotZ);
                    a.dec_m(cnt); { size_t j=a.jne32(); a.patch(j,L); }
                    a.mov_m_imm(done,1); a.ret();
                    return emit_stub(a); };
                // T: TryEnter in a loop — a success must obey the SAME
                // exclusion (it increments the same occupancy counter).
                auto build_T=[&]()->uint32_t{
                    Asm a; const size_t L=0;
                    a.push_imm(csva); a.call_ind(slotT);
                    a.test_eax(); size_t jz=a.je32();
                    a.inc_m(inside);
                    a.push_1(); a.call_ind(slotZ);
                    a.mov_eax_m(inside); a.cmp_eax_1();
                    { size_t j=a.je32(); a.inc_m(viol); a.patch(j,a.b.size()); }
                    a.dec_m(inside);
                    a.push_imm(csva); a.call_ind(slotL);
                    a.inc_m(tryok);
                    size_t jn=a.jmp32();
                    a.patch(jz,a.b.size()); a.inc_m(tryfail);
                    a.patch(jn,a.b.size());
                    a.push_1(); a.call_ind(slotZ);
                    a.dec_m(cnt3); { size_t j=a.jne32(); a.patch(j,L); }
                    a.mov_m_imm(done3,1); a.ret();
                    return emit_stub(a); };
                auto build_M=[&]()->uint32_t{
                    Asm a; const size_t L=0;
                    a.push_b(50); a.call_ind(slotZ);
                    a.mov_eax_m(done1); a.and_eax_m(done2); a.and_eax_m(done3);
                    a.test_eax(); size_t jout=a.jne32();
                    a.dec_m(cntM); { size_t j=a.jne32(); a.patch(j,L); }
                    a.mov_eax_imm(1); a.ret();                      // plafond atteint
                    a.patch(jout,a.b.size());
                    a.mov_eax_imm(0); a.ret();                      // tous finis
                    return emit_stub(a); };
                // --- phase 1: InitializeCriticalSection + a DETERMINISTIC
                // TryEnter sequence, before any thread. It goes through the
                // SAME trap-dispatch path as the game (so through the
                // intrinsic when armed) and is checked directly against
                // KCrit.
                uint32_t rr=0; const char* ff=nullptr; int p1=0, p1f=0;
                auto CK=[&](const char* what,bool ok){ if(ok) ++p1; else { ++p1f;
                    std::printf("  [CSTEST] ECHEC phase 1 : %s\n",what); } };
                CK("InitializeCriticalSection", br.call_va(vI,{csva},rr,&ff));
                KCrit* K0=crit_for(csva);
                CK("etat initial libre (count=0 owner=0)", K0->count==0 && K0->owner==0);
                CK("TryEnter sur section LIBRE rend TRUE", br.call_va(vT,{csva},rr,&ff) && rr==1u);
                CK("apres 1 TryEnter : count=1", K0->count==1 && K0->owner!=0);
                uint32_t own1=K0->owner;
                CK("TryEnter RECURSIF (meme fil) rend TRUE", br.call_va(vT,{csva},rr,&ff) && rr==1u);
                CK("apres 2 TryEnter : count=2, meme proprietaire", K0->count==2 && K0->owner==own1);
                CK("Leave #1", br.call_va(vL,{csva},rr,&ff));
                CK("apres Leave #1 : count=1 et section TOUJOURS DETENUE",
                   K0->count==1 && K0->owner==own1);
                CK("Leave #2", br.call_va(vL,{csva},rr,&ff));
                CK("apres Leave #2 : count=0 et section LIBRE", K0->count==0 && K0->owner==0);
                // --- phase 2 : trois fils invites en dispute
                uint32_t W1=build_W(cnt1,done1), W2=build_W(cnt2,done2);
                uint32_t T3=build_T(), M3=build_M();
                GuestThread* mainCT=sched.set_main(M3, MAIN_STACK_TOP, MAIN_TIB);
                GuestThread *t1=nullptr,*t2=nullptr,*t3=nullptr;
                { gil::Guard gg;
                  t1=sched.create_thread(W1,0,0,false);
                  t2=sched.create_thread(W2,0,0,false);
                  t3=sched.create_thread(T3,0,0,false); }
                if(!t1||!t2||!t3){ d2_crashlog("CSTEST: FAIL (create_thread %p %p %p)",
                    (void*)t1,(void*)t2,(void*)t3); return 1; }
                uint64_t t0=rt_now_us();
                d2vita_progress("scheduler.run() entered (CSTEST)");
                sched.run();
                uint32_t wall=(uint32_t)((rt_now_us()-t0)/1000ull);
                const uint32_t nViol=cpu->read_u32(viol), nRec=cpu->read_u32(violrec);
                const uint32_t nIn=cpu->read_u32(inside);
                const uint32_t nOk=cpu->read_u32(tryok), nKo=cpu->read_u32(tryfail);
                const uint32_t r1=cpu->read_u32(cnt1), r2=cpu->read_u32(cnt2), r3=cpu->read_u32(cnt3);
                KCrit* Kf=crit_for(csva);
                bool ok = (p1f==0)
                       && mainCT->exit_code==0
                       && cpu->read_u32(done1)==1 && cpu->read_u32(done2)==1 && cpu->read_u32(done3)==1
                       && nViol==0 && nRec==0 && nIn==0
                       && r1==0 && r2==0 && r3==0
                       && (nOk+nKo)==N
                       && (g_schedNative || nOk>0)   // coop = deterministic: the "TryEnter
                                                     // SUCCEEDS" path MUST be exercised.
                                                     // Under native the interleaving depends
                                                     // on the host: printed, not required.
                       && Kf->count==0 && Kf->owner==0;
                char line[240];
                std::snprintf(line,sizeof line,
                    "CSTEST: %s (intrinseque=%s phase1=%d/%d viol-exclusion=%u viol-recursion=%u"
                    " occupation-finale=%u tryenter ok=%u ko=%u restes=%u/%u/%u wall=%ums exit=0x%x)",
                    ok?"PASS":"FAIL", g_csIntrin?"1":"0", p1, p1+p1f, nViol, nRec, nIn,
                    nOk, nKo, r1, r2, r3, wall, mainCT->exit_code);
                std::printf("  %s\n", line); d2_crashlog("%s", line);
                cs_intrin_report();
                d2_crashlog("CLEAN EXIT (CSTEST)");
                return ok?0:1;
              } }
            GuestThread* mainT=sched.set_main(g->entry_va(), MAIN_STACK_TOP, MAIN_TIB);
            // Keep the guest tick page's base in step with virtual time — the
            // guest-inlined GetTickCount stub reads it without trapping.
            if(g_rtwant){   // switch to the real clock, continuous with the virtual tick
                g_tickOfs = tick_now(*cpu) - (uint32_t)rt_now_ms_scaled();
                g_realclock = true;
                d2vita_progress(g_rtScale>1?"clock: real-time mode on (D2_RTSCALE)":"clock: real-time mode on");
            }
            { static Cpu* TC=cpu; sched.set_time_sink([](uint64_t ms){
                  if(g_tickvirt) return;   // diag: [T+4] frozen -> tick = pure per-call counter
                  if(g_realclock)
                      TC->write_u32(g_tickVA+4, g_tickOfs+(uint32_t)rt_now_ms_scaled()-TC->read_u32(g_tickVA));
                  else
                      TC->write_u32(g_tickVA+4,0x10000+(uint32_t)ms); }); }
            if(g_realclock && coop){   // coop-only knobs (native: real-clock by construction)
                coop->set_real_clock([]{ return rt_now_ms_scaled(); },
                                     [](uint32_t ms){ rt_sleep_ms_scaled(ms); });
                const char* rx=getenv("D2_RTEXPIRE");
                if(rx&&*rx=='0') coop->set_real_expire(false);   // diagnostic: idle-only expiry
                // Halt-316 containment (see sched_cooperative.h defer_towake_):
                // no timed-wait EXPIRY delivery while a budget-preempted thread
                // has not reached a voluntary boundary.
                { const char* dw=getenv("D2_TOWAKE_DEFER");
                  if(dw&&*dw=='1'){ coop->set_defer_towake(true);
                      d2vita_progress("sched: timed-wake defer ON (halt-316 containment)"); } }
            }
#ifdef PROF_COUNTERS
            // Per-phase profile dump (§1.4): cumulative counters — a phase's own
            // cost is the delta from the previous dump (aggregated by the report
            // script). run_ns only covers scheduler slices, so ratios are exact
            // from P1 onward; P0 (pre-scheduler DllMain) shows in traps + wall.
            g_profT0=prof::now_ns();
            static CooperativeScheduler* PSC=g_schedCoop; static Bridge* PBR=&br;
            g_profDump=[](const char* lbl){
                uint64_t wall=prof::now_ns()-g_profT0;
                double w=wall/1e9, r=prof::run_ns/1e9, t=prof::trap_ns/1e9;
                std::printf("=== PROFILE %s ===\n", lbl);
                std::printf("  wall=%.2fs  run=%.2fs (%.1f%%)  [dynarec=%.2fs (%.1f%%) | shims=%.2fs (%.1f%%), %llu calls]  outside-run=%.2fs\n",
                    w, r, w?100*r/w:0, r-t, w?100*(r-t)/w:0, t, w?100*t/w:0,
                    (unsigned long long)prof::trap_calls, w-r);
                std::printf("  waits: %llu calls, %llu immediate (%.1f%%)  cs: %llu enter, %llu contended (%.4f%%)\n",
                    (unsigned long long)prof::wait_calls,(unsigned long long)prof::wait_immediate,
                    prof::wait_calls?100.0*prof::wait_immediate/prof::wait_calls:0,
                    (unsigned long long)prof::cs_enter,(unsigned long long)prof::cs_contended,
                    prof::cs_enter?100.0*prof::cs_contended/prof::cs_enter:0);
                if(PSC) std::printf("  switches=%llu  frames=%d\n",(unsigned long long)PSC->switches(),g_frame);
                std::printf("  intrinsics: %llu GetTickCount/timeGetTime calls serviced inline (0 Bridge round-trips)\n",
                    (unsigned long long)g_tickIntrinHits);
                PBR->dump_prof(25);
                if(PSC){ std::printf("  [prof threads]\n"); PSC->dump_thread_times(); }
                std::printf("  [prof eip] %llu preempt samples, top 20 hot blocks (64B buckets):\n",
                    (unsigned long long)prof::eip_samples());
                prof::top_eips(20,[](uint32_t eip,uint64_t hits,void*){
                    std::printf("    %8llu  %s\n",(unsigned long long)hits,
                        g_locp?(*g_locp)(eip).c_str():"?"); },nullptr);
                std::fflush(stdout);
            };
#endif
            if(coop){
                if(getenv("MAXSW")) coop->set_max_switches(strtoull(getenv("MAXSW"),nullptr,10));
                if(getenv("QUANTUM")) coop->set_quantum((uint32_t)strtoul(getenv("QUANTUM"),nullptr,10));
            } else if(getenv("MAXSW")||getenv("QUANTUM"))
                d2vita_progress("natif: MAXSW/QUANTUM ignores (mecanique coop)");   // spec D5: declared, not simulated
            d2vita_progress("scheduler.run() entered");
            if(coop)   // coop: famine pointers are null => the fam= field is ABSENT from the alive line
                d2vita_watchdog_start(&g_pmN, &g_frame, &g_readN, cpu->ip_ptr(), coop->switches_ptr(),
                                  &g_ioUs, &dyn86_fill_ns, &dyn86_fill_count, nullptr, nullptr);
            else   // native: heartbeat "switches" = total wakeups; + famine counters (fam=<det>/<naps>)
                d2vita_watchdog_start(&g_pmN, &g_frame, &g_readN, cpu->ip_ptr(), nat->wakes_ptr(),
                                  &g_ioUs, &dyn86_fill_ns, &dyn86_fill_count,
                                  nat->fam_detections_ptr(), nat->fam_naps_ptr());
            sched.run();
            jp_finish(g_frame);   // D2_JITPROFILE: safety net if the POST window couldn't close
            fp_dump();            // D2_FRAMEPROF: histogram + the 24 slowest frames
            gr_inventory_dump();  // D2_GLIDEINV: Glide states ACTUALLY emitted (silent without the knob)
            // Top-10 trap slots by NUMBER OF HITS (runtime/trapcnt.h).
            // Deliberately OUTSIDE PROF_COUNTERS: it's the unit-price
            // denominator for everything that goes through a trap, and it
            // must exist in the SHIPPED binary — a price borrowed from
            // another operation silently imports its error. Unlike
            // dump_prof, it also counts slots served as an intrinsic.
            // Published via d2vita_progress (the only readable channel on
            // console) as well as printf.
            // D2_TRAPCNT_TOP=<n> widens the list (default 10): the top 10 is
            // enough to point at a hot spot, not to price an exact slot
            // already known to be lukewarm — and a unit price borrowed from
            // another operation silently imports its error.
            { const char* e=getenv("D2_TRAPCNT_TOP"); int n=e?atoi(e):10;
              br.dump_trap_counts(n>0?n:10); }
            signtag_dump();       // D2_SIGNTAG: "complemented pointer" patterns seen / rewritten
            mmu_dump();           // fastmmu: folded absolutes / chained stack links
            // Per-thread state resolutions, PER SLOT. Inert (a single
            // "NOTHING TO REPORT" line) outside a -DD2_TLSCOUNT build.
            { const char* e=getenv("D2_TLSCNT_TOP"); int n=e?atoi(e):14;
              br.dump_tls_counts(n>0?n:14); }
#ifdef PROF_COUNTERS
            if(g_profDump) g_profDump("final");
#endif
            // Interlocked*: the share that's genuinely atomic. Printed as
            // soon as a single call has happened, and ALWAYS with both
            // numbers: "atomic=N fallback=0" is proof, "fallback=0" alone
            // would be indistinguishable from "the counter doesn't exist".
            unsigned long long g_ilkAtomic=0,g_ilkFallback=0,g_ilkUnaligned=0;
            wx86_ilk_stats(&g_ilkAtomic,&g_ilkFallback,&g_ilkUnaligned);
            if(g_ilkAtomic || g_ilkFallback)
                // The memory ORDER used is part of the record: without it,
                // comparing two runs means trusting memory of what was set.
            {   // std::printf ALONE is MUTE on console (no stdout): a report
                // could print nothing even though the counters existed.
                // d2_crashlog is the only readable channel on hardware — same
                // reason as the blocks/thread line further below.
                char il[192];
                std::snprintf(il, sizeof il,
                    "interlocked: atomiques=%llu replis=%llu (dont non-alignes=%llu) | ordre=%s",
                    g_ilkAtomic, g_ilkFallback, g_ilkUnaligned,
                    wx86_ilk_seqcst() ? "SEQ_CST (WX86_ILK_SEQCST=1)" : "RELAXED (mono-coeur epingle)");
                std::printf("  %s\n", il); d2_crashlog("%s", il); }
#ifdef D2_TLSCOUNT
            // emutls counting (a MEASUREMENT build, never shipped, no default
            // changed). Printed on BOTH backends: coop is the shipped
            // default, and E() resolves t_emu there exactly the same way (a
            // single host thread doesn't make the __thread variable any less
            // __thread).
            { std::printf("  [tlscount] t_emu=%llu traps=%llu => %.2f resolutions/trap (BORNE INF: t_* de bridge/sched non comptes)\n",
                  d2_tls_hits, d2_tls_traps,
                  d2_tls_traps ? (double)d2_tls_hits/(double)d2_tls_traps : 0.0); }
#endif
            if(coop) std::printf("  wakes: signal=%llu timeout=%llu  io=%llums jit=%llums n=%u\n",
                        (unsigned long long)coop->wake_signal_,(unsigned long long)coop->wake_timeout_,
                        (unsigned long long)(g_ioUs/1000ull),(unsigned long long)(dyn86_fill_ns/1000000ull),dyn86_fill_count);
            else if(nat){ std::printf("  wakes(native): signal=%llu timeout=%llu  io=%llums jit=%llums n=%u\n",
                        (unsigned long long)nat->wake_signal_,(unsigned long long)nat->wake_timeout_,
                        (unsigned long long)(g_ioUs/1000ull),(unsigned long long)(dyn86_fill_ns/1000000ull),dyn86_fill_count);
                // Per-THREAD block census (Amdahl gate for spreading threads
                // across cores). ONE line at teardown, zero cost at runtime:
                // the counter is already kept by translated blocks' prologue.
                // This is what tells whether spreading threads across three
                // cores could gain anything, BEFORE writing a single barrier.
                { char bc[320];
                  if(nat->blocks_census(bc,sizeof bc)){
                      std::printf("  %s\n", bc); d2_crashlog(bc); } }
                // Armed at boot but fam_armed() came back false = the
                // heartbeat's pthread_create failed inside run(): without
                // this line, a post-mortem crash.log would say "net armed"
                // even though the net never actually beat.
                if(famArmed && !nat->fam_armed())
                    d2_crashlog("famine: filet etait INERTE (battement absent)");
                // Anti-starvation net end-of-run stats — only if armed:
                // D2_FAMINE=0 prints ONLY the DISABLED stamp, nothing else.
                // naps >= detections is possible and normal: each epoch takes
                // ONE nap per Running thread (N threads poked = N naps).
                if(nat->fam_armed()){
                    char m[96]; std::snprintf(m,sizeof m,"famine: detections=%u siestes=%u sautees-gil=%u",
                        nat->fam_detections(), nat->fam_naps(), nat->fam_gil_skips());
                    std::printf("  %s\n", m); d2vita_progress(m);
                }
            }
            { extern unsigned long long dyn86_emit_arm_bytes, dyn86_emit_x86_bytes, dyn86_emit_blocks;
              if(dyn86_emit_x86_bytes){
                  unsigned long long r = dyn86_emit_arm_bytes*1000ull/dyn86_emit_x86_bytes;
                  jpline("[emis] blocs=%llu x86=%lluo arm=%lluo expansion=%llu.%03llux",
                         dyn86_emit_blocks, dyn86_emit_x86_bytes, dyn86_emit_arm_bytes,
                         r/1000ull, r%1000ull); } }
            // TRANSLATED BLOCKS EXECUTED over the whole run (base emu; under
            // coop all guest threads share it, so it's the total). This is
            // the only "guest work" metric comparable between two binaries on
            // the deterministic bench: a knob that removes work lowers it, a
            // knob that only moves time around does not. SAYS NOTHING ABOUT
            // TIME: the fps verdict still belongs to console.
            { uint32_t nb=0; if(cpu->thread_emu_blocks(nullptr,&nb) && nb)
                  jpline("[blocs] executes=%u", (unsigned)nb); }
#ifdef D2_EMITPROF
            // EMITTED-CODE PROFILE — measurement build only. Goes through
            // jpline (so d2vita_progress), not printf: a proof counter on
            // stdout doesn't exist on console, and the rule doesn't bend just
            // because this bench happens to run under qemu.
            d2ep_report(+[](const char* l){ jpline("%s", l); });
            { const char* dp = getenv("D2_EMITDUMP");
              int dn = 8; if(const char* ds=getenv("D2_EMITDUMPN")) dn = atoi(ds);
              if(dp && *dp) { d2ep_dump(dp, dn); jpline("[emitprof] vidage ARM brut (%d blocs) -> %s", dn, dp); }
              if(const char* cp=getenv("D2_EMITCSV")) if(*cp) { d2ep_csv(cp); jpline("[emitprof] CSV de tous les blocs -> %s", cp); } }
#endif
            // WATCHDOG STOP, FOR BOTH SCHEDULERS. This call used to live only
            // in the coop branch, so under D2SCHED=native the watchdog kept
            // running through the whole teardown — and its own comment
            // (vita_present.cpp) says what that causes: it touches pointers
            // whose state disappears out from under it, faulting and core
            // dumping on every NORMAL exit. Native is exactly the mode where,
            // on console, the log just stops after the last window: no
            // "scheduler stopped", no final report, no CLEAN EXIT. Even
            // without a full repro of the hang, leaving the watchdog running
            // during teardown isn't defensible in either mode.
            d2vita_watchdog_stop();
            d2vita_present_stop();   // same reason: a live thread during teardown
            // The crash on game CLOSE happens HERE, between the scheduler's
            // return and the end of main — so BEFORE "CLEAN EXIT" and before
            // the function's ordered exit. Three host threads still read
            // guest memory and sceGxm at this point: the 60 Hz replay, the
            // ring's FLUSH thread (since D2_FLUSHFIL), and the GPU itself.
            // They must stop BEFORE the final reports, not after: a report
            // has never crashed a process, but a thread reading an arena
            // mid-teardown will.
            d2gr::r60().stop();
            gr_flush_stop();
            // The audio thread reads the guest arena through a host view:
            // same reason, same place. The sink is closed here, so the proof
            // WAV carries an up-to-date header even if the rest of teardown
            // goes wrong.
            son_codec_report();   // Storm codecs: OURS, no longer the engine's
            d2rt::dsound::shutdown();
            d2gxm_shutdown();
            d2vita_progress("sortie: rejeu + flush + son + GPU arretes avant les rapports");
            // ---- B5: census + NON-EMPTY tests ------------------------------
            // "served" is the non-empty test for BOTH legs: a leg where the
            // clock was never served as an intrinsic proves nothing, neither
            // in pixels nor in sequence. jpline (not printf): this line must
            // exist on console, or the measurement never happened.
            jpline("[b5] servis=%llu | horloge groupee=%s index direct=%s (n=%u)",
                   (unsigned long long)g_tickIntrinHits,
                   g_b5clock?"OUI":"non", d2rt_b5_index_on?"OUI":"non", d2rt_b5_direct_n);
            if(d2rt_b5_calls)   // -DD2_B5CENSUS build only
                jpline("[b5/recensement] appels=%llu lectures-table=%llu servis=%llu emutls=%llu",
                       d2rt_b5_calls, d2rt_b5_loads, d2rt_b5_hits, d2rt_b5_emutls);
            if(g_clockLogN){
                jpline("[b5/horloge] valeurs=%llu empreinte=0x%016llx",
                       (unsigned long long)g_clockSeqCnt,(unsigned long long)g_clockSeqHash);
                for(int r=0;r<4;r++){
                    char b[200]; int o=0;
                    o+=std::snprintf(b+o,sizeof b-o,"[b5/horloge] v%02d:",r*8);
                    for(int k=0;k<8;k++){ unsigned idx=r*8+k;
                        if(idx<g_clockSeqCnt&&idx<32) o+=std::snprintf(b+o,sizeof b-o," %u",g_clockSeqFirst[idx]); }
                    jpline("%s",b); }
            }
            if(coop){   // switches()/dump_threads() = coop diagnostics outside the interface
            std::printf("  scheduler stopped: %s  (%llu switches, %d live threads)  main exit code=0x%08x\n",
                        sched.stop_reason(), (unsigned long long)coop->switches(), sched.live_count(), mainT->exit_code);
            { char m[128]; std::snprintf(m,sizeof m,"scheduler stopped: %s (%llu switches) exit=0x%08x",
                sched.stop_reason(), (unsigned long long)coop->switches(), mainT->exit_code); d2vita_progress(m); }
            // Persist the stop reason to crash.log — the pre-mortem breadcrumb if
            // the run ended here rather than at CLEAN EXIT below.
            d2_crashlog("scheduler stopped: %s | frame=%d switches=%llu main-exit=0x%08x",
                sched.stop_reason(), g_frame, (unsigned long long)coop->switches(), mainT->exit_code);
            coop->dump_threads();
            }
            else {
                // Pre-mortem breadcrumb for a NON-coop backend: stop_reason/
                // live_count are interface methods — a native run ending here
                // must still name why in crash.log (the "lying diagnostic"
                // family). Coop keeps its richer line (with switches) above,
                // byte-identical; d2_crashlog also mirrors this one to stderr
                // and boot_progress, so console/Vita see the reason too.
                d2_crashlog("scheduler stopped: %s | frame=%d main-exit=0x%08x",
                            sched.stop_reason(), g_frame, mainT->exit_code);
            }
            // Same stop_reason/main_exit regardless of which branch
            // above ran — cr_evidence.cpp classifies abnormal_exit from these
            // fields themselves, not from whether CLEAN EXIT also gets
            // logged further down (it can, on this very path: spec §2,
            // "CLEAN EXIT ne prouve rien").
            d2cr::d2cr_session_stopped(sched.stop_reason(), (uint32_t)mainT->exit_code);
            { int held=0;
              wx86_crit_each_held([](uint32_t va,uint32_t owner,int count,void* ud){
                  int& n=*(int*)ud;
                  if(n<12) std::printf("  [crit] cs=0x%08x held by thread %u (count=%d)\n",va,owner,count);
                  n++; }, &held);
              std::printf("  [crit] %d critsec(s) currently held\n",held); }
            std::printf("  [soak] frames=%d  heap now/peak=%u/%u KB  va now/peak=%u/%u KB  host-files=%zu  handles=%zu  msgs: injected=%llu queued=%zu peek=%llu get=%llu dispatch=%llu\n",
                        g_frame, g_heapA.cur()/1024,g_heapA.peak()/1024, g_vaA.cur()/1024,g_vaA.peak()/1024,
                        g_files.size(), (size_t)wx86_handle_count(),
                        (unsigned long long)g_injN, g_msgQ.size(),
                        (unsigned long long)g_pmN,(unsigned long long)g_gmN,(unsigned long long)g_dispN);
            ls_line("final");   // D2_LAZYSEEK (always active): summary of fseeks avoided
            if(g_ioStat) io_line("final");  // D2_IOSTAT: summary of the last window
            // ReadFile length histogram (D2_IOHIST): numeric proof of what
            // Storm's block size actually changes. The seek+read pair is 1:1
            // on this path, so "32 KiB reads" divided by eight must become
            // "256 KiB reads".
            if(g_ioHist>=2){
                std::vector<std::pair<uint32_t,RdSite>> v(g_rdSite.begin(),g_rdSite.end());
                std::sort(v.begin(),v.end(),[](const std::pair<uint32_t,RdSite>&a,const std::pair<uint32_t,RdSite>&b){ return a.second.n>b.second.n; });
                for(size_t i=0;i<v.size()&&i<12;i++)
                    std::printf("  [iosite] %s lectures=%llu octets=%lluK moy=%lluo\n",
                        g_locp?(*g_locp)(v[i].first).c_str():"?",
                        (unsigned long long)v[i].second.n,(unsigned long long)(v[i].second.bytes>>10),
                        (unsigned long long)(v[i].second.bytes/v[i].second.n)); }
            if(g_ioHist){
                std::printf("  [iohist] lectures=%llu octets=%lluK",
                            (unsigned long long)g_readN,(unsigned long long)(g_rdBytes>>10));
                for(int b=0;b<24;b++) if(g_rdHist[b])
                    std::printf("  %u%s=%llu",(b>=20?(1u<<(b-20)):(b>=10?(1u<<(b-10)):(1u<<b))),
                                (b>=20?"M":b>=10?"K":"o"),(unsigned long long)g_rdHist[b]);
                std::printf("\n"); }
            if(g_ioHist && g_cpuForStats && g_d2base)
                // ds:0x7790a0 = block size set by SFileSetConfig(id=2);
                // ds:0x779038 = its working copy (initialized at 0x41b341);
                // ds:0x779050 = the shared buffer allocated at that size. A
                // 0x779038 that STAYS ZERO proves this streaming path was
                // never actually used.
                std::printf("  [storm] ds:7790a0=0x%x ds:779038=0x%x ds:779050=0x%08x\n",
                            g_cpuForStats->read_u32(g_d2base+0x3790a0),
                            g_cpuForStats->read_u32(g_d2base+0x379038),
                            g_cpuForStats->read_u32(g_d2base+0x379050));
            if(g_celWatch && g_cpuForStats && g_d2base){
                uint32_t use=g_cpuForStats->read_u32(g_d2base+0x48db28);
                uint32_t cap=g_cpuForStats->read_u32(g_d2base+0x48db24);
                char m[192]; std::snprintf(m,sizeof m,
                    "[CELDATA/final] usage=%uK/%uK pic=%uK (%u%% du plafond) evictions=%llu (autres caches=%llu)",
                    use>>10,cap>>10,g_celPeak>>10,cap?(unsigned)((uint64_t)g_celPeak*100/cap):0u,
                    (unsigned long long)g_celEvict,(unsigned long long)g_celEvictOther);
                std::printf("  %s\n",m); d2vita_progress(m); }
            std::printf("  [blit] native palette-blit invocations=%llu\n",
                        (unsigned long long)g_blitN);
            if(g_blitFast|g_blitSlow)
                std::printf("  [blit] etendue globale: rapide=%llu repli=%llu (%.3f%% de replis)\n",
                            (unsigned long long)g_blitFast,(unsigned long long)g_blitSlow,
                            100.0*(double)g_blitSlow/(double)(g_blitFast+g_blitSlow));
            // Cell loop: census and native port. Printed HERE because the
            // watchdog's "boucle:" line lives in vita_present.cpp, which is
            // NOT compiled into the qemu oracle's ARM binary — without this
            // line, the oracle's non-empty test (served>0 on one side, 0 on
            // the other) couldn't be checked where it actually runs.
            if(g_cellCalls)
                std::printf("  [cellules] appels=%llu declarees=%llu pures=%llu | rej=%llu plate=%llu clip=%llu horspiste=%llu\n",
                            (unsigned long long)g_cellCalls,(unsigned long long)g_cellDecl,(unsigned long long)g_cellPure,
                            (unsigned long long)g_cellB[1],(unsigned long long)g_cellB[2],
                            (unsigned long long)g_cellB[3],(unsigned long long)(g_cellB[4]+g_cellB[5]));
            { bool any=false; for(int i=0;i<D2_CALLER_N;i++) if(g_callerHit[i]) any=true;
              if(any){ std::printf("  [appelants de la boucle du sol]\n");
                for(int r=0;r<6;r++){ int best=-1;
                  for(int i=0;i<D2_CALLER_N;i++){ if(!g_callerHit[i]) continue;
                    if(best<0||g_callerHit[i]>g_callerHit[best]) best=i; }
                  if(best<0) break;
                  std::printf("      retour=0x%08x  %llu appels\n",
                              g_callerKey[best],(unsigned long long)g_callerHit[best]);
                  g_callerHit[best]=0; } } }
            if(g_shFrames)
                std::printf("  [forme] trames=%llu ev/img=%.1f boucle/img=%.1f avant=%.1f | intrus IMAGE: rle=%.1f blit-repli=%.1f | hors-image=%.1f | apres=%.1f | rafale=%.2fms fenetre=%.2fms (%.0f%% de %.2fms)\n",
                            (unsigned long long)g_shFrames,
                            (double)g_shSumEv/g_shFrames,(double)g_shSumN/g_shFrames,
                            (double)g_shSumBefore/g_shFrames,(double)g_shSumIntrusOther/g_shFrames,
                            (double)g_shSumIntrusBlit/g_shFrames,
                            (double)g_shSumIntrusNodraw/g_shFrames,
                            (double)g_shSumAfter/g_shFrames,
                            (double)g_shSumUsBurst/g_shFrames/1000.0,
                            (double)g_shSumUsWindow/g_shFrames/1000.0,
                            g_shSumUsFrame? 100.0*(double)g_shSumUsWindow/(double)g_shSumUsFrame : 0.0,
                            (double)g_shSumUsFrame/g_shFrames/1000.0);
            if(g_loopServed|g_loopFB[0]|g_loopFB[1]|g_loopFB[2]|g_loopFB[3])
                std::printf("  [boucle] servis=%llu cellules=%llu dont-ECHELLE=%llu (clippees=%llu) repli=%llu (degrade=%llu echelle=%llu vuehote=%llu trop=%llu tables=%llu)\n",
                            (unsigned long long)g_loopServed,(unsigned long long)g_loopCells,
                            (unsigned long long)g_loopScale,(unsigned long long)g_loopScaleClip,
                            (unsigned long long)(g_loopFB[0]+g_loopFB[1]+g_loopFB[2]+g_loopFB[3]+g_loopFB[4]),
                            (unsigned long long)g_loopFB[0],(unsigned long long)g_loopFB[4],
                            (unsigned long long)g_loopFB[1],
                            (unsigned long long)g_loopFB[2],(unsigned long long)g_loopFB[3]);
            // ---- FORK-JOIN (D2_CELLPAR) and OVERLAP (D2_CELLOVL) -----------
            // jpline: on console, printf goes nowhere. Published as soon as
            // the knob is armed, EVEN AT ZERO — an armed-but-ineffective leg
            // and a disarmed leg would otherwise produce the same log.
            if(g_parLive>0 || g_parN>0)
                jpline("cellpar: ouvriers=%d appels=%llu (sequentiels=%llu)"
                       " cellules-ouvriers=%llu jointures=%llu attente-max=%llu moy=%llu"
                       " | vols=%llu chevauchements=%llu bandes-vides=%llu pas-non-aligne=%llu"
                       " echelle-hors-vue=%llu",
                       g_parLive,(unsigned long long)g_parCalls,(unsigned long long)g_parSeqCalls,
                       (unsigned long long)g_parCellsW,(unsigned long long)g_parJoins,
                       (unsigned long long)g_parWaitMax,
                       (unsigned long long)(g_parJoins?g_parWaitSum/g_parJoins:0),
                       (unsigned long long)g_parSteals,(unsigned long long)g_parStraddle,
                       (unsigned long long)g_parDegen,(unsigned long long)g_parPitchOdd,
                       (unsigned long long)g_parScaleBail);
            // LOAD BALANCE. "share requested" is the knob; "share obtained"
            // is what the split ACTUALLY gave the workers, in span bytes and
            // in cells drawn — the two differ, since boundaries snap up to
            // the start of the next line and cells aren't evenly spread
            // across the height.
            // The two wait measurements close the case: the calling thread's
            // wait at the join ("avg=" above), and the worker's wait BEFORE
            // its 1st band, split into prologue / off-call.
            if(g_parLive>0 && g_parJoins){
                const uint64_t sp=g_parSpanC+g_parSpanW, ce=g_parCellsC+g_parCellsW;
                jpline("cellpar-part: demandee=%s obtenue-octets=%llu%% obtenue-cellules=%llu%%"
                       " | attente-appelant=%llu attente-ouvrier-prologue=%llu"
                       " attente-ouvrier-hors-appel=%llu (prises=%llu)",
                       g_parShare?"reglee":"egale",
                       (unsigned long long)(sp?g_parSpanW*100ull/sp:0),
                       (unsigned long long)(ce?g_parCellsW*100ull/ce:0),
                       (unsigned long long)(g_parWaitSum/g_parJoins),
                       (unsigned long long)(g_parWIdleN?g_parWIdlePro/g_parWIdleN:0),
                       (unsigned long long)(g_parWIdleN?g_parWIdleGap/g_parWIdleN:0),
                       (unsigned long long)g_parWIdleN);
                jpline("cellpar-part: part demandee=%u%% (0 = egale) | octets appelant=%llu"
                       " ouvriers=%llu | cellules appelant=%llu ouvriers=%llu",
                       (unsigned)g_parShare,
                       (unsigned long long)g_parSpanC,(unsigned long long)g_parSpanW,
                       (unsigned long long)g_parCellsC,(unsigned long long)g_parCellsW); }
            if(g_ovlOn)
                jpline("cellovl: appels=%llu octets-ecrits=%llu | RECOUVREMENT inter-cellules=%llu"
                       " (meme cellule=%llu) dont VALEUR DIFFERENTE=%llu | non-examines=%llu",
                       (unsigned long long)g_ovlCalls,(unsigned long long)g_ovlWr,
                       (unsigned long long)g_ovlHit,(unsigned long long)g_ovlSelf,
                       (unsigned long long)g_ovlDiff,(unsigned long long)g_ovlNoMap);
            // ---- CELL LOOP CENSUS (D2_CELLSTAT=1) -------------------------
            // Published via jpline: on console, printf goes nowhere. The TWO
            // pass1/pass2 columns are the cross-check — they count the same
            // quantity at two different points in the code.
            if(g_cellStat && g_csCalls){
                char a[24],b[24],d[24],e[24],f[24];
                jp_fr(a,sizeof a,g_csCells,g_csCalls);
                jp_fr(b,sizeof b,g_csPixP2[0]+g_csPixP2[1]+g_csPixP2[2],g_csCells?g_csCells:1);
                jp_fr(d,sizeof d,g_csHP,g_csCells?g_csCells:1);
                jp_fr(e,sizeof e,g_csP2Band,g_csCells?g_csCells:1);
                jp_fr(f,sizeof f,g_csPixP2[2],g_csRunP2?g_csRunP2:1);
                jpline("cellstat: appels=%llu cellules=%llu (%s/appel) | pixels/cellule=%s"
                       " | vuehote/cellule=%s bandes/cellule=%s | run-echelle=%s",
                       (unsigned long long)g_csCalls,(unsigned long long)g_csCells,a,b,d,e,f);
                jpline("cellstat: pixels passe1 plat=%llu clip=%llu echelle=%llu"
                       " | passe2 plat=%llu clip=%llu echelle=%llu",
                       (unsigned long long)g_csPixP1[0],(unsigned long long)g_csPixP1[1],
                       (unsigned long long)g_csPixP1[2],
                       (unsigned long long)g_csPixP2[0],(unsigned long long)g_csPixP2[1],
                       (unsigned long long)g_csPixP2[2]);
                jpline("cellstat: cellules plat=%llu clip=%llu echelle=%llu"
                       " | bandes passe1=%llu passe2=%llu | runs p1=%llu p2=%llu",
                       (unsigned long long)g_csCell3[0],(unsigned long long)g_csCell3[1],
                       (unsigned long long)g_csCell3[2],
                       (unsigned long long)g_csP1Band,(unsigned long long)g_csP2Band,
                       (unsigned long long)g_csRunP1,(unsigned long long)g_csRunP2);
                jpline("cellstat: vuehote=%llu (lut %llu/%llu ligne %llu/%llu)"
                       " | vecteur=%llu o | bandes rapide=%llu lent=%llu | at()=%llu o",
                       (unsigned long long)g_csHP,
                       (unsigned long long)g_csLutHit,(unsigned long long)g_csLutMiss,
                       (unsigned long long)g_csRowHit,(unsigned long long)g_csRowMiss,
                       (unsigned long long)g_csVecB,
                       (unsigned long long)g_csBandFast,(unsigned long long)g_csBandSlow,
                       (unsigned long long)g_csScaleAt);
                jpline("cellstat: pixels recopies(identite)=%llu traduits=%llu"
                       " | tests d identite=%llu",
                       (unsigned long long)g_csIdentPix,(unsigned long long)g_csXlatPix,
                       (unsigned long long)g_csIdentTest);
            }
            if(g_loopServed|g_blitFast)
                std::printf("  [image] hors-DIB boucle=%llu blit=%llu replis=%llu | DIB=[%08x,%08x) | 1ere fautive=[%08x,%08x)\n",
                            (unsigned long long)g_oobLoop,(unsigned long long)g_oobBlit,
                            (unsigned long long)g_loopFB[6],
                            g_dibBits, g_dibBits + g_dibW*g_dibH*(g_dibBpp?g_dibBpp/8u:1u),
                            g_oobLo, g_oobHi);
            std::printf("  [scomp] native explode: served=%llu fallback=%llu | SCompExplode served=%llu fallback=%llu\n",
                        (unsigned long long)g_scompN,(unsigned long long)g_scompFB,
                        (unsigned long long)g_scomp2N,(unsigned long long)g_scomp2FB);
            if(g_dccNatN||g_dccNatFB||g_dccVerN){
                char m[200]; std::snprintf(m,sizeof m,"[dcc] natif: servis=%llu replis=%llu cadres=%llu octets=%llu | oracle: compares=%llu divergences=%llu",
                        (unsigned long long)g_dccNatN,(unsigned long long)g_dccNatFB,(unsigned long long)g_dccNatFrames,
                        (unsigned long long)g_dccNatBytes,(unsigned long long)g_dccVerN,(unsigned long long)g_dccVerBad);
                std::printf("  %s\n",m); d2vita_progress(m); }
            // Native intrinsics (D2_INTRIN): end-of-run report, for the qemu
            // oracle (where main returns). On console it's the 10 s window
            // that serves this purpose (main never returns there) — two
            // channels, two audiences.
            { extern int dyn86_nopend; extern unsigned long dyn86_nopend_dropped, dyn86_nopend_kept, dyn86_nopend_seen;
              extern unsigned long dyn86_pendor0_need, dyn86_pendor0_pess, dyn86_pendor0_cut;
              extern unsigned long dyn86_pendor0_pess3, dyn86_pendor0_cut3;
              char m[160]; std::snprintf(m,sizeof m,
                "[nopend] mode=%d | fin-de-bloc: appels=%lu relaches=%lu"
                " | archivage: besoin=%lu pessimiste=%lu coupes=%lu"
                " | PASSE 3: pessimiste=%lu coupes=%lu",
                dyn86_nopend, dyn86_nopend_seen, dyn86_nopend_dropped,
                dyn86_pendor0_need, dyn86_pendor0_pess, dyn86_pendor0_cut,
                dyn86_pendor0_pess3, dyn86_pendor0_cut3);
              std::printf("  %s\n",m); d2vita_progress(m); }
            if(dyn86_intrin_on){
                char m[320]; dyn86_intrin_report(m,sizeof m);
                std::printf("  %s\n",m); d2vita_progress(m);
            }
            if(d2_proj_verify || d2_proj_ver_n){
                char m[160]; std::snprintf(m,sizeof m,
                    "projverify: compares=%llu divergences=%llu sautes=%llu",
                    (unsigned long long)d2_proj_ver_n,(unsigned long long)d2_proj_ver_bad,
                    (unsigned long long)d2_proj_ver_skip);
                std::printf("  %s\n",m); d2vita_progress(m);
            }
            if(dyn86_mi_cpy_calls||dyn86_mi_set_calls){
                char m[320]; std::snprintf(m,sizeof m,
                        "[memintrin] memcpy: appels=%llu servis=%llu replis=%llu octets=%llu | memset: appels=%llu servis=%llu replis=%llu octets=%llu | rejets arene=%llu taille=%llu | oracle: compares=%llu divergences=%llu sautes=%llu",
                        (unsigned long long)dyn86_mi_cpy_calls,(unsigned long long)dyn86_mi_cpy_served,
                        (unsigned long long)dyn86_mi_cpy_fb,(unsigned long long)dyn86_mi_cpy_bytes,
                        (unsigned long long)dyn86_mi_set_calls,(unsigned long long)dyn86_mi_set_served,
                        (unsigned long long)dyn86_mi_set_fb,(unsigned long long)dyn86_mi_set_bytes,
                        (unsigned long long)dyn86_mi_rej[0],(unsigned long long)dyn86_mi_rej[1],
                        (unsigned long long)dyn86_mi_ver_n,(unsigned long long)dyn86_mi_ver_bad,
                        (unsigned long long)dyn86_mi_ver_skip);
                std::printf("  %s\n",m); d2vita_progress(m);
                // Distribution: the number that decides whether the port is
                // worth anything (an 8-byte copy gains nothing, a 4 KiB one
                // does).
                { unsigned long long hs=0; for(int k=0;k<DYN86_MI_HBITS;k++) hs+=dyn86_mi_cpy_hist[k]+dyn86_mi_set_hist[k];
                  if(hs){   // census armed (D2_MEMINTRIN=2): otherwise the lists would be empty
                std::printf("  [memintrin] memcpy tailles (seau 2^k):");
                for(int k=0;k<DYN86_MI_HBITS;k++) if(dyn86_mi_cpy_hist[k])
                    std::printf(" %d:%llu",k?(1<<(k-1)):0,(unsigned long long)dyn86_mi_cpy_hist[k]);
                std::printf("\n  [memintrin] memcpy align(dst|src)&3: 0=%llu 1=%llu 2=%llu 3=%llu | n<16=%llu | chevauche=%llu | max=%llu\n",
                        (unsigned long long)dyn86_mi_cpy_align[0],(unsigned long long)dyn86_mi_cpy_align[1],
                        (unsigned long long)dyn86_mi_cpy_align[2],(unsigned long long)dyn86_mi_cpy_align[3],
                        (unsigned long long)dyn86_mi_cpy_small,(unsigned long long)dyn86_mi_cpy_overlap,
                        (unsigned long long)dyn86_mi_cpy_maxseen);
                std::printf("  [memintrin] memset tailles (seau 2^k):");
                for(int k=0;k<DYN86_MI_HBITS;k++) if(dyn86_mi_set_hist[k])
                    std::printf(" %d:%llu",k?(1<<(k-1)):0,(unsigned long long)dyn86_mi_set_hist[k]);
                std::printf("\n");
                // What the GUEST ALREADY vectorizes (the CRT's SSE2 branch,
                // armed because my_cpuid reports SSE2): box86 translates its
                // movdqa into NEON, 16 bytes per instruction. This is the
                // share the native port CANNOT gain back.
                if(dyn86_mi_cpy_sse[0]||dyn86_mi_set_sse[0]){
                    const uint32_t* fl=(const uint32_t*)cpu->hostptr(dyn86_mi_sse2_va,4);
                    std::printf("  [memintrin] deja SSE2 cote invite (ds:%08x=%u) : memcpy %llu appels / %llu o | memset %llu appels / %llu o\n",
                        (unsigned)dyn86_mi_sse2_va, fl?(unsigned)*fl:0u,
                        (unsigned long long)dyn86_mi_cpy_sse[0],(unsigned long long)dyn86_mi_cpy_sse[1],
                        (unsigned long long)dyn86_mi_set_sse[0],(unsigned long long)dyn86_mi_set_sse[1]); }} } }
            if(g_lbServed||g_lbRepli||g_lbVerifN){
                char m[220]; std::snprintf(m,sizeof m,"[lightmap] natif: servis=%llu replis=%llu cases=%llu poses=%llu | oracle: compares=%llu divergences=%llu",
                        (unsigned long long)g_lbServed,(unsigned long long)g_lbRepli,(unsigned long long)g_lbCells,
                        (unsigned long long)g_lbSplats,(unsigned long long)g_lbVerifN,(unsigned long long)g_lbVerifBad);
                std::printf("  %s\n",m); d2vita_progress(m); }
            if(g_fbHashN) std::printf("  [fbhash] frames hachees=%llu  empreinte=0x%016llx\n",
                        (unsigned long long)g_fbHashN,(unsigned long long)g_fbHash);
            // Cumulative Glide RING fingerprint (the -3dfx path): the
            // equivalent of fbhash for a run with no framebuffer. Two
            // binaries that never touched the ring must produce the same
            // value at equal frame counts.
            gr_final_line();
            // D2_DIBWATCH: jpline, not printf — the answer must exist on
            // console too. Silent without the knob: a control leg has no
            // line, an armed leg with frames=0 is a dead leg.
            if(g_dwFrames) jpline("[dibwatch] images comparees=%llu reecrites=%llu (%llu %%) | octets changes=%llu/%llu (%llu.%llu %%) | par image: min=%u.%u %% max=%u.%u %%",
                        (unsigned long long)g_dwFrames,(unsigned long long)g_dwChanged,
                        (unsigned long long)(g_dwChanged*100ull/g_dwFrames),
                        (unsigned long long)g_dwBytes,(unsigned long long)g_dwTotal,
                        (unsigned long long)(g_dwTotal?g_dwBytes*100ull/g_dwTotal:0ull),
                        (unsigned long long)(g_dwTotal?(g_dwBytes*1000ull/g_dwTotal)%10ull:0ull),
                        g_dwMinPct>999?0u:g_dwMinPct/10u, g_dwMinPct>999?0u:g_dwMinPct%10u,
                        g_dwMaxPct/10u, g_dwMaxPct%10u);
            // Printed ONLY if the intrinsic path actually served: leg A (knob
            // absent) therefore has no line, leg B must have one with a
            // nonzero count — this is the oracle's non-empty test.
            cs_intrin_report();
            std::printf("  [hot] servi: grille=%llu lumiere=%llu blend=%llu rle=%llu (%llu Ko copies) coll=%llu\n"
                        "  [hot] replis: lumiere=%llu blend=%llu SCompExplode=%llu scomp=%llu  (rle et coll n'en ont pas)\n",
                        (unsigned long long)g_hotGridN,(unsigned long long)g_hotLightN,
                        (unsigned long long)g_hotBlendN,(unsigned long long)g_hotRleN,
                        (unsigned long long)(g_hotRleBytes>>10),
                        (unsigned long long)g_hotCollN,
                        (unsigned long long)g_hotLightFB,(unsigned long long)g_hotBlendFB,
                        (unsigned long long)g_scomp2FB,(unsigned long long)g_scompFB);
            if(g_gridMemoHit|g_gridMemoMiss){
                uint64_t tot=g_gridMemoHit+g_gridMemoMiss;
                std::printf("  [hot] memo salle: %llu/%llu cases servies sans relire la chaine (%.1f%%)\n",
                            (unsigned long long)g_gridMemoHit,(unsigned long long)tot,
                            100.0*(double)g_gridMemoHit/(double)tot);
            }
            std::printf("  [roomguard] iter=%llu defer=%llu disp=%llu disp-s2=%llu depth=%d\n",
                        (unsigned long long)g_rgIter,(unsigned long long)g_rgSkip,
                        (unsigned long long)g_rgDispCalls,(unsigned long long)g_rgDispS2,g_rgDepth);
        } else std::printf("  Game.exe not loaded\n");
    }
    std::printf("heap used=%u KB  va used=%u KB\n", g_heapA.used_bytes()/1024, g_vaA.used_bytes()/1024);
    if(g_dccN) std::printf("[DCCPROF] appels=%llu total=%llu ms (moy %llu us)\n",
        (unsigned long long)g_dccN,(unsigned long long)(g_dccUs/1000),
        (unsigned long long)(g_dccUs/(g_dccN?g_dccN:1)));
    eipprof_report("total (fin de run)");
    timeprof_report("total (fin de run)");
    std::printf("[VASTATS] reserves=%u (%llu MB) commit-in=%llu MB commit-fresh=%llu MB releases=%u (%llu MB) rel-miss=%u decommit=%llu MB map=%u (%llu MB) unmap=%u (%llu MB)\n",
        g_vaResN,(unsigned long long)(g_vaResB>>20),(unsigned long long)(g_vaComInB>>20),
        (unsigned long long)(g_vaComFrB>>20),g_vaRelN,(unsigned long long)(g_vaRelB>>20),
        g_vaRelFailN,(unsigned long long)(g_vaDecB>>20),
        g_fmapN,(unsigned long long)(g_fmapB>>20),g_unmapN,(unsigned long long)(g_unmapB>>20));
    { uint32_t ta[8]={0},ts[8]={0};
      for(auto&p:g_vaA.used_map()){ uint32_t a=p.first,s=p.second;
          for(int i=0;i<8;i++) if(s>ts[i]){ for(int j=7;j>i;j--){ts[j]=ts[j-1];ta[j]=ta[j-1];}
              ts[i]=s; ta[i]=a; break; } }
      std::printf("[VATOP] n=%u top:",(unsigned)g_vaA.used_map().size());
      for(int i=0;i<8&&ts[i];i++) std::printf(" %uK@%08x",ts[i]>>10,ta[i]);
      std::printf("\n"); }
    for(int i=0;i<256;i++){ if(!g_frb[i].h || g_frb[i].bytes<(64u<<10)) continue;
        auto pit=g_filePathByH.find(g_frb[i].h);
        const char* nm=pit!=g_filePathByH.end()?pit->second.c_str():"(closed)";
        const char* b=std::strrchr(nm,'/'); if(b) nm=b+1;
        std::printf("[FILES] h=%08x reads=%u bytes=%lluK %s\n",
            g_frb[i].h,g_frb[i].reads,(unsigned long long)(g_frb[i].bytes>>10),nm); }
    pc_report(true);   // forced summary of the directory index: counters in the open, even for a short run
    d2_crashlog("CLEAN EXIT (game path, frame=%d)", g_frame);
    d2cr::d2cr_session_clean_exit();   // state=exited, before the teardown block below
    // ---- ORDERED TEARDOWN: the crash + dump on game close ("Start" back to
    // the main menu) happens AFTER the final report — i.e. during static
    // destructors and thread teardown, while sceGxm and the replay thread
    // were still alive. Stop the replay, drain the GPU, then EXIT WITHOUT
    // destructors (sceKernelExitProcess): nothing left needs destroying, and
    // a thread alive during teardown isn't defensible in any mode (same rule
    // as d2vita_watchdog_stop).
    d2vita_progress("sortie: ExitProcess sans destructeurs");
    d2gr::r60().stop();   // idempotent: already done above on the normal path
    gr_flush_stop();
    // AUDIO TOO. This safety net exists because the normal path isn't always
    // taken (exit outside GAMEEXE, early exit): it used to repeat the replay,
    // flush and GPU stops but FORGOT the audio thread, which reads the guest
    // arena through a host view exactly like they do. dsound::shutdown() is
    // idempotent (a flag + atomic sink swap): calling it twice costs only one
    // check.
    son_codec_report();
    d2rt::dsound::shutdown();
    d2gxm_shutdown();
    // Stops and JOINS d2cr_upload (bounded), aborting any in-flight
    // request — same rule as d2vita_watchdog_stop above: not defensible to
    // reach sceKernelExitProcess with it still alive. No-op if no upload
    // thread was ever started this run.
    d2cr::d2cr_shutdown();
#ifdef __vita__
    sceKernelExitProcess(0);
#endif
    return 0;
}
