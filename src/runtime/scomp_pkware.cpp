// src/runtime/scomp_pkware.cpp — native PKWARE DCL "explode" for the Storm
// SCompDecompress hook (see rt_boot.cpp). Wraps the vendored StormLib pklib
// decompressor (third_party/pklib, MIT, Ladislav Zezula) behind a flat-buffer
// API. This is the exact same algorithm Storm.dll runs in guest x86 — the
// dominant compression of every Diablo II MPQ block (mask 0x08) — so the
// output is bit-identical by construction.
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "pklib.h"
}

namespace {
struct Buf {
    const uint8_t* in; uint32_t in_pos, in_len;
    uint8_t* out; uint32_t out_pos, out_max; bool overflow;
    // `truncate` reproduces SCompExplode's write callback (0x41df00): it
    // writes min(size, capacity - position) and CONTINUES. The other
    // wrapper's callback (0x415210) does NOT bound anything: there, an
    // overflow would be a real buffer overrun, so we fail and fall back to
    // the guest instead of writing past the end.
    bool truncate;
};
unsigned int read_cb(char* buf, unsigned int* size, void* param) {
    Buf* b = (Buf*)param;
    uint32_t n = b->in_len - b->in_pos;
    if (n > *size) n = *size;
    std::memcpy(buf, b->in + b->in_pos, n);
    b->in_pos += n;
    return n;
}
void write_cb(char* buf, unsigned int* size, void* param) {
    Buf* b = (Buf*)param;
    if (b->out_pos + *size > b->out_max) {
        if (!b->truncate) { b->overflow = true; return; }
        uint32_t room = b->out_max - b->out_pos;      // bounded like 0x41df00
        std::memcpy(b->out + b->out_pos, buf, room);
        b->out_pos += room;
        return;
    }
    std::memcpy(b->out + b->out_pos, buf, *size);
    b->out_pos += *size;
}
} // namespace

