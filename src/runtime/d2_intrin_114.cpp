/* src/runtime/d2_intrin_114.cpp — native intrinsics SPECIFIC to Diablo II
 * LoD 1.14d, installed behind the GENERIC mechanism in
 * src/dynarec86/dyn86_intrin.h. Nothing here is runtime: this file is GAME
 * DATA (which functions, which ABI, which arithmetic).
 *
 * ---------------------------------------------------------------------------
 * TARGET 1 — VA 0x50dd60: world -> screen projection
 *
 * By far the hottest function in the game, called 3700-5300 times per frame.
 *
 * CAPTURE PROOF (tools/verif_intrin_capture.py, Game.exe md5
 * 2b3600c10ee0d6387675a2f6cf0ebed2):
 *     1 reference in data   (vtable slot, VA 0x72f1dc)
 *    16 DIRECT `call rel32`
 *     0 direct `jmp` to the entry
 *     0 jump landing inside the body (0x50dd61..0x50de0d)
 * So every entry creates a block that STARTS at 0x50dd60.
 *
 * Hook the BODY, not the 0x4f6760 wrapper: the wrapper only sees the vtable
 * path, the body ALSO sees the 16 direct calls. (The wrapper is a separate
 * concern — it checks two globals and makes an indirect call that can't be
 * short-circuited without assuming the video mode.)
 *
 * ABI, from disassembly:
 *     ECX      = world X            (register)
 *     EDX      = world Y            (register)
 *     [esp+4]  = a3                 (stack)
 *     [esp+8]  = &outX              (stack)
 *     [esp+0xc]= &outY              (stack)
 *     ret 0xc                       (stdcall: callee pops 12 bytes)
 * EBX/ESI/EDI/EBP are saved-restored by the guest body: PRESERVED.
 * EAX/ECX/EDX are clobbered, and we reproduce exactly what the guest leaves
 * in them (see "registers left" below) — this is free and removes an entire
 * class of divergence at once.
 *
 * Globals read (VA): 0x880b60, 0x880b68 (camera), 0x880b6c (base of the
 * 8192-entry table), 0x7c9138, 0x7c913c (half-screen).
 */
#include <stdint.h>
#include <cstdio>

#include "dyn86_intrin.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

extern "C" {
#include "regs.h"
#include "emu/x86emu_private.h"
}

/* Guest -> host translation. Same contract as dyn86_memintrin.c. */
extern "C" uintptr_t dyn86_membase;
#define G2H(a) ((uintptr_t)dyn86_membase + (uintptr_t)(a))

namespace {

/* EXPLICIT arithmetic shift. `>>` on a negative int is implementation-defined
 * in C; the guest body does `sar`, so we spell it out. GCC compiles this to a
 * single ASR instruction. */
static inline int32_t sar32(int32_t v, int n) {
    return (int32_t)(v < 0 ? ~((~(uint32_t)v) >> n) : ((uint32_t)v >> n));
}

/* Global VAs, published by rt_boot after the PE loads (Game.exe can be
 * relocated: these are not constants). */
uint32_t g_vaCamX = 0, g_vaCamY = 0, g_vaTbl = 0, g_vaHalfX = 0, g_vaHalfY = 0;

static inline int32_t ld32(uint32_t va) { return *(const int32_t*)G2H(va); }

}  // namespace

extern "C" void d2_intrin_114_set_globals(uint32_t camx, uint32_t camy,
                                          uint32_t tbl, uint32_t hx, uint32_t hy)
{ g_vaCamX = camx; g_vaCamY = camy; g_vaTbl = tbl; g_vaHalfX = hx; g_vaHalfY = hy; }

/* Table-entry counters (filled in by the helper itself: the emitted code
 * knows nothing about them). */
static dyn86_intrin_t* g_projEntry = nullptr;

/* --- cross-oracle (D2_PROJVERIFY=1) ---------------------------------------
 *
 * WHAT DOESN'T WORK: comparing against the NEXT call. A native computation
 * that declines to serve and instead compares its values by re-reading
 * *pOutX / *pOutY on the following call produces false divergences: the caller
 * can already have consumed and overwritten its locals between the two
 * calls, so the deferred comparator reads leftover garbage, not what the
 * guest actually wrote. Filtering on pointer equality barely helps.
 *
 * WHAT WORKS: compare AT THE RETURN of the guest function, before the caller
 * can touch anything. The return address on the guest stack is replaced with
 * a trap; the guest runs its body, its `ret 0xc` lands in the trap, we
 * compare, then resume at the original address. Same pattern as
 * D2_MEMVERIFY (dyn86_memintrin.c: mi_verify_arm / dyn86_mi_verify_ret).
 *
 * Only one call in flight at a time: atomic exchange, the rest are counted
 * as skipped. PROOF mode only, never for benchmarking (one trap per call,
 * 4-5k per frame). */
