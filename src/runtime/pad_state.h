// src/runtime/pad_state.h — per-frame guest snapshot for the controller
// scheme v2 (see docs-site/controles.md for the mapping it feeds).
// WRITTEN by the unit/camera hooks in phase_hooks.cpp (game render thread),
// READ by the input tick (same thread: the tick runs from the message pump).
// Double-buffered: frame_begin() publishes the unit list collected during the
// previous frame together with the camera just read, so the reader never sees
// a half-collected list.
#pragma once
#include <cstdint>

namespace padst {

constexpr int MAX_UNITS = 128;
constexpr int MAX_LABELS = 32;       // the game's own array is exactly 32 entries

struct Unit {
    uint32_t id, type, cls, mode;
    int32_t  fx, fy;                 // 16.16 fine world position (what GetUnitX/Y return)
    uint32_t ownerType, ownerId;     // UnitAny+0x94 / +0x98 (type 1 only, else 0)
};

// One ground-item name label, COPIED from the array the game itself keeps at
// Game+0x3c54a8 (count at Game+0x3c54a0, 32 entries of 0x120 bytes, filled by
// Game+0xc0810 = .\UI\showitems.cpp, the Alt handler). Entry layout, from a
// full disassembly of 1.14d (20/09): +0x00 x1, +0x04 y1, +0x08 x2, +0x0c y2,
// +0x10 UnitAny*, +0x14 wchar_t name[0x80], +0x114/+0x118/+0x11c rect color,
// rect transparency and text color.
//
// This replaces every earlier attempt to GUESS a label's position from the
// item's own projected anchor. No formula can work: the game shifts labels
// that would overlap each other (up to 12 tries, Game+0xc0640), which is
// exactly why two screenshots of the SAME two items, taken from different
// camera positions, disagreed on the offset (console, 20/09).
//
// It is also the rect the game hit-tests the mouse against to decide which
// item the cursor hovers -- so putting the cursor inside it makes the game
// highlight the label natively AND makes a left click pick that item up.
struct Label { int32_t x1, y1, x2, y2; uint32_t unitId; };

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
    // The Level POINTER the type was read from. It moved in lockstep with the
    // value on console, which is what proved the room chain sound and sent the
    // investigation to the field's meaning instead.
    uint32_t levelPtr;
    uint32_t selValid, selId, selType;   // [0x3a6a94] / [0x3a6a78] / [0x3a6a8c]: unit the game hovers
    uint32_t uiVars[38];             // [0x3a27c0 + 4*i]
    int      nUnits; Unit units[MAX_UNITS];
    int      nLabels; Label labels[MAX_LABELS];
};

bool on();                           // D2_PAD absent or != "0" -> true (hooks armed)
void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelType,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* uiVars38);
void add_unit(const Unit& u);
// Whole-array snapshot, read in one go by the camera hook (unlike units,
// which accumulate call by call). Call it BEFORE frame_begin, which is what
// publishes it to the reader.
void set_labels(const Label* l, int n);
// Call before frame_begin, like set_labels.
void set_level_ptr(uint32_t p);
bool read(Snapshot& out);            // false until the first frame_begin

} // namespace padst
