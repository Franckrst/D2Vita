// src/runtime/scomp_audio.cpp — Storm's adaptive Huffman and ADPCM, NATIVE.
//
// THIS IS NOT AN OPTIMIZATION, IT'S A WORKAROUND. D2's x86 PKWARE decompressor
// produced CORRUPTED data through the dynarec on REAL ARM hardware while qemu
// stayed clean (Halt CelDataHash:1420 / Gfx.cpp:1632 — see rt_boot.cpp around
// the NATIVESCOMP hook). Huffman and ADPCM are the SAME Storm code family and
// have NEVER run at volume: enabling sound on console before this port means
// sending ~1.3 GiB of PCM through translated x86 immediately next to that bug
// family. The expected symptom would be CRACKLING, not a crash, making it
// tempting to blame the mixer instead — hence a one-pass discrimination test:
// NATIVEHUFF=0/NATIVEADPCM=0 hand the guest back the work.
//
// Sources ALREADY vendored (third_party/StormLib, MIT, Ladislav Zezula): the
// same algorithm as the guest's Storm, so output is identical by construction.
// Both StormLib units are self-contained (assert + string.h).
#include <cstdint>
#include <cstring>
#include <new>

#include "../../third_party/StormLib/src/huffman/huff.h"
#include "../../third_party/StormLib/src/adpcm/adpcm.h"

namespace d2rt {

// The Huffman tree weighs ~15 KiB: NEVER on the caller's stack (the trap runs
// on a scheduler thread's host stack, whose size isn't ours to set) and NEVER
// thread_local (this toolchain's GCC --disable-tls makes emutls a real cost).
// One heap allocation per call: ~21 allocations/s for a music stream,
// negligible.
uint32_t audio_huff_decompress(const uint8_t* in, uint32_t inLen,
                               uint8_t* out, uint32_t outCap) {
    if (!in || !out || !inLen || !outCap) return 0;
    THuffmannTree* ht = new (std::nothrow) THuffmannTree(false);
    if (!ht) return 0;
    TInputStream is((void*)in, (size_t)inLen);
    const unsigned int n = ht->Decompress(out, outCap, &is);
    delete ht;
    return (n > outCap) ? 0u : (uint32_t)n;
}

uint32_t audio_adpcm_decompress(const uint8_t* in, uint32_t inLen,
                                uint8_t* out, uint32_t outCap, int channels) {
    if (!in || !out || !inLen || !outCap) return 0;
    const int n = DecompressADPCM(out, (int)outCap, (void*)in, (int)inLen, channels);
    return (n <= 0 || (uint32_t)n > outCap) ? 0u : (uint32_t)n;
}

} // namespace d2rt

// Counters published to the watchdog (0 huffman, 1 adpcm mono, 2 adpcm
// stereo, 3 faithful fallbacks). Defined HERE rather than in rt_boot.cpp so
// the watchdog's "son:" line and the hook read the SAME slot.
extern "C" unsigned long long d2rt_audio_codec_n[4] = {0,0,0,0};
