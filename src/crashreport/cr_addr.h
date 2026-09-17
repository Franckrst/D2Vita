// src/crashreport/cr_addr.h — module-relative naming of guest (x86) addresses.
#pragma once
#include <cstdint>
#include <vector>

#include "crashreport/cr_types.h"

namespace d2cr {

// SizeOfImage from the PE optional header of Game.exe 1.14d (.text 0x1000,
// .rdata 0x2cc000, .data 0x305000, .rsrc 0x595000, .reloc 0x597000, image
// end 0x5ba000). Every player runs this same binary (spec §4.4), so its RVAs
// are comparable across consoles and a guest module of exactly this size is
// Game.exe.
constexpr uint32_t kGame114dImageSize = 0x5ba000;

struct GuestModule { uint32_t base = 0, size = 0; };

// Module tokens of contract claim.v1 (D2Vita-website
// contract/signature-rules.v1.md §7, "Module names"): one spelling per
// module, so that one bug never gets several signatures.
constexpr const char* kGameModuleToken = "Game";
constexpr const char* kAbsModuleToken = "ABS";

// "Game+<rva>" inside Game.exe (loaded at game_base, or a listed module of
// kGame114dImageSize bytes); "ABS+<va>", the address itself, anywhere else.
// The contract names another guest image by its lowercase file name, but no
// D2Vita log names them: the NATIVE FAULT block lists modules by base and
// size only (sched_native.cpp:612-615), and a base inside the token would
// give one bug a signature per load address. A listed module that is not
// Game.exe is therefore ABS as well.
Addr guest_addr(uint32_t va, uint32_t game_base, const std::vector<GuestModule>& modules);

}  // namespace d2cr
