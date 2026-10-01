// src/platform/pad_core.h — aim assist: pure host logic.
// No VitaSDK, no engine include: everything here is testable on the desktop
// (tests/pad/pad_core_test.cpp). It runs only when controls.txt binds the
// `aim` action to a button: vita_present.cpp then feeds it the sticks, that
// button's held state and the per-frame guest snapshot (runtime/pad_state.h),
// and applies the returned actions. All coordinates are GAME pixels
// (800x600 or 640x480), +y down. Documented in docs-site/controles.md.
#pragma once
#include <cstdint>

namespace pad {

struct Config {
    float deadzone = 0.25f;
    int   orbitMin = 40, orbitMax = 110;   // px at 600 lines (scaled by h/600)
    float coneDeg  = 35.f;                 // half-angle of the right-stick aim cone
    int   hoverH   = 28;                   // default hover height above a unit's feet, px
    // Bottom band the cursor never enters. Must stay > 49: the game drops a
    // click command outright, with no packet sent, when the cursor sits in
    // the last 49 px at the moment the command resolves (Game+0x61700).
    int   hudH     = 60;
    // How far Cross reaches for a chest, door, portal or town NPC, in WORLD
    // units. Stated explicitly because measuring in world units rather than
    // screen pixels halved what the old 220 gave vertically.
    int   reach    = 300;
    float sens     = 10.f;                 // free-cursor speed in panels
};

struct Ctl { float lx = 0, ly = 0, rx = 0, ry = 0; bool aim = false; };   // sticks in [-1,1]; aim = the bound button is held

struct View { int w = 800, h = 600; int32_t playerFx = 0, playerFy = 0, viewX = 0, viewY = 0; };

// A drawn unit as the core sees it: already projected to game pixels.
struct Unit {
    uint32_t id = 0, type = 0, cls = 0;
    int sx = 0, sy = 0;             // feet anchor on screen
    bool hostile = false;           // can be a skill target
    // OUR OWN body, left behind by a death with all our gear on it. Getting it
    // back outranks anything else Cross could be pointed at.
    bool ownCorpse = false;
    bool interact = false;          // can be an aim-button target (item, object, NPC)
    // The game's own targetable bit. It filters the PROXIMITY list only: the
    // cursor pointed straight at something outranks it, because the bit turns
    // out to disown things that really are usable -- shrines and the stash,
    // console 21/09 -- and being unable to reach those is worse than being
    // offered a torch we never asked for.
    // Defaults to true: usable unless the game says otherwise. The glue sets
    // it explicitly from the flag for every kind of unit it collects.
    bool selectable = true;
};

struct Ctx {
    bool inGame = false, panelOpen = false;
    uint32_t selValid = 0, selId = 0, selType = 0;   // unit the GAME hovers (previous frame)
};

enum ActKind { A_MOVE, A_LDOWN, A_LUP, A_CLICK };
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

// Units the game refused to hover, whatever cursor height we tried. We cannot
// tell a critter or a still-airborne vulture apart up front, but the game
// declining to select one for a while says the same thing -- and without this
// the nearest such creature captured every Cross press and blocked the real
// target behind it (console, 21/09).
struct Reject {
    enum { NID = 32, NCLS = 32 };
    // Creatures are rejected per INSTANCE: a vulture still in the air will
    // land and become a target. Scenery is rejected per CLASS: a torch class
    // is never operable, so learning it once spares every other torch on the
    // level -- which is what "X keeps targeting torches" came down to.
    uint32_t id[NID] = {}, idType[NID] = {}; int nId = 0, nextId = 0;
    uint32_t cls[NCLS] = {}, clsType[NCLS] = {}; int nCls = 0;
    bool has(const Unit& u) const;
    void add(const Unit& u);
};

// Aim direction for the cone: the player->cursor vector, normalized. Returns
// false while the cursor sits on the player, where there is no direction to
// read -- the assist then falls back to "nearest". The cursor is what the
// player aims with (right stick, or the touch screen), so the assist reads it
// rather than the stick itself: the stick is only one of the ways to move it.
bool aim_from_cursor(const View& v, int cx, int cy, float* ax, float* ay);

// Hostile target: aimed -> nearest-ish inside a +-coneDeg cone around (ax,ay)
// (score = distance + 300*(1-cos)), <= 500 px; not aimed -> nearest <= 420 px.
// `current` = index of the current target in `u` (or -1); kept while it
// qualifies and its score <= 1.25*best + 20 (hysteresis). Returns -1 if none.
int  pick_hostile(const Unit* u, int n, const View& v, float ax, float ay, bool aimed,
                  const Config& cfg, int current, const Reject* rej = nullptr);
// Nearest interactable chest / door / town NPC within cfg.reach (world units).
// Ground items are deliberately NOT candidates -- the item-assist action
// already browses and picks them up. Returns -1 if none.
int  pick_interact(const Unit* u, int n, const View& v, const Config& cfg, const Reject* rej = nullptr);
// The unit whose hit box contains (cx, cy): what the player is literally
// pointing at, which beats any cone. Interactables and hostiles only; ties go
// to the nearest anchor. -1 if the cursor is over nothing.
int  pick_at(const Unit* u, int n, int cx, int cy, const Reject* rej = nullptr);

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
// The assist itself: one tick per controller sample (30 Hz). Emits the actions
// the glue injects, in order. Modes: WORLD (in game, no panel), PANEL (in
// game, a panel open). Out of game the glue does not call tick(); it calls
// leave() once so everything held is released.
//
// What it does, and all it does: the right stick is a free cursor; the left
// stick walks by clicking on a ring around the player (never onto a unit box);
// the bound `aim` button acts on the best target -- what the cursor points at,
// else the hostile in the aim cone, else the nearest chest/door/NPC -- by
// moving the cursor onto it and holding the left button once the game reports
// the hover. Everything else (potions, skills, Alt, keys) stays with the
// ordinary controls.txt mapping.
class Assist {
public:
    explicit Assist(const Config& c) : cfg_(c) {}
    void tick(const Ctl& c, const Ctx& x, const View& v, const Unit* units, int n, Actions& out);
    void leave(Actions& out);                      // release everything, forget the mode
    Target target() const { return tgt_; }
    bool cursorOwned() const { return lmb_ || interact_; }
    int  cx() const { return cx_; }
    int  cy() const { return cy_; }
    // The cursor was moved from outside (touch screen, item assist): that is
    // the player pointing, so it becomes the spot an interaction borrows from
    // and hands back to.
    void setCursor(int x, int y) { cx_ = x; cy_ = y; userX_ = x; userY_ = y; }
    bool interacting() const { return interact_; }
    uint32_t interactId() const { return interId_; }
    // Why the left stick did or did not produce a walk this tick. Two wrong
    // diagnoses of "stuck in a melee" were reasoned out and both missed; this
    // reports the state instead of inferring it.
    struct Walk {
        bool pushed = false;     // stick beyond the dead zone
        bool gated = false;      // an interaction owned the tick
        bool clicked = false;    // a walk click was actually issued
        bool onUnit = false;     // ... and it landed inside a unit's box
        int  px = 0, py = 0, nUnits = 0;
    };
    Walk walk() const { return walk_; }
    // What the last aim press chose, and by which rule. "We are not hitting
    // the nearest mob" needs the branch, not another round of reasoning.
    struct Pick {
        uint32_t id = 0, type = 0; int dist = 0;
        int branch = -1;     // 0 corpse, 1 under cursor, 2 aim cone, 3 nearest object,
                             // 4 nearest hostile, -1 nothing
    };
    Pick lastPick() const { return pick_; }

private:
    enum Mode { M_NONE, M_WORLD, M_PANEL };
    void moveTo(int x, int y, Actions& out);
    // Float cursor delta for one stick, quadratic response, ADDED into dx/dy:
    // a mode driven by two sticks sums them and rounds once, because rounding
    // each stick on its own loses a pixel per axis per tick.
    void stickDelta(float sx, float sy, const View& v, float* dx, float* dy) const;
    // Apply an accumulated delta, bounded by the screen edges only -- NOT by
    // clamp_point, whose bottom band exists to keep ASSISTED clicks out of the
    // 49 px the game silently drops. The player still has to reach the belt
    // and the skill buttons down there.
    bool cursorStep(float dx, float dy, const View& v, int* px, int* py) const;
    void worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, bool down, Actions& out);
    void panelTick(const Ctl& c, const View& v, bool down, bool up, Actions& out);
    void releaseAll(Actions& out);
    int  findId(const Unit* u, int n, uint32_t id, uint32_t type) const;
    void hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const;

