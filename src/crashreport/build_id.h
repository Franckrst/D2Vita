// src/crashreport/build_id.h — the console's own build_id (spec §4.10,
// claim.v1#BuildId).
//
// build_id.cpp is a single translation unit whose two string constants come
// entirely from -D defines that tools/build_rt_boot_vpk.sh recomputes on
// every invocation (VERSION file + `git rev-parse --short=12 HEAD` +
// `-dirty` when the tree is not clean — the same recipe
// tools/crash/d2vcrash/cli.py:local_build_id() uses, so a console report and
// `crash builds register` name the same build the same way). Because the
// value lives in the COMMAND LINE and not in a header, the incremental build
// cache's command fingerprint (tools/build_rt_boot_vpk.sh: need_rebuild())
// naturally forces a recompile of this one unit whenever the commit or the
// dirty flag changes — no separate "always recompiled" mechanism is needed.
#pragma once
#include <cstddef>
#include <string>

namespace d2cr {

// "<VERSION>+<12 lowercase hex>[-dirty]" (contract/schemas/claim.v1
// #BuildId). Empty only if the build script failed to inject a value AND no
// fallback below applied — build_id.cpp always returns a matching string.
const char* build_id();

// "release" for a build made with D2VPK_CHANNEL=release (the maintainer's
// own publish step), "dev" otherwise (spec §4.10: unset -> dev, never
// release by accident — dev/test builds get the relaxed rate-limit tier of
// contract §5.5, so defaulting release would risk an ordinary local/CI build
// quietly eating the shared production quota).
const char* channel();

// Portable (no Vita dependency, so a host test can check what the build
// script computed): "^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}(-dirty)?$",
// contract/schemas/claim.v1#BuildId and tools/crash/d2vcrash/cli.py:19's
// BUILD_ID — kept byte-for-byte the same pattern on purpose.
inline bool valid_build_id(const std::string& s) {
    size_t i = 0;
    auto digits = [&](size_t min1) {
        size_t n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++n; }
        return n >= min1;
    };
    if (!digits(1) || i >= s.size() || s[i++] != '.') return false;
    if (!digits(1) || i >= s.size() || s[i++] != '.') return false;
    if (!digits(1) || i >= s.size() || s[i++] != '+') return false;
    size_t hex_start = i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) ++i;
    if (i - hex_start != 12) return false;
    if (i == s.size()) return true;
    return s.compare(i, std::string::npos, "-dirty") == 0;
}

}  // namespace d2cr
