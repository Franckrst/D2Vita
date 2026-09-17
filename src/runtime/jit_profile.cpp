// jit_profile.cpp -- see jit_profile.h.
//
// ============ D2_JITPROFILE: where does dynarec time go? ====================
// Two phases: PRE-ROGUE (boot -> actual entry into camp, cold JIT cache) and
// POST-ROGUE (gameplay, warm cache). ONE aggregated dump at the end of the
// POST window; at the boundary only an IN-RAM snapshot is taken — no log
// line, so as not to disturb the tail of loading.
//
// BOUNDARY: the first presented frame where "world" work has started, as seen
// by the native-port counters (collision / grid / light). These three stay at
// ZERO through the whole menu and character creation, and go non-zero with
// the first camp frames. This is therefore not an arbitrary delay but a guest
// event. They depend on the NATIVEHOT/NATIVEGRID knobs (on by default): the
// dump prints the frame it locked onto so the boundary stays verifiable after
// the fact.
#include "jit_profile.h"
#include "platform/vita_present.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
extern "C" unsigned long long d2rt_hot_stat(int which);   // tools/rt_boot.cpp
extern "C" uint64_t dyn86_fill_ns;   // src/dynarec86 (also declared in tools/rt_boot.cpp)
extern "C" uint32_t dyn86_fill_count;

extern "C" {
    extern int dyn86_jitprof;
    extern uint64_t dyn86_jp_epoch_us, dyn86_jp_clock_bias_ns, dyn86_jp_run_us;
    extern uint64_t dyn86_jp_lookup_calls, dyn86_jp_lookup_hits, dyn86_jp_lookup_miss;
    extern uint64_t dyn86_jp_created, dyn86_jp_recompiles, dyn86_jp_invalid;
    extern uint64_t dyn86_jp_x86_insns, dyn86_jp_arm_bytes;
    extern uint64_t dyn86_jp_lookup_ns, dyn86_jp_lookup_smp;
    uint64_t dyn86_jp_now_us(void);
    extern uint32_t dyn86_fill_fail;
#ifdef __vita__
    extern uint64_t dyn86_sync_us;   // mman_vita.c: sceKernelSyncVMDomain
#endif
}
#ifndef __vita__
static const uint64_t dyn86_sync_us = 0;   // no VM domain outside console
#endif

struct JpSnap {
    uint64_t t_us, run_us, jit_ns, sync_us, look_ns, look_smp, calls, hits, miss,
             created, recomp, inval, insns, armb;
    uint32_t fills, fails;
};
static JpSnap jp_take(){
    JpSnap s;
    s.t_us    = dyn86_jp_now_us();       s.run_us  = dyn86_jp_run_us;
    s.jit_ns  = dyn86_fill_ns;           s.sync_us = dyn86_sync_us;
    s.look_ns = dyn86_jp_lookup_ns;      s.look_smp= dyn86_jp_lookup_smp;
    s.calls   = dyn86_jp_lookup_calls;   s.hits    = dyn86_jp_lookup_hits;
    s.miss    = dyn86_jp_lookup_miss;    s.created = dyn86_jp_created;
    s.recomp  = dyn86_jp_recompiles;     s.inval   = dyn86_jp_invalid;
    s.insns   = dyn86_jp_x86_insns;      s.armb    = dyn86_jp_arm_bytes;
    s.fills   = dyn86_fill_count;        s.fails   = dyn86_fill_fail;
    return s;
}
static JpSnap g_jp0{}, g_jpA{}, g_jpB{};
static int g_jpPhase = 0;                       // 0 pre, 1 post, 2 dumped
static int g_jpFrameA = -1, g_jpFrameB = -1;
static uint64_t g_jpWindowUs = 120ull * 1000000ull;   // D2_JITPROFILE_POST (s)

