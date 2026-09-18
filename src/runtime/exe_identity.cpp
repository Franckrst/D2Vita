// src/runtime/exe_identity.cpp -- see exe_identity.h.
#include "runtime/exe_identity.h"

#include <cstdio>
#include <cstring>

namespace d2exe {
namespace {

// The builds we can name. Size + link timestamp together: an in-place patch
// keeps both (and stays accepted, as a real PC install would run it), a
// relinked, repacked or appended-to executable changes at least one.
struct Known { uint32_t size, stamp; const char* build; bool supported; };
const Known kKnown[] = {
    { 3618792, 0x574ddfbc, "1.14d", true  },   // 2016-05-31 -- the one monolith the port's hooks are laid out for
    { 3590120, 0x56fc78a8, "1.14b", false },   // 2016-03-31 -- a monolith too: same import surface, different code offsets
    { 61440,   0x4b95ca4b, "1.13c", false },   // 2010-03-09 -- the split-install launcher (Storm/Fog/D2Win/... next to it)
};

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

} // namespace

Identity identify(const uint8_t* b, size_t n) {
    Identity id;
    id.size = (uint32_t)n;
    if (!b || n < 0x40 || b[0] != 'M' || b[1] != 'Z') return id;
    const uint32_t pe = rd32(b + 0x3c);                    // e_lfanew
    if (pe > n || n - pe < 24) return id;                  // "PE\0\0" + the 20-byte COFF header
    if (std::memcmp(b + pe, "PE\0\0", 4) != 0) return id;
    id.pe = true;
    id.timestamp = rd32(b + pe + 8);                       // COFF TimeDateStamp
    for (const Known& k : kKnown)
        if (k.size == n && k.stamp == id.timestamp) { id.build = k.build; id.supported = k.supported; break; }
    return id;
}

// Proleptic Gregorian civil date from days since 1970-01-01 (H. Hinnant's
// algorithm): no <ctime>, so no locale, no time zone, same answer on host and
// console.
std::string date_utc(uint32_t t) {
    const int64_t z   = (int64_t)(t / 86400) + 719468;
    const int64_t era = z / 146097;                        // z >= 0 here
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp  = (5 * doy + 2) / 153;
    const uint32_t d   = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m   = mp < 10 ? mp + 3 : mp - 9;
    const int64_t  y   = (int64_t)yoe + era * 400 + (m <= 2 ? 1 : 0);
    char s[32]; std::snprintf(s, sizeof s, "%04lld-%02u-%02u", (long long)y, (unsigned)m, (unsigned)d);
    return s;
}

std::string progress_line(const Identity& id) {
    char s[224];
    if (!id.pe) {
        std::snprintf(s, sizeof s, "install: Game.exe taille=%u octets : pas un executable Windows (PE)"
                      " -> NON SUPPORTEE, il faut le Game.exe 1.14d officiel", (unsigned)id.size);
        return s;
    }
    const std::string date = date_utc(id.timestamp);
    if (id.supported)
        std::snprintf(s, sizeof s, "install: Game.exe taille=%u octets timestamp=0x%08x (%s) = %s OK",
                      (unsigned)id.size, (unsigned)id.timestamp, date.c_str(), id.build);
    else
        std::snprintf(s, sizeof s, "install: Game.exe taille=%u octets timestamp=0x%08x (%s) = %s"
                      " -> NON SUPPORTEE, il faut le Game.exe 1.14d officiel",
                      (unsigned)id.size, (unsigned)id.timestamp, date.c_str(),
                      id.build ? id.build : "version inconnue");
    return s;
}

std::string screen_line(const Identity& id) {
    char s[160];
    if (!id.pe) {
        std::snprintf(s, sizeof s, "Game.exe is not a Windows executable (%u bytes)", (unsigned)id.size);
        return s;
    }
    const std::string date = date_utc(id.timestamp);
    if (id.supported)
        std::snprintf(s, sizeof s, "Game.exe = Diablo II %s (%u bytes, built %s)", id.build, (unsigned)id.size, date.c_str());
    else if (id.build)
        std::snprintf(s, sizeof s, "Game.exe = Diablo II %s (%u bytes, built %s), not 1.14d", id.build, (unsigned)id.size, date.c_str());
    else
        std::snprintf(s, sizeof s, "Game.exe = unknown build (%u bytes, built %s), not 1.14d", (unsigned)id.size, date.c_str());
    return s;
}

} // namespace d2exe
