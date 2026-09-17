// src/crashreport/cr_addr.cpp — module-relative naming of guest addresses.
#include "crashreport/cr_addr.h"

namespace d2cr {

Addr guest_addr(uint32_t va, uint32_t game_base, const std::vector<GuestModule>& modules) {
    Addr a;
    a.module = kAbsModuleToken;
    a.offset = va;
    for (const GuestModule& m : modules) {
        if (m.size == 0 || va - m.base >= m.size) continue;   // unsigned: va < base wraps
        if ((game_base != 0 && m.base == game_base) || m.size == kGame114dImageSize) {
            a.module = kGameModuleToken;
            a.offset = va - m.base;
        }
        return a;   // an image the log does not name stays ABS
    }
    if (game_base != 0 && va - game_base < kGame114dImageSize) {
        a.module = kGameModuleToken;
        a.offset = va - game_base;
    }
    return a;
}

}  // namespace d2cr
