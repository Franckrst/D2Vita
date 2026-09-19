// src/runtime/pad_state.h — per-frame guest snapshot for the controller
// scheme v2 (docs/superpowers/specs/2026-09-19-manette-ciblage-design.md).
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
    int32_t  fx, fy;                 // 16.16 fine world position (what GetUnitX/Y return)
    uint32_t ownerType, ownerId;     // UnitAny+0x94 / +0x98 (type 1 only, else 0)
    uint32_t monFlags;               // MonsterData+0x16 byte (type 1 only, else 0)
};

struct Snapshot {
    uint32_t frame;                  // +1 per camera hook exit (one per in-game frame)
    bool     inGame;                 // camera exit saw a non-null player
    uint32_t playerId; int32_t playerFx, playerFy;
    int32_t  viewX, viewY;           // [Game+0x3a520c] / [Game+0x3a5208]
    uint32_t levelNo;                // Path->Room1->Room2->Level->dwLevelNo, 0 if unreadable
    uint32_t selValid, selId, selType;   // [0x3a6a94] / [0x3a6a78] / [0x3a6a8c]: unit the game hovers
    uint32_t uiVars[38];             // [0x3a27c0 + 4*i]
    int      nUnits; Unit units[MAX_UNITS];
};

bool on();                           // D2_PAD absent or != "0" -> true (hooks armed)
void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelNo,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* uiVars38);
void add_unit(const Unit& u);
bool read(Snapshot& out);            // false until the first frame_begin

} // namespace padst
