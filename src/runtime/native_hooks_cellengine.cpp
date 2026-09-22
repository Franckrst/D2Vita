// native_hooks_cellengine.cpp -- see native_hooks_cellengine.h. Globals
// crossing the file boundary lost their `static` (or moved to
// runtime/rt_host.h) to stay visible here. The "standalone" block (celwatch,
// codecs, DCC, fog raise snapshot) is already in native_hooks_codec.cpp;
// native_hooks_codec_install_codecs is still called BETWEEN install_blit and
// install_rest below -- that's what keeps shim registration order unchanged.
#include "native_hooks_cellengine.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/layout.h"
#include "runtime/rt_host.h"
#include "platform/vita_present.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
using namespace d2rt;

// The out-of-DIB guard only checks the AGGREGATE extent [dlo,dhi) measured
// by pass 1. It cannot see an INDIVIDUAL write in pass 2 that lands outside
// that extent -- which happens whenever the two passes don't walk the
// stream identically. So every write site checks [off, off+n) against
// [0, dspan) directly. The subtractions are UNSIGNED: if the address is
// below dlo, off wraps to a huge value and the check catches that too.
uint64_t g_wrOob=0;      // writes rejected
uint32_t g_wrOobOff=0, g_wrOobLen=0, g_wrOobSpan=0, g_wrOobKind=0;
static inline bool wr_ok(uint32_t off, uint32_t n, uint32_t dspan, uint32_t kind){
    if(!dspan) return true;                     // measurement mode: no known bound
    if(off<=dspan && n<=dspan-off) return true;
    if(!g_wrOob){ g_wrOobOff=off; g_wrOobLen=n; g_wrOobSpan=dspan; g_wrOobKind=kind; }
    ++g_wrOob; return false;
}
uint32_t g_oobLo=0, g_oobHi=0;      // FIRST offending extent observed
static int      g_dibGuard=0;              // 0 = measurement only, 1 = fallback armed
static inline bool dib_outside(uint32_t lo, uint32_t hi){
    if(!g_dibBits || !g_dibW || !g_dibH) return false;   // unknown geometry: no verdict
    const uint32_t end = g_dibBits + g_dibW*g_dibH*(g_dibBpp?g_dibBpp/8u:1u);
    if(lo>=g_dibBits && hi<=end) return false;
    if(!g_oobLo){ g_oobLo=lo; g_oobHi=hi; }
    return true;
}

// ===========================================================================
//  D2_CELLOPT -- cell-loop optimizations, Game+0xf8b80
// ===========================================================================
// One of the hottest single functions in the game, and it's our C code (not
// translated guest code).
//
// Each stage lives behind its own BIT. Mask 0 (default, knob absent) is the
// original code byte-for-byte, vectors and copies included: a binary built
// without D2_CELLOPT is the reference path for the FBHASH oracle
// (0xd332da5981bade4a), and the same binary arms each stage one at a time.
enum {
    CO_NOVEC = 1,   // removes both std::vector copies per served call
    CO_LUTC  = 2,   // caches host views (colour LUT + light row)
    CO_WORD  = 4,   // translates 32-bit WORDS at a time (4 pixels per step)
    CO_BANDS = 8,   // precomputed band table, once per call
    CO_SCALE = 16,  // source window resolved per line in the SCALE leaves
    CO_IDENT = 32,  // identity LUT -> plain copy (no lookup, no recomposition)
    CO_ALL   = 63
};
static int  g_cellOpt  = 0;       // bitmask; 0 = original code
bool g_cellStat = false;   // D2_CELLSTAT=1: instrumentation counters

// ---- Instrumentation counters (D2_CELLSTAT=1) ------------------------------
// One `if(g_cellStat)` per site: a perfectly predictable branch, zero cost
// on the shipped binary. Counters are DOUBLED -- pass 1 and pass 2 count the
// same quantity at two different points in the code. If they disagree, the
// counts cannot be trusted.
uint64_t g_csCalls=0;      // calls SERVED
uint64_t g_csCells=0;      // cells painted (nj)
uint64_t g_csHP=0;         // hostptr calls (host view) -- the cost of C-side traversal
uint64_t g_csP1Band=0;     // band iterations of PASS 1
uint64_t g_csP2Band=0;     // band iterations of PASS 2
uint64_t g_csPixP1[3]={0,0,0};   // bytes ANNOUNCED by pass 1 (flat, flat-clipped, scale)
uint64_t g_csPixP2[3]={0,0,0};   // bytes WRITTEN by pass 2 (same buckets)
uint64_t g_csCell3[3]={0,0,0};   // cells per bucket
uint64_t g_csRunP1=0, g_csRunP2=0;   // SCALE-leaf runs (pass 1 / pass 2)
uint64_t g_csScaleAt=0;    // source bytes read one at a time by at() in SCALE leaves
uint64_t g_csVecB=0;       // bytes copied by the two std::vector
uint64_t g_csLutHit=0, g_csLutMiss=0;    // colour LUT cache
uint64_t g_csRowHit=0, g_csRowMiss=0;    // light row cache
uint64_t g_csBandFast=0, g_csBandSlow=0; // flat cells served via the band table
uint64_t g_csIdentPix=0, g_csXlatPix=0;  // bytes copied as-is / translated
uint64_t g_csIdentTest=0;                // 256-byte comparisons (at most one per call)

// ---- LUT translation: the hot path ----------------------------------------
// The naive version processes ONE BYTE AT A TIME. Under ARM -O2 it compiles
// to five instructions per pixel, including a DEPENDENT CHAIN of two loads
// (ldrb source -> ldrb lut[source]) that nothing can overlap:
//     ldrb r10,[r3],#1 ; cmp ; ldrb r10,[r6,r10] ; strb r10,[r0,#1]! ; bne
// The word version reads FOUR pixels with a single `ldr`, issues the four
// LUT lookups independently (they overlap on a dual-issue A9), and writes a
// single `str`. Same result byte for byte -- pure reassociation, no new
// arithmetic.
//
// LITTLE-ENDIAN REQUIRED: byte 0 of the word must be s[q]. The target
// (ARMv7 and the test host) is little-endian; the static_assert enforces it.
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "d2_xlat: la version par mots suppose un hote petit-boutiste"
#endif
// Identity table, to recognize a light row that changes nothing.
static const uint8_t* d2_ident256(){
    static uint8_t t[256]; static bool init=false;
    if(!init){ for(int i=0;i<256;i++) t[i]=(uint8_t)i; init=true; }
    return t; }
static inline void d2_copy(uint8_t* d, const uint8_t* s, uint32_t n){
    for(uint32_t nw=n>>2; nw; --nw){
        uint32_t w; __builtin_memcpy(&w,s,4); s+=4; __builtin_memcpy(d,&w,4); d+=4; }
    for(uint32_t q=n&3u; q; --q) *d++=*s++; }
static inline void d2_xlat(uint8_t* d, const uint8_t* s, uint32_t n, const uint8_t* lut,
                           bool ident=false){
    if(ident){                       // CO_IDENT: lut[x] == x, the LUT is a no-op
        if(g_cellStat) g_csIdentPix+=n;
        d2_copy(d,s,n); return; }
    if(g_cellStat) g_csXlatPix+=n;
    if(g_cellOpt & CO_WORD){
        // POST-INCREMENTED cursors, not an index: this is what makes the
        // compiler emit `ldr rX,[rS],#4` and `str rY,[rD],#4` instead of
        // three address additions per iteration. The fourth byte is read
        // without a mask (`ldrb rX,[lut, w, lsr #24]`); ARM's shifted
        // operand makes that free.
        for(uint32_t nw=n>>2; nw; --nw){
            uint32_t w; __builtin_memcpy(&w, s, 4); s+=4;
            const uint32_t r = (uint32_t)lut[(uint8_t)w]
                             | ((uint32_t)lut[(uint8_t)(w>>8)]  << 8)
                             | ((uint32_t)lut[(uint8_t)(w>>16)] << 16)
                             | ((uint32_t)lut[w>>24]            << 24);
            __builtin_memcpy(d, &r, 4); d+=4;
        }
        // Tail: never reached on the FLAT NON-CLIPPED path (the 0x4f69b0
        // guard forces len to a multiple of 4); needed for the clipped path.
        for(uint32_t q=n&3u; q; --q) *d++ = lut[*s++];
        return;
    }
    for(uint32_t q=0;q<n;q++) d[q]=lut[s[q]];
}

// ---- Host view cache (CO_LUTC) ---------------------------------------------
// hostptr() is PURE: arena_check(va,n) only depends on g_arena_span, fixed
// after init, and the result is H(va) = va + membase. Two calls with the
// same (va,n) therefore ALWAYS return the same thing -- a cache is exact,
// not an approximation. A failure (nullptr) is NEVER cached: the failure
// path must stay the original one.
// The KEY includes the length: hostptr(va,n) refuses an extent that runs
// past the arena, so two different n values don't necessarily agree.
// Without this field, a cached success for n=0x6d could serve a request for
// n=0xd9 that the original call would have refused -- a divergence at the
// edge of the arena.
struct D2HostCache {
    uint32_t key[32], klen[32]; const uint8_t* val[32]; bool used[32];
    void clear(){ for(int i=0;i<32;i++) used[i]=false; }
    const uint8_t* get(d2rt::Cpu& c, uint32_t va, uint32_t n, uint64_t* hit, uint64_t* miss){
        const unsigned h=((va>>8)^(va>>3)) & 31u;
        if(used[h] && key[h]==va && klen[h]==n){ if(g_cellStat) ++*hit; return val[h]; }
        if(g_cellStat){ ++*miss; ++g_csHP; }
        const uint8_t* p=(const uint8_t*)c.hostptr(va,n);
        if(p){ key[h]=va; klen[h]=n; val[h]=p; used[h]=true; }
        return p; }
};
// Host view NOT cached, but COUNTED: this is the instrumentation cost
// counter (each hostptr call is a virtual call plus an arena check).
static inline const uint8_t* d2_hp(d2rt::Cpu& c, uint32_t va, uint32_t n){
    if(g_cellStat) ++g_csHP;
    return (const uint8_t*)c.hostptr(va,n); }

// ===========================================================================
//  D2_CELLOVL -- overlap instrumentation (prerequisite for D2_CELLPAR)
// ===========================================================================
// Parallelizing the cells of a single call is pixel-exact only if no two
// cells ever write the same byte -- or write the same value to it. "Floor
// tiles are interlocking diamonds, so it's disjoint" is an ASSUMPTION; this
// counter turns it into a measurement.
//
// Principle: a STAMP map over the call's [dlo,dhi) extent. Each byte written
// gets (generation << 12) | (cell index + 1). A byte whose stamp already
// carries the current generation was therefore written twice WITHIN THE
// SAME CALL -- we then check whether the value about to be written differs
// from what's already there: that's the only kind of overlap that makes the
// result depend on ORDER.
//
// This mode is NECESSARILY SEQUENTIAL (D2_CELLPAR is disarmed while it's
// active): the stamps and counters are global, unlocked, touched by a single
// thread. That's intentional -- the instrument measures a property of the
// DRAWING, not of any particular parallel execution.
bool     g_ovlOn     = false;
static uint64_t*g_ovlStamp  = nullptr;   // one stamp per byte of [dlo,dhi)
static uint32_t g_ovlCap    = 0;
static uint64_t g_ovlGen    = 0;         // generation = one served call
static uint32_t g_ovlCell   = 0;         // index of the current cell (+1)
static uint8_t* g_ovlBase   = nullptr;   // dbase of the call in progress
uint64_t g_ovlCalls=0, g_ovlWr=0, g_ovlHit=0, g_ovlDiff=0, g_ovlSelf=0;
uint64_t g_ovlNoMap=0;            // extents not examined (map too small)
// Opens a generation. Returns false if the map can't cover the extent: the
// call is then counted as NOT EXAMINED rather than silently omitted.
static bool ovl_begin(uint8_t* dbase, uint32_t dspan){
    if(!g_ovlOn) return false;
    if(dspan>g_ovlCap){
        uint64_t* p=(uint64_t*)std::realloc(g_ovlStamp,(size_t)dspan*sizeof(uint64_t));
        if(!p){ ++g_ovlNoMap; g_ovlBase=nullptr; return false; }
        std::memset(p+g_ovlCap,0,(size_t)(dspan-g_ovlCap)*sizeof(uint64_t));
        g_ovlStamp=p; g_ovlCap=dspan; }
    ++g_ovlGen; ++g_ovlCalls; g_ovlBase=dbase; return true;
}
// Records a write extent BEFORE it happens: dbase[off+k] still holds the
// previous byte, so the "old value vs new value" comparison is exact.
static inline void ovl_note(uint32_t off, uint32_t n, const uint8_t* s,
                            const uint8_t* lut, bool ident){
    if(!g_ovlStamp || !g_ovlBase || off+n>g_ovlCap){ if(g_ovlOn) ++g_ovlNoMap; return; }
    const uint64_t st=(g_ovlGen<<12)|(uint64_t)g_ovlCell;
    for(uint32_t k=0;k<n;k++){
        ++g_ovlWr;
        uint64_t& e=g_ovlStamp[off+k];
        if((e>>12)==g_ovlGen){
            const uint8_t nv = ident ? s[k] : lut[s[k]];
            if((e&0xfffu)==(uint64_t)g_ovlCell) ++g_ovlSelf; else ++g_ovlHit;
            if(g_ovlBase[off+k]!=nv) ++g_ovlDiff; }
        e=st; }
}

static void d2_blit_bands(d2rt::Cpu& c, uint32_t GB, uint32_t src, uint32_t dst, uint32_t table){
    using namespace d2rt;
    // One of the hottest functions in the game. The naive body pays many
    // bounds checks per call (4 for CONSTANT tables re-resolved every
    // time, 1 for the LUT, 30 for the 15 bands) to copy at most 480
    // bytes, while the dynarec compiles each guest access to a bare
    // `ldr` -- so a native port here only pays off if it doesn't re-add
    // that overhead itself.
    // Two safety fixes of the same class as 0x415240: hostptr(va) only
    // validates one byte (cpu.h:52), but the LUT is indexed over 256 and
    // each band reads/writes `len` bytes.
    // Table addresses are constant (Game.exe's .rdata, fixed arena) so
    // they're resolved once as pointers; the VALUES are still re-read
    // every call since line height can change.
    static const uint32_t *dofs=nullptr,*lens=nullptr,*sofs=nullptr,*ppitch=nullptr;
    static bool tinit=false;
    if(!tinit){
        dofs  =(const uint32_t*)c.hostptr(GB+0x32db48,60);
        lens  =(const uint32_t*)c.hostptr(GB+0x32db84,60);
        sofs  =(const uint32_t*)c.hostptr(GB+0x32dbc0,60);
        ppitch=(const uint32_t*)c.hostptr(GB+0x3d544c,4);
        tinit = dofs&&lens&&sofs&&ppitch;
    }
    const uint8_t* lut = tinit ? (const uint8_t*)c.hostptr(table,256) : nullptr;
    if(lut){
        uint32_t pitch=*ppitch;
        // One check for the whole call: the [lo,hi) extent touched by
        // the 15 bands, source side and destination side.
        uint32_t dlo=0xFFFFFFFFu,dhi=0,slo=0xFFFFFFFFu,shi=0;
        bool any=false, wrap=false;
        uint32_t dcur=dst;
        for(int i=0;i<15;i++,dcur+=pitch){
            uint32_t len=lens[i];
            if(len-4>0x1c || (len&3)) continue;     // non-4-multiple or >32: skip
            uint32_t d=dofs[i]+dcur, sp=sofs[i]+src;
            if(d+len<d || sp+len<sp){ wrap=true; break; }   // 32-bit wraparound
            if(d<dlo) dlo=d;   if(d+len>dhi) dhi=d+len;
            if(sp<slo) slo=sp; if(sp+len>shi) shi=sp+len;
            any=true;
        }
        if(!any && !wrap) return;                 // nothing to copy
        if(!wrap && dib_outside(dlo,dhi)) ++g_oobBlit;   // measurement only: see the guard
        uint8_t* dbase = wrap?nullptr:(uint8_t*)c.hostptr(dlo,dhi-dlo);
        const uint8_t* sbase = wrap?nullptr:(const uint8_t*)c.hostptr(slo,shi-slo);
        if(dbase && sbase){
            dcur=dst;
            ++g_blitFast;
            for(int i=0;i<15;i++,dcur+=pitch){
                uint32_t len=lens[i];
                if(len-4>0x1c || (len&3)) continue;
                uint8_t* d = dbase+((dofs[i]+dcur)-dlo);
                const uint8_t* s2 = sbase+((sofs[i]+src)-slo);
                for(uint32_t k=0;k<len;k++) d[k]=lut[s2[k]];
            }
            return;
        }
        // extent outside the arena (or wraparound): virtual path below
    }
    // Fallback (no flat host view): same remap via virtual read/write.
    // Logged only ONCE: printf doesn't reach the Vita log, so the
    // absence of this line on console is what proves the fast path
    // served every call.
    if(!g_blitSlow) d2vita_progress("blit: 1er repli voie virtuelle (etendue globale hors arene)");
    ++g_blitSlow;
    uint32_t pitch=c.read_u32(GB+0x3d544c);
    uint8_t tbl[256]; c.read(table,tbl,256); uint8_t b[32];
    for(int i=0;i<15;i++,dst+=pitch){
        uint32_t len=c.read_u32(GB+0x32db84+4*i);
        if(len-4>0x1c || (len&3)) continue;
        uint32_t d=c.read_u32(GB+0x32db48+4*i)+dst;
        uint32_t sp=c.read_u32(GB+0x32dbc0+4*i)+src;
        c.read(sp,b,len); for(uint32_t k=0;k<len;k++) b[k]=tbl[b[k]]; c.write(d,b,len);
    }
}
// ---- "SCALE" blit, Game+0xf6f60, ported ------------------------------------
// Structure per disassembly: nLines lines; each line is a sequence of
// (skip, length) pairs terminated by the pair (0,0). `skip` advances the
// cursor without writing; `length` copies that many bytes THROUGH THE LUT.
// A length whose (length-1) exceeds 0x1f is NOT copied but still advances
// both cursors (0x4f70ff) -- a trap: it's not a `continue`, it's a jump over
// just the copy.
//   __fastcall(ecx = src, edx = dst); [esp+4] = LUT row; [esp+8] = nLines
//   ret 8; returns the destination cursor.
// dbase == nullptr: MEASURE only (written extent and source extent), no
// write -- this is pass 1. Otherwise write, indexing from dbase/dlo.
// ---- Extra parameters for parallel mode (D2_CELLPAR) -----------------------
// nullptr = original path, unchanged. Non-null = the caller is a drawing
// LANE that is NOT allowed to touch the guest CPU: the source is already
// resolved to a host view ([sLo,sHi), measured by pass 1) and writes are
// filtered by an offset window.
struct D2ParW {
    const uint8_t* sbase; uint32_t sLo, sHi;   // host view of the ENTIRE source read
    uint32_t wlo, whi;                         // write window (offsets from dlo)
};
// A read outside [sLo,sHi) would prove pass 1 and pass 2 don't walk the
// same stream. Must stay at ZERO.
uint64_t g_parScaleBail=0;

static uint32_t d2_scale_walk(d2rt::Cpu* c, uint32_t src, uint32_t dst, uint32_t pitch,
                              int nLines, const uint8_t* lut,
                              uint8_t* dbase, uint32_t dlo, uint32_t dspan,
                              uint32_t* wlo, uint32_t* whi,
                              uint32_t* slo, uint32_t* shi, bool* ok,
                              const D2ParW* pw=nullptr)
{
    using namespace d2rt;
    const uint8_t* win=pw?pw->sbase:nullptr;
    uint32_t winLo=pw?pw->sLo:0, winHi=pw?pw->sHi:0;
    auto at=[&](uint32_t a, uint8_t* out)->bool{
        if(a>=winLo && a<winHi){ *out=win[a-winLo]; return true; }
        if(pw||!c){ __atomic_fetch_add(&g_parScaleBail,1,__ATOMIC_RELAXED); return false; }
        if(const void* h=c->hostptr(a,1024)){ win=(const uint8_t*)h; winLo=a; winHi=a+1024;
            *out=win[0]; return true; }
        win=nullptr; winLo=winHi=0; return c->read(a,out,1); };
    uint32_t cur=dst, line=dst; int n=nLines; uint64_t guard=0;
    if(slo){ *slo=src; }
    while(n>0){
        if(++guard>(1u<<20)){ if(ok) *ok=false; break; }   // corrupted stream
        uint8_t skip=0, run=0;
        if(!at(src,&skip) || !at(src+1,&run)){ if(ok) *ok=false; break; }
        src+=2;
        if(!skip && !run){ --n; line+=pitch; cur=line; continue; }   // 0x4f6f95
        cur+=skip;                                                    // 0x4f6fa9
        if((uint32_t)(run-1)<=0x1fu){                                 // 0x4f6fae
            if(!dbase){ if(cur<*wlo) *wlo=cur; if(cur+run>*whi) *whi=cur+run;
                        if(g_cellStat){ ++g_csRunP1; g_csPixP1[2]+=run; } }
            else if((!pw || ((cur-dlo)>=pw->wlo && (cur-dlo)<pw->whi))
                    && wr_ok(cur-dlo,run,dspan,1)){
                   uint8_t* d=dbase+(cur-dlo);
                   if(g_cellStat){ ++g_csRunP2; g_csPixP2[2]+=run; }
                   // CO_SCALE: the 1024-byte window from at() already
                   // exists; it only needs to be RESOLVED ONCE for the run
                   // instead of re-checking both its bounds per byte. The
                   // fallback is the original byte-by-byte body, so a
                   // window failure (end of arena) is still handled exactly
                   // as before.
                   bool fast=false;
                   if(g_cellOpt&CO_SCALE){
                       if(!(src>=winLo && src+run<=winHi)){ uint8_t b0=0; at(src,&b0); }
                       if(src>=winLo && src+run<=winHi){
                           if(g_ovlOn) ovl_note(cur-dlo,run,win+(src-winLo),lut,false);
                           d2_xlat(d, win+(src-winLo), run, lut); fast=true; } }
                   if(!fast){
                       if(g_cellStat) g_csScaleAt+=run;
                       for(uint32_t k=0;k<run;k++){ uint8_t b=0; at(src+k,&b);
                           if(g_ovlOn) ovl_note(cur-dlo+k,1,&b,lut,false);
                           d[k]=lut[b]; } } }
        }
        cur+=run; src+=run;                                           // 0x4f70ff
    }
    if(shi){ *shi=src; }
    return cur;
}

