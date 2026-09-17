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
    return 1;
}
