// src/platform/pad_core.cpp — see pad_core.h.
#include "platform/pad_core.h"
#include <cmath>
#include <cstring>

namespace pad {

static const float kPi = 3.14159265358979f;

// D2's projection is x*16 / y*8 per subtile, so one pixel DOWN is worth two
// pixels ACROSS in world terms. Measuring targets in raw screen pixels made
// anything north or south of the player look half as far as it is -- console,
// 21/09: Lysander picked over Fara although Fara was nearer and faced. Every
// distance and direction the assist reasons about goes through this first.
static inline void to_iso(float dx, float dy, float* ix, float* iy) { *ix = dx; *iy = 2.f * dy; }
static inline float iso_len(float dx, float dy) { return std::sqrt(dx * dx + 4.f * dy * dy); }

// You cannot click what is not drawn. Without this the assist reached for
// units well outside the viewport -- console log, 21/09, targets at x=-86 and
// x=864 on an 800-wide screen -- and they beat what was in front of the
// player.
static inline bool on_screen(const View& v, int sx, int sy) {
    return sx >= 0 && sx <= v.w - 1 && sy >= 0 && sy <= v.h - 1;
}

void world_to_screen(const View& v, int32_t fx, int32_t fy, int* sx, int* sy) {
    const int64_t dx = (int64_t)fx - (int64_t)fy;
    const int64_t sm = (int64_t)fx + (int64_t)fy;
    *sx = (int)((dx * 16) / 65536) - v.viewX;
    *sy = (int)((sm * 8) / 65536) - v.viewY;
}

void screen_dir_to_world(float dx, float dy, float* wx, float* wy) {
    *wx = dx / 32.f + dy / 16.f;
    *wy = dy / 16.f - dx / 32.f;
}

void clamp_point(const View& v, const Config& cfg, int* px, int* py) {
    if (*px < 4) *px = 4;
    if (*px > v.w - 5) *px = v.w - 5;
    const int ymax = v.h - cfg.hudH - 1;
    if (*py < 4) *py = 4;
    if (*py > ymax) *py = ymax;
}

bool in_unit_box(const Unit& u, int x, int y) {
    if (u.type == 0 || u.type == 1)
        return x >= u.sx - 16 && x <= u.sx + 16 && y >= u.sy - 48 && y <= u.sy + 4;
    if (u.type == 2 || u.type == 4)
        return x >= u.sx - 20 && x <= u.sx + 20 && y >= u.sy - 16 && y <= u.sy + 8;
    return false;
}

// 24 px: below that the cursor is close enough to the player's own feet that
// the direction it would give is mostly rounding noise.
static const float kAimMinPx = 24.f;

bool aim_from_cursor(const View& v, int cx, int cy, float* ax, float* ay) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    float dx, dy; to_iso((float)(cx - psx), (float)(cy - psy), &dx, &dy);
    const float m = std::sqrt(dx * dx + dy * dy);
    if (m < kAimMinPx) return false;
    *ax = dx / m; *ay = dy / m;
    return true;
}

int pick_hostile(const Unit* u, int n, const View& v, float ax, float ay, bool aimed,
                 const Config& cfg, int current, const Reject* rej) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    if (aimed) {
        const float am = std::sqrt(ax * ax + ay * ay);
        if (am > 1e-6f) { ax /= am; ay /= am; } else aimed = false;
    }
    const float cosMin = std::cos(cfg.coneDeg * kPi / 180.f);
    const float maxD = aimed ? 500.f : 420.f;
    auto score = [&](int i, float* s) -> bool {
        const Unit& t = u[i];
        if (!t.hostile) return false;
        if (!on_screen(v, t.sx, t.sy)) return false;
        if (rej && rej->has(t)) return false;                  // the game will not select it
        float dx, dy; to_iso((float)(t.sx - psx), (float)(t.sy - psy), &dx, &dy);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > maxD) return false;
        if (aimed) {
            const float cs = d > 1.f ? (dx * ax + dy * ay) / d : 1.f;
            if (cs < cosMin) return false;
            *s = d + 300.f * (1.f - cs);
        } else *s = d;
        return true;
    };
    int best = -1; float bs = 0.f;
    for (int i = 0; i < n; ++i) { float s; if (score(i, &s) && (best < 0 || s < bs)) { best = i; bs = s; } }
    if (current >= 0 && current < n) {
        float s;
        if (score(current, &s) && (best < 0 || s <= bs * 1.25f + 20.f)) return current;
    }
    return best;
}

