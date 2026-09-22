// src/runtime/screen_state.h — is D2 on its literal title screen right now?
//
// Offset found empirically (see docs/superpowers/specs/
// 2026-09-22-controls-help-title-screen-design.md, section 1, and
// tools/screen_scan_diff.py) by comparing guest memory across the title
// screen, character select (the "Select Hero Class" screen reached via
// Single Player), the in-game Options screen (pause menu -> Options), and
// in a game (Rogue Encampment) — NOT a community/RE database value.
// Re-verify with the same method if 1.14d's Game.exe build ever changes.
//
// Game+0x308d0c reads as the sentinel 0xffffffff only on the literal title
// screen; it reads the same stable value (0x00000004) on character select,
// the in-game Options screen, and in a game — confirmed on two independent
// qemu-arm runs per state (title_a/b, charselect_a/b, options_a/b, game_a/b)
// plus a third, independent re-verification run for both title and game
// (title_c, game_c).
#pragma once
#include <cstdint>
#include "runtime/cpu.h"

extern uint32_t g_d2base;
extern const bool g_114;

// True only on the literal title screen; false on character select, the
// in-game Options screen, and in a game.
inline bool d2_title_screen_active(d2rt::Cpu* cpu) {
    if (!cpu || !g_114 || !g_d2base) return false;
    uint32_t v = 0;
    cpu->read(g_d2base + 0x00308d0cu, &v, 4);   // Step 3's verified offset
    return v == 0xffffffffu;                     // Step 3's verified value
}