// ---- "SCALE" blit, CLIPPED, Game+0xf7580, ported ---------------------------
// Same stream format as 0x4f6f60 (skip/length pairs, (0,0) ends the line),
// plus clipping. Three quirks found by disassembly, all traps:
//   - EARLY EXIT: if absY >= clipYhi (0x3c97a0) the function stops
//     ENTIRELY (0x4f75ef), it does not just skip the line;
//   - if absY < clipYlo (0x3d2334) the line is skipped but BOTH cursors
//     still advance by `length` (0x4f77b3);
//   - the X clip is computed from (cursor - line base) + absX, not from a
//     stored X: it's the ABSOLUTE position of the run's start
//     (0x4f7617-0x4f761c).
//   __fastcall(ecx = absX, edx = absY); [esp+4]=src [esp+8]=dst
//   [esp+0xc]=LUT row [esp+0x10]=nLines; ret 0x10.
static void d2_scale_clip_walk(d2rt::Cpu* c, uint32_t src, uint32_t dst, uint32_t pitch,
                               int nLines, int32_t aX, int32_t aY,
                               int32_t cY0, int32_t cY1, int32_t cX0, int32_t cX1,
                               const uint8_t* lut, uint8_t* dbase, uint32_t dlo, uint32_t dspan,
                               uint32_t* wlo, uint32_t* whi,
                               uint32_t* slo, uint32_t* shi, bool* ok,
                               const D2ParW* pw=nullptr)
{
    using namespace d2rt;
    const uint8_t* win=pw?pw->sbase:nullptr;
    uint32_t winLo=pw?pw->sLo:0, winHi=pw?pw->sHi:0;
    auto at=[&](uint32_t a, uint8_t* out)->bool{
        if(a>=winLo && a<winHi){ *out=win[a-winLo]; return true; }
        if(pw||!c){ __atomic_fetch_add(&g_parScaleBail,1,__ATOMIC_RELAXED); return false; }
        if(const void* h=c->hostptr(a,1024)){ win=(const uint8_t*)h; winLo=a; winHi=a+1024;
            *out=win[0]; return true; }
        win=nullptr; winLo=winHi=0; return c->read(a,out,1); };
    uint32_t cur=dst, line=dst; int li=0; int32_t y=aY; uint64_t guard=0;
    if(slo) *slo=src;
    while(li<nLines){
        if(++guard>(1u<<20)){ if(ok) *ok=false; break; }
        uint8_t skip=0, run=0;
        if(!at(src,&skip) || !at(src+1,&run)){ if(ok) *ok=false; break; }
        src+=2;
        if(!skip && !run){ ++li; ++y; line+=pitch; cur=line; continue; }   // 0x4f75cb
        if(y>=cY1) break;                                                  // 0x4f75ef: EXIT
        cur+=skip;                                                         // 0x4f75f5
        if(y<cY0){ cur+=run; src+=run; continue; }                         // 0x4f77b3
        const int32_t pos=(int32_t)(cur-line)+aX;                          // 0x4f7617
        int32_t l=cX0-pos; if(l<0) l=0;                                    // 0x4f7622
        int32_t r=cX1-pos; if(r<0) r=0;                                    // 0x4f7636
        if(r>(int32_t)run) r=(int32_t)run;                                 // 0x4f7643
        const int32_t nn=r-l;
        if(nn>0 && (uint32_t)(nn-1)<=0x1fu){                               // 0x4f765a
            const uint32_t d0=cur+(uint32_t)l, s0=src+(uint32_t)l;
            if(!dbase){ if(d0<*wlo) *wlo=d0; if(d0+(uint32_t)nn>*whi) *whi=d0+(uint32_t)nn;
                        if(g_cellStat){ ++g_csRunP1; g_csPixP1[2]+=(uint32_t)nn; } }
            else if((!pw || ((d0-dlo)>=pw->wlo && (d0-dlo)<pw->whi))
                    && wr_ok(d0-dlo,(uint32_t)nn,dspan,2)){
                   uint8_t* d=dbase+(d0-dlo);
                   if(g_cellStat){ ++g_csRunP2; g_csPixP2[2]+=(uint32_t)nn; }
                   bool fast=false;
                   if(g_cellOpt&CO_SCALE){
                       if(!(s0>=winLo && s0+(uint32_t)nn<=winHi)){ uint8_t b0=0; at(s0,&b0); }
                       if(s0>=winLo && s0+(uint32_t)nn<=winHi){
                           if(g_ovlOn) ovl_note(d0-dlo,(uint32_t)nn,win+(s0-winLo),lut,false);
                           d2_xlat(d, win+(s0-winLo), (uint32_t)nn, lut); fast=true; } }
                   if(!fast){
                       if(g_cellStat) g_csScaleAt+=(uint32_t)nn;
                       for(int32_t k=0;k<nn;k++){ uint8_t b=0; at(s0+(uint32_t)k,&b);
                           if(g_ovlOn) ovl_note(d0-dlo+(uint32_t)k,1,&b,lut,false);
                           d[k]=lut[b]; } } }
        }
        cur+=run; src+=run;                                                // 0x4f77a8
    }
    if(shi) *shi=src;
}

// ===========================================================================
//  Cell-loop drawing body, extracted into a FUNCTION
// ===========================================================================
// Used to be a lambda captured in the shim; it's a standalone function so
// the SAME code can be called by the calling thread AND by worker threads
// (D2_CELLPAR). A duplicated body would risk a pixel divergence in whichever
// leg isn't being tested (same reasoning as CO_NOVEC).
struct D2CellJob { uint32_t dst, src, sLo, sHi; const uint8_t* sp;
                   uint8_t colour; int32_t aX, aY; uint8_t clip; uint8_t kind; };
// kind 0 = FLAT cell (0x4f69b0 / 0x4f88b0)
// kind 1 = SCALE variant, unclipped (0x4f6f60)
// kind 2 = SCALE variant, clipped   (0x4f7580)

// Host-view caches (CO_LUTC). Since hostptr() is pure, they're never
// cleared: H(va) never changes for the life of the process. READ AND
// WRITTEN ONLY BY THE CALLING THREAD (pass 1 and parallel-mode
// pre-resolution) -- never by a worker.
static D2HostCache g_cellLutC, g_cellRowC;
// Writes that overflow their lane's window (BAND mode). A band fits within
// ONE DIB line and boundaries are placed at line starts: this counter must
// stay at ZERO. If it rises, two lanes share a cache line and band slicing
// no longer holds its guarantee.
uint64_t g_parStraddle=0;

struct D2CellCtx {
    d2rt::Cpu* c;                 // NULL for a worker: no guest access
    uint32_t GB;
    uint8_t*  dbase; uint32_t dlo, dspan, pitch;
    int32_t   cY0,cY1,cX0,cX1;
    const uint32_t *dofs,*lens,*sofs;
    const uint32_t *BD,*BS,*BL; int nb;
    // Parallel mode: LUT and identity flag already resolved by the calling
    // thread. nullptr = original path (resolved inline).
    const uint8_t* const* lutv; const uint8_t* identv;
    uint32_t wlo, whi;            // write window (offsets from dlo)
    uint64_t cells;               // output: cells drawn by this lane
};

// PAR=false: the original body, byte for byte (no window test emitted).
// PAR=true : sliced by BANDS -- each lane only writes within [wlo,whi).
template<bool PAR>
static void d2_cell_paint(D2CellCtx& X, const D2CellJob* JV, uint32_t k0, uint32_t k1)
{
    d2rt::Cpu* pc=X.c;
    // Single-entry IDENTITY cache, valid FOR THIS CALL ONLY: the LUT
    // pointer is stable, but its CONTENTS are rewritten by the light shim
    // between calls. Indexed by light LEVEL (colour>>3, 5 bits).
    signed char idTab[32]; std::memset(idTab,-1,sizeof idTab);
    // Everything is copied into locals before the loop. The body writes
    // into the DIB through a uint8_t*, which can ALIAS anything as far as
    // the compiler is concerned: left inside the struct, every bound would
    // be RE-READ after each band drawn -- including the write window,
    // right in the parallel-mode hot path.
    const uint32_t pitch=X.pitch, dlo=X.dlo, dspan=X.dspan;
    const uint32_t wlo=X.wlo, whi=X.whi;
    const int32_t cY0=X.cY0, cY1=X.cY1, cX0=X.cX0, cX1=X.cX1;
    const uint32_t GB=X.GB;
    const uint8_t* const* const lutv=X.lutv; const uint8_t* const identv=X.identv;
    uint8_t* const dbase=X.dbase;
    const uint32_t *dofs=X.dofs,*lens=X.lens,*sofs=X.sofs;
    const uint32_t *BD=X.BD,*BS=X.BS,*BL=X.BL; const int nb=X.nb;
    uint64_t cells=0;
    for(uint32_t k=k0;k<k1;k++){
        const D2CellJob& j=JV[k];
        if(PAR && j.kind==0){
            // Cell-level skimming: the fifteen bands of a flat cell fit
            // within [dst, dst+15*pitch). Signed and 64-bit because dst can
            // be BELOW dlo (dlo is the min over all bands).
            const int64_t a0=(int64_t)j.dst-(int64_t)dlo;
            const int64_t a1=a0+15*(int64_t)pitch;
            if(a1<=(int64_t)wlo || a0>=(int64_t)whi) continue;
        }
        const uint8_t* lut; bool ident=false;
        if(lutv){ lut=lutv[k]; ident=identv[k]!=0; }
        else {
            const uint32_t lva=GB+0x3d2348+(((uint32_t)j.colour>>3)<<8);
            lut=(g_cellOpt&CO_LUTC)
                ? g_cellLutC.get(*pc,lva,256,&g_csLutHit,&g_csLutMiss)
                : d2_hp(*pc,lva,256);   // pc is NEVER null on this path (lutv==nullptr)
            if(!lut) continue;      // cannot happen: same table as pass 1
            if(g_cellOpt&CO_IDENT){
                const unsigned lv=((uint32_t)j.colour>>3)&31u;
                if(idTab[lv]<0){
                    idTab[lv]=(std::memcmp(lut,d2_ident256(),256)==0)?1:0;
                    if(g_cellStat) ++g_csIdentTest; }
                ident=(idTab[lv]!=0); }
        }
        if(!lut) continue;
        ++cells;
        if(g_ovlOn) g_ovlCell=k+1;
        if(j.kind==2){
            const D2ParW pw{ j.sp, j.sLo, j.sHi, wlo, whi };
            d2_scale_clip_walk(pc,j.src,j.dst,pitch,15,j.aX,j.aY,cY0,cY1,cX0,cX1,
                               lut,dbase,dlo,dspan,nullptr,nullptr,nullptr,nullptr,nullptr,
                               lutv?&pw:nullptr);
        } else if(j.kind==1){
            const D2ParW pw{ j.sp, j.sLo, j.sHi, wlo, whi };
            d2_scale_walk(pc,j.src,j.dst,pitch,15,lut,dbase,dlo,dspan,
                          nullptr,nullptr,nullptr,nullptr,nullptr,
                          lutv?&pw:nullptr);
        } else if(!j.clip){
          if(g_cellOpt&CO_BANDS){
            // Valid bands are already sorted and offset: no more
            // re-reading lens[]/dofs[]/sofs[], no more validity check, no
            // more pitch accumulation.
            for(int b=0;b<nb;b++){
                const uint32_t L=BL[b], d0=j.dst+BD[b], s0=j.src+BS[b], off=d0-dlo;
                if(PAR){ if(off<wlo || off>=whi) continue;
                         if(off+L>whi) __atomic_fetch_add(&g_parStraddle,1,__ATOMIC_RELAXED); }
                if(!wr_ok(off,L,dspan,0)) continue;
                if(g_cellStat){ ++g_csP2Band; g_csPixP2[0]+=L; }
                if(g_ovlOn) ovl_note(off,L,j.sp+(s0-j.sLo),lut,ident);
                d2_xlat(dbase+off, j.sp+(s0-j.sLo), L, lut, ident); }
          } else {
            uint32_t dcur=j.dst;
            for(int b=0;b<15;b++,dcur+=pitch){
                const uint32_t L=lens[b];
                if(L-4>0x1c || (L&3)) continue;
                const uint32_t off=(dofs[b]+dcur)-dlo;
                if(PAR){ if(off<wlo || off>=whi) continue;
                         if(off+L>whi) __atomic_fetch_add(&g_parStraddle,1,__ATOMIC_RELAXED); }
                if(!wr_ok(off,L,dspan,0)) continue;
                if(g_cellStat){ ++g_csP2Band; g_csPixP2[0]+=L; }
                uint8_t* d=dbase+off;
                const uint8_t* sp=j.sp+((sofs[b]+j.src)-j.sLo);
                if(g_ovlOn) ovl_note(off,L,sp,lut,ident);
                d2_xlat(d,sp,L,lut,ident); } }
        } else {
            int32_t b0=cY0-j.aY; if(b0<0) b0=0;
            int32_t b1=cY1-j.aY; if(b1>15) b1=15;
            uint32_t cur=j.dst+pitch*(uint32_t)b0;
            for(int32_t b=b0;b<b1;b++,cur+=pitch){
                const int32_t dOff=(int32_t)dofs[b];
                const int32_t pos=dOff+j.aX;
                int32_t l=cX0-pos; if(l<0) l=0;
                int32_t r=cX1-pos; if(r<0) r=0;
                if(r>(int32_t)lens[b]) r=(int32_t)lens[b];
                const int32_t nn=r-l; if(nn<=0) continue;
                const uint32_t off=(cur+(uint32_t)(dOff+l))-dlo;
                if(PAR){ if(off<wlo || off>=whi) continue;
                         if(off+(uint32_t)nn>whi) __atomic_fetch_add(&g_parStraddle,1,__ATOMIC_RELAXED); }
                if(!wr_ok(off,(uint32_t)nn,dspan,3)) continue;
                if(g_cellStat){ ++g_csP2Band; g_csPixP2[1]+=(uint32_t)nn; }
                uint8_t* d=dbase+off;
                const uint8_t* sp=j.sp+((j.src+sofs[b]+(uint32_t)l)-j.sLo);
                if(g_ovlOn) ovl_note(off,(uint32_t)nn,sp,lut,ident);
                d2_xlat(d,sp,(uint32_t)nn,lut,ident); }
        }
    }
    X.cells=cells;
}

// ===========================================================================
//  D2_CELLPAR -- fork-join INSIDE the call
// ===========================================================================
// This loop runs on a single core while two others sit idle. This knob
// splits one call's cells between the calling thread and N workers, and
// only hands control back to the game once EVERYTHING IS DRAWN: the game
// sees no order difference, same pixels, same logical return point.
//
// This is NOT a dedicated background thread. Deferring drawing past the
// call would change draw order relative to the game and break any oracle
// that assumes in-order consumption. Here the join happens INSIDE the
// call: nothing is deferred.
//
// THREE INVARIANTS:
//  1. DISJOINTNESS. See D2_CELLOVL above: the default split is by DIB LINE
//     BANDS, disjoint BY CONSTRUCTION regardless of overlap between cells.
//     Cell-level splitting (D2_CELLPARCELL=1) is only valid when the
//     overlap measurement reads 0.
//  2. THE GIL. A worker NEVER touches the guest CPU: no cpu.read_*, no
//     hostptr, no shim, no GIL. The calling thread resolves everything
//     before handing out work: the cell list (pass 1), source host views,
//     and each cell's colour LUT and identity flag. Workers only see host
//     pointers and copied scalars.
//  3. COHERENCE. The cores share L2 but have separate L1s: the join carries
//     a barrier (acquire on the caller side, release on the worker side,
//     plus an explicit __sync_synchronize) so StretchBlt correctly
//     re-reads DIB bytes written by other cores. Bands are aligned to DIB
//     LINE STARTS, i.e. to the pitch -- no false cache-line sharing
//     between lanes as long as the pitch is a multiple of 32 (checked by a
//     counter).
enum { D2_PAR_MAXW = 3 };
struct D2ParLane {
    D2CellCtx        ctx;
    const D2CellJob* JV;
    uint32_t         k0, k1;
    volatile uint32_t claim;      // 0 = unclaimed; CAS to 1 = "I'll take it"
    volatile uint32_t doneGen;    // generation completed (by whoever)
    uint8_t          par;         // 1 = split by bands (window filter)
    uint64_t         cells;       // cells drawn by this lane
};
static volatile uint32_t g_parGen  = 0;      // published work generation
static volatile int      g_parQuit = 0;
static D2ParLane g_parLane[D2_PAR_MAXW];
int      g_parN       = 0;            // workers REQUESTED (D2_CELLPAR)
int      g_parLive    = 0;            // workers actually started
static bool     g_parCellMode= false;        // D2_CELLPARCELL=1
static uint32_t g_parMinCell = 8;            // below this: sequential
static uint32_t g_parSpin    = 20000;        // pure spin before yielding
static uint32_t g_parYields  = 2000;         // yields before napping
static uint32_t g_parIdleUs  = 200;          // long-wait nap duration
static uint32_t g_parStealAt = 100000;       // join rounds before stealing
static const char* g_parCpu  = "12";         // worker cores (USER_n)
// D2_CELLPARSHARE -- workers' share of the split, as a percentage.
//
// 0 (default) = equal share among the L = workers+1 lanes, i.e. the same
// split as before, byte for byte (cut[i] = dspan*i/L). With one worker
// that's 50/50; with two, an even three-way split.
//
// 1..99 = workers share <n>% of the extent, the calling thread keeps
// (100-n)%. This knob exists because giving the calling thread half as much
// drawing to do does not halve its wall time. Two possible explanations --
// the split is unbalanced (the calling thread does its half PLUS a serial
// prologue), or the serial prologue dominates and no split will catch up
// with it. The two wait counters below settle which.
uint32_t g_parShare   = 0;
// Counters (calling thread only, unless noted).
uint64_t g_parCalls=0, g_parSeqCalls=0, g_parCellsW=0, g_parJoins=0;
uint64_t g_parWaitMax=0, g_parWaitSum=0, g_parSteals=0, g_parDegen=0;
uint64_t g_parPitchOdd=0;
// EFFECTIVE share. The REQUESTED share is a fraction of the [dlo,dhi)
// extent; the OBTAINED share isn't equal to it -- boundaries are rounded up
// to the next line start, and cells aren't distributed evenly across
// height. Two pairs: window extent (bytes) and cells actually drawn by each
// side. One addition per call per lane, nothing more.
uint64_t g_parSpanC=0, g_parSpanW=0, g_parCellsC=0;
// THE TWO WAITS THAT MUST BOTH TREND TOWARD ZERO.
//   - caller side: g_parWaitSum / g_parJoins ("avg="), rounds of the JOIN
//     loop;
//   - worker side: rounds of the SPIN loop spent waiting for work, SPLIT in
//     two -- while the calling thread is INSIDE the call (the SERIAL
//     PROLOGUE: pass 1, band table, pre-resolution, plus any drawing left
//     for the caller) and OUTSIDE the call (the game is doing something
//     else). Without that split, a worker waiting 100,000 rounds doesn't
//     say whether it's waiting on the prologue or on the next frame.
//
// The two numbers are NOT in the same unit: they're rounds of two DIFFERENT
// loops. The criterion is "both trend toward zero", not "both are equal";
// and the floor of the worker-prologue wait is precisely the quantity no
// share adjustment can reduce.
static volatile uint32_t g_parInCall = 0;    // written by the caller, read by workers
uint64_t g_parWIdlePro=0, g_parWIdleGap=0, g_parWIdleN=0;   // atomic increments

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include "platform/vita_host.h"   // engine: pinning of ANOTHER thread (wx86_vita_pin_thread)
// sceKernelDelayThread(0) yields the core to a ready thread of the SAME
// priority. This is what makes the join safe under a RUN-TO-BLOCK
// scheduler, even if pinning failed and two lanes end up sharing a core.
static inline void d2_par_yield(){ sceKernelDelayThread(0); }
static inline void d2_par_nap(uint32_t us){ sceKernelDelayThread(us?us:1); }
#else
#include <sched.h>
#include <unistd.h>
static inline void d2_par_yield(){ sched_yield(); }
static inline void d2_par_nap(uint32_t us){ ::usleep(us?us:1); }
#endif

static void d2_par_run_lane(D2ParLane& L){
    L.ctx.cells=0;
    if(L.JV && L.k1>L.k0){
        if(L.par) d2_cell_paint<true >(L.ctx,L.JV,L.k0,L.k1);
        else      d2_cell_paint<false>(L.ctx,L.JV,L.k0,L.k1);
    }
    L.cells=L.ctx.cells;
}

