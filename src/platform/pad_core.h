// src/platform/pad_core.h — controller scheme v2 ("aim"): pure host logic.
// No VitaSDK, no engine include: everything here is testable on the desktop
// (tests/pad/pad_core_test.cpp). vita_present.cpp feeds it the controller
// state and the per-frame guest snapshot (runtime/pad_state.h) and emits the
// returned actions through d2vita_inject. All coordinates are GAME pixels
// (800x600 or 640x480), +y down. Design: docs/superpowers/specs/
// 2026-09-19-manette-ciblage-design.md
#pragma once
#include <cstdint>

namespace pad {

// Vita button bits (SceCtrlData.buttons), duplicated so the core stays free
// of VitaSDK headers. Values match vita_present.cpp's B_* constants.
enum : uint32_t {
    B_SELECT = 0x000001, B_START = 0x000008, B_UP = 0x000010, B_RIGHT = 0x000020,
    B_DOWN = 0x000040, B_LEFT = 0x000080, B_L = 0x000100, B_R = 0x000200,
    B_TRI = 0x001000, B_CIR = 0x002000, B_CROSS = 0x004000, B_SQR = 0x008000
};

struct Config {
    float deadzone = 0.25f;
    int   orbitMin = 40, orbitMax = 110;   // px at 600 lines (scaled by h/600)
    int   rangeMin = 6,  rangeMax = 20;    // subtiles: ground-cast distance vs stick tilt
    float coneDeg  = 35.f;                 // half-angle of the right-stick aim cone
    int   hoverH   = 28;                   // default hover height above a unit's feet, px
    int   hudH     = 60;                   // bottom band the cursor never enters, px
    bool  aim      = true;                 // automatic hostile target selection
    float sens     = 10.f;                 // free-cursor speed in panels
};

struct Ctl { float lx = 0, ly = 0, rx = 0, ry = 0; uint32_t buttons = 0; };   // sticks in [-1,1]

struct View { int w = 800, h = 600; int32_t playerFx = 0, playerFy = 0, viewX = 0, viewY = 0; };

// A drawn unit as the core sees it: already projected to game pixels.
struct Unit {
    uint32_t id = 0, type = 0, cls = 0, mode = 0;
    int sx = 0, sy = 0;             // feet anchor on screen
    bool hostile = false;           // can be a skill target
    bool interact = false;          // can be an L target (item, object, NPC)
};

struct Ctx {
    bool inGame = false, panelOpen = false, skillTree = false;
    uint32_t selValid = 0, selId = 0, selType = 0;   // unit the GAME hovers (previous frame)
    uint32_t playerId = 0;
};

enum ActKind { A_MOVE, A_LDOWN, A_LUP, A_RDOWN, A_RUP, A_CLICK, A_KEYDOWN, A_KEYUP, A_KEY };
struct Action { ActKind k; int a, b; };
struct Actions {
    enum { MAX = 24 };
    Action v[MAX]; int n = 0;
    void push(ActKind k, int a = 0, int b = 0) { if (n < MAX) v[n++] = Action{k, a, b}; }
};

struct Target { bool has = false, verified = false; uint32_t id = 0; int sx = 0, sy = 0; };

// ---- pure helpers -----------------------------------------------------------
// sx = (fx - fy)*16/65536 - viewX ; sy = (fx + fy)*8/65536 - viewY  (fx/fy 16.16 fine)
void world_to_screen(const View& v, int32_t fx, int32_t fy, int* sx, int* sy);
// Inverse of the delta part (direction only, not normalized).
void screen_dir_to_world(float dx, float dy, float* wx, float* wy);
void clamp_point(const View& v, const Config& cfg, int* px, int* py);
bool in_unit_box(const Unit& u, int x, int y);

} // namespace pad
