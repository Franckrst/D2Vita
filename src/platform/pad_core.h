// src/platform/pad_core.h — controller scheme v2 ("aim"): pure host logic.
// No VitaSDK, no engine include: everything here is testable on the desktop
// (tests/pad/pad_core_test.cpp). vita_present.cpp feeds it the controller
// state and the per-frame guest snapshot (runtime/pad_state.h) and emits the
// returned actions through d2vita_inject. All coordinates are GAME pixels
// (800x600 or 640x480), +y down. The mapping this drives is documented
// in docs-site/controles.md.
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

// What a skill slot aims at. Most skills want a live enemy, but a ground
// spell (teleport, meteor, blizzard) must land where the player points --
// snapping teleport onto a monster puts you ON it -- and the necromancer's
// bread and butter (corpse explosion, revive, raise skeleton) wants a DEAD
// one, which the hostile filter excludes by construction.
enum TargetKind : uint8_t { T_HOSTILE = 0, T_GROUND = 1, T_CORPSE = 2 };

struct Config {
    float deadzone = 0.25f;
    int   orbitMin = 40, orbitMax = 110;   // px at 600 lines (scaled by h/600)
    int   rangeMin = 6,  rangeMax = 20;    // subtiles: ground-cast distance vs stick tilt
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
    bool  aim      = true;                 // automatic hostile target selection
    float sens     = 10.f;                 // free-cursor speed in panels
    // Per slot (0..6 = F1..F7), from controls.txt `slot1=ground` and friends.
    // Zero-initialised, and T_HOSTILE is 0: a slot nobody configured behaves
    // exactly as before.
    uint8_t slotKind[7] = {};
};

struct Ctl { float lx = 0, ly = 0, rx = 0, ry = 0; uint32_t buttons = 0; };   // sticks in [-1,1]

struct View { int w = 800, h = 600; int32_t playerFx = 0, playerFy = 0, viewX = 0, viewY = 0; };

// A drawn unit as the core sees it: already projected to game pixels.
struct Unit {
    uint32_t id = 0, type = 0, cls = 0;
    int sx = 0, sy = 0;             // feet anchor on screen
    bool hostile = false;           // can be a skill target
    bool corpse  = false;           // a DEAD monster: what the corpse skills want
    // OUR OWN body, left behind by a death with all our gear on it. Getting it
    // back outranks anything else Cross could be pointed at.
    bool ownCorpse = false;
    bool interact = false;          // can be an L target (item, object, NPC)
    // This unit's Alt name label, when the game drew one for it this frame:
    // lx/ly = the CENTER of the label's rectangle, lw/lh its size. Copied
    // from the game's OWN label array and keyed by unit id, so it is exact
    // and never guessed (see padst::Label). Ground items only. The center
    // matters twice over: the D-pad navigates by what the player actually
    // SEES (the name, which the game may have shifted to keep it from
    // overlapping another label), and parking the cursor there makes the
    // game hover that item natively — which is what makes the pickup click
    // land. Defaults to false/0: any caller that never sets this (including
    // every existing PC test) keeps using sx/sy untouched.
    bool hasLabel = false; int lx = 0, ly = 0, lw = 0, lh = 0;
};

struct Ctx {
    bool inGame = false, panelOpen = false, skillTree = false;
    uint32_t selValid = 0, selId = 0, selType = 0;   // unit the GAME hovers (previous frame)
};

enum ActKind { A_MOVE, A_LDOWN, A_LUP, A_RDOWN, A_RUP, A_CLICK, A_KEYDOWN, A_KEYUP, A_KEY };
struct Action { ActKind k; int a, b; };
struct Actions {
    enum { MAX = 24 };
    Action v[MAX]; int n = 0;
    void push(ActKind k, int a = 0, int b = 0) { if (n < MAX) v[n++] = Action{k, a, b}; }
};

// w/h: the label rectangle's size, loot cursor only (0 = draw the old
// fixed-size marker — the hostile-target diamond never sets these).
struct Target { bool has = false, verified = false; uint32_t id = 0; int sx = 0, sy = 0, w = 0, h = 0; };

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
    enum { NID = 8, NCLS = 32 };
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
                  const Config& cfg, int current, TargetKind kind = T_HOSTILE,
                  const Reject* rej = nullptr);