    Config   cfg_;
    Mode     mode_ = M_NONE;
    bool     prevAim_ = false;
    int      cx_ = 400, cy_ = 300;
    // Where the PLAYER left the cursor. An interaction borrows the cursor to
    // snap it onto its target and hands it back here on release, so assisted
    // aiming never costs the player the spot they were pointing at. While it
    // holds, the right stick steers this instead of cx_/cy_.
    int      userX_ = 400, userY_ = 300;
    // lmb_: the left button is down, whoever pressed it -- releaseAll lifts it
    // on every exit. lsClick_: it is down because of the LEFT STICK, the one
    // case the movement block may cancel on its own.
    bool     lsOn_ = false, lmb_ = false, lsClick_ = false;
    // Ticks the walk has been waiting for the game's hover to clear before it
    // presses. A click acts on the PREVIOUS frame's hover, so pressing in the
    // same breath as the move attacks whatever the cursor used to sit on.
    int      lsArm_ = 0;
    float    lastDx_ = 0.f, lastDy_ = 1.f;
    // interaction in progress
    bool     interact_ = false;
    uint32_t interId_ = 0, interType_ = 0, interCls_ = 0;
    // interArm_ > 0: the cursor is on the target and we are waiting for the
    // game to report the hover before pressing (see the aim block).
    int      interAttempt_ = 0, interH_ = 0, interArm_ = 0;
    bool     interPin_ = false;      // the game already hovers it: hold the cursor where it is
    int      pinX_ = 0, pinY_ = 0;
    Walk     walk_;
    Pick     pick_;
    Reject   rej_;                     // units the game would not hover
    // Passive learning: the cursor is sitting inside a unit's box and the game
    // reports no hover for it. After a few frames that is a statement, not a
    // race, and it costs the player no button press to find out.
    uint32_t coldId_ = 0, coldType_ = 0; int coldTicks_ = 0;
    bool     hudClick_ = false;        // the aim button is clicking the HUD, not the world
    // overlay
    uint32_t tgtId_ = 0; Target tgt_;
    HoverTable hover_;
};

} // namespace pad
