// src/crashreport/cr_crashtxt_parse.h — facts from the game's own Crash.txt.
//
// Crash.txt is written by the Fog error manager of Game.exe 1.14d when it
// halts (Inspector report, CRLF). Format sources are listed in
// cr_crashtxt_parse.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_types.h"

namespace d2cr {

// Fog error-reporting code in Game.exe 1.14d, as RVAs: the common reporter
// (VA 0x408900) and its five thunks (VA 0x408a30, 0x408a60 = Halt,
// 0x408a90, 0x408ac0, 0x408af0; each calls 0x40d8d0 then 0x408900). Return
// addresses in [lo, hi) at the top of a Crash.txt stack are the reporter
// itself, not the failing code.
constexpr uint32_t kFogReporterRvaLo = 0x8900;
constexpr uint32_t kFogReporterRvaHi = 0x8b20;

struct HaltFacts {
  bool is_halt = false;            // a summary line or <Inspector.LineNumber> was found
  std::string error_type;          // "Halt", "Assertion Failure", ... ("" if only the line number)
  int code = 0;                    // line number of "failed at ...(N)"
  std::string location;            // "Codec.cpp:1377" when a file name is present, else ""
  std::vector<Addr> frames;        // <= 16 Game.exe frames of the halting thread, reporter frames removed
  uint32_t game_base = 0;          // base used for the frames (argument, else module list, else 0)
  int reporter_frames_removed = 0;
};

HaltFacts parse_crash_txt(const std::string& text, uint32_t game_base);

}  // namespace d2cr
