// src/runtime/exe_identity.h -- which Diablo II Game.exe is this?
//
// The port patches Game.exe at fixed 1.14d offsets (the native hooks, the
// alternates, the intrinsics). Any other build -- 1.14a/b/c are monoliths
// too and pass the "no split DLL imports" guard -- receives those patches on
// unrelated bytes and fails in whatever way it happens to fail (seen: D2's
// own "Error 1: Unsupported graphics mode" at frame 0 on a 1.14b Game.exe,
// with nothing in the log naming the real cause). So the loader identifies
// the executable from the two things the bytes alone give, the file size and
// the PE link timestamp, and refuses anything that is not the official 1.14d.
//
// Pure C++17, no VitaSDK, no I/O: host-tested by tools/tests/exe_identity_test.cpp
// (run tools/oracle_exe_identity.sh).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace d2exe {

struct Identity {
    bool        pe        = false;    // "MZ" + "PE\0\0" found where e_lfanew points
    uint32_t    size      = 0;        // file size in bytes
    uint32_t    timestamp = 0;        // COFF TimeDateStamp (seconds since 1970, UTC)
    const char* build     = nullptr;  // "1.14d", "1.14b", "1.13c", ... ; nullptr = unknown build
    bool        supported = false;    // exactly the official 1.14d monolith
};

// Reads only the headers; never touches bytes beyond `n`.
Identity identify(const uint8_t* bytes, size_t n);

// "YYYY-MM-DD" (UTC) for a link timestamp.
std::string date_utc(uint32_t unix_seconds);

// The boot_progress line (French, no accents, "install:" family), e.g.
//   install: Game.exe taille=3618792 octets timestamp=0x574ddfbc (2016-05-31) = 1.14d OK
std::string progress_line(const Identity& id);

// The "found" line of the wrong-version screen (English, the screen's language), e.g.
//   Game.exe = Diablo II 1.14b (3590120 bytes, built 2016-03-31), not 1.14d
std::string screen_line(const Identity& id);

} // namespace d2exe