// Output: printf for qemu, d2vita_progress for console (printf never reaches
// the Vita log). A single burst, at the end.
void jpline(const char* fmt, ...){
    char b[220]; va_list ap; va_start(ap,fmt);
    std::vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
    std::printf("%s\n", b); std::fflush(stdout);
    d2vita_progress(b);
}
// Integer fractions: floating output isn't guaranteed everywhere, and a %f
// in a console log is an unnecessary risk.
void jp_fr(char* o, size_t n, uint64_t num, uint64_t den){
    if(!den){ std::snprintf(o,n,"n/a"); return; }
    unsigned long long h = (unsigned long long)((num*100ull)/den);
    std::snprintf(o,n,"%llu.%02llu", h/100ull, h%100ull);
}
static void jp_pct(char* o, size_t n, uint64_t part, uint64_t whole){
    if(!whole){ std::snprintf(o,n,"n/a"); return; }
    unsigned long long h = (unsigned long long)((part*10000ull)/whole);
    std::snprintf(o,n,"%llu.%02llu", h/100ull, h%100ull);
}
struct JpPhase { uint64_t total,jit,run,look,sync,created,recomp,insns,armb,hits,miss,inval,trans; };
static JpPhase jp_phase(const JpSnap& a, const JpSnap& b){
    JpPhase p;
    p.total = b.t_us - a.t_us;
    p.jit   = (b.jit_ns - a.jit_ns)/1000ull;
    p.run   = b.run_us - a.run_us;
    p.sync  = b.sync_us - a.sync_us;
    uint64_t smp = b.look_smp - a.look_smp, calls = b.calls - a.calls;
    // Sample extrapolation (1 lookup out of every DYN86_JP_SAMPLE).
    p.look  = smp ? ((b.look_ns - a.look_ns) * calls / smp) / 1000ull : 0;
    p.created = b.created - a.created;  p.recomp = b.recomp - a.recomp;
    p.insns   = b.insns   - a.insns;    p.armb   = b.armb   - a.armb;
    p.hits    = b.hits    - a.hits;     p.miss   = b.miss   - a.miss;
    p.inval   = b.inval   - a.inval;    p.trans  = p.created + p.recomp;
    return p;
}
static void jp_emit(const char* name, const JpSnap& a, const JpSnap& b){
    JpPhase p = jp_phase(a,b);
    char c1[32],c2[32],c3[32];
    jpline("=== DYNAREC PROFILE: %s ===", name);
    jpline("total_us=%llu",          (unsigned long long)p.total);
    jpline("jit_compile_us=%llu",    (unsigned long long)p.jit);
    jpline("guest_run_us=%llu",      (unsigned long long)p.run);
    jpline("lookup_us=%llu",         (unsigned long long)p.look);
    jpline("icache_sync_us=%llu",    (unsigned long long)p.sync);
    jpline("dynablocks_created=%llu",     (unsigned long long)p.created);
    jpline("x86_insns_compiled=%llu",     (unsigned long long)p.insns);
    jpline("arm_bytes_generated=%llu",    (unsigned long long)p.armb);
    jpline("cache_hits=%llu",        (unsigned long long)p.hits);
    jpline("cache_misses=%llu",      (unsigned long long)p.miss);
    jpline("recompiles=%llu",        (unsigned long long)p.recomp);
    jpline("invalidations=%llu",     (unsigned long long)p.inval);
    jp_pct(c1,sizeof c1,p.jit,p.total); jp_pct(c2,sizeof c2,p.look,p.total);
    jp_pct(c3,sizeof c3,p.sync,p.total);
    jpline("jit_compile_pct=%s",  c1);
    jpline("lookup_pct=%s",       c2);
    jpline("icache_sync_pct=%s",  c3);
    jp_fr(c1,sizeof c1,p.jit,  p.trans);
    jp_fr(c2,sizeof c2,p.insns,p.trans);
    jp_fr(c3,sizeof c3,p.armb, p.trans);
    jpline("avg_compile_us_per_block=%s", c1);
    jpline("avg_x86_insns_per_block=%s",  c2);
    jpline("avg_arm_bytes_per_block=%s",  c3);
    // MUTUALLY EXCLUSIVE categories. guest_run is INCLUSIVE (it contains JIT,
    // lookups, cache sync, and OS shims), so it cannot itself serve as a slice
    // of the total. The remainder is published by subtraction.
    uint64_t exec = (p.run > p.jit + p.look) ? p.run - p.jit - p.look : 0;
    uint64_t host = (p.total > p.run) ? p.total - p.run : 0;
    jp_pct(c1,sizeof c1,exec,p.total); jp_pct(c2,sizeof c2,host,p.total);
    jpline("exclusif: exec_et_traps_us=%llu (%s%%)  hors_invite_us=%llu (%s%%)",
           (unsigned long long)exec, c1, (unsigned long long)host, c2);
    jp_pct(c1,sizeof c1,p.sync,p.jit?p.jit:1);
    jpline("note: icache_sync est INCLUS dans jit_compile_us (%s%% du JIT)", c1);
    jpline("fills=%u fill_fail=%u", (unsigned)(b.fills-a.fills), (unsigned)(b.fails-a.fails));
}
static void jp_dump(const char* why){
    jpline("=== DYNAREC PROFILE (D2_JITPROFILE) : %s ===", why);
    jpline("frontiere_rogue: frame=%d t=%llums | fin: frame=%d t=%llums",
           g_jpFrameA, (unsigned long long)((g_jpA.t_us-g_jp0.t_us)/1000ull),
           g_jpFrameB, (unsigned long long)((g_jpB.t_us-g_jp0.t_us)/1000ull));
    jp_emit("PRE-ROGUE",  g_jp0, g_jpA);
    jp_emit("POST-ROGUE", g_jpA, g_jpB);
    JpPhase a = jp_phase(g_jp0,g_jpA), b = jp_phase(g_jpA,g_jpB);
    char r[32];
    jpline("=== PRE vs POST ===");
    jp_fr(r,sizeof r,a.jit,    b.jit);     jpline("compile_time_ratio=%s", r);
    jp_fr(r,sizeof r,a.created,b.created); jpline("new_blocks_ratio=%s", r);
    jp_fr(r,sizeof r,a.miss,   b.miss);    jpline("cache_miss_ratio=%s", r);
    jp_fr(r,sizeof r,a.armb,   b.armb);    jpline("arm_generated_ratio=%s", r);
    jpline("methode: lookup ECHANTILLONNE 1/%u (pre smp=%llu/%llu, post smp=%llu/%llu),"
           " biais horloge retire=%lluns",
           (unsigned)64,
           (unsigned long long)(g_jpA.look_smp-g_jp0.look_smp),
           (unsigned long long)(g_jpA.calls  -g_jp0.calls),
           (unsigned long long)(g_jpB.look_smp-g_jpA.look_smp),
           (unsigned long long)(g_jpB.calls  -g_jpA.calls),
           (unsigned long long)dyn86_jp_clock_bias_ns);
    jpline("=== FIN DYNAREC PROFILE ===");
}
// Profile origin = the instant set by dyn86_init(), i.e. right before the
// first translation, hence before any guest execution.
static void jp_origin(){
    if(g_jp0.t_us) return;
    g_jp0 = JpSnap{}; g_jp0.t_us = dyn86_jp_epoch_us;
    if(const char* e = getenv("D2_JITPROFILE_POST")){
        uint64_t s = strtoull(e,nullptr,10); if(s) g_jpWindowUs = s*1000000ull;
    }
}
void jp_tick(int frame){
    if(!dyn86_jitprof || g_jpPhase>=2) return;
    jp_origin();
    if(g_jpPhase==0){
        if(d2rt_hot_stat(1) || d2rt_hot_stat(0) || d2rt_hot_stat(3)){
            g_jpA = jp_take(); g_jpFrameA = frame; g_jpPhase = 1;   // RAM only
        }
        return;
    }
    if(dyn86_jp_now_us() - g_jpA.t_us >= g_jpWindowUs){
        g_jpB = jp_take(); g_jpFrameB = frame; g_jpPhase = 2;
        jp_dump("fenetre POST complete");
    }
}
// Safety net: if the run stops before the window ends (MAXFRAMES too short,
// game exit, handled crash), the profile is still dumped, marked truncated.
void jp_finish(int frame){
    if(!dyn86_jitprof || g_jpPhase>=2) return;
    jp_origin();
    if(g_jpPhase==0){ g_jpA = jp_take(); g_jpFrameA = -1; }   // Rogue never reached
    g_jpB = jp_take(); g_jpFrameB = frame; g_jpPhase = 2;
    jp_dump(g_jpFrameA<0 ? "TRONQUE - frontiere Rogue jamais franchie"
                         : "TRONQUE - fenetre POST incomplete");
}
// D2_SIGNTAG: census of "sign-discriminated complemented pointer" sites
// (dynarec_arm_signtag.h). Published on BOTH legs as soon as the recognizer
// sees a pattern, so "rewritten=0" is proof of a clean witness, not a missing line.
extern "C" { extern int dyn86_signtag;
             extern unsigned long dyn86_signtag_seen, dyn86_signtag_done; }