int pick_interact(const Unit* u, int n, const View& v, const Config& cfg, const Reject* rej) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        const Unit& t = u[i];
        if (!t.interact || t.type == 4) continue;      // ground items belong to the item assist
        if (!t.selectable) continue;                  // proximity offers only what the game owns
        if (!on_screen(v, t.sx, t.sy)) continue;
        if (rej && rej->has(t)) continue;
        const float d = iso_len((float)(t.sx - psx), (float)(t.sy - psy));
        if (d > (float)cfg.reach) continue;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

bool Reject::has(const Unit& u) const {
    for (int k = 0; k < nCls; ++k) if (cls[k] == u.cls && clsType[k] == u.type) return true;
    for (int k = 0; k < nId; ++k) if (id[k] == u.id && idType[k] == u.type) return true;
    return false;
}
void Reject::add(const Unit& u) {
    if (has(u)) return;
    if (u.type == 2) {                       // scenery: the whole class is out
        if (nCls < NCLS) { cls[nCls] = u.cls; clsType[nCls] = u.type; ++nCls; }
        return;
    }
    id[nextId] = u.id; idType[nextId] = u.type;
    nextId = (nextId + 1) % NID;
    if (nId < NID) ++nId;
}

int pick_at(const Unit* u, int n, int cx, int cy, const Reject* rej) {
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        const Unit& t = u[i];
        if (!t.interact && !t.hostile) continue;
        if (rej && rej->has(t)) continue;
        if (!in_unit_box(t, cx, cy)) continue;
        const float d = iso_len((float)(t.sx - cx), (float)(t.sy - cy));
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

int HoverTable::get(uint32_t type, uint32_t cls, int def) const {
    const uint32_t k = (type << 16) | (cls & 0xffffu);
    for (int i = 0; i < n; ++i) if (key[i] == k) return h[i];
    return def;
}

void HoverTable::learn(uint32_t type, uint32_t cls, int hv) {
    const uint32_t k = (type << 16) | (cls & 0xffffu);
    for (int i = 0; i < n; ++i) if (key[i] == k) { h[i] = hv; return; }
    if (n < N) { key[n] = k; h[n] = hv; ++n; }
}

int HoverTable::try_seq(int attempt, int def) {
    static const int seq[4] = { 14, 44, 64, 90 };
    if (attempt <= 0 || attempt > 4) return def;
    return seq[attempt - 1];
}

bool orbit_point(const View& v, const Ctl& c, const Config& cfg, const Unit* u, int n, int* px, int* py) {
    float lx = c.lx, ly = c.ly;
    const float m = std::sqrt(lx * lx + ly * ly);
    if (m <= cfg.deadzone) return false;
    lx /= m; ly /= m;
    float t = (m - cfg.deadzone) / (1.f - cfg.deadzone);
    if (t > 1.f) t = 1.f;
    const float sc = (float)v.h / 600.f;
    const float r = (cfg.orbitMin + t * (cfg.orbitMax - cfg.orbitMin)) * sc;
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    static const float dAng[7] = { 0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f };
    // Radii to try, in order of preference: the intended one, a little
    // shorter, then progressively FURTHER OUT. Shrinking alone was not enough
    // in a melee -- every ring inside the crowd is occupied, and the old
    // last-resort point then sat on a monster, which D2 reads as "attack"
    // rather than "move": console, 21/09, "in a group of monsters, impossible
    // to get out with the stick, my barbarian keeps launching attacks". A
    // point PAST the crowd is an ordinary walk order, and it is the only thing
    // that keeps the stick a way out.
    static const float dR[8] = { 0.f, -20.f, -40.f, 30.f, 60.f, 90.f, 130.f, 180.f };
    for (int j = 0; j < 8; ++j) {
        const float rr = r + dR[j] * sc;
        if (rr < 16.f * sc) continue;
        for (int k = 0; k < 7; ++k) {
            const float a = dAng[k] * kPi / 180.f, ca = std::cos(a), sa = std::sin(a);
            const float dx = lx * ca - ly * sa, dy = lx * sa + ly * ca;
            int x = psx + (int)std::lround(dx * rr), y = psy + (int)std::lround(dy * rr);
            clamp_point(v, cfg, &x, &y);
            bool hit = false;
            for (int i = 0; i < n; ++i) if (in_unit_box(u[i], x, y)) { hit = true; break; }
            if (!hit) { *px = x; *py = y; return true; }
        }
    }
    // Occupied all the way out: aim as far along the stick as the screen
    // allows. Still better than the near point, which is certainly on the
    // monster standing in our face.
    const float rFar = r + 180.f * sc;
    *px = psx + (int)std::lround(lx * rFar); *py = psy + (int)std::lround(ly * rFar);
    clamp_point(v, cfg, px, py);
    return true;
}

// ---------------------------------------------------------------------------
// Assist
// ---------------------------------------------------------------------------
// Four candidate heights, one tried every 3 ticks: two full sweeps, for a
// creature whose sprite has to be hunted for. An OBJECT is static and sits
// where it is drawn, so one sweep is already conclusive -- and every tick
// spent on a decorative fire is a tick the player is not attacking.
static const int kHoverGiveUp = 24, kHoverGiveUpObj = 9;
// Frames of cursor-inside-the-box with no hover before scenery is written off.
static const int kColdTicks = 5;

void Assist::stickDelta(float sx, float sy, const View& v, float* dx, float* dy) const {
    if (std::fabs(sx) <= cfg_.deadzone && std::fabs(sy) <= cfg_.deadzone) return;
    const float sc = cfg_.sens * ((float)v.w / 800.f);
    *dx += sx * std::fabs(sx) * sc;
    *dy += sy * std::fabs(sy) * sc;
}

bool Assist::cursorStep(float dx, float dy, const View& v, int* px, int* py) const {
    int nx = *px + (int)dx, ny = *py + (int)dy;
    if (nx < 0) nx = 0;
    if (nx > v.w - 1) nx = v.w - 1;
    if (ny < 0) ny = 0;
    if (ny > v.h - 1) ny = v.h - 1;
    if (nx == *px && ny == *py) return false;
    *px = nx; *py = ny;
    return true;
}

void Assist::moveTo(int x, int y, Actions& out) {
    if (x != cx_ || y != cy_) { cx_ = x; cy_ = y; out.push(A_MOVE, x, y); }
}

// D2 unit ids are unique per TYPE, not across types: a monster and an item can
// both be id 50. Matching on the id alone made the marker jump onto a monster
// that happened to share the id.
int Assist::findId(const Unit* u, int n, uint32_t id, uint32_t type) const {
    if (!id) return -1;
    for (int i = 0; i < n; ++i) if (u[i].id == id && u[i].type == type) return i;
    return -1;
}

void Assist::hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const {
    *px = t.sx; *py = t.sy - h;
    clamp_point(v, cfg_, px, py);
}

void Assist::releaseAll(Actions& out) {
    if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }
    interact_ = false; interId_ = 0; interArm_ = 0;
    lsOn_ = false; lsArm_ = 0; tgt_ = Target{}; tgtId_ = 0; hudClick_ = false;
}

