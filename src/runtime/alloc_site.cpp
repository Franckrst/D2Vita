// src/runtime/alloc_site.cpp — see alloc_site.h. Pure: no CPU, no I/O.
#include "runtime/alloc_site.h"

#include <cstdio>

namespace d2diag {

std::string format_guest_chain(const uint32_t* stack, int n,
                               uint32_t base, uint32_t lo, uint32_t hi, int max) {
    std::string s;
    if (!stack || n <= 0 || max <= 0) return s;
    char b[32];
    int shown = 0;
    for (int i = 0; i < n && shown < max; ++i) {
        uint32_t v = stack[i];
        if (v >= lo && v < hi) {
            std::snprintf(b, sizeof b, "%sGame+0x%x", shown ? " " : "",
                          (unsigned)(v - base));
            s += b;
            ++shown;
        }
    }
    return s;
}

}  // namespace d2diag
