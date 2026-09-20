// tools/radial_oracle.cpp — host-side oracle for the radial menu blitter.
//
// The menu is drawn by CPU into the framebuffer once per presented frame
// (d2vita_overlay), so its cost lands straight on the frame time. Any change
// to that blitter has to answer two questions, and this harness answers both
// without a console round-trip:
//
//   1. does it still draw EXACTLY the same pixels?  -> checksum per menu state
//   2. how much work did it drop?                   -> wall time for N draws
//
// The checksum is the point: a blitter "optimization" that shifts one pixel by
// one alpha step is a rendering bug that no FPS number would catch. Built
// against the SAME header the console ships (src/platform/radial_menu.h), for
// x86 and for ARM under qemu — see tools/oracle_radial.sh.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "platform/radial_menu.h"

namespace {

constexpr int W = 960, H = 544;

// A flat background would hide blending mistakes (blending against a constant
// makes several wrong formulas agree). This one varies per pixel in all three
// channels, so a wrong alpha, a swapped channel or an off-by-one source pixel
// all move the checksum.
void fill_background(uint32_t* fb) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t r = (uint8_t)(x * 7 + y * 3);
            const uint8_t g = (uint8_t)(x * 3 + y * 11);
            const uint8_t b = (uint8_t)(x ^ y);
            fb[(size_t)y * W + x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
}

uint64_t fnv1a(const void* p, size_t n) {
    const uint8_t* q = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    return h;
}

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// The 9 states the presenter can ever ask for: closed, open with the stick
// centered, and open over each of the 7 sectors.
struct Case { const char* name; radial_menu::State st; };

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? atoi(argv[1]) : 0;

    Case cases[2 + RM_N];
    int n = 0;
    cases[n].name = "ferme";          cases[n].st = radial_menu::State{0, 0, 0}; ++n;
    cases[n].name = "ouvert-centre";  cases[n].st = radial_menu::State{1, 0, 0}; ++n;
    for (int i = 0; i < RM_N; ++i) {
        static char nm[RM_N][24];
        std::snprintf(nm[i], sizeof nm[i], "ouvert-secteur-%d", i);
        cases[n].name = nm[i];
        cases[n].st = radial_menu::State{1, 1, i};
        ++n;
    }

    uint32_t* fb = (uint32_t*)std::malloc((size_t)W * H * 4);
    if (!fb) { std::fprintf(stderr, "malloc KO\n"); return 2; }

    for (int c = 0; c < n; ++c) {
        fill_background(fb);
        radial_menu::draw(cases[c].st, fb, W, H);
        std::printf("%-20s %016llx\n", cases[c].name,
                    (unsigned long long)fnv1a(fb, (size_t)W * H * 4));
    }

    if (iters > 0) {
        // Timed on the WORST state (a sector lit): that is what the player
        // actually holds on screen while aiming at a wedge.
        const radial_menu::State hot{1, 1, 0};
        fill_background(fb);
        radial_menu::draw(hot, fb, W, H);        // warm the caches, don't time it
        const double t0 = now_s();
        for (int i = 0; i < iters; ++i) radial_menu::draw(hot, fb, W, H);
        const double dt = now_s() - t0;
        std::printf("temps: %d dessins en %.3f s -> %.3f ms/dessin\n",
                    iters, dt, dt * 1000.0 / iters);
    }

    std::free(fb);
    return 0;
}