namespace d2rt {

// ---------------------------------------------------------------------------
// THE RESIDUE: WHAT WE KNOW, AND WHAT WAS TRIED AND REMOVED.
//
// Some MPQ blocks contain a back-copy that reaches BEFORE the start of the
// block, i.e. into the decompressor's leftover scratch history. The stream
// therefore reads uninitialized memory: neither pklib nor the guest's Storm
// is "right", they just return whatever their own history happened to hold.
// A handful of bytes can end up differing depending on what the scratch
// buffer held beforehand, and those bytes then flow into game data, where
// they fail a consistency check much later (Halt 1420 on animation blocks,
// Halt 452 on a heap object's "Asyn" signature).
//
// The strategy is: don't zero the buffer, keep the residue from the previous
// call. The justification differs between the two wrappers. The guest-side
// work buffer, per disassembly:
//   0x415240      -> global ds:0x77903c, allocated ONCE, reused
//                    => history = end of the previous block, and so is ours:
//                    our residue chain follows Storm's BY CONSTRUCTION (a
//                    single chain — see the fidelity counters below);
//   SCompExplode  -> SMemAlloc/SMemFree on EVERY call
//                    => history = recycled heap block, UNKNOWABLE: no
//                    "identical by construction" argument holds here; this
//                    path is instead justified by a hardware cross-check
//                    (D2_EXPLVERIFY) comparing our native output to the
//                    guest's byte-for-byte, with zero divergence.
// Static analysis confirms these are the only two call sites into Storm's
// explode core (0x6b01b0), and both are hooked: widening the port further
// would add no coverage.
//
// TRIED AND REMOVED — the "double-residue proof": decompress twice with
// opposite residues (0x00/0xFF) and serve only outputs that agree, falling
// back to the guest otherwise. Applied to both wrappers it broke the game
// (crash entering a game). Restricted to SCompExplode, it changed nothing:
// the hardware cross-check above already showed zero divergence, so the
// double pass just paid for decompressing twice plus a very large
// thread_local object (worst emutls footprint in the codebase — on the Vita
// toolchain, --disable-tls makes every thread_local a lazy heap allocation on
// each thread's first access, mid-game). Removed from the default path.

// Fidelity counters for the 0x415240 wrapper:
//  - how many distinct THREADS go through the decompressor. This wrapper's
//    fidelity argument ("our residue history matches Storm's by
//    construction") only holds with a SINGLE chain. Storm has exactly one:
//    global buffer ds:0x77903c under critical section 0x779080 (confirmed by
//    disassembly). If this counter exceeds 1 with a thread_local buffer, the
//    match is BROKEN.
//  - the largest output actually produced. The flat wrapper gives NO output
//    capacity: the caller just assumes 256 KiB. If an output ever exceeds a
//    sector's size, that's a heap-block overflow.
static uint32_t g_flatThreads=0, g_flatMaxOut=0, g_flatCalls=0;
uint32_t scomp_flat_threads() { return g_flatThreads; }
uint32_t scomp_flat_maxout()  { return g_flatMaxOut;  }
uint32_t scomp_flat_calls()   { return g_flatCalls;   }

// Decompress a PKWARE-imploded block. Returns the decompressed size, or 0 on
// any error (caller falls back to the guest implementation).
uint32_t scomp_explode_pkware(const uint8_t* in, uint32_t in_len,
                              uint8_t* out, uint32_t out_max) {
    // 12.6 KiB thread_local DELIBERATELY: this runs on ANY guest thread through
    // the Storm hook, and two Fog IOCP jobs may decompress concurrently under
    // the native scheduler — a shared static here would be silent data
    // corruption (the sprite-crash symptom class). thread_local keeps
    // per-thread isolation for the native scheduler with byte-identical reuse
    // semantics to the hardware-validated static under coop (single host
    // thread): first access = zeroed like BSS, then residue of the previous
    // call (this buffer's initial content matters on hardware; stack garbage
    // would be a third, untested state). On the Vita toolchain (newlib) this
    // is emutls, lazily allocated only for threads that decompress — no
    // pressure on the 256 KiB host stacks.
    // NEVER apply the double pass HERE. This wrapper is the twin of
    // Game+0x415240, whose work buffer is the GLOBAL ds:0x77903c: allocated
    // once (push 0x3134 ; call 0x413020 if null) then REUSED on every call.
    // Its history is therefore the end of the previous block, and so is
    // ours — they match by construction. Overwriting our buffer would break
    // that alignment.
    //
    // This buffer MUST be the shared static, not thread_local. Disassembly of
    // the twin wrapper Game+0x415240:
    //     415246: push 0x779080 ; call ds:0x6cc218   ; EnterCriticalSection
    //     415251: mov  eax, ds:0x77903c              ; GLOBAL buffer, allocated once
    //     4152a3: call 0x6b01b0                      ; explode
    //     4152ab: push 0x779080 ; call ds:0x6cc214   ; LeaveCriticalSection
    // Storm SERIALIZES every thread on ONE buffer: its residue chain is
    // single and globally ordered. Using thread_local here would create ONE
    // CHAIN PER THREAD and break this wrapper's fidelity argument ("its
    // history is the end of the previous block, and so is ours — they match
    // by construction"), which only holds with a single chain.
    // There is no real race to guard against: shim bodies already run under
    // the GIL (`gil::Guard gg;` cpu_box86.cpp:828, wrapping trap_fn_), so
    // going thread_local buys nothing and only costs fidelity.
    // D2_SCOMP_TLSBUF=1 restores thread_local, for console A/B comparison.
    static TDcmpStruct shared;                 // single chain, like Storm
    static thread_local TDcmpStruct per_thread;
    static const bool tlsbuf = getenv("D2_SCOMP_TLSBUF") != nullptr;
    TDcmpStruct& work = tlsbuf ? per_thread : shared;
    { static thread_local bool vu=false; if(!vu){ vu=true; ++g_flatThreads; } }
    ++g_flatCalls;
    Buf b{in, 0, in_len, out, 0, out_max, false, false};
    if (explode(read_cb, write_cb, (char*)&work, &b) != CMP_NO_ERROR) return 0;
    if (b.overflow) return 0;
    if (b.out_pos > g_flatMaxOut) g_flatMaxOut = b.out_pos;
    return b.out_pos;
}

// Variant for SCompExplode (Game+0x1e000), whose guest write callback
// TRUNCATES instead of failing: it writes min(size, capacity - position) and
// lets the stream run to completion. So this returns min(total produced,
// capacity), exactly what the original writes to *pcbOutBuffer.
//
// This matters because pklib flushes its window in 0x1000-byte chunks, and
// the LAST (empty) flush can overrun by a hair even on an exact-size block:
// the guest truncates and still reports the full size, while a version that
// fails on any overflow would report 0 instead.
uint32_t scomp_explode_pkware_trunc(const uint8_t* in, uint32_t in_len,
                                    uint8_t* out, uint32_t out_max) {
    // 12.6 KiB thread_local DELIBERATELY: per-thread isolation for the native
    // scheduler (any guest thread traverses the Storm hook; two Fog IOCP jobs
    // may decompress concurrently), with byte-identical reuse semantics to the
    // hardware-validated static under coop (single host thread): first access
    // = zeroed like BSS, then residue of the previous call. See the note
    // below — this buffer's initial content matters on hardware; stack
    // garbage would be a third, untested state.
    static thread_local TDcmpStruct work;
    // Do NOT zero the buffer. The tempting assumption — SCompExplode
    // allocates its buffer via SMemAlloc, and Storm returns freshly committed
    // (zeroed) memory — holds under our shim but is FALSE on console, where a
    // recycled block holds something else. Zeroing it causes a reproducible
    // hardware crash (Halt CelDataHash:1420 loading town) even though a
    // qemu-only cross-check passes fine: this is a hardware-only failure mode.
    // D2_EXPL_ZERO=1 re-enables zeroing for further study; never on console.
    //
    // The buffer is thread_local (per-thread isolation for the native
    // scheduler). Under cooperative scheduling (single host thread), "no
    // memset" preserves EXACTLY the semantics above: first access is zero
    // (like the old static's BSS), then residue of the previous call. Under
    // native scheduling, each decompressor thread sees its own residue; its
    // first access is also zero (emutls/TLS zero-initializes the copy).
    static const bool zero = getenv("D2_EXPL_ZERO")!=nullptr;
    if(zero) std::memset(&work, 0, sizeof work);
    static const bool notrunc = getenv("D2_EXPL_NOTRUNC")!=nullptr;
    // Disassembly of SCompExplode (Game+0x1e000):
    //     41e015: push 0x3134        ; = sizeof(TDcmpStruct)
    //     41e01e: call 0x413020      ; SMemAlloc -> NEW block
    //     41e050: call 0x6b01b0      ; explode
    //     41e064: call 0x412650      ; SMemFree
    // Storm allocates and FREES its buffer on EVERY call: its history isn't
    // the end of the previous block, it's whatever a recycled heap block
    // contains. The "double-residue proof" that was meant to cover this case
    // was tried and REMOVED (see the file header): single pass, residue kept,
    // same as the flat wrapper.
    Buf b{in, 0, in_len, out, 0, out_max, false, !notrunc};
    if (explode(read_cb, write_cb, (char*)&work, &b) != CMP_NO_ERROR) return 0;
    return b.out_pos;
}

} // namespace d2rt
