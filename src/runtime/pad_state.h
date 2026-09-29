// src/runtime/pad_state.h — per-frame guest snapshot for the aim
// assist (the `aim` action of controls.txt, see docs-site/controles.md).
// WRITTEN by the unit/camera hooks in phase_hooks.cpp (game render thread),
// READ by the input tick (same thread: the tick runs from the message pump).
// Double-buffered: frame_begin() publishes the unit list collected during the
// previous frame together with the camera just read, so the reader never sees
// a half-collected list.
#pragma once
#include <cstdint>

namespace padst {

constexpr int MAX_UNITS = 128;

struct Unit {
    uint32_t id, type, cls, mode;
    // UnitAny+0xC4, raw. Bit 1 (0x02) is UNITFLAG_TARGETABLE, which the game
    // maintains from ObjectTxt.Selectable[dwMode] and reads in its own hover
    // path -- so it answers "will the cursor be allowed to hover this?"
    // BEFORE we offer it, instead of the assist learning it by wasting a
    // press on every decorative fire. +0xC4 sits in the 0x94..0xF3 window
    // that is common to client and server units in the merged 1.14d binary,
    // unlike 0x64..0x93; the neighbouring ownerType/ownerId reads below use
    // that same safe window.
    uint32_t flags;
    int32_t  fx, fy;                 // 16.16 fine world position (what GetUnitX/Y return)
    uint32_t ownerType, ownerId;     // UnitAny+0x94 / +0x98 (type 1 only, else 0)
};

struct Snapshot {
    uint32_t frame;                  // +1 per camera hook exit (one per in-game frame)
    bool     inGame;                 // camera exit saw a non-null player
    uint32_t playerId; int32_t playerFx, playerFy;
    int32_t  viewX, viewY;           // [Game+0x3a520c] / [Game+0x3a5208]
    // Path->Room1->Room2->Level->+0x1c0. This is dwLevelTYPE, not dwLevelNo:
    // console, 21/09, an out-and-back from Lut Gholein read 12 <-> 16, which
    // are Act 2 Town and Act 2 Desert in LvlTypes.txt -- Lut Gholein's level
    // NUMBER is 40. The earlier "1 at the Rogue camp, 2 in the Blood Moor"
    // check passed by coincidence: those are Act 1 Town and Act 1 Wilderness,
    // whose type ids happen to equal their level numbers. 0 = unreadable.
    uint32_t levelType;
    uint32_t selValid, selId, selType;   // [0x3a6a94] / [0x3a6a78] / [0x3a6a8c]: unit the game hovers
    uint32_t uiVars[38];             // [0x3a27c0 + 4*i]
    int      nUnits; Unit units[MAX_UNITS];
};

bool on();                           // controls.txt binds `aim` -> true (hooks armed)
void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelType,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* uiVars38);
void add_unit(const Unit& u);
bool read(Snapshot& out);            // false until the first frame_begin

} // namespace padst
