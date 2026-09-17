// src/crashreport/build_id.cpp — see build_id.h. D2CR_BUILD_ID_STR and
// D2CR_CHANNEL_STR are injected by tools/build_rt_boot_vpk.sh; the fallbacks
// below only matter for an ad hoc compile of this one file (e.g. by hand,
// outside the VPK build) and are deliberately obvious placeholders — a
// report carrying "0.0.0+000000000000-dirty" says plainly that the build
// script's injection did not run, rather than silently claiming a real build.
#include "crashreport/build_id.h"

#ifndef D2CR_BUILD_ID_STR
#define D2CR_BUILD_ID_STR "0.0.0+000000000000-dirty"
#endif
#ifndef D2CR_CHANNEL_STR
#define D2CR_CHANNEL_STR "dev"
#endif

namespace d2cr {

const char* build_id() { return D2CR_BUILD_ID_STR; }
const char* channel() { return D2CR_CHANNEL_STR; }

}  // namespace d2cr