// L target: nearest interactable chest / door / town NPC <= 220 px. Ground
// items are deliberately NOT candidates -- Alt + Cross already browses and
// picks them up, and having both made L a second, blurrier way to do it.
// Returns -1 if none.
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
    Target lootCursor() const { return lootTgt_; }
    bool cursorOwned() const { return lmb_ || rmb_ || interact_; }
    int  cx() const { return cx_; }
    int  cy() const { return cy_; }
    // The touch screen moved the cursor: that is the player pointing, so it
    // becomes the spot a cast borrows from and hands back to.
    void setCursor(int x, int y) { cx_ = x; cy_ = y; userX_ = x; userY_ = y; }
    bool casting() const { return castSlot_ >= 0; }
    bool interacting() const { return interact_; }
    // Why the left stick did or did not produce a walk this tick. Two wrong
    // diagnoses of "stuck in a melee" were reasoned out and both missed; this
    // reports the state instead of inferring it.
    struct Walk {
        bool pushed = false;     // stick beyond the dead zone
        bool gated = false;      // a cast / interaction / Alt owned the tick
        bool clicked = false;    // a walk click was actually issued
        bool onUnit = false;     // ... and it landed inside a unit's box
        int  px = 0, py = 0, nUnits = 0;
    };
    Walk walk() const { return walk_; }
    // What the last Cross press chose, and by which rule. "We are not hitting
    // the nearest mob" needs the branch, not another round of reasoning.
    struct Pick {
        uint32_t id = 0, type = 0; int dist = 0;
        int branch = -1;     // 0 corpse, 1 under cursor, 2 aim cone, 3 nearest object,
                             // 4 nearest hostile, -1 nothing
    };
    Pick lastPick() const { return pick_; }

private:
    enum Mode { M_NONE, M_WORLD, M_PANEL };
    struct Held { int vk = 0; bool shift = false; };
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
    void worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, uint32_t down, uint32_t up, Actions& out);
    void panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out);
    void commonButtons(const Ctl& c, uint32_t down, uint32_t up, Actions& out);
    void releaseAll(Actions& out);
    // Shift has several owners at once (L standing still, Square in a panel,
    // R + belt sending a potion to the mercenary). Pressing and lifting the
    // key per owner made one release cancel another's, so ownership is a mask
    // and the key only moves on the 0 <-> non-0 edges.
    void shiftOwn(uint32_t who, bool on, Actions& out);
    int  findId(const Unit* u, int n, uint32_t id, uint32_t type) const;
    void hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const;

    Config   cfg_;
    Mode     mode_ = M_NONE;
    uint32_t prev_ = 0;
    int      cx_ = 400, cy_ = 300;
    // Where the PLAYER left the cursor. A cast borrows the cursor to snap it
    // onto its target and hands it back here on release, so assisted aiming
    // never costs the player the spot they were pointing at. Saved on press;
    // while the cast holds, the right stick steers this instead of cx_/cy_.
    int      userX_ = 400, userY_ = 300;
    // lmb_: the left button is down, whoever pressed it -- releaseAll lifts it
    // on every exit. lsClick_: it is down because of the LEFT STICK, the one
    // case the movement block may cancel on its own.
    bool     lsOn_ = false, lmb_ = false, lsClick_ = false, rmb_ = false;
    // Ticks the walk has been waiting for the game's hover to clear before it
    // presses. A click acts on the PREVIOUS frame's hover, so pressing in the
    // same breath as the move attacks whatever the cursor used to sit on.
    int      lsArm_ = 0;
    float    lastDx_ = 0.f, lastDy_ = 1.f;
    // cast in progress
    int      castSlot_ = -1; uint32_t castBit_ = 0;
    uint32_t castId_ = 0, castType_ = 0, castCls_ = 0;
    int      castAttempt_ = 0, castH_ = 0; bool castVerified_ = false;
    TargetKind castKind_ = T_HOSTILE;      // what the slot being held aims at
    // interaction in progress
    bool     interact_ = false;
    uint32_t interId_ = 0, interType_ = 0, interCls_ = 0;
    // interArm_ > 0: the cursor is on the target and we are waiting for the
    // game to report the hover before pressing (see the L block).
    int      interAttempt_ = 0, interH_ = 0, interArm_ = 0;
    Walk     walk_;
    Pick     pick_;
    Reject   rej_;                     // units the game would not hover
    // Passive learning: the cursor is sitting inside a unit's box and the game
    // reports no hover for it. After a few frames that is a statement, not a
    // race, and it costs the player no button press to find out.
    uint32_t coldId_ = 0, coldType_ = 0; int coldTicks_ = 0;
    bool     rmbL_ = false;            // L + D-pad left holds the right button
    // modifiers / keys held
    bool     alt_ = false, esc_ = false, wkey_ = false;
    uint32_t shiftOwners_ = 0;
    // L: held = stand still, and a short press that modified nothing toggles
    // walk/run. lTicks_ counts ticks since the press (30 Hz), lTap_ drops as
    // soon as any other button joins in.
    int      lTicks_ = 0; bool lTap_ = false;
    Held     dpad_[4];
    // overlay
    uint32_t tgtId_ = 0; Target tgt_;
    // ground-item browsing (Alt held: D-pad moves the cursor, Croix confirms)
    // lootArm_ > 0: the cursor has been moved onto the label and we are waiting
    // for the GAME to register that it hovers the item before pressing the
    // button. Clicking in the same breath makes the game act on whatever it
    // hovered last (usually nothing), which it reads as "walk to that spot" --
    // console, 20/09: the label turned blue and the character walked off.
    // The press itself sets lmb_, like every other A_LDOWN here, so releaseAll
    // lifts it on any exit (panel opening, leaving the game).
    uint32_t lootCursorId_ = 0; bool lootConfirm_ = false; Target lootTgt_;
    int      lootArm_ = 0;
    HoverTable hover_;
};

} // namespace pad