void signtag_dump(){
    if(!dyn86_signtag_seen) return;
    jpline("signe: knob=%d motifs_vus=%lu reecrits_bit30=%lu",
           dyn86_signtag, dyn86_signtag_seen, dyn86_signtag_done);
}
// fastmmu: census of TRANSLATED sites (counted in pass 3). Same discipline as
// signtag_dump above: published on BOTH legs as soon as the recognizer sees
// anything, so "folded=0" / "links=0" is proof of a clean witness, not a
// missing line.
extern "C" {
    extern int dyn86_mmufold, dyn86_mmustack;
    extern unsigned long dyn86_mmu_folds, dyn86_mmu_sites, dyn86_mmu_foldable;
    extern unsigned long dyn86_mmu_st_heads, dyn86_mmu_st_links;
    extern unsigned long dyn86_mmu_st_pop, dyn86_mmu_st_push;
    extern unsigned long dyn86_mmu_st_rej_pos, dyn86_mmu_st_rej_pred;
    extern unsigned long dyn86_mmu_st_rej_kind;
}
void mmu_dump(){
    if(!dyn86_mmu_sites && !dyn86_mmu_st_heads && !dyn86_mmu_st_links) return;
    jpline("=== fastmmu (D2_MMUFOLD=%d D2_MMUSTACK=%d) ===",
           dyn86_mmufold, dyn86_mmustack);
    // "foldable" is the PURE verdict, independent of the switch: the control
    // leg publishes it too, proving the recognizer runs on both sides and only
    // EMISSION differs.
    jpline("absolus: sites=%lu pliables=%lu plies=%lu",
           dyn86_mmu_sites, dyn86_mmu_foldable, dyn86_mmu_folds);
    // POP and PUSH do NOT make the same promise: POP shortens the dependent
    // chain (depth 2k-1 -> 1 for a burst of k), PUSH only removes one
    // instruction (rewriting src via STR ... ]! keeps the same depth). Counted
    // separately so a console A/B can tell them apart — code size and runtime
    // gain do not always move together.
    jpline("pile: tetes=%lu maillons=%lu (POP=%lu maillon-de-moins, PUSH=%lu instruction-de-moins)",
           dyn86_mmu_st_heads, dyn86_mmu_st_links,
           dyn86_mmu_st_pop, dyn86_mmu_st_push);
    jpline("pile refus: code_emis_entre=%lu cible_de_saut=%lu autre_famille=%lu",
           dyn86_mmu_st_rej_pos, dyn86_mmu_st_rej_pred, dyn86_mmu_st_rej_kind);
    jpline("=== FIN fastmmu ===");
}