extern "C" int      d2_proj_verify = 0;
extern "C" uint32_t d2_proj_verify_trap = 0;
extern "C" unsigned long long d2_proj_ver_n = 0, d2_proj_ver_bad = 0, d2_proj_ver_skip = 0;

namespace {
    volatile int v_armed = 0;
    uint32_t v_outX, v_outY, v_ra;
    int32_t  v_expX, v_expY;
    int32_t  v_inCX, v_inCY, v_inA3, v_inK; uint32_t v_inIdx;
}

/* Installs the return trap. Returns 1 if armed (the caller must then FALL BACK). */
static int proj_verify_arm(uint32_t esp, uint32_t pOutX, uint32_t pOutY,
                           int32_t eX, int32_t eY,
                           int32_t cx, int32_t cy, int32_t a3, int32_t k, uint32_t idx)
{
    if (!d2_proj_verify || !d2_proj_verify_trap) return 0;
    int z = 0;
    if (!__atomic_compare_exchange_n(&v_armed, &z, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        ++d2_proj_ver_skip; return 0; }
    v_outX = pOutX; v_outY = pOutY; v_expX = eX; v_expY = eY;
    v_inCX = cx; v_inCY = cy; v_inA3 = a3; v_inK = k; v_inIdx = idx;
    uint32_t* sp = (uint32_t*)G2H(esp);
    v_ra = sp[0]; sp[0] = d2_proj_verify_trap;
    return 1;
}

/* Called by the trap shim: compares, then returns the real return address. */
extern "C" uint32_t d2_proj_verify_ret(void)
{
    if (!v_armed) return 0;
    ++d2_proj_ver_n;
    const int32_t gx = *(const int32_t*)G2H(v_outX);
    const int32_t gy = *(const int32_t*)G2H(v_outY);
    if (gx != v_expX || gy != v_expY) {
        ++d2_proj_ver_bad;
        if (d2_proj_ver_bad <= 12)
            std::printf("[projverify] appel #%llu : cx=%d cy=%d a3=%d idx=%u k=%d "
                        "| natif=(%d,%d) invite=(%d,%d)\n",
                        (unsigned long long)d2_proj_ver_n,
                        (int)v_inCX,(int)v_inCY,(int)v_inA3,(unsigned)v_inIdx,(int)v_inK,
                        (int)v_expX,(int)v_expY,(int)gx,(int)gy);
    }
    uint32_t ra = v_ra;
    __atomic_store_n(&v_armed, 0, __ATOMIC_RELEASE);
    return ra;
}

extern "C" int d2_intrin_proj_50dd60(void* emu, uint32_t esp)
{
    x86emu_t* e = (x86emu_t*)emu;
    if (g_projEntry) ++g_projEntry->calls;

    /* Mode 2: PLUMBING ONLY (dyn86_intrin.h contract). We pay for the call,
     * the ABI round-trip, and setting up the input registers, then do
     * nothing and let the guest compute. The difference from baseline
     * isolates the plumbing cost. */
    if (dyn86_intrin_on == 2) return 0;

    /* This is only safe once the globals have been published AND the arena
     * is in place (G2H assumes membase). Otherwise: faithful fallback. */
    if (!g_vaCamX || !g_vaTbl || !g_vaHalfX) return 0;

    const uint32_t* a = (const uint32_t*)G2H(esp);   /* [0]=return, [1..3]=args */
    const int32_t  a3    = (int32_t)a[1];
    const uint32_t pOutX = a[2];
    const uint32_t pOutY = a[3];
    if (!pOutX || !pOutY) return 0;                  /* never seen; refuse */

    int32_t ecx = (int32_t)e->regs[_CX].dword[0] - ld32(g_vaCamX);
    int32_t edx = (int32_t)e->regs[_DX].dword[0] - ld32(g_vaCamY);

    /* eax = (24*dx)>>16 ; c = (24*dy)>>16   (the six `add rX,rX` in the body) */
    const int32_t A = sar32((int32_t)((uint32_t)ecx * 24u), 16);
    const int32_t C = sar32((int32_t)((uint32_t)edx * 24u), 16);

    int32_t edi = (int32_t)(((uint32_t)(A + C)) * 0x2d4u);
    int32_t esi = (int32_t)((uint32_t)a3 * 0x4ccu);

    /* edx' = edi*0x376 + (esi<<9), then table index */
    int32_t t = (int32_t)((uint32_t)edi * 0x376u + ((uint32_t)esi << 9));
    uint32_t idx = (uint32_t)(sar32(t, 20) - 0x1000) & 0x1fffu;
    const int32_t k = *(const int32_t*)G2H((uint32_t)ld32(g_vaTbl) + idx * 4u);

    /* X = ((C-A)*k*0x2d4)>>20  +  half(0x7c9138)
     * `cltd ; sub edx,eax ; sar eax,1` is the compiled form of INTEGER
     * DIVISION BY 2 (truncation toward zero) — exactly `/ 2` in C. */
    int32_t cx = (int32_t)((uint32_t)(C - A) * (uint32_t)k);
    cx = (int32_t)((uint32_t)cx * 0x2d4u);
    const int32_t halfX = ld32(g_vaHalfX) / 2;
    const int32_t outX = sar32(cx, 20) + halfX;

    /* Y = ((esi*0x376 - (edi<<9))>>10 * k)>>20 + half(0x7c913c) - 0x20 */
    int32_t sy = (int32_t)((uint32_t)esi * 0x376u - ((uint32_t)edi << 9));
    sy = sar32(sy, 10);
    sy = (int32_t)((uint32_t)sy * (uint32_t)k);
    const int32_t halfY = ld32(g_vaHalfY) / 2;
    const int32_t outY = sar32(sy, 20) + halfY - 0x20;

    /* PROOF mode: install the return trap and FALL BACK (the guest computes). */
    if (d2_proj_verify) {
        proj_verify_arm(esp, pOutX, pOutY, outX, outY, ecx, edx, a3, k, idx);
        return 0;
    }

    *(int32_t*)G2H(pOutX) = outX;
    *(int32_t*)G2H(pOutY) = outY;

    /* Registers left by the guest body, reproduced exactly:
     *   EAX = halfY   (0x50de01: sar eax,1 on 0x7c913c, sign-corrected)
     *   ECX = outY    (0x50de04: lea ecx,[esi+eax-0x20])
     *   EDX = &outY   (0x50ddfe: mov edx,[ebp+0x10])
     * EBX/ESI/EDI/EBP: saved-restored by the guest, so unchanged — the
     * emitted code doesn't reload them (regmask), they keep their value. */
    e->regs[_AX].dword[0] = (uint32_t)halfY;
    e->regs[_CX].dword[0] = (uint32_t)outY;
    e->regs[_DX].dword[0] = pOutY;

    if (g_projEntry) ++g_projEntry->served;
    return 1;
}


/* =========================================================================
 * Targets 2..4 -- pure integer leaves named by the console block profile of
 * 2026-09-25 (Act V, Perspective on): the per-vertex colour LUT, the light
 * grid cell lookup, the light grid rectangle fill. Same mechanism, same
 * discipline: the helper reproduces EVERY observable effect of the guest
 * body (memory AND the registers a caller could read), refuses whenever it
 * is not sure, and can prove itself with a return trap (D2_INTRINVERIFY=1).
 * ========================================================================= */
namespace {
uint32_t g_vaLutBase = 0;     /* ds:0x880b5c : pointer to the 256x256 colour table  */
uint32_t g_vaLgOrgX = 0, g_vaLgOrgY = 0;   /* ds:0x7b0a54/58 : light grid origin (cells)  */
uint32_t g_vaLgFlag = 0;      /* ds:0x712b8c : "also return the three RGB bytes"       */
uint32_t g_vaLgTbl  = 0;      /* ds:0x7b0e68 : 48x48 grid, 8 bytes per cell            */
static inline uint8_t ld8(uint32_t va) { return *(const uint8_t*)G2H(va); }
static inline bool fp_ok(uint32_t va, const uint8_t* sig, int n) { return std::memcmp((const void*)G2H(va), sig, (size_t)n) == 0; }
}

/* --- generic return-trap oracle (one call in flight at a time) ------------- */
extern "C" int      d2_intrin_verify = 0;
extern "C" uint32_t d2_intrin_verify_trap = 0;
extern "C" unsigned long long d2_iv_n[5] = {0,0,0,0,0}, d2_iv_bad[5] = {0,0,0,0,0}, d2_iv_skip = 0;
static const char* const g_ivName[5] = { "proj", "lut", "lgrid", "lfill", "lut2" };
namespace {
    enum { IV_LUT = 1, IV_LGRID = 2, IV_LFILL = 3, IV_LUT2 = 4 };
    volatile int iv_armed = 0;
    int      iv_which = 0;
    uint32_t iv_ra = 0;
    uint32_t iv_in[8];                  /* inputs, to recompute the expectation at the return */
    uint32_t iv_eax, iv_ecx, iv_edx;    /* expected registers at the return */
    uint32_t iv_regmask;                /* which of eax/ecx/edx the guest body defines (bit0..2) */
    int iv_arm(uint32_t esp, int which, uint32_t regmask) {
        if (!d2_intrin_verify || !d2_intrin_verify_trap) return 0;
        int z = 0;
        if (!__atomic_compare_exchange_n(&iv_armed, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) { ++d2_iv_skip; return 0; }
        iv_which = which; iv_regmask = regmask;
        uint32_t* sp = (uint32_t*)G2H(esp);
        iv_ra = sp[0]; sp[0] = d2_intrin_verify_trap;
        return 1;
    }
}

/* ---- target 2: Game+0x10dc30, per-vertex colour LUT ----------------------
 *   ecx = vertex, edx = ARGB colour; writes [ecx+8]; bare RET.
 *   Body (disassembly 1.14d): row = [0x880b5c] + (dl<<8);
 *   result = 0xFF000000 | T[row+b1]<<16 | T[row+b2]<<8 | T[row+b3]
 *   Registers left: EAX = row, ECX = result, EDX = T[row+b3] (movzx). */
static inline void lut_compute(uint32_t colour, uint32_t* row, uint32_t* res, uint32_t* b3o) {
    const uint32_t r = (uint32_t)ld32(g_vaLutBase) + ((colour & 0xffu) << 8);
    const uint32_t b1 = ld8(r + ((colour >> 8) & 0xffu));
    const uint32_t b2 = ld8(r + ((colour >> 16) & 0xffu));
    const uint32_t b3 = ld8(r + (colour >> 24));
    *row = r; *res = 0xFF000000u | (b1 << 16) | (b2 << 8) | b3; *b3o = b3;
}
static dyn86_intrin_t* g_lutEntry = nullptr;
extern "C" int d2_intrin_lut_50dc30(void* emu, uint32_t esp)
{
    x86emu_t* e = (x86emu_t*)emu;
    if (g_lutEntry) ++g_lutEntry->calls;
    if (dyn86_intrin_on == 2) return 0;
    if (!g_vaLutBase) return 0;
    const uint32_t vtx = e->regs[_CX].dword[0], colour = e->regs[_DX].dword[0];
    if (!vtx) return 0;
    uint32_t row, res, b3; lut_compute(colour, &row, &res, &b3);
    if (d2_intrin_verify) {
        if (iv_arm(esp, IV_LUT, 7u)) { iv_in[0] = vtx; iv_in[1] = colour; iv_eax = row; iv_ecx = res; iv_edx = b3; }
        return 0;
    }
    *(uint32_t*)G2H(vtx + 8) = res;
    e->regs[_AX].dword[0] = row; e->regs[_CX].dword[0] = res; e->regs[_DX].dword[0] = b3;
    if (g_lutEntry) ++g_lutEntry->served;
    return 1;
}

/* ---- target 3: Game+0x075aa0, light grid cell lookup ----------------------
 *   ecx = world x, edx = world y (>>3 = cell), stack: 4 output byte pointers;
 *   ret 0x10. cx,cy clamped to 0..47; cell = tbl + (cy*48+cx)*8;
 *   *p0 = cell[4]; if ([0x712b8c]) { *p1 = cell[5]; *p2 = cell[6]; *p3 = cell[7]; }
 *   Registers left (disassembly): flag off: EAX = cell, ECX = cell[4], EDX = p0;
 *   flag on: EAX = (cell & ~0xff) | cell[7], ECX = p3, EDX = p2. */
static dyn86_intrin_t* g_lgridEntry = nullptr;
struct LgOut { uint32_t cell; uint8_t b[4]; int flag; uint32_t p[4]; };
static inline void lgrid_compute(int32_t x, int32_t y, const uint32_t* args, LgOut* o) {
    int32_t cx = sar32(x, 3) - ld32(g_vaLgOrgX);
    int32_t cy = sar32(y, 3) - ld32(g_vaLgOrgY);
    if (cx < 0) cx = 0; else if (cx > 0x2f) cx = 0x2f;
    if (cy < 0) cy = 0; else if (cy > 0x2f) cy = 0x2f;
    const uint32_t idx = (uint32_t)(cy * 48 + cx);
    o->cell = g_vaLgTbl + idx * 8u;
    for (int k = 0; k < 4; ++k) { o->b[k] = ld8(o->cell + 4u + (uint32_t)k); o->p[k] = args[k]; }
    o->flag = ld32(g_vaLgFlag) != 0;
}
extern "C" int d2_intrin_lgrid_475aa0(void* emu, uint32_t esp)
{
    x86emu_t* e = (x86emu_t*)emu;
    if (g_lgridEntry) ++g_lgridEntry->calls;
    if (dyn86_intrin_on == 2) return 0;
    if (!g_vaLgTbl) return 0;
    const uint32_t* a = (const uint32_t*)G2H(esp);   /* [0]=return, [1..4]=p0..p3 */
    if (!a[1]) return 0;
    LgOut o; lgrid_compute((int32_t)e->regs[_CX].dword[0], (int32_t)e->regs[_DX].dword[0], a + 1, &o);
    if (o.flag && (!a[2] || !a[3] || !a[4])) return 0;
    const uint32_t eax = o.flag ? ((o.cell & 0xffffff00u) | o.b[3]) : o.cell;
    const uint32_t ecx = o.flag ? o.p[3] : o.b[0];
    const uint32_t edx = o.flag ? o.p[2] : o.p[0];
    if (d2_intrin_verify) {
        if (iv_arm(esp, IV_LGRID, 7u)) { iv_in[0] = e->regs[_CX].dword[0]; iv_in[1] = e->regs[_DX].dword[0];
            for (int k = 0; k < 4; ++k) iv_in[2 + k] = a[1 + k]; iv_eax = eax; iv_ecx = ecx; iv_edx = edx; }
        return 0;
    }
    *(uint8_t*)G2H(o.p[0]) = o.b[0];
    if (o.flag) { *(uint8_t*)G2H(o.p[1]) = o.b[1]; *(uint8_t*)G2H(o.p[2]) = o.b[2]; *(uint8_t*)G2H(o.p[3]) = o.b[3]; }
    e->regs[_AX].dword[0] = eax; e->regs[_CX].dword[0] = ecx; e->regs[_DX].dword[0] = edx;
    if (g_lgridEntry) ++g_lgridEntry->served;
    return 1;
}

/* ---- target 4: Game+0x0744b0, light grid rectangle fill -------------------
 *   Custom convention: ecx = row0, eax = row1, edx = ARGB, edi = col1,
 *   [esp+4] = col0; ret 4. For row in row0..row1, if col0 <= col1: cells
 *   (row*48 + col0 .. col1) get {dword 0, b0, b1, b2, b3}. Rows beyond the
 *   48x48 table are REFUSED here (the guest would write outside; the native
 *   side never does the guest's out-of-bounds writes for it).
 *   Registers left: EAX = pointer after the last cell written in the last
 *   row (or the last row's first cell when col0 > col1); ECX = 0 if rows
 *   were filled, row0 if every row was skipped, unchanged if row0 > row1. */
static dyn86_intrin_t* g_lfillEntry = nullptr;
static inline int lfill_expect(int32_t row0, int32_t row1, int32_t col0, int32_t col1,
                               uint32_t ecx_in, uint32_t eax_in, uint32_t* eax_out, uint32_t* ecx_out) {
    if (row0 > row1) { *eax_out = eax_in; *ecx_out = ecx_in; return 0; }
    if (row0 < 0 || row1 > 47 || col0 < 0 || col1 > 47) return -1;
    const uint32_t last = g_vaLgTbl + ((uint32_t)(row1 * 48 + col0)) * 8u;
    if (col0 > col1) { *eax_out = last; *ecx_out = ecx_in; }
    else { *eax_out = last + (uint32_t)(col1 - col0 + 1) * 8u; *ecx_out = 0; }
    return 1;
}
extern "C" int d2_intrin_lfill_4744b0(void* emu, uint32_t esp)
{
    x86emu_t* e = (x86emu_t*)emu;
    if (g_lfillEntry) ++g_lfillEntry->calls;
    if (dyn86_intrin_on == 2) return 0;
    if (!g_vaLgTbl) return 0;
    const uint32_t* a = (const uint32_t*)G2H(esp);
    const int32_t row0 = (int32_t)e->regs[_CX].dword[0], row1 = (int32_t)e->regs[_AX].dword[0];
    const int32_t col0 = (int32_t)a[1], col1 = (int32_t)e->regs[_DI].dword[0];
    const uint32_t colour = e->regs[_DX].dword[0];
    uint32_t eaxo, ecxo;
    const int r = lfill_expect(row0, row1, col0, col1, e->regs[_CX].dword[0], e->regs[_AX].dword[0], &eaxo, &ecxo);
    if (r < 0) return 0;
    if (d2_intrin_verify) {
        if (iv_arm(esp, IV_LFILL, 3u)) { iv_in[0] = (uint32_t)row0; iv_in[1] = (uint32_t)row1; iv_in[2] = (uint32_t)col0;
            iv_in[3] = (uint32_t)col1; iv_in[4] = colour; iv_eax = eaxo; iv_ecx = ecxo; }
        return 0;
    }
    if (r > 0 && col0 <= col1) {
        for (int32_t row = row0; row <= row1; ++row) {
            uint8_t* c = (uint8_t*)G2H(g_vaLgTbl + ((uint32_t)(row * 48 + col0)) * 8u);
            for (int32_t col = col0; col <= col1; ++col, c += 8) {
                c[0] = c[1] = c[2] = c[3] = 0;
                c[4] = (uint8_t)colour; c[5] = (uint8_t)(colour >> 8); c[6] = (uint8_t)(colour >> 16); c[7] = (uint8_t)(colour >> 24);
            }
        }
    }
    e->regs[_AX].dword[0] = eaxo; e->regs[_CX].dword[0] = ecxo;
    if (g_lfillEntry) ++g_lfillEntry->served;
    return 1;
}

/* ---- target 5: Game+0x10dbe0, second colour LUT (F2 tile path) -----------
 *   ecx = vertex, dl = row selector; the three column indices come from the
 *   global bytes 0x880b5a/58/59 (not from edx). Bare RET.
 *   res = 0xFF000000 | T[row+b5a]<<16 | T[row+b58]<<8 | T[row+b59]; [ecx+8] = res.
 *   Registers left: EAX = T[row+b59] (movzx), EDX = res, ECX intact. */
namespace { uint32_t g_vaLutSel = 0; }   /* ds:0x880b58 (three bytes: +0, +1, +2) */
static inline void lut2_compute(uint32_t sel, uint32_t* res, uint32_t* last) {
    const uint32_t row = (uint32_t)ld32(g_vaLutBase) + ((sel & 0xffu) << 8);
    const uint32_t hi = ld8(row + ld8(g_vaLutSel + 2)), mid = ld8(row + ld8(g_vaLutSel + 0)), lo = ld8(row + ld8(g_vaLutSel + 1));
    *res = 0xFF000000u | (hi << 16) | (mid << 8) | lo; *last = lo;
}
static dyn86_intrin_t* g_lut2Entry = nullptr;
extern "C" int d2_intrin_lut2_50dbe0(void* emu, uint32_t esp)
{
    x86emu_t* e = (x86emu_t*)emu;
    if (g_lut2Entry) ++g_lut2Entry->calls;
    if (dyn86_intrin_on == 2) return 0;
    if (!g_vaLutBase || !g_vaLutSel) return 0;
    const uint32_t vtx = e->regs[_CX].dword[0], sel = e->regs[_DX].dword[0];
    if (!vtx) return 0;
    uint32_t res, last; lut2_compute(sel, &res, &last);
    if (d2_intrin_verify) {
        if (iv_arm(esp, IV_LUT2, 5u)) { iv_in[0] = vtx; iv_in[1] = sel; iv_eax = last; iv_edx = res; }
        return 0;
    }
    *(uint32_t*)G2H(vtx + 8) = res;
    e->regs[_AX].dword[0] = last; e->regs[_DX].dword[0] = res;
    if (g_lut2Entry) ++g_lut2Entry->served;
    return 1;
}

/* ---- D2_QUADCOUNT=1: entry counters on the three perspective tile drawers
 * (0x50acc0 quad, 0x50ca00 subdivided tile, 0x50cf70 variant). The helper
 * never serves: the guest runs its body untouched, only `appels` moves. One
 * pass with this knob says how the ~21 000 perspective vertices per frame
 * split between the three, which decides whether a per-call hook can pay
 * for its own trap. Costs one native call (~70 ns) per entry: a probe, off
 * by default. */
/* The generic `calls` counter is bumped by each helper itself (the emitted
 * code knows nothing about it), so every counted target needs its own helper
 * bound to its own table entry -- a shared helper counted nothing. */
namespace { dyn86_intrin_t* g_cntEntry[8] = {nullptr}; }
template <int K> static int d2_intrin_count(void* emu, uint32_t esp)
{ (void)emu; (void)esp; if (g_cntEntry[K]) ++g_cntEntry[K]->calls; return 0; }

/* The return trap calls this with the registers the guest left. Compares the
 * guest's memory effects and registers with what the native helper would
 * have produced; returns the real return address (0 = nothing in flight). */
extern "C" uint32_t d2_intrin_verify_ret(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    if (!iv_armed) return 0;
    const int w = iv_which; ++d2_iv_n[w];
    int bad = 0; char why[160] = "";
    if (w == IV_LUT) {
        const uint32_t got = *(const uint32_t*)G2H(iv_in[0] + 8);
        if (got != iv_ecx) { bad = 1; std::snprintf(why, sizeof why, "[vtx+8] natif=%08x invite=%08x", iv_ecx, got); }
    } else if (w == IV_LUT2) {
        const uint32_t got = *(const uint32_t*)G2H(iv_in[0] + 8);
        if (got != iv_edx) { bad = 1; std::snprintf(why, sizeof why, "[vtx+8] natif=%08x invite=%08x", iv_edx, got); }
    } else if (w == IV_LGRID) {
        LgOut o; lgrid_compute((int32_t)iv_in[0], (int32_t)iv_in[1], iv_in + 2, &o);
        const int n = o.flag ? 4 : 1;
        for (int k = 0; k < n && !bad; ++k) { const uint8_t g = ld8(o.p[k]);
            if (g != o.b[k]) { bad = 1; std::snprintf(why, sizeof why, "octet %d natif=%02x invite=%02x", k, o.b[k], g); } }
    } else if (w == IV_LFILL) {
        const int32_t row0 = (int32_t)iv_in[0], row1 = (int32_t)iv_in[1], col0 = (int32_t)iv_in[2], col1 = (int32_t)iv_in[3];
        const uint32_t colour = iv_in[4];
        for (int32_t row = row0; row <= row1 && !bad && col0 <= col1; ++row) {
            const uint8_t* c = (const uint8_t*)G2H(g_vaLgTbl + ((uint32_t)(row * 48 + col0)) * 8u);
            for (int32_t col = col0; col <= col1 && !bad; ++col, c += 8) {
                const uint8_t want[8] = {0,0,0,0,(uint8_t)colour,(uint8_t)(colour>>8),(uint8_t)(colour>>16),(uint8_t)(colour>>24)};
                if (std::memcmp(c, want, 8)) { bad = 1; std::snprintf(why, sizeof why, "cellule (%d,%d) differe", (int)row, (int)col); } }
        }
    }
    if (!bad) {
        if ((iv_regmask & 1u) && eax != iv_eax) { bad = 1; std::snprintf(why, sizeof why, "EAX natif=%08x invite=%08x", iv_eax, eax); }
        else if ((iv_regmask & 2u) && ecx != iv_ecx) { bad = 1; std::snprintf(why, sizeof why, "ECX natif=%08x invite=%08x", iv_ecx, ecx); }
        else if ((iv_regmask & 4u) && edx != iv_edx) { bad = 1; std::snprintf(why, sizeof why, "EDX natif=%08x invite=%08x", iv_edx, edx); }
    }
    if (bad) { ++d2_iv_bad[w];
        if (d2_iv_bad[w] <= 12) std::printf("[intrinverify] %s appel #%llu : %s\n", g_ivName[w], (unsigned long long)d2_iv_n[w], why); }
    const uint32_t ra = iv_ra;
    __atomic_store_n(&iv_armed, 0, __ATOMIC_RELEASE);
    return ra;
}

/* One line for the periodic window and the end-of-run report. */
extern "C" int d2_intrin_verify_line(char* out, unsigned cap)
{
    if (!d2_intrin_verify) return 0;
    return std::snprintf(out, cap, "intrinverify: lut=%llu/%llu lgrid=%llu/%llu lfill=%llu/%llu lut2=%llu/%llu (compares/divergences) sautes=%llu",
                         (unsigned long long)d2_iv_n[1], (unsigned long long)d2_iv_bad[1],
                         (unsigned long long)d2_iv_n[2], (unsigned long long)d2_iv_bad[2],
                         (unsigned long long)d2_iv_n[3], (unsigned long long)d2_iv_bad[3],
                         (unsigned long long)d2_iv_n[4], (unsigned long long)d2_iv_bad[4],
                         (unsigned long long)d2_iv_skip);
}

/* Registration. Called by rt_boot after the PE loads. */
extern "C" int d2_intrin_114_register(uint32_t d2base)
{
    d2_intrin_114_set_globals(d2base + 0x480b60, d2base + 0x480b68,
                              d2base + 0x480b6c, d2base + 0x3c9138,
                              d2base + 0x3c913c);
    const uintptr_t va = (uintptr_t)d2base + 0x10dd60;
    if (!dyn86_intrin_add(va, d2_intrin_proj_50dd60, /*retn*/0x0c,
                          /*in */ DYN86_IR_CX | DYN86_IR_DX,   /* world X and Y */
                          /*out*/ DYN86_IR_AX | DYN86_IR_CX | DYN86_IR_DX,
                          "proj50dd60"))
        return 0;
    for (int i = 0; i < dyn86_intrin_n; ++i)
        if (dyn86_intrin_tbl[i].va == va) g_projEntry = &dyn86_intrin_tbl[i];

    /* Targets 2..4. Each is fingerprinted at its entry (opcode bytes that
     * carry no relocation) and refused individually: a mismatch names the
     * target and leaves the others armed. verif_intrin_capture.py: lut and
     * lgrid CAPTURE COMPLETE; lfill has one jump landing in its body, from
     * 0x4744bc -- INSIDE the same function (its early-exit `jg` to the
     * epilogue), so every entry still starts a block at 0x4744b0. */
    g_vaLutBase = d2base + 0x480b5c;
    g_vaLgOrgX = d2base + 0x3b0a54; g_vaLgOrgY = d2base + 0x3b0a58;
    g_vaLgFlag = d2base + 0x312b8c; g_vaLgTbl = d2base + 0x3b0e68;
    static const uint8_t sigLut[9]   = {0x53,0x56,0x8b,0xf1,0x0f,0xb6,0xc2,0x8b,0xca};
    static const uint8_t sigLgrid[8] = {0x55,0x8b,0xec,0xc1,0xf9,0x03,0x2b,0x0d};
    static const uint8_t sigLfill[12]= {0x55,0x8b,0xec,0x83,0xec,0x10,0x3b,0xc8,0x56,0x8b,0x75,0x08};
    static const uint8_t sigLut2[6]  = {0x0f,0xb6,0xc2,0x0f,0xb6,0x15};
    g_vaLutSel = d2base + 0x480b58;
    struct T { uint32_t rva; const uint8_t* sig; int n; dyn86_intrin_fn fn; uint16_t retn, in, out; const char* name; dyn86_intrin_t** slot; } tgt[4] = {
        { 0x10dc30, sigLut, 9,   d2_intrin_lut_50dc30,   0,    DYN86_IR_CX|DYN86_IR_DX,                         DYN86_IR_AX|DYN86_IR_CX|DYN86_IR_DX, "lut50dc30",   &g_lutEntry },
        { 0x075aa0, sigLgrid, 8, d2_intrin_lgrid_475aa0, 0x10, DYN86_IR_CX|DYN86_IR_DX,                         DYN86_IR_AX|DYN86_IR_CX|DYN86_IR_DX, "lgrid475aa0", &g_lgridEntry },
        { 0x0744b0, sigLfill, 12,d2_intrin_lfill_4744b0, 4,    DYN86_IR_AX|DYN86_IR_CX|DYN86_IR_DX|DYN86_IR_DI, DYN86_IR_AX|DYN86_IR_CX,             "lfill4744b0", &g_lfillEntry },
        { 0x10dbe0, sigLut2, 6,  d2_intrin_lut2_50dbe0,  0,    DYN86_IR_CX|DYN86_IR_DX,                         DYN86_IR_AX|DYN86_IR_DX,             "lut2_50dbe0", &g_lut2Entry } };
    for (int t = 0; t < 4; ++t) {
        const uint32_t tva = d2base + tgt[t].rva;
        if (!fp_ok(tva, tgt[t].sig, tgt[t].n)) { std::printf("intrin: %s REFUSE (empreinte d'entree inattendue)\n", tgt[t].name); continue; }
        if (tgt[t].rva == 0x075aa0 && ld32(tva + 8) != g_vaLgOrgX) { std::printf("intrin: %s REFUSE (operande absolu inattendu)\n", tgt[t].name); continue; }
        if (tgt[t].rva == 0x10dbe0 && ld32(tva + 6) != g_vaLutSel + 2) { std::printf("intrin: %s REFUSE (operande absolu inattendu)\n", tgt[t].name); continue; }
        if (!dyn86_intrin_add(tva, tgt[t].fn, tgt[t].retn, tgt[t].in, tgt[t].out, tgt[t].name)) continue;
        for (int i = 0; i < dyn86_intrin_n; ++i)
            if (dyn86_intrin_tbl[i].va == tva) *tgt[t].slot = &dyn86_intrin_tbl[i];
    }
    if (const char* qc = std::getenv("D2_QUADCOUNT"); qc && *qc && *qc != '0') {
        /* F1/F2/F3: the perspective tile drawers. Then the texture path the
         * 2026-09-26 console pass pointed at (511 small uploads per frame in
         * the worst window): 0x50a450 = F1's tile texture, 0x50fbd0 = F2/F3's
         * per-cell texture, 0x50f820 = tile-cache slot allocation (a miss),
         * 0x50fad0 = the download to the TMU (grTexDownloadMipMap). Every
         * external entry of each lands on its first byte (no jmp to entry,
         * verif_intrin_capture.py), which is all a counter needs. */
        static const struct { uint32_t rva; const char* name; dyn86_intrin_fn fn; } cnt[7] = {
            { 0x10acc0, "cntF1_50acc0",   d2_intrin_count<0> },
            { 0x10ca00, "cntF2_50ca00",   d2_intrin_count<1> },
            { 0x10cf70, "cntF3_50cf70",   d2_intrin_count<2> },
            { 0x10a450, "cntTexF1_50a450", d2_intrin_count<3> },
            { 0x10fbd0, "cntTexCel_50fbd0", d2_intrin_count<4> },
            { 0x10f820, "cntSlot_50f820", d2_intrin_count<5> },
            { 0x10fad0, "cntDl_50fad0",   d2_intrin_count<6> } };
        for (int k = 0; k < 7; ++k) {
            const uintptr_t cva = (uintptr_t)d2base + cnt[k].rva;
            if (!dyn86_intrin_add(cva, cnt[k].fn, 0, 0, 0, cnt[k].name)) continue;
            for (int i = 0; i < dyn86_intrin_n; ++i) if (dyn86_intrin_tbl[i].va == cva) g_cntEntry[k] = &dyn86_intrin_tbl[i];
        }
    }
    return 1;
}