// Worker body. Tiered ACTIVE WAIT: pure spin (a condition-variable wakeup
// costs tens of microseconds on Vita, for ~420 us of work per call), then
// yield, then nap -- otherwise a worker steals time from the main thread
// during the ~60 s loading screens, or whenever cores aren't actually
// separate.
static void d2_par_worker(int id){
    uint32_t mine=__atomic_load_n(&g_parGen,__ATOMIC_ACQUIRE), idle=0;
    // Worker wait before its FIRST band, split into buckets (see
    // g_parWIdlePro). Two LOCAL counters, flushed to the globals ONCE per
    // work pickup: three atomic increments per call served, never one per
    // spin round.
    uint64_t pro=0, gap=0;
    for(;;){
        if(__atomic_load_n(&g_parQuit,__ATOMIC_RELAXED)) return;
        const uint32_t g=__atomic_load_n(&g_parGen,__ATOMIC_ACQUIRE);
        if(g!=mine){
            mine=g; D2ParLane& L=g_parLane[id];
            __atomic_fetch_add(&g_parWIdlePro,pro,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_parWIdleGap,gap,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_parWIdleN,1ull,__ATOMIC_RELAXED);
            pro=0; gap=0;
            // The calling thread may have STOLEN this lane (join took too
            // long): the CAS settles it, and only the winner publishes
            // doneGen.
            uint32_t z=0;
            if(__atomic_compare_exchange_n(&L.claim,&z,1u,false,
                                           __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
                d2_par_run_lane(L);
                __atomic_store_n(&L.doneGen,g,__ATOMIC_RELEASE);
            }
            idle=0; continue;
        }
        // One extra RELAXED read per wait round: the thread is idle anyway,
        // and this read is what distinguishes "waiting on this call's
        // serial prologue" from "waiting on the next frame". The cache
        // line only bounces twice per call (the caller's two writes to it).
        if(__atomic_load_n(&g_parInCall,__ATOMIC_RELAXED)) ++pro; else ++gap;
        if(idle<g_parSpin){ ++idle; __atomic_signal_fence(__ATOMIC_SEQ_CST); continue; }
        if(idle<g_parSpin+g_parYields){ ++idle; d2_par_yield(); continue; }
        d2_par_nap(g_parIdleUs);
    }
}

#ifdef __vita__
extern "C" int d2vita_pin_self(int mask, unsigned* relu);
static int d2_par_mask_of(int i){
    const char* cpu=g_parCpu; const char cn = cpu[i]?cpu[i]:'1';
    return (cn=='0')?SCE_KERNEL_CPU_MASK_USER_0 : (cn=='2')?SCE_KERNEL_CPU_MASK_USER_2
         : (cn=='3')?0x00080000 : SCE_KERNEL_CPU_MASK_USER_1; }
static int d2_par_entry(SceSize, void* argp){
    const int i=*(const int*)argp;
    // SELF-PINNING (see d2vita_pin_self): the mask set by the creator
    // thread does not reliably hold once the new thread starts running
    // (both workers can end up on the same core). The thread pins itself
    // instead, like the guest runners.
    { unsigned relu=0; const int m=d2_par_mask_of(i); const int rc=d2vita_pin_self(m,&relu);
      char s[112]; std::snprintf(s,sizeof s,"cellpar: ouvrier %d auto-epinglage masque=0x%x rc=0x%08x relu=0x%x",i,(unsigned)m,(unsigned)rc,relu);
      d2vita_progress(s); }
    d2_par_worker(i); return 0; }
#else
static void* d2_par_entry(void* a){ d2_par_worker((int)(intptr_t)a); return nullptr; }
#endif

// Worker startup. Done ONCE, from the shim-install thread (not from inside
// a shim body): creating a thread while holding the GIL from a guest
// runner has never been validated on target, and there's no reason to risk
// it.
static void d2_par_start(){
    if(g_parN<=0) return;
    if(g_parN>D2_PAR_MAXW) g_parN=D2_PAR_MAXW;
    for(int i=0;i<g_parN;i++){
        g_parLane[i].doneGen=0; g_parLane[i].claim=0; g_parLane[i].JV=nullptr;
#ifdef __vita__
        static int s_id[D2_PAR_MAXW];
        s_id[i]=i;
        // Same priority and stack size as the present/watchdog thread (a
        // known-good configuration).
        SceUID th=sceKernelCreateThread("d2_cellpar",d2_par_entry,0x10000100,
                                        64*1024,0,0,nullptr);
        if(th<0){ char m[96]; std::snprintf(m,sizeof m,
                    "cellpar: CreateThread %d KO (rc=0x%08x)",i,(unsigned)th);
                  d2vita_progress(m); break; }
        // Pinned BY THE CREATOR, before start (same discipline as
        // d2_present and the starvation watchdog). Of the three USER masks
        // (psp2/kernel/cpu.h): USER_0 carries ALL guest runners, USER_2
        // already carries present + watchdog + starvation heartbeat, so
        // USER_1 is the free core and is served first, with a second
        // worker defaulting to USER_2.
        // The value `3` requests a FOURTH core (0x00080000), which the SDK
        // does not name; if pinning is refused it shows up in the rc
        // published right below, and the worker just stays wherever the
        // kernel put it.
        const char* cpu=g_parCpu; const char cn = cpu[i]?cpu[i]:'1';
        const int mask = (cn=='0')?SCE_KERNEL_CPU_MASK_USER_0
                       : (cn=='2')?SCE_KERNEL_CPU_MASK_USER_2
                       : (cn=='3')?0x00080000
                                  :SCE_KERNEL_CPU_MASK_USER_1;
        const int prc=wx86_vita_pin_thread((int)th,mask,nullptr);
        // Core registry: diagnostic tooling reads back the mask this
        // thread ACTUALLY carries. A requested pin is not necessarily an
        // obtained pin, so this is read back rather than assumed.
        { char nm[14]; std::snprintf(nm,sizeof nm,"ouvrier%d",i);
          d2vita_core_register(nm,(int)th,(unsigned)mask,prc); }
        if(prc<0){ char m[112]; std::snprintf(m,sizeof m,
                     "cellpar: epinglage ouvrier %d KO (rc=0x%08x) — voie NON separee",
                     i,(unsigned)prc); d2vita_progress(m); }
        if(sceKernelStartThread(th,sizeof(int),&s_id[i])<0){
            d2vita_progress("cellpar: StartThread KO — ouvrier absent"); break; }
#else
        pthread_t th;
        if(pthread_create(&th,nullptr,d2_par_entry,(void*)(intptr_t)i)!=0) break;
        pthread_detach(th);
#endif
        ++g_parLive;
    }
}

static uint32_t d2_room_at(d2rt::Cpu& c, uint32_t room, int32_t x, int32_t y){
    if(!room) return 0;
    auto inside=[&](uint32_t r)->bool{
        uint32_t b[4]; gread_n(c,r+0x4c,b,16);  // +0x4c x0, +0x50 y0, +0x54 w, +0x58 h
        int32_t x0=(int32_t)b[0];
        if(x<x0 || x >= (int32_t)((uint32_t)x0+b[2])) return false;
        int32_t y0=(int32_t)b[1];
        return y>=y0 && y < (int32_t)((uint32_t)y0+b[3]);
    };
    if(inside(room)) return room;
    uint32_t list=c.read_u32(room+0x00), n=c.read_u32(room+0x24);
    if(!list) return 0;
    for(uint32_t i=0;i<n;i++){
        uint32_t p=c.read_u32(list+i*4);
        if(!p) continue;                       // the original skips gaps
        if(inside(p)) return p;
    }
    return 0;
}

// 0x64cb30 -- collision mask at (x,y): room -> sub-object [+0x20] -> grid
// [+0x20], index (y - [sub+4]) * [sub+8] - [sub+0] + x, 16-bit read AND
// mask. 0x27 when a link is missing (the original's value, not ours).
// Variant WITHOUT room lookup: `r` is already resolved for this (x,y). It
// avoids a double lookup -- the original does it twice per cell (0x4756d0
// calls 0x463740, then 0x64cb30 calls it again), but the second result is
// necessarily the same once the first one succeeded.
static uint32_t d2_coll_in(d2rt::Cpu& c, uint32_t r, int32_t x, int32_t y, uint32_t mask){
    if(!r) return 0x27u;
    uint32_t sub=c.read_u32(r+0x20);           // 0x61a010
    if(!sub) return 0x27u;
    uint32_t f[3]; gread_n(c,sub+0x00,f,12);   // +0x00, +0x04, +0x08 contigus
    uint32_t grid=c.read_u32(sub+0x20);
    if(!grid) return 0x27u;
    int32_t idx=(y-(int32_t)f[1])*(int32_t)f[2]-(int32_t)f[0]+x;
    uint16_t v=0; gread_n(c,grid+(uint32_t)idx*2u,&v,2);
    return (uint32_t)(uint16_t)(v & (uint16_t)mask);
}
static uint32_t d2_coll_at(d2rt::Cpu& c, uint32_t room, int32_t x, int32_t y, uint32_t mask){
    return d2_coll_in(c, d2_room_at(c,room,x,y), x, y, mask);
}

// --- per-room memoization ---------------------------------------------------
// In the 48x48 loop at 0x4756d0, five memory accesses are checked per cell,
// four of which stay constant as long as we remain in the same room: the
// bounds [r+0x4c..0x58], then [r+0x20], [sub+0x00..0x08] and [sub+0x20] on
// the collision side. Neighbouring cells almost always fall in the same
// room, so the chain is memoized and only a 16-bit read is paid per cell.
// Why this is safe: the memo is a variable LOCAL to one call of
// d2_lightgrid_build, and the 48x48 loop never yields control (only memory
// reads -- no guest call, no wait, no allocation). There is therefore no
// point at which another thread could modify the room between memoizing it
// and using it. The reasoning does not rely on any global scheduler
// property.
struct D2RoomMemo {
    uint32_t r=0;                       // memoized room, 0 = empty memo
    int32_t  x0=0, y0=0; uint32_t w=0, h=0;
    uint32_t sub=0, grid=0;             // resolved collision chain
    int32_t  f0=0, f1=0, f2=0;
};
static void d2_memo_load(d2rt::Cpu& c, D2RoomMemo& m, uint32_t r){
    m.r=r; m.sub=0; m.grid=0;
    uint32_t b[4]; gread_n(c,r+0x4c,b,16);
    m.x0=(int32_t)b[0]; m.y0=(int32_t)b[1]; m.w=b[2]; m.h=b[3];
    uint32_t sub=c.read_u32(r+0x20);
    if(!sub) return;                    // d2_coll_in would return 0x27
    uint32_t f[3]; gread_n(c,sub+0x00,f,12);
    m.f0=(int32_t)f[0]; m.f1=(int32_t)f[1]; m.f2=(int32_t)f[2];
    m.sub=sub; m.grid=c.read_u32(sub+0x20);
}
static inline bool d2_memo_inside(const D2RoomMemo& m, int32_t x, int32_t y){
    if(x<m.x0 || x >= (int32_t)((uint32_t)m.x0+m.w)) return false;
    return y>=m.y0 && y < (int32_t)((uint32_t)m.y0+m.h);
}
// Identical to d2_coll_in, chain already resolved. Same fallback value 0x27.
static inline uint32_t d2_memo_coll(d2rt::Cpu& c, const D2RoomMemo& m,
                                    int32_t x, int32_t y, uint32_t mask){
    if(!m.sub || !m.grid) return 0x27u;
    int32_t idx=(y-m.f1)*m.f2-m.f0+x;
    uint16_t v=0; gread_n(c,m.grid+(uint32_t)idx*2u,&v,2);
    return (uint32_t)(uint16_t)(v & (uint16_t)mask);
}

// 0x620bb0 -- starting room of the current level. `mode` = [ctx]; 2 and
// 4..5 take *[ctx+0x2c], the others [[ctx+0x2c]+0x1c] (that's 0x648a80).
// The original assert-aborts on a null ctx and dereferences without a
// check on the first path; we return 0, which lets the caller exit
// cleanly.
static uint32_t d2_first_room(d2rt::Cpu& c, uint32_t ctx){
    if(!ctx) return 0;
    int32_t mode=(int32_t)c.read_u32(ctx);
    uint32_t p=c.read_u32(ctx+0x2c);
    if(mode==2 || (mode>3 && mode<=5)) return p?c.read_u32(p):0u;
    if(!p) return 0;
    return c.read_u32(p+0x1c);
}

// --- 8x8 light grid (Game+0xddef0, with 0x475aa0 inlined) -------------------
// Fills the 64 12-byte entries pointed to by `p` (guest memory in normal
// operation, a host COPY under D2_LIGHTVERIFY audit mode: single source).
// Returns the EAX the original would have left (the last 0x475aa0's return
// value).
#define D2_LIGHTOUT (63u*12u+4u)
static uint32_t d2_light_fill(d2rt::Cpu& c, uint32_t GB, const uint8_t* grid,
                              uint8_t* p, uint32_t argx, uint32_t argy)
{
    int32_t orgx=(int32_t)c.read_u32(GB+0x3b0a54);
    int32_t orgy=(int32_t)c.read_u32(GB+0x3b0a58);
    uint32_t colored=c.read_u32(GB+0x312b8c);
    int32_t xs=(int32_t)(argx*8u-8u);                 // lea ecx,[eax*8-8]
    int32_t ys=(int32_t)(argy*8u-8u);
    uint32_t last=0;
    for(int j=0;j<8;j++, ys=(int32_t)((uint32_t)ys+8u)){       // outer loop: y
        int32_t cy=(ys>>3)-orgy; if(cy<0) cy=0; else if(cy>0x2f) cy=0x2f;
        int32_t cx0=xs;
        for(int i=0;i<8;i++, cx0=(int32_t)((uint32_t)cx0+8u), p+=12){   // inner loop: x
            int32_t cx=(cx0>>3)-orgx; if(cx<0) cx=0; else if(cx>0x2f) cx=0x2f;
            uint32_t off=(uint32_t)((cy*48+cx)*8);
            const uint8_t* e=grid+off;
            p[0]=e[4];                                 // intensity, always written
            // Output EAX = the last 0x475aa0's return value: the entry
            // pointer, whose low byte is overwritten by e[7] when colour is active.
            last=GB+0x3b0e68+off;
            if(colored){ p[1]=e[5]; p[2]=e[6]; p[3]=e[7];   // colour: ONLY if
                         last=(last&0xFFFFFF00u)|e[7]; }    // 0x712b8c != 0
        }
    }
    return last;
}
// --- light grid reconstruction (Game+0x756d0) -------------------------------
// 48x48 double loop = 2304 cells around the player. For each cell: the room
// containing (origX+col, origY+row), then collision mask 0x22; if nonzero,
// the entry's "blocked" flag is set to 1. Each entry is 8 bytes: offset 0 =
// that flag, offsets +4..+7 = intensity and colour, later read back by
// 0x475aa0 (the "light grid" port).
// Each cell used to pay for TWO hooked traps (0x463740 and 0x64cb30): 4608
// traps per call, collapsed to a single one.
// Returns the original's EAX: 0x900 on normal exit, 0 on abort.
static uint32_t d2_lightgrid_build(d2rt::Cpu& c, uint32_t GB, uint8_t* shadow=nullptr){
    uint32_t ctx=c.read_u32(GB+0x3a6a70);                  // 0x463dd0
    if(!ctx) return 0;
    uint32_t room=d2_first_room(c,ctx);
    if(!room) return 0;
    int32_t origx=(int32_t)c.read_u32(GB+0x3b0a54);
    int32_t origy=(int32_t)c.read_u32(GB+0x3b0a58);
    uint32_t gbase=GB+0x3b0e68;
    uint8_t* g = shadow ? shadow : (uint8_t*)c.hostptr(gbase,48*48*8);
    D2RoomMemo m;
    for(int32_t row=0;row<48;row++){
        int32_t Y=(int32_t)((uint32_t)origy+(uint32_t)row);
        for(int32_t col=0;col<48;col++){
            int32_t X=(int32_t)((uint32_t)origx+(uint32_t)col);
            uint32_t v;
            // Fast path: same room as the previous cell. d2_room_at tests
            // `room` first anyway, so it would have returned `room` -- same
            // result, without the four constant reads.
            if(m.r && m.r==room && d2_memo_inside(m,X,Y)){
                ++g_gridMemoHit;
                v=d2_memo_coll(c,m,X,Y,0x22u);
            } else {
                ++g_gridMemoMiss;
                uint32_t nr=d2_room_at(c,room,X,Y);
                if(nr){ room=nr; d2_memo_load(c,m,nr);           // room already resolved
                        v=d2_memo_coll(c,m,X,Y,0x22u); }
                else  { room=d2_first_room(c,ctx); m.r=0;        // the original starts over from the base room
                        v=d2_coll_at(c,room,X,Y,0x22u); }        // and redoes the search
            }
            if((uint16_t)v){
                uint32_t off=(uint32_t)((row*48+col)*8);
                if(g){ g[off]=1; g[off+1]=0; g[off+2]=0; g[off+3]=0; }  // dword = 1
                else c.write_u32(gbase+off,1u);   // no host view: slow path
            }
        }
    }
    return 0x900u;
}

// --- DYNAMIC LIGHT placement into the 48x48 grid (Game+0x75420) ------------
// The WRITER counterpart of NATIVELIGHT (which ports the READER, 0x4ddef0):
// the per-frame light pass (Game+0x75800, one call per frame) loops over
// dynamic lights and calls 0x4755a0, which calls 0x475420 for each one.
// 0x475420 places a light disc: it walks the light's occlusion buffer
// ((2r>>3)+1 cells per side, int32) and, for each unoccluded cell, computes
// an intensity then calls 0x4747c0 to blend the colour into the
// corresponding cell of the 48x48x8 grid at 0x7b0e68. This collapses
// thousands of per-cell guest round-trips per frame into a single trap.
//
// ABI per disassembly (Game.exe 1.14d):
//   EDI = light object; NO stack arguments; bare `ret`.
//   Fields read: +0x10 x, +0x14 y, +0x18 radius, +0x24 intensity (byte),
//   +0x25/+0x26/+0x27 R/G/B (bytes), +0x2c nonzero flag (asserted),
//   +0x30 occlusion buffer.
//   Globals read: 0x7b0a54/0x7b0a58 (grid origin), 0x7b0a5c/0x7b0a60
//   (bounds), 0x712b8c (coloured light), 0x7b0a68 (table of 256 16.16
//   reciprocals). Writes ONLY bytes +4..+7 of the grid cells at 0x7b0e68.
//   EAX is DEAD at the caller: 0x4756af does `call 0x475420` then
//   `pop edi/esi/ebp ; ret 4`, and 0x4755a0's only caller (0x4758bb) does
//   `test esi,esi ; mov eax,esi` right after -- the returned value is
//   overwritten before any use. The port therefore leaves EAX UNCHANGED.
//
// Any unrecognized shape (missing buffer, buffer side different from what
// 0x4755a0 allocated, host view unavailable) = FAITHFUL FALLBACK: the guest
// runs its own body, nothing is written.
uint64_t g_lbServed=0, g_lbRepli=0, g_lbCells=0, g_lbSplats=0;
uint64_t g_lbVerifN=0, g_lbVerifBad=0;
// 0x474080: approximate distance (alpha*max + beta*min), pure.
static inline int32_t d2_lb_dist(int32_t dx,int32_t dy){
    const uint32_t a=(uint32_t)(dx<0?-dx:dx), b=(uint32_t)(dy<0?-dy:dy);
    const uint32_t hi=(a>=b)?a:b, lo=(a>=b)?b:a;
    return (int32_t)((hi*0x3d7u + lo*0x197u) >> 10);   // shr, not sar
}
// 0x4747c0: blends one contribution into a grid cell.
static inline void d2_lb_splat(uint8_t* g, const uint32_t* recip,
                               int32_t px,int32_t py,int32_t a,
                               uint32_t R,uint32_t G,uint32_t B,
                               int32_t ox,int32_t oy,int32_t colored){
    const uint32_t gx=(uint32_t)((px>>3)-ox); if(gx>0x2fu) return;
    const uint32_t gy=(uint32_t)((py>>3)-oy); if(gy>0x2fu) return;
    uint8_t* p=g+(size_t)(gy*48u+gx)*8u;
    const uint32_t old=p[4];
    uint32_t ni=old+(uint32_t)a; if(ni>=0xffu) ni=0xffu;      // cmp/jb: unsigned
    if(!colored){ p[4]=(uint8_t)ni; p[5]=0; p[6]=0; p[7]=0; ++g_lbSplats; return; }
    uint32_t w; std::memcpy(&w,recip+ni,4);                   // table of 16.16 reciprocals
    int32_t cr=(int32_t)(((uint32_t)((int32_t)(p[5]*old)+(int32_t)(R*(uint32_t)a)))*w>>16);
    int32_t cg=(int32_t)(((uint32_t)((int32_t)(p[6]*old)+(int32_t)(G*(uint32_t)a)))*w>>16);
    int32_t cb=(int32_t)(((uint32_t)((int32_t)(p[7]*old)+(int32_t)(B*(uint32_t)a)))*w>>16);
    if(cr>0xff) cr=0xff; if(cg>0xff) cg=0xff; if(cb>0xff) cb=0xff;   // cmp/jle: signed
    p[4]=(uint8_t)ni; p[5]=(uint8_t)cr; p[6]=(uint8_t)cg; p[7]=(uint8_t)cb;
    ++g_lbSplats;
}
// Ported body. `g` = host view of the grid (18432 bytes): guest memory
// itself, or a COPY (D2_LIGHTMAPVERIFY audit mode).
// Returns false = FALLBACK (nothing was written).
static bool d2_lightblob_apply(d2rt::Cpu& c, uint32_t GB, uint32_t L, uint8_t* g){
    if(!g||!L) return false;
    const int32_t r=(int32_t)c.read_u32(L+0x18);
    if(r<=0 || r>=0x100) return true;                       // early exit in the original
    const int32_t lx=(int32_t)c.read_u32(L+0x10), ly=(int32_t)c.read_u32(L+0x14);
    const int32_t x0=lx-(int32_t)((uint32_t)lx&7u)-r;
    const int32_t y0=ly-(int32_t)((uint32_t)ly&7u)-r;
    const int32_t n=(2*r)>>3;                               // sar
    const int32_t ox=(int32_t)c.read_u32(GB+0x3b0a54), oy=(int32_t)c.read_u32(GB+0x3b0a58);
    const int32_t bx=(int32_t)c.read_u32(GB+0x3b0a5c), by=(int32_t)c.read_u32(GB+0x3b0a60);
    if((x0>>3)>bx) return true;
    if((x0>>3)+n+1<ox) return true;
    if((y0>>3)>by) return true;
    if((y0>>3)+n+1<oy) return true;
    const uint32_t buf=c.read_u32(L+0x30);
    if(!buf) return false;                                  // the original asserts here: fallback
    if(!c.read_u32(L+0x2c)) return false;                   // same
    if(n<0) return true;
    // GUARD: 0x4755a0 allocates ((r>>3)*2+1)^2 cells of 4 bytes, while the
    // loop walks (n+1)^2 with n=(2r)>>3. The two coincide as long as r is a
    // multiple of 8 (which the game always uses: +/-8 steps, capped at
    // 0xf8). If that were ever not the case, the original would read past
    // the buffer: we REFUSE instead of reproducing an out-of-bounds read.
    if(n+1 != 2*(r>>3)+1) return false;
    const int32_t rows=n+1;
    // HOST VIEWS. Without them the port is SLOWER than the translated
    // code: the original reads the occlusion buffer via one translated
    // block `mov`, while a c.read_u32() per cell is a virtual call plus an
    // address resolution. Both ranges are contiguous and of known size; if
    // either lacks a host view, FALLBACK (nothing half-done).
    const uint8_t* bh=(const uint8_t*)c.hostptr(buf,(uint32_t)rows*(uint32_t)rows*4u);
    const uint32_t* recip=(const uint32_t*)c.hostptr(GB+0x3b0a68,256u*4u);
    if(!bh||!recip) return false;
    const int32_t step=(int32_t)((c.read_u32(L+0x24)&0xffu)<<16)/r;      // movzx+cdq+idiv
    const uint32_t rgb=c.read_u32(L+0x24);
    const uint32_t R=(rgb>>8)&0xffu, G=(rgb>>16)&0xffu, B=(rgb>>24)&0xffu;
    const int32_t colored=(int32_t)c.read_u32(GB+0x312b8c);
    int32_t py=y0;
    for(int32_t row=0;row<rows;row++){
        const int32_t dyv=ly-py, dy=(dyv<0)?-dyv:dyv;
        int32_t px=x0;
        for(int32_t col=0;col<rows;col++,bh+=4,px+=8){
            int32_t v; std::memcpy(&v,bh,4);
            if(v>=0x10) continue;
            const int32_t dxv=lx-px, dx=(dxv<0)?-dxv:dxv;
            int32_t a=r-d2_lb_dist(dx,dy);
            a=(a*step)>>16;                                  // imul puis sar 16
            a=(a*(8-(v>>1)))>>3;
            if(a<=0) continue;
            d2_lb_splat(g,recip,px,py,a,R,G,B,ox,oy,colored);
        }
        py+=8;
    }
    g_lbCells+=(uint64_t)rows*(uint64_t)rows;
    return true;
}

// ===========================================================================
//  NATIVELIGHTOCC -- per-light OCCLUSION FIELD, Game+0x750f0 (.\DRAW\dLightMap.cpp)
// ===========================================================================
// UPSTREAM of the two ports above. 0x4755a0 clears the light's "computed"
// flag (0x475606) on EVERY frame, so 0x4750f0 rebuilds the complete 64x64
// occlusion field of every dynamic light every frame, and 0x475420
// (NATIVELIGHTMAP) then consumes the result. A console D2_LAGWATCH pass
// (21/09, town patrol, frames over 70 ms) attributes 22.4 % of the guest
// samples taken during the spikes to this single function -- second only to
// memcpy, and it recomputes exactly when the view moves.
//
// WHAT THE ORIGINAL DOES. ESI = the light object, no stack argument, bare
// `ret`, EAX DEAD at the only call site (0x4756a8 falls straight into
// `mov edi,esi ; call 0x475420`). Three phases:
//
//   phase 0  r=[L+0x18]; return if r<=0 or r>=0x100. memset(0x7a8a50,0,0x1000).
//            return if [L+0]==6. Resolve the light's unit through the unit
//            hash tables (0x463990/0x4639b0 -> 0x463940), then that unit's
//            room (0x620bb0) and its (x,y) (0x45adf0/0x45ae20).
//   phase 1  4096 cells: BLOCKED[j*64+i] = collision(room, X0+i, Y0+j, 0x22)
//            ? 0x10 : 0, into the int32 field at 0x7aca50, with X0/Y0 = the
//            unit's position minus 32. ONE guest call to 0x64cb30 per cell,
//            which itself calls 0x463740 (plus 0x619790 on its slow path) and
//            0x61a010: 12 000 to 16 000 guest calls.
//   phase 2  R=r>>3 >= 2: eight symmetric sweeps propagate light outwards
//            into the int32 field at 0x7a8a50. Each step is a PAIR of guest
//            calls -- 0x474b50 publishes sx/sy/frac/mode into four scratch
//            globals, 0x474c00 reads them back and writes one cell. 16 calls
//            per inner iteration, about 2 400 for a radius of 128.
//   phase 3  copies the (2R+1)^2 int32 sub-square centred on the light out of
//            0x7a8a50 into the light's own buffer [L+0x30], then [L+0x2c]=1.
//
// WIDTH OF THE PORT. All of that is absorbed: ONE trap per rebuild in place
// of roughly 15 000 guest calls. Porting only the leaf 0x64cb30 would have
// paid one traversal per cell -- the shape that measured -2.7 % on the
// world-to-screen projection. The climb STOPS AT 0x4750f0 and not at its
// caller 0x4755a0, which allocates and frees the light buffer through D2's
// pooled allocator (0x40b380 / 0x40b3c0, with file+line arguments): a side
// effect that cannot be reproduced natively without owning the pool.
//
// EVERYTHING ABSORBED IS PURE. 0x64cb30 -> 0x463740 (+0x619790) -> 0x61a010
// only READ room and collision-map fields. 0x474b50 / 0x474c00 touch nothing
// but the two scratch fields and the four scratch globals, and are called
// from nowhere else in the image. The only memory this port writes is the
// memory the original writes.
//
// ONE HOST VIEW. The two fields and the four scratch globals are CONTIGUOUS:
//   0x7a8a48 sy | 0x7a8a4c frac | 0x7a8a50 LUM[4096] | 0x7aca50 BLOCKED[4096]
//   | 0x7b0a50 sx | (4 unrelated globals) | 0x7b0a64 mode
// so the port takes a single host view of [0x7a8a48, +0x8110) and runs all
// three phases inside it. The span is 0x8110 and not 0x8020 bytes ON PURPOSE:
// at the maximum radius (r=0xf8 => R=31) phase 2 samples BLOCKED[4160], i.e.
// 65 int32 PAST the end of the field. The original reads the adjacent globals
// there; a contiguous host view reproduces that byte for byte instead of
// running off the end of a shorter buffer.
uint64_t g_loServed=0, g_loRepli=0, g_loCells=0, g_loSteps=0;
uint64_t g_loVerifN=0, g_loVerifBad=0;

enum {                                   // byte offsets inside the host view
    LO_SY=0, LO_FRAC=4, LO_LUM=8, LO_GRID=8+4096*4,
    LO_SX=0x8008, LO_MODE=0x801c, LO_SPAN=0x8110,
    LO_MAXIDX=4160                       // highest BLOCKED[] element phase 2 samples
};
// The host view must hold BLOCKED[LO_MAXIDX] whole.
static_assert(LO_GRID + (LO_MAXIDX+1)*4 <= LO_SPAN, "vue hote trop courte pour BLOCKED[4160]");

// One room of the search list with its collision chain already resolved. The
// original re-walks that chain for EVERY one of the 4096 cells (0x64cb30 is
// called with the same base room each time and never carries a result
// forward); resolving it once per room and indexing a host array is the same
// arithmetic on the same bytes.
struct D2LoRoom {
    uint32_t room;                       // guest VA, for list de-duplication
    int32_t  x0,y0; uint32_t w,h;        // [room+0x4c..+0x58] -- the 0x463740 test
    uint32_t sub, grid;                  // [room+0x20] (0x61a010), [sub+0x20]
    int32_t  f0,f1,f2;                   // [sub+0x00..+0x08]
    uint32_t gn;                         // collision cells = [sub+8]*[sub+0xc]
    const uint16_t* gh;                  // host view of the collision map, or null
};
struct D2LoRooms { D2LoRoom e[64]; int n=0; int hint=0; bool disjoint=false; };

static inline bool d2_lo_inside(const D2LoRoom& r, int32_t x, int32_t y){
    if(x<r.x0 || x >= (int32_t)((uint32_t)r.x0+r.w)) return false;
    return y>=r.y0 && y < (int32_t)((uint32_t)r.y0+r.h);
}
static void d2_lo_load(d2rt::Cpu& c, D2LoRoom& e, uint32_t r){
    e=D2LoRoom{}; e.room=r;
    uint32_t b[4]; gread_n(c,r+0x4c,b,16);
    e.x0=(int32_t)b[0]; e.y0=(int32_t)b[1]; e.w=b[2]; e.h=b[3];
    const uint32_t sub=c.read_u32(r+0x20);
    if(!sub) return;                                   // 0x64cb30 -> 0x27
    e.sub=sub;
    uint32_t f[4]; gread_n(c,sub+0x00,f,16);           // +0 x0, +4 y0, +8 w, +0xc h
    e.f0=(int32_t)f[0]; e.f1=(int32_t)f[1]; e.f2=(int32_t)f[2];
    e.grid=c.read_u32(sub+0x20);
    if(!e.grid) return;                                // 0x64cb30 -> 0x27
    const uint64_t n=(uint64_t)f[2]*(uint64_t)f[3];
    if(n && n<(1u<<24)){ e.gn=(uint32_t)n;
        e.gh=(const uint16_t*)c.hostptr(e.grid,(uint32_t)n*2u); }
}
// Snapshot of the search list 0x463740 walks: the base room, then its
// neighbours [base+0x00][0..[base+0x24]) in ORDER. Entries already present
// are dropped -- a repeat can never be the FIRST match, so the answer is
// unchanged. Returns false if the list is longer than the snapshot.
static bool d2_lo_rooms(d2rt::Cpu& c, D2LoRooms& S, uint32_t base){
    d2_lo_load(c,S.e[0],base); S.n=1;
    const uint32_t list=c.read_u32(base+0x00), cnt=c.read_u32(base+0x24);   // 0x619790
    if(list) for(uint32_t i=0;i<cnt;i++){
        const uint32_t p=c.read_u32(list+i*4);
        if(!p) continue;                               // the original skips gaps
        bool dup=false; for(int k=0;k<S.n;k++) if(S.e[k].room==p){ dup=true; break; }
        if(dup) continue;
        if(S.n>=(int)(sizeof S.e/sizeof S.e[0])) return false;
        d2_lo_load(c,S.e[S.n++],p);
    }
    // Pairwise-disjoint boxes make "first match in order" and "any match" the
    // same room, which is what makes the one-entry hint below EXACT. Any
    // overlap -- or any box wide enough that the original's wrapping bound
    // test and this one could disagree -- falls back to the ordered scan.
    S.disjoint=true;
    for(int a=0;a<S.n && S.disjoint;a++){
        if(S.e[a].w>0x10000u || S.e[a].h>0x10000u){ S.disjoint=false; break; }
        for(int b=a+1;b<S.n;b++){
            const D2LoRoom &A=S.e[a], &B=S.e[b];
            const int64_t ax1=(int64_t)A.x0+A.w, ay1=(int64_t)A.y0+A.h;
            const int64_t bx1=(int64_t)B.x0+B.w, by1=(int64_t)B.y0+B.h;
            if(A.x0<bx1 && B.x0<ax1 && A.y0<by1 && B.y0<ay1){ S.disjoint=false; break; }
        }
    }
    return true;
}
static inline const D2LoRoom* d2_lo_find(D2LoRooms& S, int32_t x, int32_t y){
    if(S.disjoint && d2_lo_inside(S.e[S.hint],x,y)) return &S.e[S.hint];
    for(int k=0;k<S.n;k++) if(d2_lo_inside(S.e[k],x,y)){ S.hint=k; return &S.e[k]; }
    return nullptr;                                    // 0x463740 returns 0
}
// 0x64cb30 with mask 0x22, room search included. 0x27 is the ORIGINAL's value
// for a missing link, not ours -- and it is non-zero, so the cell reads as
// blocked, exactly as in the guest.
static inline uint32_t d2_lo_coll(d2rt::Cpu& c, D2LoRooms& S, int32_t x, int32_t y){
    const D2LoRoom* r=d2_lo_find(S,x,y);
    if(!r || !r->sub || !r->grid) return 0x27u;
    const int32_t idx=(y-r->f1)*r->f2-r->f0+x;
    uint16_t v=0;
    if(r->gh && idx>=0 && (uint32_t)idx<r->gn) v=r->gh[idx];
    else gread_n(c,r->grid+(uint32_t)idx*2u,&v,2);     // outside the known extent:
    return (uint32_t)(uint16_t)(v & (uint16_t)0x22u);  // read it the guest's way
}

// 0x474c00's sample: BLOCKED wins when non-zero, otherwise the current LUM.
static inline int32_t d2_lo_sample(const int32_t* lum, const int32_t* blk, int32_t i){
    const int32_t b=blk[i]; return b?b:lum[i];
}
// 0x474b50 -- Bresenham-style setup. NOTE which globals each branch leaves
// UNTOUCHED: mode 0 writes only the mode, modes 1 and 4 leave `frac` stale.
// The port reproduces that, because the four globals survive the call and the
// oracle compares them.
static inline void d2_lo_setup(int32_t* sx,int32_t* sy,int32_t* fr,int32_t* md,
                               int32_t lx,int32_t ly,int32_t ax,int32_t ay){
    int32_t dx=lx-ax, dy=ly-ay;
    if(dx==0 && dy==0){ *md=0; return; }               // 0x474b70
    *sx = (dx<0)?-1:1; if(dx<0) dx=-dx;                // 0x474b7d / 0x474b86
    *sy = (dy<0)?-1:1; if(dy<0) dy=-dy;                // 0x474b90 / 0x474b99
    if(dx==0){ *md=4; return; }                        // 0x474ba3
    if(dy==0){ *md=1; return; }                        // 0x474bb6
    if(dx>=dy){ *fr=(int32_t)((uint32_t)dy<<8)/dx; *md=2; }   // 0x474bc5, idiv
    else      { *fr=(int32_t)((uint32_t)dx<<8)/dy; *md=3; }   // 0x474be1
}
// 0x474c00 -- one propagation step, dispatched on the mode the setup left.
// Mode 0 and anything above 4 are a bare `ret` (0x474d4d / 0x474d50).
static inline void d2_lo_step(int32_t* lum,const int32_t* blk,
                              int32_t sx,int32_t sy,int32_t fr,int32_t md,
                              int32_t row,int32_t col){
    const int32_t o=row*64+col;
    switch(md){
    case 1: lum[o]=d2_lo_sample(lum,blk,row*64+sx+col); return;              // 0x474cf0
    case 4: lum[o]=d2_lo_sample(lum,blk,(row+sy)*64+col); return;            // 0x474d1f
    case 2: { const int32_t a=d2_lo_sample(lum,blk,row*64+sx+col);           // 0x474c19
              const int32_t b=d2_lo_sample(lum,blk,(row+sy)*64+sx+col);
              lum[o]=(a*(0x100-fr)+b*fr)>>8; return; }                       // sar 8
    case 3: { const int32_t a=d2_lo_sample(lum,blk,(row+sy)*64+col);         // 0x474c86
              const int32_t b=d2_lo_sample(lum,blk,(row+sy)*64+sx+col);
              lum[o]=(a*(0x100-fr)+b*fr)>>8; return; }
    default: return;
    }
}
// Bytes the light's occlusion buffer was allocated with at 0x4756a0:
// ((r>>3)*2+1)^2 int32. 0 when the function returns before phase 3.
static inline uint32_t d2_lo_bufbytes(int32_t r){
    if(r<=0||r>=0x100) return 0;
    const uint32_t s=(uint32_t)((r>>3)*2+1);
    return s*s*4u;
}

// Ported body of Game+0x750f0.
//   S   host view of [0x7a8a48, +LO_SPAN) -- guest memory, or a COPY (oracle)
//   B   host view of the light's buffer [L+0x30] -- guest memory, or a COPY
//   Bn  size of that view, which must be exactly what 0x4756a0 allocated
// Returns 0 = FAITHFUL FALLBACK (nothing written at all, the guest runs its
// own body), 1 = served through one of the original's early returns,
// 2 = served in full (the caller owes [L+0x2c]=1).
//
// EVERY reason to refuse is evaluated BEFORE the first byte is written, so a
// fallback is never half a rebuild. The phases themselves cannot fail.
static int d2_lightocc_build(d2rt::Cpu& c, uint32_t GB, uint32_t L,
                             uint8_t* S, uint8_t* B, uint32_t Bn)
{
    if(!S || !L || ((uintptr_t)S & 3u)) return 0;      // the fields are int32

    const int32_t r=(int32_t)c.read_u32(L+0x18);
    if(r<=0 || r>=0x100) return 1;                     // 0x4750fb / 0x475106

    int32_t* const lum=(int32_t*)(S+LO_LUM);
    int32_t* const blk=(int32_t*)(S+LO_GRID);
    int32_t* const psx=(int32_t*)(S+LO_SX),  * const psy=(int32_t*)(S+LO_SY);
    int32_t* const pfr=(int32_t*)(S+LO_FRAC),* const pmd=(int32_t*)(S+LO_MODE);

    const int32_t n  =(int32_t)(((uint32_t)r*2u)>>3);  // add eax,eax ; sar eax,3
    const int32_t R  =r>>3;
    const int32_t cx8=(int32_t)c.read_u32(L+0x10)>>3;
    const int32_t cy8=(int32_t)c.read_u32(L+0x14)>>3;
    const uint32_t typ=c.read_u32(L+0x00);

    // The memset is idempotent, so the three early returns below may be
    // decided before it without changing what the guest sees.
    if(typ==6u){ std::memset(lum,0,0x1000); return 1; }        // 0x475148

    // The light's unit: 128-entry hash page per type, bucket id&0x7f, chained
    // on +0xe4 (0x463940). Table 0x7a5e70 client / 0x7a5270 server.
    if(typ>5u) return 0;                               // wild page index: let the guest do it
    uint8_t srv=0; c.read(L+0x08,&srv,1);              // cmp BYTE PTR [esi+8],0
    const uint32_t id=c.read_u32(L+0x04);
    uint32_t unit=c.read_u32(GB+(srv?0x3a5270u:0x3a5e70u)+typ*512u+(id&0x7fu)*4u);
    for(int guard=0; unit && c.read_u32(unit+0x0c)!=id; ++guard){
        if(guard>4096) return 0;                       // circular chain: refuse
        unit=c.read_u32(unit+0xe4);
    }
    if(!unit){ std::memset(lum,0,0x1000); return 1; }  // 0x475168
    if(c.read_u32(unit+0x00)!=typ) return 0;           // 0x46396c: the original ABORTS

    const uint32_t room=d2_first_room(c,unit);         // 0x620bb0
    if(!room) return 0;                                // 0x47517b: the original ABORTS

    // 0x45adf0 / 0x45ae20: the unit's position, two shapes by unit kind.
    const uint32_t up=c.read_u32(unit+0x2c);
    int32_t ux,uy;
    if(typ==2u || (typ>3u && typ<=5u)){
        if(!up) return 0;                              // the original dereferences null
        ux=(int32_t)c.read_u32(up+0x0c); uy=(int32_t)c.read_u32(up+0x10);
    } else if(!up){ ux=0; uy=0; }                      // 0x45ae0f / 0x45ae3f
    else { uint16_t a=0,b=0; gread_n(c,up+0x02,&a,2); gread_n(c,up+0x06,&b,2);
           ux=(int32_t)(uint32_t)a; uy=(int32_t)(uint32_t)b; }
    const int32_t X0=ux-0x20, Y0=uy-0x20;

    // Phase 3's preconditions, checked here so the refusal costs nothing.
    if(R>31) return 0;                                 // unreachable (r<0x100); the
                                                       // LO_MAXIDX bound assumes it
    if(c.read_u32(L+0x2c)!=0) return 0;                // 0x47539b: the original ABORTS
    const uint32_t bva=c.read_u32(L+0x30);
    if(!bva) return 0;                                 // 0x4753ab: the original ABORTS
    // 0x4756a0 allocates (2*(r>>3)+1)^2 int32 while the copy walks (n+1)^2
    // with n=(2r)>>3. They coincide only while r is a multiple of 8 (which is
    // all the game ever uses: +/-8 steps capped at 0xf8). Otherwise the
    // original overruns its own buffer -- we REFUSE instead of reproducing it.
    if(n<0 || n+1 != 2*R+1) return 0;
    if(!B || Bn != (uint32_t)(n+1)*(uint32_t)(n+1)*4u) return 0;
    D2LoRooms rooms;
    if(!d2_lo_rooms(c,rooms,room)) return 0;           // search list too long

    // ---- nothing below can refuse ----------------------------------------
    std::memset(lum,0,0x1000);                         // 0x681ef0, 0x1000 BYTES

    // phase 1 -- 0x4751c3..0x475205
    for(int32_t j=0;j<64;j++){
        const int32_t Y=(int32_t)((uint32_t)Y0+(uint32_t)j);
        int32_t* row=blk+j*64;
        for(int32_t i=0;i<64;i++){
            const int32_t X=(int32_t)((uint32_t)X0+(uint32_t)i);
            row[i]=d2_lo_coll(c,rooms,X,Y)?0x10:0;     // neg ax ; sbb ; and 0x10
        }
    }
    g_loCells+=4096;

    // phase 2 -- 0x475207..0x475398. Eight sweeps per inner step; each pair
    // is setup-then-step, and the step reads the globals the setup just
    // wrote, so the order may not be rearranged.
    // The loop bounds are what makes the LO_MAXIDX bound hold: i is at most
    // R-2 <= 29 and k at most 2+i <= 31, so row and col both stay inside
    // [1,63] and the largest index SAMPLED is 64*64+64 = LO_MAXIDX, while
    // every index WRITTEN stays inside [65,4095].
    if(R>=2){
        // 0x474b50 re-reads [L+0x10]/[L+0x14] on each of its calls; nothing in
        // these loops writes the light object, so the two reads are hoisted.
        const int32_t lx=(int32_t)c.read_u32(L+0x10), ly=(int32_t)c.read_u32(L+0x14);
        for(int32_t i=0;i<=R-2;i++){
            const int32_t A=cy8*8+20+8*i, C=cy8*8-12-8*i;    // Y of the horizontal sweeps
            const int32_t Bx=cx8*8+20+8*i, D=cx8*8-12-8*i;   // X of the vertical sweeps
            const int32_t rlo=30-i, rhi=34+i;
            for(int32_t k=0;k<=2+i;k++){
                const int32_t Fp=cx8*8+4+8*k, Fm=cx8*8+4-8*k;
                const int32_t Ep=cy8*8+4+8*k, Em=cy8*8+4-8*k;
                const int32_t clo=32-k, chi=32+k;
                struct { int32_t ax,ay,row,col; } P[8]={
                    {Fm,C ,rlo,clo}, {Fp,C ,rlo,chi},        // 0x475288 / 0x4752a1
                    {Fm,A ,rhi,clo}, {Fp,A ,rhi,chi},        // 0x4752ba / 0x4752d3
                    {Bx,Em,clo,rhi}, {Bx,Ep,chi,rhi},        // 0x4752ec / 0x475305
                    {D ,Em,clo,rlo}, {D ,Ep,chi,rlo},        // 0x47531e / 0x475337
                };
                for(int q=0;q<8;q++){
                    d2_lo_setup(psx,psy,pfr,pmd,lx,ly,P[q].ax,P[q].ay);
                    d2_lo_step(lum,blk,*psx,*psy,*pfr,*pmd,P[q].row,P[q].col);
                }
                ++g_loSteps;          // COUNTED, not derived from a closed form
            }
        }
    }

    // phase 3 -- 0x4753bc..0x47540b. Source row (32-R), column (32-R), row
    // stride 64; the extent is (32-R)*65 + 2R*65 = 2080+65R <= 4095 for R<=31.
    const int32_t* src=lum+(int32_t)(32-R)*65;
    int32_t* dst=(int32_t*)B;
    for(int32_t row=0;row<n+1;row++, src+=64, dst+=n+1)
        std::memcpy(dst,src,(size_t)(n+1)*4u);
    return 2;
}

// --- cell RLE stream walker (Game+0x206d40), SINGLE source ------------------
// Three modes, one body: if the port and the audit diverged, the audit
// would prove nothing. Returns the EAX the original would have left.
//   RLE_APPLY   writes to guest memory (normal path)
//   RLE_MEASURE writes nothing, only measures the [lo,hi) extent of writes
//   RLE_WINDOW  writes into a host window (D2_RLEVERIFY audit mode)
enum { RLE_APPLY=0, RLE_MEASURE=1, RLE_WINDOW=2 };
struct RleWin { uint8_t* p; uint32_t lo, n; };
static uint32_t d2_rle_walk(d2rt::Cpu& c, int mode, uint32_t src, uint32_t dst,
                            uint32_t lines, uint32_t stride,
                            uint32_t* lo, uint32_t* hi, RleWin* w)
{
    using namespace d2rt;
    uint32_t nextLine=dst+stride;                    // [ebp-4]
    uint32_t eax=stride;                             // mov eax,[ebp+0xc]
    uint64_t guard=0;
    // Host window over the control stream: it used to be read ONE BYTE AT A
    // TIME through a virtual call plus a bounds check. A sliding 1 KiB
    // window cuts that to one check per 1024 bytes. If the host view is
    // unavailable, falls back to the same one-byte-at-a-time read.
    const uint8_t* win=nullptr; uint32_t winLo=0, winHi=0;
    auto ctl_at=[&](uint32_t a, uint8_t* out)->bool{
        if(a>=winLo && a<winHi){ *out=win[a-winLo]; return true; }
        uint32_t n=1024;
        if(const void* h=c.hostptr(a,n)){ win=(const uint8_t*)h; winLo=a; winHi=a+n;
            *out=win[0]; return true; }
        win=nullptr; winLo=winHi=0;
        return c.read(a,out,1);
    };
    for(;;){
        if(++guard>(1u<<22)){                        // corrupted stream: the guest
            static bool once=false;                  // would loop forever too, but here
            if(!once){ once=true;                    // nothing preempts the shim.
                std::printf("[hot/rle] flux RLE non terminant, arret de securite\n"); }
            break; }
        uint8_t ctl; if(!ctl_at(src,&ctl)) break; ++src;
        eax=(eax&0xFFFFFF00u)|ctl;                   // mov al,[ebx]
        if(!(ctl&0x80u)){                            // test al,al ; jge -> copie
            uint32_t n=ctl;
            if(n){
                if(mode==RLE_MEASURE){ if(dst<*lo) *lo=dst; if(dst+n>*hi) *hi=dst+n; }
                else if(mode==RLE_WINDOW){
                    for(uint32_t k=0;k<n;k++){ uint8_t b=0;
                        if(src+k>=w->lo && src+k-w->lo<w->n) b=w->p[src+k-w->lo];
                        else c.read(src+k,&b,1);
                        if(dst+k>=w->lo && dst+k-w->lo<w->n) w->p[dst+k-w->lo]=b; }
                } else {
                    uint8_t* dH=(uint8_t*)c.hostptr(dst,n);
                    const uint8_t* sH=(const uint8_t*)c.hostptr(src,n);
                    // memmove: MSVC's memcpy at 0x6829c0 handles overlap
                    // (checks dst against [src,src+n) up front) -- we
                    // reproduce that guarantee, not a weaker one.
                    if(dH&&sH) std::memmove(dH,sH,n);
                    else { uint8_t tmp[128]; c.read(src,tmp,n); c.write(dst,tmp,n); }
                    g_hotRleBytes+=n;
                    // If the copy writes into the cached control window, that
                    // window becomes stale: the guest always re-reads fresh
                    // memory, so we drop the cache.
                    if(win && dst < winHi && dst+n > winLo){ win=nullptr; winLo=winHi=0; }
                }
            }
            eax=dst;                                 // memcpy renvoie son dst
            src+=n; dst+=n;
        } else {
            uint32_t v=(uint32_t)(ctl&0x7fu);
            eax=(eax&0xFFFFFF00u)|v;                 // and al,0x7f
            if(!v){                                  // 0x80: next line
                if(--lines==0) break;
                eax=nextLine; dst=eax; eax+=stride; nextLine=eax;
            } else dst+=v;                           // 0x81..FF: skip v pixels
        }
    }
    return eax;
}

void native_hooks_cellengine_install_blit(Cpu* cpu, Bridge& br){
    // ---- native cell blit (profile: ~32% of ALL time) ---------------------
    // NATIVEBLIT=0 disables. Pixel-exactness is enforced by the title gate
    // (framebuffer md5 unchanged; verify changes with a NATIVEBLIT=0 A/B).
    if(!getenv("NATIVEBLIT")||strcmp(getenv("NATIVEBLIT"),"0")){
        // 1.14d monolith: palette-translate cell blit statically linked into
        // Game.exe at RVA 0xf69b0. Profile (Rogue Encampment, 6000 frames):
        // this single function is ~32% of ALL execution samples — the hottest
        // code in the game and, until now, fully dynarec'd. 15 bands, per-band
        // dst/len/src offset tables spaced 0x3c apart, len-4>0x1c / len&3 skip
        // guard, dst cursor += pitch per band. ARG mapping: __fastcall(ecx=src
        // base, edx=dst cursor) + one stack arg (LUT), ret 4. Tables (RVA):
        // dstOff 0x32db48, len 0x32db84, srcOff 0x32dbc0, pitch 0x3d544c.
        if(g_114 && !br.module_base("D2gfx.dll")){
            uint32_t GB=g_d2base;
            Shim s; s.argc=1; s.stdcall_cleanup=true; s.tag="native!d2_cell_blit_114";
            auto blitBody=[GB](Cpu&c)->uint32_t{
                ++g_blitN;
                sh_ev(1);  // shape: guest blit (disappears if the port is widened)
                // BATCHED READ (cpu.h regs_gp): ESP+ECX+EDX in ONE
                // per-thread state resolution instead of three. Same
                // values -- reading general registers is pure.
                uint32_t R[8]; c.regs_gp(R);
                uint32_t src=R[R_ECX], dst=R[R_EDX];
                uint32_t table=c.read_u32(R[R_ESP]+4);          // stack arg1: LUT
                // The body lives in d2_blit_bands (a free function): the
                // arguments are enough to replay it -- it only reads
                // read-only resources (tile bitmap, LUT).
                d2_blit_bands(c,GB,src,dst,table);
                return 0; };
            s.fn=blitBody;
            br.register_shim("native.hook","d2_cell_blit_114",s);
            uint32_t trap=br.shim_trap("native.hook","d2_cell_blit_114");
            uint32_t entry=GB+0xf69b0;
            // ALWAYS via translation-time redirection now. This was the
            // LAST hook that wrote a 5-byte `jmp` into Game.exe's .text
            // (the ~25 others already go through set_alternate) -- a
            // modification an online memory scan could see. The alternate
            // path is proven equivalent (D2_ALT_SELFTEST) and guest code
            // stays byte-identical to disk (D2_PRISTINE_CHECK: 0 bytes
            // changed).
            cpu->set_alternate(entry,trap);
            std::printf("native blit (1.14d): Game+0xf69b0 => trap 0x%08x (alternate, no .text patch)\n",trap);
        }
    }
}

void native_hooks_cellengine_install_rest(Cpu* cpu, Bridge& br){
    // ---- native ports of the hot spots --------------------------------------
    // Profiling identified these hot addresses, in roughly decreasing cost:
    //     0x4ddef0 + 0x475aa0     light-grid sampling
    //     memcpy CRT 0x6829c0     (mostly fed by 0x606d40)
    //     0x64cb30/463740/61a010  collision lookup (movement, AI)
    //     0x606d40                cell RLE blit
    //     0x4f6bb0                15-band blit, "blend" variant
    //
    // Rule followed here: port the CALLER, never the leaf. A trap only pays
    // off if it replaces a lot of guest code; porting memcpy directly would
    // be counterproductive (0x606d40 calls it with lengths from 1 to 127
    // bytes -- a trap per copy would cost more than the copy itself).
    // Porting 0x606d40 absorbs those memcpy calls natively, with no trap
    // per copy.
    //
    // Verified by disassembly for all four targets: each is reached only by
    // DIRECT `call rel32` sites (2, 1, 1 and 18 respectively), zero
    // tail-jmp, zero data reference -- so the alternate captures all of
    // them, and the entry frame is always that of a call.
    //
    // NATIVEHOT=0 disables all four; NATIVELIGHT / NATIVEBLEND / NATIVERLE /
    // NATIVECOLL = 0 disable one target each (for bisection on regression).
    //
    // RULE: a faithful fallback returns `c.reg(R_EAX)`, NEVER 0. The bridge
    // writes the returned value into EAX before the guest body resumes;
    // returning 0 overwrites it. On 0x4ddef0, compiled under LTCG, EAX is
    // the "output pointer" argument: a fallback returning 0 would fill 760
    // bytes at guest address 0. Every faithful fallback in this file
    // follows this rule.
    // BISECTION TRAP: the cell-loop port (NATIVECELLLOOP, :7588) is NESTED
    // inside this block, so NATIVEHOT=0 silently disables it too. A knob
    // that lies is worse than no knob, so this is logged explicitly below.
    // NATIVECELLLOOP is now always attempted by default, so the only guard
    // that still matters here is NATIVEHOT=0.
    if(getenv("NATIVEHOT") && !strcmp(getenv("NATIVEHOT"),"0"))
        d2vita_progress("ATTENTION: NATIVEHOT=0 desactive AUSSI NATIVECELLLOOP "
                        "(le portage de la boucle est imbrique dans ce bloc) — "
                        "la boucle N'EST PAS active dans ce run");
    if(g_114 && !br.module_base("D2gfx.dll") && !(getenv("NATIVEHOT") && !strcmp(getenv("NATIVEHOT"),"0"))){
        uint32_t GB=g_d2base;
        auto on=[](const char* k){ const char* v=getenv(k); return !(v && !strcmp(v,"0")); };

        // --- 1. 8x8 light grid: Game+0xddef0 (with 0x475aa0 inlined) --------
        // LTCG convention: the output pointer arrives in EAX, 3 stack
        // arguments (x, y, flag), ret 0xc. The flag!=0 branch calls two
        // guest functions (global colour): NOT ported, faithful fallback.
        // The flag==0 branch (the hot one): 8x8 calls to 0x475aa0, a pure
        // table lookup -- (coord>>3) - origin, clamped to [0,0x2f], index
        // (dy*48+dx)*8 into the grid at 0x7b0e68; byte +4 is intensity,
        // +5/+6/+7 is colour, written ONLY if 0x712b8c != 0. We reproduce
        // that "only if": without coloured light the port leaves the 3
        // colour bytes INTACT, exactly like the original.
        if(on("NATIVELIGHT")){
            uint32_t entry=GB+0xddef0;
            // D2_LIGHTVERIFY=1: byte-by-byte audit of the 8x8 buffer (same
            // scheme as D2_RLEVERIFY -- the port fills a COPY, the guest
            // fills the real buffer, compared at return).
            static uint32_t lv_exit=0, lv_ra=0, lv_lo=0, lv_eax=0;
            static bool lv_armed=false;
            static std::vector<uint8_t> lv_exp;
            static uint64_t lv_calls=0, lv_bad=0;
            { Shim lx; lx.argc=0; lx.stdcall_cleanup=false; lx.tag="native!d2_light_verify_exit";
              lx.fn=[&br](Cpu&c)->uint32_t{
                uint32_t geax=c.reg(R_EAX);
                if(lv_armed){ lv_armed=false; ++lv_calls;
                    std::vector<uint8_t> got(D2_LIGHTOUT);
                    c.read(lv_lo,got.data(),D2_LIGHTOUT);
                    int first=-1; uint32_t bad=0;
                    for(uint32_t i=0;i<D2_LIGHTOUT;i++) if(got[i]!=lv_exp[i]){ if(first<0) first=(int)i; ++bad; }
                    if(first>=0 && ++lv_bad<=12)
                        std::printf("[lightverify] appel #%llu : %u/%u octets differents ; 1er +0x%x "
                                    "(entree %d, octet %d) invite=%02x natif=%02x | eax invite=%08x natif=%08x\n",
                                    (unsigned long long)lv_calls,bad,(unsigned)D2_LIGHTOUT,first,
                                    first/12,first%12,got[first],lv_exp[first],geax,lv_eax);
                    if(!(lv_calls%50000)) std::printf("[lightverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)lv_calls,(unsigned long long)lv_bad);
                    br.redirect_next(lv_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);
                return geax; };
              br.register_shim("native.hook","d2_light_verify_exit",lx);
              lv_exit=br.shim_trap("native.hook","d2_light_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_light_grid_114";
            auto lightBody=[GB,entry,&br](Cpu&c)->uint32_t{
                uint32_t Rg[8]; c.regs_gp(Rg);             // ESP+EAX+EBP: one batched read (cpu.h)
                uint32_t E=Rg[R_ESP];                          // -> retaddr
                uint32_t flag=c.read_u32(E+12);
                if(!flag){
                    uint32_t out=Rg[R_EAX];                    // LTCG: output in EAX
                    const uint8_t* grid=(const uint8_t*)c.hostptr(GB+0x3b0e68,48*48*8);
                    static const bool verify = getenv("D2_LIGHTVERIFY")!=nullptr;
                    if(grid && verify && !lv_armed){
                        lv_exp.assign(D2_LIGHTOUT,0);
                        if(c.read(out,lv_exp.data(),D2_LIGHTOUT)){
                            lv_eax=d2_light_fill(c,GB,grid,lv_exp.data(),
                                                 c.read_u32(E+4),c.read_u32(E+8));
                            lv_lo=out; lv_armed=true;
                            lv_ra=c.read_u32(E); c.write_u32(E,lv_exit); }
                    } else if(grid){
                        if(uint8_t* o=(uint8_t*)c.hostptr(out,D2_LIGHTOUT)){
                            uint32_t last=d2_light_fill(c,GB,grid,o,
                                                        c.read_u32(E+4),c.read_u32(E+8));
                            ++g_hotLightN;
                            sh_ev(2);  // DOES NOT WRITE THE FRAME: fills a buffer of
                                       // light values (out = EAX, LTCG convention).
                                       // Doesn't factor into the ordering problem.
                            c.set_reg(R_ESP,E+12);         // + handler's pop = ret 0xc
                            return last;
                        }
                    }
                }
                ++g_hotLightFB;                                 // faithful fallback: original body
                c.write_u32(E-4,Rg[R_EBP]);
                c.set_reg(R_ESP,E-8);
                br.redirect_next(entry+1);
                // CRITICAL: the bridge writes this shim's return value into
                // EAX before the guest body resumes. Here EAX is an
                // ARGUMENT (LTCG convention: it's the output pointer).
                // Returning 0 would fill the 64 entries at guest address 0.
                return Rg[R_EAX]; };
            s.fn=lightBody;
            br.register_shim("native.hook","d2_light_grid_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_light_grid_114"));
            std::printf("native hot: grille de lumiere Game+0xddef0 (alternate)\n");
        }

        // --- 2. 15-band "blend" blit: Game+0xf6bb0 ----------------------------
        // Exact sibling of the already-ported blit (0xf69b0): same band
        // tables (0x72db48 dst / 0x72db84 len / 0x72dbc0 src) and same
        // pitch 0x7d544c; the difference is the pixel source: dst =
        // LUT[(bg<<8)|src] instead of LUT[src], with bg coming from the
        // buffer at 0x7ca320 indexed by ((X>>7)<<5 + (Y>>7))<<5 + dstOff,
        // X/Y advancing by dX/dY per band.
        // Skip rule verified against the binary's tables (index 0x4f6f3c,
        // jumps at 0x4f6f18): a band is served iff (len-4) <= 0x1c AND
        // len%4 == 0 -- identical to the already-ported blit's rule.
        // Two passes: all 15 bands are validated BEFORE any write, or a
        // mid-loop fallback would leave some bands half-written.
        if(on("NATIVEBLEND")){
            uint32_t entry=GB+0xf6bb0;
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_cell_blend_114";
            auto blendBody=[GB,entry,&br](Cpu&c)->uint32_t{
                uint32_t Rg[8]; c.regs_gp(Rg);             // batched read (cpu.h regs_gp)
                uint32_t E=Rg[R_ESP];
                uint32_t src=Rg[R_ECX], dst=Rg[R_EDX];
                int32_t X=(int32_t)c.read_u32(E+4),  dX=(int32_t)c.read_u32(E+8);
                int32_t Y=(int32_t)c.read_u32(E+12), dY=(int32_t)c.read_u32(E+16);
                const uint32_t* dofs=(const uint32_t*)c.hostptr(GB+0x32db48,60);
                const uint32_t* lens=(const uint32_t*)c.hostptr(GB+0x32db84,60);
                const uint32_t* sofs=(const uint32_t*)c.hostptr(GB+0x32dbc0,60);
                const uint8_t*  lut =(const uint8_t*) c.hostptr(GB+0x3d2348,0x10000);
                if(dofs&&lens&&sofs&&lut){
                    uint32_t pitch=c.read_u32(GB+0x3d544c);
                    struct B{ uint8_t* d; const uint8_t* s; const uint8_t* b; uint32_t n; };
                    B bands[15]; int nb=0; bool ok=true;
                    uint32_t d2=dst; int32_t x2=X, y2=Y;
                    for(int i=0;i<15;i++){
                        uint32_t len=lens[i];
                        if((uint32_t)(len-4u)<=0x1cu && !(len&3u)){
                            uint32_t bg=((((uint32_t)(x2>>7)<<5)+(uint32_t)(y2>>7))<<5)+dofs[i]+GB+0x3ca320;
                            uint8_t* dH=(uint8_t*)c.hostptr(dofs[i]+d2,len);
                            const uint8_t* sH=(const uint8_t*)c.hostptr(sofs[i]+src,len);
                            const uint8_t* bH=(const uint8_t*)c.hostptr(bg,len);
                            if(!dH||!sH||!bH){ ok=false; break; }
                            bands[nb].d=dH; bands[nb].s=sH; bands[nb].b=bH; bands[nb].n=len; ++nb;
                        }
                        d2+=pitch; x2=(int32_t)((uint32_t)x2+(uint32_t)dX); y2=(int32_t)((uint32_t)y2+(uint32_t)dY);
                    }
                    if(ok){
                        // DESCENDING order, same as the original (jump table
                        // unrolled from len-1 down to 0): free, and it
                        // removes any question of dst/bg overlap.
                        for(int i=0;i<nb;i++){ const B& b=bands[i];
                            for(uint32_t k=b.n;k-->0;) b.d[k]=lut[((uint32_t)b.b[k]<<8)+b.s[k]]; }
                        ++g_hotBlendN;
                        sh_ev(0);  // shape: INDEPENDENT frame write (rle/light/grid)
                        c.set_reg(R_ESP,E+16);             // ret 0x10
                        return (uint32_t)dY;                   // the original's output EAX
                    }
                }
                ++g_hotBlendFB;
                c.write_u32(E-4,Rg[R_EBP]);
                c.set_reg(R_ESP,E-8);
                br.redirect_next(entry+1);
                return Rg[R_EAX]; };               // faithful fallback: EAX unchanged
            s.fn=blendBody;
            br.register_shim("native.hook","d2_cell_blend_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_cell_blend_114"));
            std::printf("native hot: blit blend Game+0xf6bb0 (alternate)\n");
        }

        // --- 3. cell RLE blit: Game+0x206d40 ----------------------------------
        // __fastcall(ecx=src, edx=dst) + 2 stack args (lines, pitch), ret 8.
        // Control byte stream: bit 7 clear => copy N bytes (guest memcpy --
        // this is the big client of 0x6829c0); 0x80 => next line (and
        // decrement the line counter, ending at zero); 0x81..FF => skip
        // (ctl & 0x7f) pixels. EAX is reproduced exactly: it's the dst
        // before the copy after a memcpy (memcpy's return value), otherwise
        // the current value with the low byte overwritten.
        if(on("NATIVERLE")){
            uint32_t entry=GB+0x206d40;
            // D2_RLEVERIFY audit EXIT trap: the guest body's return address
            // is redirected here to compare, on return, the memory it
            // actually wrote against what the port would have written.
            static uint32_t v_exit=0, v_ra=0, v_lo=0, v_n=0, v_eax=0;
            static bool v_armed=false;
            static std::vector<uint8_t> v_exp;
            static uint64_t v_calls=0, v_badN=0;
            { Shim sx; sx.argc=0; sx.stdcall_cleanup=false; sx.tag="native!d2_rle_verify_exit";
              sx.fn=[&br](Cpu&c)->uint32_t{
                uint32_t geax=c.reg(R_EAX);
                if(v_armed){ v_armed=false; ++v_calls;
                    std::vector<uint8_t> got(v_n);
                    c.read(v_lo,got.data(),v_n);
                    uint32_t bad=0, first=0;
                    for(uint32_t i=0;i<v_n;i++) if(got[i]!=v_exp[i]){ if(!bad) first=i; ++bad; }
                    if(!(v_calls%20000)) std::printf(
                        "[rleverify] %llu appels compares, %llu divergences\n",
                        (unsigned long long)v_calls,(unsigned long long)v_badN);
                    if(bad || geax!=v_eax){
                        if(++v_badN<=12) std::printf(
                            "[rleverify] appel #%llu : %u/%u octets differents (1er +0x%x invite=%02x natif=%02x)"
                            " eax invite=%08x natif=%08x fenetre=[%08x,+%u)\n",
                            (unsigned long long)v_calls,bad,v_n,first,
                            bad?got[first]:0,bad?v_exp[first]:0,geax,v_eax,v_lo,v_n);
                    }
                    br.redirect_next(v_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);       // the bridge adds +4 back: ESP unchanged
                return geax; };                        // do NOT overwrite the guest's EAX
              br.register_shim("native.hook","d2_rle_verify_exit",sx);
              v_exit=br.shim_trap("native.hook","d2_rle_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_cell_rle_114";
            auto rleBody=[entry,&br](Cpu&c)->uint32_t{
                uint32_t Rg[8]; c.regs_gp(Rg);             // ESP+ECX+EDX+EBP+EAX: one batched read
                uint32_t E=Rg[R_ESP];
                uint32_t lines=c.read_u32(E+4), stride=c.read_u32(E+8);
                uint32_t src=Rg[R_ECX], dst=Rg[R_EDX];
                static const bool verify = getenv("D2_RLEVERIFY")!=nullptr;
                if(verify && !v_armed){
                    uint32_t lo=0xFFFFFFFFu, hi=0;
                    d2_rle_walk(c,RLE_MEASURE,src,dst,lines,stride,&lo,&hi,nullptr);
                    if(hi>lo && (hi-lo)<=(8u<<20)){
                        v_lo=lo; v_n=hi-lo; v_exp.assign(v_n,0);
                        if(c.read(v_lo,v_exp.data(),v_n)){
                            RleWin w{v_exp.data(),v_lo,v_n};
                            v_eax=d2_rle_walk(c,RLE_WINDOW,src,dst,lines,stride,nullptr,nullptr,&w);
                            v_armed=true; v_ra=c.read_u32(E); c.write_u32(E,v_exit); } }
                    // the GUEST does the real work; we compare at its return
                    c.write_u32(E-4,Rg[R_EBP]);
                    c.set_reg(R_ESP,E-8);
                    br.redirect_next(entry+1);
                    return Rg[R_EAX];              // faithful fallback: EAX unchanged
                }
                // The RLE is ALREADY split: d2_rle_walk computes EAX from
                // just the control-stream walk (eax = dst, memcpy's return
                // value) -- the mode only governs the writes. So the value
                // can be returned now and written later.
                const uint32_t eax=d2_rle_walk(c,RLE_APPLY,src,dst,lines,stride,nullptr,nullptr,nullptr);
                ++g_hotRleN;
                sh_ev(0);  // shape: INDEPENDENT frame write (rle/light/grid)
                c.set_reg(R_ESP,E+8);                      // ret 8
                return eax; };
            s.fn=rleBody;
            br.register_shim("native.hook","d2_cell_rle_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_cell_rle_114"));
            std::printf("native hot: blit RLE Game+0x206d40 (alternate)\n");
        }


        // --- 4. collision lookup: Game+0x24cb30 -------------------------------
        // stdcall(room, x, y, mask16), ret 0x10. Original chain: 0x463740
        // (find the room containing the point) then 0x61a010 (sub-object)
        // then a 16-bit grid read. We port the FAST PATH -- the point is in
        // the passed room, bounds [+0x4c,+0x4c+0x54) in x and
        // [+0x50,+0x50+0x58) in y, SIGNED comparisons like the original --
        // and fall back faithfully to the guest body whenever that's not
        // the case (0x463740 then scans the neighbouring-room list via
        // 0x619790, which we don't port). No writes: this shim is pure
        // read.
        // D2_COLLVERIFY=1: direct audit. The shim computes its value,
        // does NOT return it, lets the guest body answer, and compares the
        // two EAX at return. The output fits in a register here, so the
        // comparison is exact with no memory window, unlike D2_RLEVERIFY.
        if(on("NATIVECOLL")){
            uint32_t entry=GB+0x24cb30;
            static uint32_t cv_exit=0, cv_ra=0, cv_val=0;
            static bool cv_armed=false;
            static uint64_t cv_calls=0, cv_bad=0;
            { Shim cx; cx.argc=0; cx.stdcall_cleanup=false; cx.tag="native!d2_coll_verify_exit";
              cx.fn=[&br](Cpu&c)->uint32_t{
                uint32_t geax=c.reg(R_EAX);
                if(cv_armed){ cv_armed=false; ++cv_calls;
                    if(geax!=cv_val && ++cv_bad<=12)
                        std::printf("[collverify] appel #%llu : invite=0x%08x natif=0x%08x\n",
                                    (unsigned long long)cv_calls,geax,cv_val);
                    if(!(cv_calls%200000)) std::printf("[collverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)cv_calls,(unsigned long long)cv_bad);
                    br.redirect_next(cv_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);
                return geax; };
              br.register_shim("native.hook","d2_coll_verify_exit",cx);
              cv_exit=br.shim_trap("native.hook","d2_coll_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_coll_lookup_114";
            auto collBody=[entry,&br](Cpu&c)->uint32_t{
                uint32_t E=c.reg(R_ESP);
                uint32_t room=c.read_u32(E+4);
                int32_t  x=(int32_t)c.read_u32(E+8), y=(int32_t)c.read_u32(E+12);
                uint32_t mask=c.read_u32(E+16);
                // Slow path included: d2_room_at also scans the
                // neighbouring-room list (0x619790 being just an accessor).
                // No fallback left at all -- calls that used to go back to
                // the guest are now served natively too.
                uint32_t r=d2_coll_at(c,room,x,y,mask);
                static const bool verify = getenv("D2_COLLVERIFY")!=nullptr;
                if(verify && !cv_armed){                   // the guest answers, we'll compare
                    cv_val=r; cv_armed=true;
                    cv_ra=c.read_u32(E); c.write_u32(E,cv_exit);
                    c.write_u32(E-4,c.reg(R_EBP));
                    c.set_reg(R_ESP,E-8);
                    br.redirect_next(entry+1);
                    return c.reg(R_EAX);   // faithful fallback: EAX unchanged
                }
                ++g_hotCollN;
                c.set_reg(R_ESP,E+16);                 // ret 0x10
                return r; };
            s.fn=collBody;
            br.register_shim("native.hook","d2_coll_lookup_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_coll_lookup_114"));
            std::printf("native hot: lookup collision Game+0x24cb30 (alternate)\n");
        }
        // --- 6. light grid reconstruction: Game+0x756d0 -----------------------
        // No arguments, bare `ret`; output EAX is 0x900 (normal) or 0
        // (abort). A single call site (0x475888), direct. The loop only
        // ever writes a CONSTANT (flag = 1) and never clears it back to
        // zero, so an abort mid-loop would be harmless: the guest would
        // rewrite the same 1s. Not needed here since everything is ported
        // (including the 0x463740 slow path), but it's what made this
        // target tractable in the first place, unlike a blit.
        // D2_GRIDVERIFY=1: byte-by-byte audit of the 2304 entries + EAX.
        if(on("NATIVEGRID")){
            uint32_t entry=GB+0x756d0;
            static uint32_t gv_exit=0, gv_ra=0, gv_eax=0;
            static bool gv_armed=false;
            static std::vector<uint8_t> gv_exp;
            static uint64_t gv_calls=0, gv_bad=0;
            { Shim gx; gx.argc=0; gx.stdcall_cleanup=false; gx.tag="native!d2_grid_verify_exit";
              gx.fn=[GB,&br](Cpu&c)->uint32_t{
                uint32_t geax=c.reg(R_EAX);
                if(gv_armed){ gv_armed=false; ++gv_calls;
                    const uint32_t N=48u*48u*8u;
                    std::vector<uint8_t> got(N);
                    c.read(GB+0x3b0e68,got.data(),N);
                    int first=-1; uint32_t bad=0;
                    for(uint32_t i=0;i<N;i++) if(got[i]!=gv_exp[i]){ if(first<0) first=(int)i; ++bad; }
                    if((first>=0 || geax!=gv_eax) && ++gv_bad<=12)
                        std::printf("[gridverify] appel #%llu : %u/%u octets differents"
                                    " (1er +0x%x = case %d octet %d, invite=%02x natif=%02x)"
                                    " eax invite=%08x natif=%08x\n",
                                    (unsigned long long)gv_calls,bad,N,first<0?0:first,
                                    first<0?0:first/8,first<0?0:first%8,
                                    first<0?0:got[first],first<0?0:gv_exp[first],geax,gv_eax);
                    if(!(gv_calls%500)) std::printf("[gridverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)gv_calls,(unsigned long long)gv_bad);
                    br.redirect_next(gv_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);
                return geax; };
              br.register_shim("native.hook","d2_grid_verify_exit",gx);
              gv_exit=br.shim_trap("native.hook","d2_grid_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_lightgrid_114";
            auto gridBody=[GB,entry,&br](Cpu&c)->uint32_t{
                uint32_t E=c.reg(R_ESP);
                static const bool verify = getenv("D2_GRIDVERIFY")!=nullptr;
                if(verify && !gv_armed){
                    const uint32_t N=48u*48u*8u;
                    gv_exp.assign(N,0);
                    if(c.read(GB+0x3b0e68,gv_exp.data(),N)){   // the port fills a COPY
                        gv_eax=d2_lightgrid_build(c,GB,gv_exp.data());
                        gv_armed=true; gv_ra=c.read_u32(E); c.write_u32(E,gv_exit); }
                    c.write_u32(E-4,c.reg(R_EBP));
                    c.set_reg(R_ESP,E-8);
                    br.redirect_next(entry+1);
                    return c.reg(R_EAX);
                }
                ++g_hotGridN;
                sh_ev(2);  // rebuilds a light grid, not the frame
                return d2_lightgrid_build(c,GB);   // bare `ret`: the bridge just pops the address
                };
            s.fn=gridBody;
            br.register_shim("native.hook","d2_lightgrid_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_lightgrid_114"));
            std::printf("native hot: reconstruction grille de lumiere Game+0x756d0 (alternate)\n");
        }
        // ---- Cell-loop instrumentation, Game+0xf8b80 ------------------------
        // D2_CELLCENSUS=1: counts CALLS and DECLARED cells.
        // D2_CELLCENSUS=2: additionally buckets each cell into six
        // categories, evaluating EXACTLY the guest body's conditions,
        // WITHOUT WRITING A SINGLE BYTE. This is literally pass 1 of the
        // eventual port, exercised before a single write line exists.
        //
        // ABI per disassembly (Game.exe 1.14d):
        //   __fastcall(ecx = tile block, edx = X); [esp+4] = Y;
        //   [esp+8] = light table; ret 8. EAX is NOT an argument
        //   (overwritten by `xor eax,eax` at 0x4f8b97).
        //   block+0x50 = cell count (int32), block+0x54 = array of 0x14-byte
        //   structs. Cell: x as int16 at +0, y as int16 at +2, light indices
        //   at +6 and +7, format at +8 (bit0 = should draw, bit2 = scale
        //   variant), data pointer at +0x10.
        //
        // This shim adds ONE trap per call: it perturbs what it measures.
        // But it COUNTS instead of comparing, so its validity doesn't depend
        // on the measurement noise floor -- and the overhead it costs IS
        // exactly the term the port would remove, so it's also a free
        // calibration of the expected gain.
        g_shOn = getenv("D2_LOOPSHAPE")!=nullptr;   // armed AFTER env.txt is read
        g_cvOn = getenv("D2_CAMVEC")!=nullptr;      // camera vector from floor blocks
        g_scaleForceClip = getenv("D2_SCALEFORCECLIP")!=nullptr;
        // D2_CELLOPT: bitmask of cell-loop optimizations. ABSENT = 0 = the
        // original code, byte for byte.
        // D2_CELLSTAT: instrumentation counters (doubled pass1/pass2).
        // Default is 63 (all optimizations enabled).
        g_cellOpt  = getenv("D2_CELLOPT")? (int)strtol(getenv("D2_CELLOPT"),nullptr,0) : 63;
        g_cellStat = getenv("D2_CELLSTAT")!=nullptr;
        // D2_CELLOVL: overlap instrumentation (prerequisite for D2_CELLPAR).
        g_ovlOn    = getenv("D2_CELLOVL")!=nullptr;
        // D2_CELLPAR=<n>: n worker threads; 0 (default) = current code.
        g_parN     = getenv("D2_CELLPAR")? atoi(getenv("D2_CELLPAR")) : 0;
        if(g_parN<0) g_parN=0;
        if(g_parN>D2_PAR_MAXW) g_parN=D2_PAR_MAXW;
        g_parCellMode = getenv("D2_CELLPARCELL") && *getenv("D2_CELLPARCELL")=='1';
        if(getenv("D2_CELLPARMIN"))   g_parMinCell=(uint32_t)atoi(getenv("D2_CELLPARMIN"));
        if(getenv("D2_CELLPARSPIN"))  g_parSpin   =(uint32_t)atoi(getenv("D2_CELLPARSPIN"));
        if(getenv("D2_CELLPARIDLE"))  g_parIdleUs =(uint32_t)atoi(getenv("D2_CELLPARIDLE"));
        if(getenv("D2_CELLPARYIELD")) g_parYields =(uint32_t)atoi(getenv("D2_CELLPARYIELD"));
        if(getenv("D2_CELLPARSTEAL")) g_parStealAt=(uint32_t)strtoul(getenv("D2_CELLPARSTEAL"),nullptr,0);
        if(getenv("D2_CELLPARCPU"))   g_parCpu    =getenv("D2_CELLPARCPU");
        // D2_CELLPARSHARE: workers' share, in percent. 0 (default) = equal
        // shares among the L lanes = the same split as before, byte for
        // byte. Outside [1,99] the value is REFUSED and logged as such: a
        // share of 0 or 100 would leave a lane empty, which is legal but
        // silent -- and a misspelled knob would then silently fall back to
        // "share=0 = default" with nothing distinguishing it from an
        // unarmed leg.
        if(getenv("D2_CELLPARSHARE")){
            const int sh=atoi(getenv("D2_CELLPARSHARE"));
            if(sh>=1 && sh<=99) g_parShare=(uint32_t)sh;
            else std::printf("cellpar: D2_CELLPARSHARE=%d hors de [1,99] — IGNORE (parts egales)\n",sh); }
        // DELIBERATE MUTUAL EXCLUSION. The instrumentation counters
        // (D2_CELLSTAT) and the stamp map (D2_CELLOVL) are UNLOCKED
        // GLOBALS: letting them run across multiple threads would produce
        // wrong numbers, which is worse than no measurement. Both
        // instruments measure a property of the DRAWING, not of a
        // particular parallel execution, so they lose nothing by staying
        // sequential.
        if(g_parN>0 && (g_cellStat||g_ovlOn)){
            g_parN=0;
            d2vita_progress("cellpar: DESARME — D2_CELLSTAT/D2_CELLOVL exigent le sequentiel");
            std::printf("cellpar: DESARME (D2_CELLSTAT/D2_CELLOVL exigent le sequentiel)\n"); }
        if(g_scaleForceClip) std::printf("test croise: echelle interieure routee par la feuille CLIPPEE\n");
        g_dibGuard = getenv("D2_DIBGUARD")? atoi(getenv("D2_DIBGUARD")) : 0;
        if(g_dibGuard) std::printf("garde-fou image: D2_DIBGUARD arme (repli si l'ecriture sort du DIB)\n");
        if(g_shOn) std::printf("forme de trame: D2_LOOPSHAPE arme\n");
        if(getenv("D2_CELLCENSUS")){
            uint32_t GB=g_d2base;
            static uint32_t s_cellEntry=0; s_cellEntry=GB+0xf8b80;   // CONSTANT init: no thread-safety guard needed
            const int lvl=getenv("D2_CELLCENSUS")?atoi(getenv("D2_CELLCENSUS")):1;
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_cell_loop_census";
            s.fn=[GB,lvl,&br](Cpu&c)->uint32_t{
                uint32_t E=c.reg(R_ESP);
                uint32_t blk=c.reg(R_ECX);
                int32_t  Xa=(int32_t)c.reg(R_EDX);
                int32_t  Ya=(int32_t)c.read_u32(E+4);
                uint32_t lt=c.read_u32(E+8);
                int32_t  n =(int32_t)c.read_u32(blk+0x50);
                ++g_cellCalls; sh_loop(); note_caller(c.read_u32(E)); cv_note(blk,Xa,Ya);
                bool pure=true;
                if(n>0){
                    g_cellDecl+=(uint64_t)n;
                    if(lvl>=2){
                        uint32_t cells=c.read_u32(blk+0x54);
                        auto I  =[&c](uint32_t va){ return (int32_t)c.read_u32(va); };
                        auto U8 =[&c](uint32_t va){ uint8_t v=0; c.read(va,&v,1); return (int)v; };
                        auto I16=[&c](uint32_t va){ uint16_t v=0; c.read(va,&v,2); return (int32_t)(int16_t)v; };
                        // Bounds re-read on EVERY call, like the original:
                        // these are render-time variables, never constants.
                        const int32_t oY0=I(GB+0x3d2330), oY1=I(GB+0x3d2338);
                        const int32_t oX0=I(GB+0x3c9788), oX1=I(GB+0x3c9798);
                        const int32_t iY0=I(GB+0x3c9780), iY1=I(GB+0x3c979c);
                        const int32_t iX0=I(GB+0x3c97a4), iX1=I(GB+0x3d2320);
                        const bool flatMode=(I(GB+0x3d2340)!=0);   // 0x4f8c1c
                        const bool gradOff=(I(GB+0x32da50)!=0);    // 0x4f8c9c
                        for(int32_t i=0;i<n;i++){
                            uint32_t cell=cells+(uint32_t)i*0x14u;
                            int fmt=U8(cell+8);
                            if(!(fmt&1)){ ++g_cellB[0]; continue; }          // 0x4f8bcf
                            int32_t aX=I16(cell+0)+Xa, aY=I16(cell+2)+Ya;
                            if(aY<oY0||aY>=oY1||aX<oX0||aX>=oX1){ ++g_cellB[1]; continue; }
                            bool flat=true;
                            if(!flatMode && !gradOff){
                                // Both colour branches produce the same
                                // byte: 9*12 = 108 = 0x6c (0x4f8c38 vs
                                // 0x4f8c73). Only one formula needs evaluating.
                                uint32_t row=lt+(uint32_t)(U8(cell+6)+U8(cell+7)*8)*12u;
                                int A=U8(row+0x6c),B=U8(row+0x78),Cc=U8(row+0xcc),D=U8(row+0xd8);
                                int d=(B>A?B-A:A-B)+(Cc>A?Cc-A:A-Cc)+(D>B?D-B:B-D);
                                flat=(d<10);                                  // 0x4f8cad
                            }
                            if(!flat){ ++g_cellB[5]; pure=false; continue; }  // 0x4f8850 / 0x4f89f0
                            if(fmt&4){ ++g_cellB[4]; pure=false; continue; }  // 0x4f6f60 / 0x4f7580
                            bool inside=(aY>=iY0&&aY<iY1&&aX>=iX0&&aX<iX1);   // 0x4f8d2f
                            ++g_cellB[inside?2:3];
                        }
                    }
                }
                // A call with no non-servable cell is trivially pure.
                if(pure) ++g_cellPure;
                // FAITHFUL fallback (same pattern as fog_raise_snap): replay
                // `push ebp` then resume at entry+1 (= `mov ebp,esp`). E-8,
                // not E-4: the bridge adds 4 to ESP after the shim runs.
                c.write_u32(E-4,c.reg(R_EBP));
                c.set_reg(R_ESP,E-8);
                br.redirect_next(s_cellEntry+1);
                return c.reg(R_EAX); };          // faithful fallback: EAX UNCHANGED
            br.register_shim("native.hook","d2_cell_loop_census",s);
            cpu->set_alternate(s_cellEntry,br.shim_trap("native.hook","d2_cell_loop_census"));
            std::printf("recensement: boucle de cellules Game+0xf8b80 niveau %d (alternate, comptage seul)\n",lvl);
        }
        // ---- Cell-loop port, Game+0xf8b80 ------------------------------------
        // Enabled by default; NATIVECELLLOOP=0 reverts to the original code
        // (the FBHASH oracle and reference path stay the reference, so a
        // binary with the knob off is bit-for-bit the old one).
        //
        // WHY THIS FUNCTION. The chain 0x4f8b80 -> 0x4f8810 -> 0x4f69b0 has
        // only ONE call site at each stage (1.14d disassembly), so this
        // single loop produces THE ENTIRETY of cell-blit calls. Porting it
        // folds N cells into ONE traversal instead of N -- the only lever
        // here that attacks the trap machinery itself rather than the work
        // it carries.
        //
        // ALL-OR-NOTHING PER CALL. Pass 1 classifies the N cells and
        // resolves every host pointer WITHOUT WRITING A SINGLE BYTE; if even
        // one cell isn't servable, nothing is touched and the ENTIRE call
        // falls back. A mid-loop fallback would leave already-drawn cells
        // that the guest body would redraw -- double drawing, a silent
        // pixel divergence (same constraint as the blend port above).
        // The variable can still be set to 2 to select a bisection mode
        // (see below).
        {
            uint32_t GB=g_d2base;
            static uint32_t s_loopEntry=0; s_loopEntry=GB+0xf8b80;
            // NATIVECELLLOOP=2: serves ONLY unclipped cells and falls back
            // as soon as a clipped cell appears. Used to BISECT an oracle
            // divergence: if the hash matches again at 2 but diverges at 1,
            // the bug is in the clipped loop (0x4f88b0).
            const char* nclEnv=getenv("NATIVECELLLOOP");
            const int loopLvl=nclEnv?atoi(nclEnv):1;
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_cell_loop_114";
            auto loopBody=[GB,loopLvl,&br](Cpu&c)->uint32_t{
                // BATCHED READ (cpu.h regs_gp): ESP+ECX+EDX, plus EBP/EAX
                // needed by the fallback path -- ONE per-thread state
                // resolution instead of three (five on fallback). No
                // set_reg happens between entry and these uses, so the
                // values are identical to what would have been re-read.
                uint32_t Rg[8]; c.regs_gp(Rg);
                const uint32_t E=Rg[R_ESP];
                // Fork-join "call in progress" FLAG. It serves ONE purpose:
                // letting a worker split its wait between this call's SERIAL
                // PROLOGUE (pass 1, band table, pre-resolution -- everything
                // no share adjustment can give it) and the silence between
                // calls. Set by a block-scoped object: the body has a dozen
                // early exits (FB), a hand-written `clear` would miss one,
                // and a flag stuck at 1 would produce a lying counter.
                struct ParMark { bool on;
                    explicit ParMark(bool o):on(o){ if(on) __atomic_store_n(&g_parInCall,1u,__ATOMIC_RELAXED); }
                    ~ParMark(){ if(on) __atomic_store_n(&g_parInCall,0u,__ATOMIC_RELAXED); } };
                ParMark parMark(g_parLive>0);
                // FAITHFUL fallback: replay `push ebp` and resume at
                // entry+1. E-8, not E-4: the bridge adds 4 to ESP after the
                // shim runs. EAX UNCHANGED.
                auto FB=[&c,&br,E,&Rg](int motif)->uint32_t{
                    ++g_loopFB[motif];
                    c.write_u32(E-4,Rg[R_EBP]);
                    c.set_reg(R_ESP,E-8);
                    br.redirect_next(s_loopEntry+1);
                    return Rg[R_EAX]; };
                // SERVED exit: the function does `ret 8`, the bridge adds 4.
                auto OK=[&c,E](uint32_t eax)->uint32_t{
                    c.set_reg(R_ESP,E+8); return eax; };

                const uint32_t blk=Rg[R_ECX];
                const int32_t  Xa =(int32_t)Rg[R_EDX];
                const int32_t  Ya =(int32_t)c.read_u32(E+4);
                const uint32_t lt =c.read_u32(E+8);
                const int32_t  n  =(int32_t)c.read_u32(blk+0x50);
                sh_loop();                                  // burst: served OR fell back
                cv_note(blk,Xa,Ya);                         // D2_CAMVEC (no-op without the knob)
                note_caller(c.read_u32(E));                 // who's calling us?
                if(n<=0){ ++g_loopServed; return OK(0); }   // 0x4f8b9b : eax=0

                // Band tables: CONSTANT addresses (.rdata), resolved once;
                // the VALUES are still re-read every call.
                static const uint32_t *dofs=nullptr,*lens=nullptr,*sofs=nullptr;
                static bool tinit=false;
                if(!tinit){
                    dofs=(const uint32_t*)c.hostptr(GB+0x32db48,60);
                    lens=(const uint32_t*)c.hostptr(GB+0x32db84,60);
                    sofs=(const uint32_t*)c.hostptr(GB+0x32dbc0,60);
                    tinit=dofs&&lens&&sofs; }
                if(!tinit) return FB(3);

                const uint32_t pitch=c.read_u32(GB+0x3d544c);
                const uint32_t surf =c.read_u32(GB+0x3d5448);   // surface base
                const uint32_t base =surf+pitch*(uint32_t)Ya+(uint32_t)Xa;
                const int32_t oY0=(int32_t)c.read_u32(GB+0x3d2330), oY1=(int32_t)c.read_u32(GB+0x3d2338);
                const int32_t oX0=(int32_t)c.read_u32(GB+0x3c9788), oX1=(int32_t)c.read_u32(GB+0x3c9798);
                const int32_t iY0=(int32_t)c.read_u32(GB+0x3c9780), iY1=(int32_t)c.read_u32(GB+0x3c979c);
                const int32_t iX0=(int32_t)c.read_u32(GB+0x3c97a4), iX1=(int32_t)c.read_u32(GB+0x3d2320);
                const int32_t cY0=(int32_t)c.read_u32(GB+0x3d2334), cY1=(int32_t)c.read_u32(GB+0x3c97a0);
                const int32_t cX0=(int32_t)c.read_u32(GB+0x3ca318), cX1=(int32_t)c.read_u32(GB+0x3d233c);
                const bool flatMode=(c.read_u32(GB+0x3d2340)!=0);
                const bool gradOff =(c.read_u32(GB+0x32da50)!=0);

                const uint32_t cells=c.read_u32(blk+0x54);
                const uint8_t* cp=d2_hp(c,cells,(uint32_t)n*0x14u);
                if(!cp) return FB(1);

                // ---- BAND TABLE, computed ONCE PER CALL (CO_BANDS) -------------
                // The fifteen bands of a FLAT UNCLIPPED cell only depend on
                // dofs/lens/sofs and the pitch -- three .rdata tables and one
                // value re-read per call. They are therefore IDENTICAL for
                // the ~19 cells in a call: only j.dst and j.src change.
                // The original code redid all fifteen passes TWICE PER CELL
                // (bounds in pass 1, drawing in pass 2): ~570 passes per call
                // where 15 suffice.
                //   BD[k] = dofs[b] + b*pitch   (the `dcur+=pitch` unrolled)
                //   BS[k] = sofs[b] ; BL[k] = lens[b]
                // bDlo/bDhi/bSlo/bShi: AGGREGATE bounds of the valid bands. An
                // unclipped cell can then bound its extent with TWO additions
                // instead of fifteen loop passes.
                uint32_t BD[15], BS[15], BL[15]; int nb=0; uint32_t bPix=0;
                uint32_t bDlo=0xFFFFFFFFu,bDhi=0,bSlo=0xFFFFFFFFu,bShi=0;
                { uint32_t off=0;
                  for(int b=0;b<15;b++,off+=pitch){
                      const uint32_t L=lens[b];
                      if(L-4>0x1c || (L&3)) continue;
                      const uint32_t D=dofs[b]+off, S=sofs[b];
                      BD[nb]=D; BS[nb]=S; BL[nb]=L; ++nb; bPix+=L;
                      if(D<bDlo) bDlo=D;  if(D+L>bDhi) bDhi=D+L;
                      if(S<bSlo) bSlo=S;  if(S+L>bShi) bShi=S+L; } }
                // Host-view caches (CO_LUTC): g_cellLutC / g_cellRowC, moved
                // outside the body once the drawing body became a free
                // function (for CELLPAR). Since hostptr() is pure, they're
                // never cleared: H(va) never changes for the life of the
                // process.
                D2HostCache& rowC=g_cellRowC;

                // Under the GIL, only one shim runs at a time
                // (cpu_box86.cpp:808; the inline path is FATAL under
                // native, rt_boot.cpp:2863), so a static table is safe and
                // avoids 32 KiB of stack.
                // We store the COLOUR (one byte), never a pointer into the
                // light table: that table is rewritten by the light shim,
                // so a pointer read back later would give a different
                // colour. Invisible as long as pass 2 immediately follows
                // pass 1; fatal the moment drawing is DEFERRED.
                using CellJob = D2CellJob;
                static CellJob job[1024];
                // Tables PARALLEL to job[], filled only in D2_CELLPAR mode:
                // kept outside CellJob so sizeof(CellJob) stays 36 bytes --
                // that size is the denominator for the "bytes copied by the
                // std::vector" instrumentation.
                static const uint8_t* jobLut[1024];
                static uint8_t        jobIdent[1024];
                if(n>1024) return FB(2);

                int nj=0; uint32_t dlo=0xFFFFFFFFu, dhi=0;
                // ---------------- PASS 1: classify, bound, VALIDATE ---------
                for(int32_t i=0;i<n;i++){
                    const uint8_t* cl=cp+(size_t)i*0x14;
                    const uint8_t fmt=cl[8];
                    if(!(fmt&1)) continue;                                  // 0x4f8bcf
                    const int32_t cx=(int32_t)(int16_t)(uint16_t)(cl[0]|(cl[1]<<8));
                    const int32_t cy=(int32_t)(int16_t)(uint16_t)(cl[2]|(cl[3]<<8));
                    const int32_t aX=cx+Xa, aY=cy+Ya;
                    if(aY<oY0||aY>=oY1||aX<oX0||aX>=oX1) continue;          // 0x4f8be6..0x4f8c16
                    const uint32_t idx=(uint32_t)(cl[6]+cl[7]*8)*12u;
                    const bool needGrad=(!flatMode && !gradOff);
                    const uint32_t rn=needGrad?0xd9u:0x6du;
                    const uint8_t* row=(g_cellOpt&CO_LUTC)
                        ? rowC.get(c,lt+idx,rn,&g_csRowHit,&g_csRowMiss)
                        : d2_hp(c,lt+idx,rn);
                    if(!row) return FB(1);
                    if(needGrad){
                        // Both colour branches produce the same byte: 9*12 =
                        // 108 = 0x6c (0x4f8c38 vs 0x4f8c73).
                        const int A=row[0x6c],B=row[0x78],Cc=row[0xcc],D=row[0xd8];
                        const int d=(B>A?B-A:A-B)+(Cc>A?Cc-A:A-Cc)+(D>B?D-B:B-D);
                        if(d>=10) return FB(0);                             // 0x4f8cad: gradient
                    }
                    CellJob& j=job[nj];
                    j.dst=base+(uint32_t)cy*pitch+(uint32_t)cx;              // 0x4f8bcd
                    j.src=(uint32_t)(cl[0x10]|(cl[0x11]<<8)|(cl[0x12]<<16)|((uint32_t)cl[0x13]<<24));
                    j.colour=row[0x6c];   // value READ NOW, not a pointer
                    j.aX=aX; j.aY=aY;
                    j.clip=(aY>=iY0&&aY<iY1&&aX>=iX0&&aX<iX1)?0:1;           // 0x4f8d2f
                    if(j.clip && loopLvl==2) return FB(0);                   // bisection
                    // Level 3: serves ONLY flat cells, rejecting both SCALE
                    // leaves, whose clip arithmetic isn't covered by any
                    // oracle.
                    if((cl[8]&4) && loopLvl==3) return FB(0);
                    // bit2 = SCALE variant. The unclipped leaf (0x4f6f60) is
                    // ported; the clipped one (0x4f7580) is not yet.
                    j.kind=(fmt&4)?(j.clip?2:1):0;
                    // D2_SCALEFORCECLIP=1: route INTERIOR SCALE cells through
                    // the CLIPPED path. Their bounds don't actually clip
                    // them, so both implementations MUST produce the same
                    // image -- the only way to get oracle coverage on the
                    // clipped leaf when the test scene never exercises it
                    // (no scale cells sit at the scene's edge).
                    if(j.kind==1 && g_scaleForceClip) j.kind=2;
                    // Exact extents, using the SAME band rule as pass 2.
                    uint32_t sLo=0xFFFFFFFFu,sHi=0; bool any=false;
                    if(j.kind==2){
                        uint32_t wlo=0xFFFFFFFFu,whi=0; bool okw=true;
                        d2_scale_clip_walk(&c,j.src,j.dst,pitch,15,aX,aY,cY0,cY1,cX0,cX1,
                                           nullptr,nullptr,0,0,&wlo,&whi,&sLo,&sHi,&okw);
                        if(!okw) return FB(5);
                        if(whi>wlo){ if(wlo<dlo) dlo=wlo; if(whi>dhi) dhi=whi; any=true; }
                    } else if(j.kind==1){
                        // SCALE extent: it's DATA-DRIVEN (a sequence of
                        // skips), so it's measured by walking the stream --
                        // exactly what RLE_MEASURE does for RLE.
                        uint32_t wlo=0xFFFFFFFFu,whi=0; bool okw=true;
                        d2_scale_walk(&c,j.src,j.dst,pitch,15,nullptr,nullptr,0,0,
                                      &wlo,&whi,&sLo,&sHi,&okw);
                        if(!okw) return FB(5);
                        if(whi>wlo){ if(wlo<dlo) dlo=wlo; if(whi>dhi) dhi=whi; any=true; }
                    } else if(!j.clip){
                      // CO_BANDS shortcut: only valid if NO band can wrap at
                      // 32 bits. The condition j.dst <= 2^32-1-bDhi
                      // guarantees this for all fifteen at once; otherwise
                      // the per-band loop is kept, carrying the original
                      // wraparound check and its FB(1).
                      if((g_cellOpt&CO_BANDS) && nb>0
                         && j.dst<=0xFFFFFFFFu-bDhi && j.src<=0xFFFFFFFFu-bShi){
                        const uint32_t d0=j.dst+bDlo, d1=j.dst+bDhi;
                        if(d0<dlo) dlo=d0;  if(d1>dhi) dhi=d1;
                        sLo=j.src+bSlo; sHi=j.src+bShi; any=true;
                        if(g_cellStat){ ++g_csBandFast; ++g_csP1Band; g_csPixP1[0]+=bPix; }
                      } else {
                        if(g_cellStat) ++g_csBandSlow;
                        uint32_t dcur=j.dst;
                        for(int b=0;b<15;b++,dcur+=pitch){
                            const uint32_t L=lens[b];
                            if(L-4>0x1c || (L&3)) continue;                  // 0x4f69b0's guard
                            const uint32_t d0=dofs[b]+dcur, s0=sofs[b]+j.src;
                            if(d0+L<d0 || s0+L<s0) return FB(1);             // 32-bit wraparound
                            if(g_cellStat){ ++g_csP1Band; g_csPixP1[0]+=L; }
                            if(d0<dlo) dlo=d0;  if(d0+L>dhi) dhi=d0+L;
                            if(s0<sLo) sLo=s0;  if(s0+L>sHi) sHi=s0+L;
                            any=true; } }
                    } else {
                        int32_t b0=cY0-aY; if(b0<0) b0=0;                    // 0x4f88fa
                        int32_t b1=cY1-aY; if(b1>15) b1=15;                  // 0x4f890e
                        uint32_t cur=j.dst+pitch*(uint32_t)b0;               // 0x4f8921
                        for(int32_t b=b0;b<b1;b++,cur+=pitch){
                            const int32_t dOff=(int32_t)dofs[b];
                            const int32_t pos=dOff+aX;
                            int32_t l=cX0-pos; if(l<0) l=0;                  // 0x4f8966
                            int32_t r=cX1-pos; if(r<0) r=0;                  // 0x4f8986
                            if(r>(int32_t)lens[b]) r=(int32_t)lens[b];       // 0x4f8990
                            const int32_t nn=r-l; if(nn<=0) continue;        // 0x4f899b
                            const uint32_t d0=cur+(uint32_t)(dOff+l), s0=j.src+sofs[b]+(uint32_t)l;
                            if(d0+(uint32_t)nn<d0 || s0+(uint32_t)nn<s0) return FB(1);
                            if(g_cellStat){ ++g_csP1Band; g_csPixP1[1]+=(uint32_t)nn; }
                            if(d0<dlo) dlo=d0;  if(d0+(uint32_t)nn>dhi) dhi=d0+(uint32_t)nn;
                            if(s0<sLo) sLo=s0;  if(s0+(uint32_t)nn>sHi) sHi=s0+(uint32_t)nn;
                            any=true; }
                    }
                    if(!any) continue;      // cell entirely clipped: nothing to write
                    j.sLo=sLo; j.sHi=sHi;
                    j.sp=d2_hp(c,sLo,sHi-sLo);
                    if(!j.sp) return FB(1);
                    if(g_cellStat) ++g_csCell3[j.kind?2u:(j.clip?1u:0u)];
                    ++nj;
                }
                if(!nj){ ++g_loopServed; return OK((uint32_t)n); }
                if(dib_outside(dlo,dhi)){ ++g_oobLoop; if(g_dibGuard) return FB(6); }
                uint8_t* dbase=(uint8_t*)d2_hp(c,dlo,dhi-dlo);
                if(!dbase) return FB(1);

                // ---------------- PASS 2: draw ------------------------
                // From here on, NO fallback is possible: everything is validated.
                const uint32_t dspan=dhi-dlo;
                // The drawing CONTEXT. The cell list is passed as an
                // ARGUMENT (pointer + index range): this is what makes
                // CO_NOVEC *and* the fork-join possible without duplicating
                // a single line of the drawing body (d2_cell_paint, one
                // definition).
                D2CellCtx X{};
                X.c=&c; X.GB=GB; X.dbase=dbase; X.dlo=dlo; X.dspan=dspan; X.pitch=pitch;
                X.cY0=cY0; X.cY1=cY1; X.cX0=cX0; X.cX1=cX1;
                X.dofs=dofs; X.lens=lens; X.sofs=sofs;
                X.BD=BD; X.BS=BS; X.BL=BL; X.nb=nb;
                X.lutv=nullptr; X.identv=nullptr;
                X.wlo=0; X.whi=0xFFFFFFFFu; X.cells=0;
                const bool ovlOpen = ovl_begin(dbase,dspan);
                (void)ovlOpen;

                // ---- Fork-join INSIDE the call (D2_CELLPAR) -------
                // The game sees no difference: control returns only once
                // every cell is drawn.
                const bool par = (g_parLive>0) && ((uint32_t)nj>=g_parMinCell);
                if(par){
                    const int L=g_parLive+1;           // lanes = workers + caller
                    // (a) PRE-RESOLUTION, on the calling thread, of
                    //     everything that comes from guest memory: colour
                    //     LUT and identity flag for each cell. A worker must
                    //     never call hostptr -- so never the GIL. Same
                    //     semantics as inline resolution: same cache, same
                    //     idTab per call, same counters.
                    signed char idTab[32]; std::memset(idTab,-1,sizeof idTab);
                    for(int k=0;k<nj;k++){
                        const uint32_t lva=GB+0x3d2348+(((uint32_t)job[k].colour>>3)<<8);
                        const uint8_t* lu=(g_cellOpt&CO_LUTC)
                            ? g_cellLutC.get(c,lva,256,&g_csLutHit,&g_csLutMiss)
                            : d2_hp(c,lva,256);
                        jobLut[k]=lu; jobIdent[k]=0;
                        if(lu && (g_cellOpt&CO_IDENT)){
                            const unsigned lv=((uint32_t)job[k].colour>>3)&31u;
                            if(idTab[lv]<0){
                                idTab[lv]=(std::memcmp(lu,d2_ident256(),256)==0)?1:0;
                                if(g_cellStat) ++g_csIdentTest; }
                            jobIdent[k]=(uint8_t)(idTab[lv]!=0); } }
                    X.lutv=jobLut; X.identv=jobIdent; X.c=&c;
                    // (b) SPLIT.
                    uint32_t cut[D2_PAR_MAXW+2];
                    // CUMULATIVE LANE WEIGHTS, in units of 100*g_parLive.
                    // g_parShare==0 (default): EQUAL shares, cum[i]=i*100 --
                    // cut[i] = span*i*100/(100*L) = span*i/L, the same
                    // computation as before, byte for byte, integer division
                    // included. Otherwise lane 0 (the calling thread) weighs
                    // (100-share)*nw and each worker weighs share: the sum
                    // is still 100*nw.
                    int64_t cum[D2_PAR_MAXW+2]; const int nw=g_parLive;
                    int64_t tot;
                    if(g_parShare==0){ tot=100ll*L; for(int i=0;i<=L;i++) cum[i]=100ll*i; }
                    else { tot=100ll*nw; cum[0]=0; cum[1]=(int64_t)(100u-g_parShare)*nw;
                           for(int i=2;i<=L;i++) cum[i]=cum[i-1]+(int64_t)g_parShare;
                           cum[L]=tot; }
                    if(g_parCellMode){
                        // Per CELL: perfectly balanced by count, but only
                        // valid if no two cells in the same call ever write
                        // the same byte (measured by D2_CELLOVL).
                        for(int i=0;i<=L;i++) cut[i]=(uint32_t)((int64_t)nj*cum[i]/tot);
                    } else {
                        // By DIB LINE BANDS: disjoint BY CONSTRUCTION. Bounds
                        // are rounded up to the next LINE START (surf +
                        // r*pitch) -- a cell band always fits within ONE
                        // line, so no write straddles two lanes (checked by
                        // g_parStraddle).
                        cut[0]=0; cut[L]=dspan;
                        for(int i=1;i<L;i++){
                            const uint64_t t=(uint64_t)((int64_t)dspan*cum[i]/tot);
                            const uint64_t a=(uint64_t)dlo+t;
                            uint64_t b=(a>surf)? ((a-surf+pitch-1)/pitch)*(uint64_t)pitch+surf : surf;
                            int64_t o=(int64_t)b-(int64_t)dlo;
                            if(o<0) o=0; if(o>(int64_t)dspan) o=(int64_t)dspan;
                            cut[i]=(uint32_t)o; }
                        for(int i=1;i<L;i++) if(cut[i]<cut[i-1]) cut[i]=cut[i-1];
                        if(pitch&31u) ++g_parPitchOdd;    // false sharing possible
                        for(int i=0;i<L;i++) if(cut[i+1]==cut[i]) { ++g_parDegen; break; }
                    }
                    // (c) PUBLISH. Lanes 1..L-1 go to workers; lane 0 is
                    //     done by the calling thread itself.
                    const uint32_t gen=g_parGen+1;
                    for(int i=0;i<g_parLive;i++){
                        D2ParLane& Ln=g_parLane[i];
                        Ln.ctx=X; Ln.ctx.c=nullptr;      // a worker has NO guest CPU
                        Ln.ctx.cells=0;
                        Ln.JV=job; Ln.par=g_parCellMode?0:1;
                        if(g_parCellMode){ Ln.k0=cut[i+1]; Ln.k1=cut[i+2]; }
                        else { Ln.k0=0; Ln.k1=(uint32_t)nj;
                               Ln.ctx.wlo=cut[i+1]; Ln.ctx.whi=cut[i+2]; }
                        Ln.claim=0; Ln.cells=0; }
                    __sync_synchronize();
                    __atomic_store_n(&g_parGen,gen,__ATOMIC_RELEASE);
                    // (d) THE CALLING THREAD'S SHARE.
                    if(g_parCellMode){ d2_cell_paint<false>(X,job,cut[0],cut[1]); }
                    else { X.wlo=cut[0]; X.whi=cut[1];
                           d2_cell_paint<true>(X,job,0,(uint32_t)nj); }
                    // EFFECTIVE share. In BAND mode cut[] is in BYTES of
                    // the written extent: the requested share and the
                    // obtained share differ (bounds rounded up to the line
                    // start). In CELL mode cut[] is a cell index -- so only
                    // cells are accumulated, not bytes that aren't cells.
                    if(!g_parCellMode){ g_parSpanC += cut[1]-cut[0];
                                        g_parSpanW += cut[L]-cut[1]; }
                    g_parCellsC += X.cells;
                    // (e) JOIN. Cost is measured in SPIN-LOOP ROUNDS, not
                    //     microseconds: reading a clock per call would cost
                    //     more than what it measures (~500 calls/s), and on
                    //     Vita it's a kernel call.
                    //     STEALING: under a run-to-block scheduler, a worker
                    //     that's never scheduled would block the game
                    //     forever. So the calling thread reclaims by CAS any
                    //     lane that has NOT STARTED, and yields between
                    //     rounds so an ALREADY-STARTED lane can finish.
                    uint64_t wait=0;
                    for(int i=0;i<g_parLive;i++){
                        D2ParLane& Ln=g_parLane[i];
                        while(__atomic_load_n(&Ln.doneGen,__ATOMIC_ACQUIRE)!=gen){
                            uint32_t z=0;
                            if(wait>=g_parStealAt &&
                               __atomic_compare_exchange_n(&Ln.claim,&z,1u,false,
                                                           __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
                                ++g_parSteals; d2_par_run_lane(Ln);
                                __atomic_store_n(&Ln.doneGen,gen,__ATOMIC_RELEASE);
                                break; }
                            ++wait;
                            // Two tiers. Before the steal threshold: a
                            // YIELD, enough for an ALREADY-STARTED lane
                            // sharing the core to finish. After: a REAL
                            // NAP, because at that point the lane is
                            // claimed by a worker we couldn't reclaim, and
                            // a plain yield has never been confirmed to
                            // give up the core on console -- only an
                            // actual sleep has. Under a run-to-block
                            // scheduler, pure spinning would deadlock.
                            if(wait>g_parStealAt){ if((wait&255u)==0) d2_par_nap(50); }
                            else if((wait&1023u)==0) d2_par_yield(); }
                        g_parCellsW+=Ln.cells; }
                    // (f) BARRIER. The A9 cores' L1s are separate: worker
                    //     writes must be visible to the main thread BEFORE
                    //     StretchBlt re-reads the DIB.
                    __sync_synchronize();
                    ++g_parCalls; ++g_parJoins; g_parWaitSum+=wait;
                    if(wait>g_parWaitMax) g_parWaitMax=wait;
                } else if(g_cellOpt&CO_NOVEC){
                    // CO_NOVEC. The old path made TWO copies per served
                    // call: building the vector from the static array, then
                    // CAPTURING THAT VECTOR BY VALUE in the `paint` closure
                    // -- so two heap allocations and two memcpy of nj*36
                    // bytes, for an array that never left the call's
                    // logical stack. On Vita an allocation takes a newlib
                    // lock: that's pure overhead.
                    if(g_parLive>0) ++g_parSeqCalls;
                    d2_cell_paint<false>(X,job,0,(uint32_t)nj);
                } else {
                    if(g_parLive>0) ++g_parSeqCalls;
                    std::vector<CellJob> V(job,job+nj);
                    auto vpaint=[&V,&X](){ d2_cell_paint<false>(X,V.data(),0,(uint32_t)V.size()); };
                    vpaint();
                    if(g_cellStat) g_csVecB += 2ull*(uint64_t)nj*sizeof(CellJob);
                }
                // NO invalidate_code: the destination is the DIB, a data
                // buffer that's never translated -- exactly what the
                // cell-blit port above already does, writing the same
                // memory via hostptr with no invalidation. Invalidating a
                // screen-sized range on every call would cost more than
                // the gain.
                for(int k=0;k<nj;k++) if(job[k].kind){ ++g_loopScale; if(job[k].kind==2) ++g_loopScaleClip; }
                ++g_loopServed; g_loopCells+=(uint64_t)nj;
                if(g_cellStat){ ++g_csCalls; g_csCells+=(uint64_t)nj; }
                return OK((uint32_t)n); };                 // 0x4f8d8f: eax = count = n
            s.fn=loopBody;
            br.register_shim("native.hook","d2_cell_loop_114",s);
            cpu->set_alternate(s_loopEntry,br.shim_trap("native.hook","d2_cell_loop_114"));
            std::printf("portage: boucle de cellules Game+0xf8b80 (alternate, NATIVECELLLOOP)\n");
            d2vita_progress("portage: boucle de cellules Game+0xf8b80 ARMEE");
            // PROOF OF ARMING. An unarmed leg and an armed-but-ineffective
            // leg produce the same log otherwise: without this line, a 0%
            // result can't distinguish the two.
            { char m[160];
              std::snprintf(m,sizeof m,
                "cellopt: masque=0x%x [%s%s%s%s%s%s] recensement=%d",
                g_cellOpt,
                (g_cellOpt&CO_NOVEC)?"sansvecteur ":"", (g_cellOpt&CO_LUTC)?"cachevue ":"",
                (g_cellOpt&CO_WORD)?"mots32 ":"",       (g_cellOpt&CO_BANDS)?"bandes ":"",
                (g_cellOpt&CO_SCALE)?"echelle ":"",     (g_cellOpt&CO_IDENT)?"identite ":"",
                g_cellStat?1:0);
              jpline("%s", m); }
            // ---- D2_CELLPAR: worker startup + PROOF OF ARMING ----
            // Threads are created HERE, from the INSTALL thread, never from
            // inside a shim body under the GIL: creating a thread from a
            // guest runner has never been validated on target, and there's
            // no reason to risk it.
            d2_par_start();
            { char m[224]; char sharebuf[16];
              std::snprintf(sharebuf,sizeof sharebuf,"%u%%",(unsigned)g_parShare);
              std::snprintf(m,sizeof m,
                "cellpar: ouvriers=%d/%d mode=%s coeurs=%s part=%s min=%u spin=%u cessions=%u sieste=%uus vol=%u",
                g_parLive,g_parN, g_parCellMode?"cellules":"bandes", g_parCpu,
                g_parShare?sharebuf:"egale",
                (unsigned)g_parMinCell,(unsigned)g_parSpin,(unsigned)g_parYields,
                (unsigned)g_parIdleUs,(unsigned)g_parStealAt);
              jpline("%s", m); }
            if(g_ovlOn) jpline("cellovl: recensement de recouvrement ARME (sequentiel force)");
        }
        d2vita_progress("portages natifs points chauds: lumiere/blend/RLE/collision/grille poses");
    }

    // ---- Native port of DYNAMIC LIGHT placement: ON by default, =0 to disable
    // Writer counterpart of NATIVELIGHT. Placed OUTSIDE the "hot spots"
    // block on purpose: NATIVEHOT=0 must not silently disable it too (a
    // knob that cuts more than it announces misleads bisection).
    // Was env-only (default off) until 2026-09-22, even though it had been in
    // the validated game env since 06/09 — the fff1904 "game env becomes the
    // compiled default" pass missed it, and the VPK ships no env.txt, so no
    // player ever ran it. Oracle replayed before flipping the default
    // (tools/oracle_lightmap.sh, 4000 frames): image fingerprint identical to
    // the control, 25002 placements served, cross-oracle 25002 / 0 divergence.
    // `alternate` hook at the ENTRY of Game+0x75420 -- a single call site
    // (0x4756af), no jump target, first byte 0x55 `push ebp` so a faithful
    // fallback is possible. The native body emulates the bare `ret` (the
    // bridge pops the return address) and leaves EAX unchanged.
    if(const char* lmEnv = getenv("NATIVELIGHTMAP"); g_114 && g_d2base && !(lmEnv && !strcmp(lmEnv,"0"))){
        const uint32_t GB=g_d2base, entry=GB+0x75420;
        uint8_t b0=0; cpu->read(entry,&b0,1);
        if(b0!=0x55){
            d2vita_progress("lightmap: REFUS — Game+0x75420 ne commence pas par push ebp");
        } else {
            static uint32_t lm_entry=0, lm_exit=0, lm_ra=0; lm_entry=entry;
            static bool lm_armed=false;
            static std::vector<uint8_t> lm_exp;
            // D2_LIGHTMAPVERIFY: CROSS ORACLE. The native path writes into a
            // COPY of the grid's 18432 bytes, the guest then runs its own
            // body, and the outputs are compared byte for byte. Same
            // pattern as D2_GRIDVERIFY and D2_DCCVERIFY: the only proof
            // that doesn't depend on the scene.
            { Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!d2_lightmap_verify_exit";
              x.fn=[GB,&br](Cpu&c)->uint32_t{
                const uint32_t eax=c.reg(R_EAX);
                if(lm_armed){ lm_armed=false; ++g_lbVerifN;
                    const uint32_t N=48u*48u*8u;
                    std::vector<uint8_t> got(N);
                    c.read(GB+0x3b0e68,got.data(),N);
                    int first=-1; uint32_t bad=0;
                    for(uint32_t i=0;i<N;i++) if(got[i]!=lm_exp[i]){ if(first<0) first=(int)i; ++bad; }
                    if(first>=0 && ++g_lbVerifBad<=12)
                        std::printf("[lightmapverify] appel #%llu : %u/%u octets differents"
                                    " (1er +0x%x = case %d octet %d, invite=%02x natif=%02x)\n",
                                    (unsigned long long)g_lbVerifN,bad,N,(unsigned)first,first/8,first%8,
                                    got[(size_t)first],lm_exp[(size_t)first]);
                    if(!(g_lbVerifN%200)) std::printf("[lightmapverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)g_lbVerifN,(unsigned long long)g_lbVerifBad);
                    br.redirect_next(lm_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);
                return eax; };
              br.register_shim("native.hook","d2_lightmap_verify_exit",x);
              lm_exit=br.shim_trap("native.hook","d2_lightmap_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_lightblob_114";
            s.fn=[GB,&br](Cpu&c)->uint32_t{
                static const bool verify=getenv("D2_LIGHTMAPVERIFY")!=nullptr;
                const uint32_t E=c.reg(R_ESP), L=c.reg(R_EDI);
                if(verify && !lm_armed){
                    const uint32_t N=48u*48u*8u;
                    lm_exp.assign(N,0);
                    if(c.read(GB+0x3b0e68,lm_exp.data(),N) && d2_lightblob_apply(c,GB,L,lm_exp.data())){
                        lm_armed=true; lm_ra=c.read_u32(E); c.write_u32(E,lm_exit); }
                    c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8);   // faithful fallback: `push ebp`
                    br.redirect_next(lm_entry+1);
                    return c.reg(R_EAX); }
                uint8_t* g=(uint8_t*)c.hostptr(GB+0x3b0e68,48*48*8);
                if(g && d2_lightblob_apply(c,GB,L,g)){ ++g_lbServed; return c.reg(R_EAX); }  // bare `ret`
                ++g_lbRepli;
                c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8);
                br.redirect_next(lm_entry+1);
                return c.reg(R_EAX); };
            br.register_shim("native.hook","d2_lightblob_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_lightblob_114"));
            std::printf("portage: pose de lumiere dynamique Game+0x75420 (alternate, NATIVELIGHTMAP)%s\n",
                        getenv("D2_LIGHTMAPVERIFY")?" + oracle croise":"");
            d2vita_progress("portage: pose de lumiere dynamique Game+0x75420 ARMEE (NATIVELIGHTMAP)");
        }
    }

    // ---- Native port of the per-light OCCLUSION FIELD -----------------------
    // ON by default since 2026-09-22, NATIVELIGHTOCC=0 to disable. Outside the
    // "hot spots" block for the same reason as NATIVELIGHTMAP: NATIVEHOT=0 must
    // not silently disable it too.
    // WHY IT IS ON EVEN THOUGH IT SHOWS ~NOTHING ON THE AVERAGE (+0.14 %): it
    // is the ONLY lever measured that moves the 100-250 ms bucket, i.e. the
    // visible drops to 16-17 fps. Console, patrol, 6200 frames: that bucket
    // reads 34 in all 3 armed passes against 38/41/41 in the controls, and
    // 37 -> 28 once paired with NATIVELIGHTMAP. That pairing is the point —
    // this function BUILDS the occlusion field that Game+0x75420
    // (NATIVELIGHTMAP) then consumes, so porting the producer without the
    // consumer returned nothing.
    // Oracle: tools/oracle_lightocc.sh PASS — image fingerprint identical over
    // 4000 frames, cross-oracle 18 rebuilds / 0 divergence. Coverage is thin
    // (the menu script barely moves the view): PATROUILLE=1 exercises it far
    // harder and is the run to repeat before trusting it further.
    //
    // MECHANISM: `alternate` at the ENTRY of Game+0x750f0, NOT the dyn86_intrin
    // table. The intrinsic table exists to avoid the trap on a tiny leaf called
    // thousands of times per frame; here it is the opposite shape -- one call
    // per light per frame, and the body it replaces is roughly 15 000 guest
    // calls deep. A single trap per call is bought back many times over, and
    // the alternate lets the helper fall back into the real body byte for byte.
    //
    // CAPTURE: tools/verif_intrin_capture.py reports 1 direct `call rel32`
    // (0x4756a3), 0 `jmp rel32` to the entry and 0 data references. The 10
    // "sauts dans le corps" it lists are ALL internal to 0x4750f0 itself
    // (0x4750fb, 0x475106, 0x475148, 0x475168, 0x47520b, 0x475255, 0x47536b,
    // 0x475395, 0x4753a6, 0x4753b7), i.e. the function's own branches: the body
    // is reachable only through the entry, which is exactly what an entry
    // alternate captures.
    if(const char* loEnv = getenv("NATIVELIGHTOCC"); g_114 && g_d2base && !(loEnv && !strcmp(loEnv,"0"))){
        const uint32_t GB=g_d2base, entry=GB+0x750f0;
        // Entry fingerprint. The first 0x28 bytes are opcode-only:
        //   55 8b ec 8b 46 18 83 ec 3c 85 c0 0f8e.. 3d 00010000 0f8d.. 68 00100000
        // The absolute `push 0x7a8a50` that follows at +0x28 is RELOCATED by
        // the loader (Game.exe is not at 0x400000 under D2LAYOUT=compact), so
        // it is checked as a RELOCATED VALUE instead of a raw byte: that also
        // proves the global VAs this port computes from GB are the right ones.
        static const uint8_t sigO[0x28]={
            0x55,0x8b,0xec,0x8b,0x46,0x18,0x83,0xec,0x3c,0x85,0xc0,
            0x0f,0x8e,0x13,0x03,0x00,0x00,0x3d,0x00,0x01,0x00,0x00,
            0x0f,0x8d,0x08,0x03,0x00,0x00,0x68,0x00,0x10,0x00,0x00,
            0x03,0xc0,0xc1,0xf8,0x03,0x6a,0x00 };
        uint8_t got[0x2d]={0}; cpu->read(entry,got,sizeof got);
        const uint32_t absOp=(uint32_t)got[0x29]|((uint32_t)got[0x2a]<<8)
                            |((uint32_t)got[0x2b]<<16)|((uint32_t)got[0x2c]<<24);
        const uint32_t wantOp=GB+0x3a8a50;              // push OFFSET LUM[]
        if(memcmp(got,sigO,sizeof sigO) || got[0x28]!=0x68 || absOp!=wantOp){
            char m[200]; std::snprintf(m,sizeof m,
                "lightocc: REFUS — Game+0x750f0 inattendu (octets %02x%02x%02x%02x%02x%02x, "
                "push=%02x abs=0x%08x attendu 0x%08x)",
                got[0],got[1],got[2],got[3],got[4],got[5],
                got[0x28],(unsigned)absOp,(unsigned)wantOp);
            std::printf("%s\n",m); jpline("%s",m); d2vita_progress(m);
        } else {
            static uint32_t lo_entry=0, lo_exit=0, lo_ra=0; lo_entry=entry;
            static bool lo_armed=false;
            static uint32_t lo_bufVA=0, lo_bufN=0;
            static std::vector<uint32_t> lo_expS, lo_expB;   // 4-aligned: the fields are int32
            // D2_LIGHTOCCVERIFY: CROSS ORACLE. The native path rebuilds into a
            // COPY of the 33040-byte scratch span AND a copy of the light's
            // buffer, the guest then runs its own body, and both are compared
            // byte for byte on EVERY call. [L+0x2c] needs no comparison: it is
            // an unconditional `=1` on the only path that reaches it, and in
            // verify mode it is the GUEST that writes it.
            { Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!d2_lightocc_verify_exit";
              x.fn=[GB,&br](Cpu&c)->uint32_t{
                const uint32_t eax=c.reg(R_EAX);
                if(lo_armed){ lo_armed=false; ++g_loVerifN;
                    std::vector<uint32_t> got((size_t)LO_SPAN/4u,0u);
                    c.read(GB+0x3a8a48,got.data(),(uint32_t)LO_SPAN);
                    int firstS=-1; uint32_t badS=0;
                    const uint8_t* g8=(const uint8_t*)got.data();
                    const uint8_t* e8=(const uint8_t*)lo_expS.data();
                    for(uint32_t i=0;i<(uint32_t)LO_SPAN;i++) if(g8[i]!=e8[i]){
                        if(firstS<0) firstS=(int)i; ++badS; }
                    int firstB=-1; uint32_t badB=0;
                    if(lo_bufN && lo_bufVA){
                        std::vector<uint32_t> gb((size_t)lo_bufN/4u,0u);
                        c.read(lo_bufVA,gb.data(),lo_bufN);
                        const uint8_t* b8=(const uint8_t*)gb.data();
                        const uint8_t* x8=(const uint8_t*)lo_expB.data();
                        for(uint32_t i=0;i<lo_bufN;i++) if(b8[i]!=x8[i]){
                            if(firstB<0) firstB=(int)i; ++badB; }
                    }
                    if((firstS>=0||firstB>=0) && ++g_loVerifBad<=12)
                        std::printf("[lightoccverify] appel #%llu : champ %u/%u octets differents"
                                    " (1er +0x%x) | tampon %u/%u (1er +0x%x)\n",
                                    (unsigned long long)g_loVerifN,badS,(unsigned)LO_SPAN,
                                    (unsigned)(firstS<0?0:firstS),badB,lo_bufN,
                                    (unsigned)(firstB<0?0:firstB));
                    if(!(g_loVerifN%200)) std::printf("[lightoccverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)g_loVerifN,(unsigned long long)g_loVerifBad);
                    br.redirect_next(lo_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);       // the bridge will pop: compensate
                return eax; };
              br.register_shim("native.hook","d2_lightocc_verify_exit",x);
              lo_exit=br.shim_trap("native.hook","d2_lightocc_verify_exit"); }
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!d2_lightocc_114";
            s.fn=[GB,&br](Cpu&c)->uint32_t{
                static const bool verify=getenv("D2_LIGHTOCCVERIFY")!=nullptr;
                const uint32_t E=c.reg(R_ESP), L=c.reg(R_ESI);   // ESI = the light object
                // Faithful fallback = the guest's own `push ebp` then entry+1.
                // The bridge pops the return address on the way out, hence the
                // extra -4. EAX is left UNCHANGED (it is dead at 0x4756a8).
                auto repli=[&]()->uint32_t{
                    c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8);
                    br.redirect_next(lo_entry+1); return c.reg(R_EAX); };
                if(verify && !lo_armed){
                    const int32_t r=(int32_t)c.read_u32(L+0x18);
                    lo_bufN=d2_lo_bufbytes(r); lo_bufVA=lo_bufN?c.read_u32(L+0x30):0u;
                    lo_expS.assign((size_t)LO_SPAN/4u,0u);
                    lo_expB.assign((size_t)(lo_bufN?lo_bufN:4u)/4u,0u);
                    if(c.read(GB+0x3a8a48,lo_expS.data(),(uint32_t)LO_SPAN)
                       && (!lo_bufN || !lo_bufVA || c.read(lo_bufVA,lo_expB.data(),lo_bufN))
                       && d2_lightocc_build(c,GB,L,(uint8_t*)lo_expS.data(),
                                            lo_bufVA?(uint8_t*)lo_expB.data():nullptr,
                                            lo_bufVA?lo_bufN:0u)){
                        lo_armed=true; lo_ra=c.read_u32(E); c.write_u32(E,lo_exit); }
                    return repli(); }
                uint8_t* S=(uint8_t*)c.hostptr(GB+0x3a8a48,(uint32_t)LO_SPAN);
                const int32_t r=(int32_t)c.read_u32(L+0x18);
                const uint32_t bn=d2_lo_bufbytes(r), bva=bn?c.read_u32(L+0x30):0u;
                uint8_t* B=bva?(uint8_t*)c.hostptr(bva,bn):nullptr;
                const int rc=d2_lightocc_build(c,GB,L,S,B,B?bn:0u);
                if(!rc){ ++g_loRepli; return repli(); }
                if(rc==2) c.write_u32(L+0x2c,1u);      // 0x47540b
                ++g_loServed; return c.reg(R_EAX); };  // bare `ret`
            br.register_shim("native.hook","d2_lightocc_114",s);
            cpu->set_alternate(entry,br.shim_trap("native.hook","d2_lightocc_114"));
            std::printf("portage: champ d'occlusion de lumiere Game+0x750f0 (alternate, NATIVELIGHTOCC)%s\n",
                        getenv("D2_LIGHTOCCVERIFY")?" + oracle croise":"");
            d2vita_progress("portage: champ d'occlusion de lumiere Game+0x750f0 ARMEE (NATIVELIGHTOCC)");
        }
    }
}
