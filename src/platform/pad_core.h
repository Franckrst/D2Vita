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

// Hostile target: right stick pushed -> nearest-ish inside a +-coneDeg cone
// (score = distance + 300*(1-cos)), <= 500 px; idle -> nearest <= 420 px.
// `current` = index of the current target in `u` (or -1); kept while it
// qualifies and its score <= 1.25*best + 20 (hysteresis). Returns -1 if none.
int  pick_hostile(const Unit* u, int n, const View& v, const Ctl& c, const Config& cfg, int current);
// L target: nearest item (type 4) <= 220 px, else nearest other interactable
// (object / town NPC) <= 160 px. Returns -1 if none.
int  pick_interact(const Unit* u, int n, const View& v, const Config& cfg);

// Learned hover height per (type, class): where the cursor must sit for the
// game to hover a unit of that class. Reset at boot (RAM only).
struct HoverTable {
    enum { N = 64 };
    uint32_t key[N]; int h[N]; int n = 0;
    int  get(uint32_t type, uint32_t cls, int def) const;
    void learn(uint32_t type, uint32_t cls, int hv);
    static int try_seq(int attempt, int def);   // 0 -> def, then 14, 44, 64, 90
};

// Left-stick "move only" click point: on a ring around the player (radius
// orbitMin..orbitMax by tilt, scaled by h/600), rotated/shrunk until it sits
// on no unit box. Returns false when the stick is inside the dead zone.
bool orbit_point(const View& v, const Ctl& c, const Config& cfg, const Unit* u, int n, int* px, int* py);
// Ground cast point: along the right stick (or the fallback direction
// fbx/fby when idle), rangeMin..rangeMax subtiles by tilt, clamped on screen.
void ground_point(const View& v, const Ctl& c, const Config& cfg, float fbx, float fby, int* px, int* py);

// Directional focus navigation among on-screen ground items (type==4 only,
// matching Alt's own item-only scope — objects/NPCs stay L-only). Returns
// the index in `u` of the nearest item to u[fromIdx] in screen direction
// `d`, or -1 if none qualifies. Non-item units in `u` are never candidates.
enum Dir { D_UP, D_DOWN, D_LEFT, D_RIGHT };
int nav_direction(const Unit* u, int n, int fromIdx, Dir d);
// Nearest on-screen item (type==4) to the player, no distance cap (unlike
// pick_interact's 220px pass — Alt already shows everything on screen).
// Returns -1 if none.
int nearest_item(const Unit* u, int n, const View& v);

// The scheme itself: one tick per controller sample (30 Hz). Emits the Win32
// actions the glue injects, in order. Modes: WORLD (in game, no panel), PANEL
// (in game, a panel open). Out of game the glue does not call tick(); it
// calls leave() once so everything held is released.
class Scheme {
public:
    explicit Scheme(const Config& c) : cfg_(c) {}
    void tick(const Ctl& c, const Ctx& x, const View& v, const Unit* units, int n, Actions& out);
    void leave(Actions& out);                      // release everything, forget the mode
    Target target() const { return tgt_; }
    bool cursorOwned() const { return lmb_ || rmb_ || interact_; }
    int  cx() const { return cx_; }
    int  cy() const { return cy_; }
    void setCursor(int x, int y) { cx_ = x; cy_ = y; }   // touch moved the cursor
    bool aimActive() const { return aimActive_; }
    int  aimX() const { return aimX_; }
    int  aimY() const { return aimY_; }
    bool casting() const { return castSlot_ >= 0; }

private:
    enum Mode { M_NONE, M_WORLD, M_PANEL };
    struct Held { int vk = 0; bool shift = false; };
    void moveTo(int x, int y, Actions& out);
    void worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, uint32_t down, uint32_t up, Actions& out);
    void panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out);
    void commonButtons(const Ctl& c, uint32_t down, uint32_t up, Actions& out);
    void releaseAll(Actions& out);
    int  findId(const Unit* u, int n, uint32_t id) const;
    void hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const;

    Config   cfg_;
    Mode     mode_ = M_NONE;
    uint32_t prev_ = 0;
    int      cx_ = 400, cy_ = 300;
    bool     lsOn_ = false, lmb_ = false, rmb_ = false;
    float    lastDx_ = 0.f, lastDy_ = 1.f;
    // cast in progress
    int      castSlot_ = -1; uint32_t castBit_ = 0;
    uint32_t castId_ = 0, castType_ = 0, castCls_ = 0;
    int      castAttempt_ = 0, castH_ = 0; bool castVerified_ = false;
    // interaction in progress
    bool     interact_ = false, interNoop_ = false;
    uint32_t interId_ = 0, interType_ = 0, interCls_ = 0;
    int      interAttempt_ = 0, interH_ = 0; bool interVerified_ = false;
    // modifiers / keys held
    bool     alt_ = false, shiftSq_ = false, esc_ = false, wkey_ = false;
    Held     dpad_[4];
    // overlay
    uint32_t tgtId_ = 0; Target tgt_;
    bool     aimActive_ = false; int aimX_ = 0, aimY_ = 0;
    HoverTable hover_;
};

} // namespace pad