void Assist::leave(Actions& out) { releaseAll(out); walk_ = Walk{}; mode_ = M_NONE; prevAim_ = false; }

void Assist::tick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, Actions& out) {
    const Mode m = !x.inGame ? M_NONE : (x.panelOpen ? M_PANEL : M_WORLD);
    if (m != mode_) { releaseAll(out); mode_ = m; }
    const bool down = c.aim && !prevAim_, up = !c.aim && prevAim_;
    if (mode_ == M_WORLD)      worldTick(c, x, v, u, n, down, out);
    else if (mode_ == M_PANEL) panelTick(c, v, down, up, out);
    prevAim_ = c.aim;
}

void Assist::worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, bool down, Actions& out) {
    // ---- right stick: the cursor the player aims with ----
    // Runs before the target pick so a press this tick already sees the spot
    // the player is pointing at. An interaction borrows the cursor, and the
    // stick then steers the point it will be handed back to.
    {
        float dx = 0.f, dy = 0.f;
        stickDelta(c.rx, c.ry, v, &dx, &dy);
        if (interact_) cursorStep(dx, dy, v, &userX_, &userY_);
        else {
            int px = cx_, py = cy_;
            if (cursorStep(dx, dy, v, &px, &py)) moveTo(px, py, out);
        }
    }

    // ---- hostile target of the moment (overlay + next press) ----
    // Read the cone off the cursor the PLAYER owns: while an interaction
    // holds, cx_ sits on its target, so using it would re-target along a
    // direction the player may have left several frames ago.
    float ax = 0.f, ay = 0.f;
    const bool aimed = interact_ ? aim_from_cursor(v, userX_, userY_, &ax, &ay)
                                 : aim_from_cursor(v, cx_, cy_, &ax, &ay);
    const int ti = pick_hostile(u, n, v, ax, ay, aimed, cfg_, findId(u, n, tgtId_, 1), &rej_);
    tgtId_ = ti >= 0 ? u[ti].id : 0;

    // ---- learn scenery without spending a press ----
    // The cursor is sitting inside a type-2 box and the game reports no hover
    // on it: after a few frames that is a statement, not a race. Restricted to
    // scenery on purpose -- a creature's box is generous and its sprite may
    // simply not fill it, which is a different problem with its own handling
    // when the button is actually held on it.
    if (!interact_) {
        const int ci = pick_at(u, n, cx_, cy_, &rej_);
        if (ci < 0 || u[ci].type != 2) { coldId_ = 0; coldTicks_ = 0; }
        else {
            const Unit& cu = u[ci];
            const bool hov = x.selValid && x.selId == cu.id && x.selType == cu.type;
            if (hov) { coldId_ = 0; coldTicks_ = 0; }
            else if (coldId_ == cu.id && coldType_ == cu.type) {
                if (++coldTicks_ >= kColdTicks) {
                    // A selectable object the game is merely slow to hover (12 fps)
                    // must never be written off for good: only the untargetable
                    // ones are learned here.
                    if (!cu.selectable) rej_.add(cu);
                    coldId_ = 0; coldTicks_ = 0;
                }
            } else { coldId_ = cu.id; coldType_ = cu.type; coldTicks_ = 1; }
        }
    }

    // overlay target: the hostile the next press would go for
    tgt_ = Target{};
    if (ti >= 0) {
        tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy;
        tgt_.verified = x.selValid && x.selId == u[ti].id && x.selType == 1;
    }

    // ---- aim on the HUD: a plain click ----
    // The stats and skills buttons, the menu bar and the belt are not panels,
    // so this is world mode and the button meant "act on the world" over
    // them. Down in that band there is no world to act on, and the only way
    // the cursor got there is the player driving it -- assisted points are
    // clamped out of it by design.
    const bool onHud = cy_ > v.h - cfg_.hudH;
    if (down && onHud && !interact_) {
        out.push(A_LDOWN, cx_, cy_); lmb_ = true; hudClick_ = true;
    }
    if (hudClick_ && !c.aim) {
        if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
        hudClick_ = false;
    }

    // ---- the aim button: act on the best target ----
    // Order: what the player is POINTING at wins, because pointing at a
    // monster can only mean "hit that one". With the cursor idle we fall back
    // to whatever is within arm's reach (chest, door, portal, town NPC) and
    // only then to the nearest enemy -- otherwise a barrel could never be
    // opened with anything alive on screen.
    if (down && !interact_ && !hudClick_) {
        int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
        auto dist = [&](int k) { return iso_len((float)(u[k].sx - psx), (float)(u[k].sy - psy)); };
        // Our own body, with all our gear on it, outranks anything else the
        // button could mean -- but only WITHIN REACH. Taken as an absolute it
        // hijacked the button from across the screen while a monster was in
        // our face (console, 21/09).
        int ii = -1, branch = -1;
        for (int k = 0; k < n; ++k)
            if (u[k].ownCorpse && dist(k) <= (float)cfg_.reach) { ii = k; branch = 0; break; }
        // The cursor sitting ON something is the least ambiguous statement of
        // intent there is, so it comes before the cone (console, 21/09: the
        // stash was unreachable however carefully it was pointed at).
        if (ii < 0) { ii = pick_at(u, n, cx_, cy_, &rej_); if (ii >= 0) branch = 1; }
        // The game itself says the cursor is on a static object: that beats
        // any box of ours (the feet-anchored box misses the visible sprite of
        // a stash or a waypoint). The cursor is then held where it is.
        bool pin = false;
        if (ii < 0 && x.selValid && (x.selType == 2 || x.selType == 4)) {
            const int si = findId(u, n, x.selId, (int)x.selType);
            if (si >= 0 && u[si].selectable && !rej_.has(u[si])) { ii = si; branch = 5; pin = true; }
        }
        if (ii < 0 && aimed) { ii = ti; if (ii >= 0) branch = 2; }   // ti already skips the rejects
        if (ii < 0) {
            // Not aiming at anything: take whichever is actually NEAREST,
            // rather than letting a door win on precedence alone.
            const int io = pick_interact(u, n, v, cfg_, &rej_);
            if (io >= 0 && ti >= 0)      ii = dist(io) <= dist(ti) ? io : ti;
            else if (io >= 0)            ii = io;
            else                         ii = ti;
            if (ii >= 0) branch = (ii == io) ? 3 : 4;
        }
        pick_ = Pick{};
        pick_.branch = branch;
        if (ii >= 0) { pick_.id = u[ii].id; pick_.type = u[ii].type; pick_.dist = (int)dist(ii); }
        if (ii >= 0) {
            interact_ = true; interId_ = u[ii].id; interType_ = u[ii].type; interCls_ = u[ii].cls;
            interAttempt_ = 0; interArm_ = 1;
            interPin_ = pin; pinX_ = cx_; pinY_ = cy_;
            userX_ = cx_; userY_ = cy_;          // an interaction borrows the cursor
            interH_ = interType_ == 4 ? 6 : hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH);
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }
            // Move ONLY, then press once the game reports the hover: a click
            // acts on the hover computed during the PREVIOUS rendered frame,
            // so pressing in this same breath makes the game act on nothing,
            // which it sends as "walk to that spot" instead of interacting.
            int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
            if (interPin_) { px = pinX_; py = pinY_; }
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);
        }
    } else if (interact_) {
        if (!c.aim) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            interact_ = false; interId_ = 0; interArm_ = 0;
            moveTo(userX_, userY_, out);                // borrowed, now handed back
        } else {
            // EITHER stick re-picks the target. The left one steers it
            // directly -- movement is gated while the button is held, so it
            // has nothing else to do -- and the right one does it through the
            // aim cone.
            float sax = ax, say = ay; bool sAimed = aimed;
            if (std::sqrt(c.lx * c.lx + c.ly * c.ly) > cfg_.deadzone) {
                to_iso(c.lx, c.ly, &sax, &say); sAimed = true;
            }
            if (sAimed && interType_ == 1) {
                const int ni = pick_hostile(u, n, v, sax, say, sAimed, cfg_, -1, &rej_);
                if (ni >= 0 && !(u[ni].id == interId_ && u[ni].type == interType_)) {
                    if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
                    interId_ = u[ni].id; interType_ = u[ni].type; interCls_ = u[ni].cls;
                    interAttempt_ = 0; interArm_ = 1; interPin_ = false;
                    interH_ = hover_.get(interType_, interCls_, cfg_.hoverH);
                }
            }
            const int ii = findId(u, n, interId_, interType_);
            if (ii < 0) interArm_ = 0;                        // gone: pressing now would be a walk order
            else {
                int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
                if (interPin_) { px = pinX_; py = pinY_; }
                moveTo(px, py, out);
                const bool hovered = x.selValid && x.selId == interId_ && x.selType == interType_;
                // The button is down ONLY while the game confirms it hovers
                // this unit. A click with no hover is not a weaker click, it
                // is a different order -- "walk to that point". Lifting on
                // loss also re-arms the game's own per-frame hover scan.
                if (hovered && !lmb_)      { out.push(A_LDOWN, px, py); lmb_ = true; }
                else if (!hovered && lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
                if (hovered) {
                    if (interType_ != 4) hover_.learn(interType_, interCls_, interH_);
                    interArm_ = 0;                                // settled on a height that works
                } else if (interArm_ > 0) {
                    ++interArm_;
                    // Two full sweeps of the height table with no hover at
                    // all: this unit is not selectable. Remember it and let
                    // go, so the next press reaches what is behind it.
                    if (interArm_ > (interType_ == 2 && !u[ii].selectable ? kHoverGiveUpObj : kHoverGiveUp)) {
                        if (interType_ != 2 || !u[ii].selectable) rej_.add(u[ii]);   // a selectable object is only ever slow
                        if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
                        interact_ = false; interId_ = 0; interArm_ = 0;
                        interAttempt_ = 0;
                        moveTo(userX_, userY_, out);   // give the aim back, not parked on it
                    }
                    // Sprites are hit-tested against the cursor itself, so no
                    // hover means the height is wrong. Keep cycling, one
                    // candidate every 3 ticks since each needs a rendered
                    // frame before the game can answer.
                    else if (interType_ != 4 && !interPin_ && interArm_ % 3 == 0) {
                        interAttempt_ = interAttempt_ >= 4 ? 1 : interAttempt_ + 1;
                        interH_ = HoverTable::try_seq(interAttempt_, hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH));
                    }
                }
            }
        }
    }

    // ---- left stick: move-only orbit, hard stop on release ----
    const float lm = std::sqrt(c.lx * c.lx + c.ly * c.ly);
    const bool on = lsOn_ ? (lm > cfg_.deadzone) : (lm > cfg_.deadzone + 0.05f);
    if (on) { lastDx_ = c.lx / lm; lastDy_ = c.ly / lm; }
    walk_ = Walk{};
    walk_.pushed = on; walk_.nUnits = n;
    walk_.gated = interact_;
    if (!interact_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
            walk_.clicked = true; walk_.px = px; walk_.py = py;
            for (int i = 0; i < n; ++i) if (in_unit_box(u[i], px, py)) { walk_.onUnit = true; break; }
            moveTo(px, py, out);
            // Do NOT press in the same breath as the move: the game resolves a
            // click against the hover of the PREVIOUS rendered frame, so
            // pressing now acts on whatever the cursor sat on before -- a
            // monster, which D2 reads as ATTACK rather than walk.
            if (!lmb_) {
                if (!x.selValid || lsArm_ >= 4) {
                    out.push(A_LDOWN, px, py); lmb_ = true; lsClick_ = true; lsArm_ = 0;
                } else ++lsArm_;                 // and walk anyway rather than stand there
            }
        } else if (lsOn_) {
            if (lmb_ && lsClick_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            lsClick_ = false; lsArm_ = 0;
            int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
            psy += 6; clamp_point(v, cfg_, &psx, &psy);
            cx_ = psx; cy_ = psy; out.push(A_CLICK, psx, psy);            // click at the feet = stop
            // And that is where the cursor stays: restoring the aim here
            // teleports it out from under the player at the very moment they
            // stop moving (console, 21/09).
            userX_ = psx; userY_ = psy;
        }
    } else if (lsClick_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }   // only ever lift the
                                                                     // stick's OWN click here: lmb_ also covers
                                                                     // the interact press, which must not be cancelled
    lsOn_ = on;
}

void Assist::panelTick(const Ctl& c, const View& v, bool down, bool up, Actions& out) {
    // free cursor: both sticks drive it here, same step as the world mode
    {
        float dx = 0.f, dy = 0.f;
        stickDelta(c.lx, c.ly, v, &dx, &dy);
        stickDelta(c.rx, c.ry, v, &dx, &dy);
        int px = cx_, py = cy_;
        if (cursorStep(dx, dy, v, &px, &py)) moveTo(px, py, out);
    }
    // The aim button is the left click in EVERY panel, the skill tree
    // included: you still have to click an icon to spend a point on it.
    if (down) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
    if (up && lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    tgt_ = Target{};
    walk_ = Walk{};
}

} // namespace pad
