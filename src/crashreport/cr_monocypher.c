// src/crashreport/cr_monocypher.c — the reporter's Monocypher unit.
//
// Monocypher is vendored unmodified in third_party/monocypher/ (see its
// README.md and SHA256SUMS). It is compiled through this one-line wrapper
// instead of directly, because the .text budget of spec section 4.9 depends
// on BLAKE2_NO_UNROLLING: without it the unrolled BLAKE2b compression
// function alone adds 17.7 KiB at -Os for the Vita, and the reporter would be
// over the 150 KiB it is allowed. The flag changes no output -- the tests that
// reproduce the contract vectors link the sources built with it -- and the
// reporter hashes 104 bytes twice per sealed artifact, so the speed the
// unrolling buys is worth nothing here.
//
// Setting it in the source rather than in a build script is the point: a build
// cannot forget it, and what leg 5 and leg 7 of
// tools/tests/run_crashreport_tests.sh measure is what an eboot ships.
//
// Build THIS file, never third_party/monocypher/monocypher.c, or the two
// units define the same symbols twice: tools/crashreport_srcs.sh lists what
// the reporter is made of.
#ifndef BLAKE2_NO_UNROLLING
#define BLAKE2_NO_UNROLLING 1
#endif

#include "../../third_party/monocypher/monocypher.c"
