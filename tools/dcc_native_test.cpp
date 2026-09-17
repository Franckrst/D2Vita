// tools/dcc_native_test.cpp — host-side test for the native DCC decoder
// (src/runtime/dcc_native.cpp) against the Python reference:
// dcc_decode.py <f> --all --out ref.bin.
//   g++ -O2 -std=c++17 -Isrc tools/dcc_native_test.cpp src/runtime/dcc_native.cpp -o build-host/dcc_native_test
//   build-host/dcc_native_test <file.dcc> [ref.bin]
#include "runtime/dcc_native.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <dcc> [ref.bin]\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) return 2;
    std::vector<uint8_t> d; { uint8_t b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n); }
    std::fclose(f);
    if (d.size() < 15 || d[0] != 0x74) { std::printf("pas un DCC\n"); return 1; }
    uint32_t ndirs = d[2], nframes; std::memcpy(&nframes, &d[3], 4);
    uint32_t off0; std::memcpy(&off0, &d[15], 4);
    d2rt::DccResult r;
    int rc = d2rt::dcc_decode_native(&d[off0], (uint32_t)(d.size() - off0), nframes, ndirs, r);
    std::printf("%s : dirs=%u frames=%u -> rc=%d (%s) chaine=%zu o, cellules=%u\n", argv[1], ndirs, nframes, rc,
                d2rt::dcc_error_name(rc), r.chain.size(), r.cells);
    if (rc) return 1;
    if (argc > 2) {
        FILE* g = std::fopen(argv[2], "rb"); if (!g) return 2;
        std::vector<uint8_t> ref; { uint8_t b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, g)) > 0) ref.insert(ref.end(), b, b + n); }
        std::fclose(g);
        size_t n = std::min(ref.size(), r.chain.size()); size_t bad = 0, first = (size_t)-1;
        for (size_t i = 0; i < n; i++) if (ref[i] != r.chain[i]) { if (first == (size_t)-1) first = i; ++bad; }
        std::printf("  ref=%zu o natif=%zu o : %zu octets differents%s\n", ref.size(), r.chain.size(), bad + (ref.size() > n ? ref.size() - n : r.chain.size() - n),
                    (bad == 0 && ref.size() == r.chain.size()) ? "  IDENTIQUE" : "");
        if (first != (size_t)-1) std::printf("  1er ecart +0x%zx ref=%02x natif=%02x\n", first, ref[first], r.chain[first]);
        return (bad == 0 && ref.size() == r.chain.size()) ? 0 : 1;
    }
    return 0;
}
