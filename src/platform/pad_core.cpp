// src/platform/pad_core.cpp — see pad_core.h.
#include "platform/pad_core.h"
#include <cmath>
#include <cstring>

namespace pad {

static const float kPi = 3.14159265358979f;

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
    const float dx = (float)(cx - psx), dy = (float)(cy - psy);
    const float m = std::sqrt(dx * dx + dy * dy);
    if (m < kAimMinPx) return false;
    *ax = dx / m; *ay = dy / m;
    return true;
}

int pick_hostile(const Unit* u, int n, const View& v, float ax, float ay, bool aimed,
                 const Config& cfg, int current, TargetKind kind) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    if (aimed) {
        const float am = std::sqrt(ax * ax + ay * ay);
        if (am > 1e-6f) { ax /= am; ay /= am; } else aimed = false;
    }
    const float cosMin = std::cos(cfg.coneDeg * kPi / 180.f);
    const float maxD = aimed ? 500.f : 420.f;
    auto score = [&](int i, float* s) -> bool {
        const Unit& t = u[i];
        if (!(kind == T_CORPSE ? t.corpse : t.hostile)) return false;
        const float dx = (float)(t.sx - psx), dy = (float)(t.sy - psy);
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

int pick_interact(const Unit* u, int n, const View& v, const Config& /*cfg*/) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        const Unit& t = u[i];
        if (!t.interact || t.type == 4) continue;      // ground items belong to Alt + Cross
        const float dx = (float)(t.sx - psx), dy = (float)(t.sy - psy);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 220.f) continue;
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
    for (int shrink = 0; shrink < 3; ++shrink) {
        const float rr = r - shrink * 20.f * sc;
        if (rr < 16.f * sc) break;
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
    *px = psx + (int)std::lround(lx * r); *py = psy + (int)std::lround(ly * r);
    clamp_point(v, cfg, px, py);
    return true;
}

void ground_point(const View& v, const Ctl& c, const Config& cfg, float fbx, float fby, int* px, int* py) {
    float dx = c.rx, dy = c.ry;
    const float m = std::sqrt(dx * dx + dy * dy);
    float t;
    if (m > cfg.deadzone) {
        dx /= m; dy /= m;
        t = (m - cfg.deadzone) / (1.f - cfg.deadzone);
        if (t > 1.f) t = 1.f;
    } else {
        const float fm = std::sqrt(fbx * fbx + fby * fby);
        if (fm < 1e-3f) { dx = 0.f; dy = 1.f; } else { dx = fbx / fm; dy = fby / fm; }
        t = 0.f;
    }
    float wx, wy; screen_dir_to_world(dx, dy, &wx, &wy);
    const float wm = std::sqrt(wx * wx + wy * wy);
    wx /= wm; wy /= wm;
    const float dist = cfg.rangeMin + t * (cfg.rangeMax - cfg.rangeMin);      // subtiles
    const int32_t fx = v.playerFx + (int32_t)(wx * dist * 65536.f);
    const int32_t fy = v.playerFy + (int32_t)(wy * dist * 65536.f);
    world_to_screen(v, fx, fy, px, py);
    clamp_point(v, cfg, px, py);
}

// Screen position navigation/selection should actually use: the Alt name
// label when the game drew one this frame (so items sharing a ground tile,
// stacked as separate label lines, are distinguishable and reachable), the
// feet anchor otherwise.
static inline void selPos(const Unit& u, int* x, int* y) {
    if (u.hasLabel) { *x = u.lx; *y = u.ly; } else { *x = u.sx; *y = u.sy; }
}

int nav_direction(const Unit* u, int n, int fromIdx, Dir d) {
    if (!u || fromIdx < 0 || fromIdx >= n) return -1;
    const Unit& from = u[fromIdx];
    int fx, fy; selPos(from, &fx, &fy);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (i == fromIdx || u[i].type != 4) continue;
        int ix, iy; selPos(u[i], &ix, &iy);
        const float dx = (float)(ix - fx), dy = (float)(iy - fy);
        bool ok = false;
        switch (d) {
            case D_RIGHT: ok = dx > 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case D_LEFT:  ok = dx < 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case D_DOWN:  ok = dy > 0.f && std::fabs(dy) >= std::fabs(dx); break;
            case D_UP:    ok = dy < 0.f && std::fabs(dy) >= std::fabs(dx); break;
        }
        if (!ok) continue;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd || (dist == bd && u[i].id < u[best].id)) { best = i; bd = dist; }
    }
    return best;
}

int nearest_item(const Unit* u, int n, const View& v) {
    if (!u) return -1;
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (u[i].type != 4) continue;
        const float dx = (float)(u[i].sx - psx), dy = (float)(u[i].sy - psy);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd) { best = i; bd = dist; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Scheme
// ---------------------------------------------------------------------------
static const uint32_t kFaceBits[4] = { B_CROSS, B_CIR, B_SQR, B_TRI };
// Face button -> skill slot, the one rule everywhere: Circle/Square/Triangle
// are slots 1-3, R + any face is 4-7, and Cross alone is not a skill at all.
// -1 = no slot. The skill tree binds with this very mapping, so the gesture
// that assigns a skill is the gesture that casts it.
static inline int face_slot(int i, bool layer) { return layer ? i + 3 : i - 1; }

// Shift owners. SH_DPAD is the first of four consecutive bits, one per
// direction, so a merc potion on Up and another on Left cannot cancel out.
enum : uint32_t { SH_L = 1u, SH_SQR = 2u, SH_DPAD = 4u };
// A press shorter than this, with nothing else pressed, is a tap (30 Hz).
static const int kTapTicks = 10;
static const uint32_t kDpadBits[4] = { B_UP, B_LEFT, B_DOWN, B_RIGHT };   // belt 1..4, same order as the legacy table

void Scheme::stickDelta(float sx, float sy, const View& v, float* dx, float* dy) const {
    if (std::fabs(sx) <= cfg_.deadzone && std::fabs(sy) <= cfg_.deadzone) return;
    const float sc = cfg_.sens * ((float)v.w / 800.f);
    *dx += sx * std::fabs(sx) * sc;
    *dy += sy * std::fabs(sy) * sc;
}

bool Scheme::cursorStep(float dx, float dy, const View& v, int* px, int* py) const {
    int nx = *px + (int)dx, ny = *py + (int)dy;
    if (nx < 0) nx = 0;
    if (nx > v.w - 1) nx = v.w - 1;
    if (ny < 0) ny = 0;
    if (ny > v.h - 1) ny = v.h - 1;
    if (nx == *px && ny == *py) return false;
    *px = nx; *py = ny;
    return true;
}

void Scheme::shiftOwn(uint32_t who, bool on, Actions& out) {
    const uint32_t before = shiftOwners_;
    if (on) shiftOwners_ |= who; else shiftOwners_ &= ~who;
    if (!before && shiftOwners_)      out.push(A_KEYDOWN, 0x10);
    else if (before && !shiftOwners_) out.push(A_KEYUP, 0x10);
}

void Scheme::moveTo(int x, int y, Actions& out) {
    if (x != cx_ || y != cy_) { cx_ = x; cy_ = y; out.push(A_MOVE, x, y); }
}

// D2 unit ids are unique per TYPE, not across types: a monster and an item can
// both be id 50. Matching on the id alone made the loot marker jump onto a
// monster, and Croix then clicked it -- an attack instead of a pick-up.
int Scheme::findId(const Unit* u, int n, uint32_t id, uint32_t type) const {
    if (!id) return -1;
    for (int i = 0; i < n; ++i) if (u[i].id == id && u[i].type == type) return i;
    return -1;
}

void Scheme::hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const {
    // A ground item whose name label is on screen: aim at the CENTER of that
    // label. The game hit-tests the mouse against exactly that rectangle to
    // pick the hovered unit (.\UI\showitems.cpp), so this both makes it
    // highlight the label in its own style and makes the click land on the
    // right item -- no height to guess, and no HoverTable attempt sequence.
    if (t.hasLabel) { *px = t.lx; *py = t.ly; clamp_point(v, cfg_, px, py); return; }
    *px = t.sx; *py = t.sy - h;
    clamp_point(v, cfg_, px, py);
}

void Scheme::releaseAll(Actions& out) {
    if (rmb_)    { out.push(A_RUP, cx_, cy_); rmb_ = false; }
    if (lmb_)    { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }
    if (alt_)    { out.push(A_KEYUP, 0x12); alt_ = false; }
    if (shiftOwners_) { out.push(A_KEYUP, 0x10); shiftOwners_ = 0; }
    if (esc_)    { out.push(A_KEYUP, 0x1B); esc_ = false; }
    if (wkey_)   { out.push(A_KEYUP, 0x57); wkey_ = false; }
    for (Held& h : dpad_) if (h.vk) { out.push(A_KEYUP, h.vk); h = Held{}; }
    castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
    interact_ = false; interId_ = 0; interArm_ = 0;
    lootConfirm_ = false; lootCursorId_ = 0; lootTgt_ = Target{}; lootArm_ = 0;
    lsOn_ = false; tgt_ = Target{}; tgtId_ = 0;
}

void Scheme::leave(Actions& out) { releaseAll(out); mode_ = M_NONE; prev_ = 0; }

void Scheme::tick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, Actions& out) {
    const Mode m = !x.inGame ? M_NONE : (x.panelOpen ? M_PANEL : M_WORLD);
    if (m != mode_) { releaseAll(out); mode_ = m; }
    const uint32_t down = c.buttons & ~prev_, up = prev_ & ~c.buttons;
    if (mode_ == M_WORLD)      worldTick(c, x, v, u, n, down, up, out);
    else if (mode_ == M_PANEL) panelTick(c, x, v, down, up, out);
    prev_ = c.buttons;
}

// Start / D-pad / Alt — identical in both modes.
void Scheme::commonButtons(const Ctl& c, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    if (down & B_START) {
        if (layer) { out.push(A_KEYDOWN, 0x57); wkey_ = true; }       // W: weapon swap
        else       { out.push(A_KEYDOWN, 0x1B); esc_ = true; }        // Escape
    }
    if (up & B_START) {
        if (wkey_) { out.push(A_KEYUP, 0x57); wkey_ = false; }
        if (esc_)  { out.push(A_KEYUP, 0x1B); esc_ = false; }
    }
    for (int i = 0; i < 4; ++i) {
        if (!alt_ && (down & kDpadBits[i])) {   // D-pad navigates the loot cursor while Alt is held, not potions
            dpad_[i].vk = 0x31 + i; dpad_[i].shift = layer;
            if (layer) shiftOwn(SH_DPAD << i, true, out);              // Shift + belt key = potion to the mercenary
            out.push(A_KEYDOWN, 0x31 + i);
        }
        if ((up & kDpadBits[i]) && dpad_[i].vk) {
            out.push(A_KEYUP, dpad_[i].vk);
            if (dpad_[i].shift) shiftOwn(SH_DPAD << i, false, out);
            dpad_[i] = Held{};
        }
    }
    // Alt (ground item labels): R held first, then L. Ends when either goes up.
    if ((down & B_L) && layer && !interact_ && castSlot_ < 0) { out.push(A_KEYDOWN, 0x12); alt_ = true; }
    if (alt_ && ((up & B_L) || (up & B_R))) { out.push(A_KEYUP, 0x12); alt_ = false; }
}

void Scheme::worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    commonButtons(c, down, up, out);

    // ---- L: stand still while held, walk/run on a clean tap ----
    // L stopped being the interaction button when Cross took that over, so it
    // is free to be the modifier the PC plays with: Shift = act without
    // moving. R + L is the Alt label layer and is claimed in commonButtons,
    // which is why `layer` is excluded here.
    if ((down & B_L) && !layer && !alt_) { shiftOwn(SH_L, true, out); lTicks_ = 1; lTap_ = true; }
    if ((c.buttons & B_L) && lTicks_ > 0) {
        ++lTicks_;
        if (c.buttons & ~(uint32_t)B_L) lTap_ = false;   // it modified something: not a tap
    }
    if (up & B_L) {
        shiftOwn(SH_L, false, out);
        if (lTap_ && lTicks_ <= kTapTicks) out.push(A_KEY, 0x52);   // VK 'R', D2's own run toggle
        lTicks_ = 0; lTap_ = false;
    }

    // ---- right stick: the cursor the player aims with ----
    // Runs before the target pick so a cast pressed this tick already sees the
    // spot the player is pointing at. Alt browsing and an interaction own the
    // cursor outright; a cast borrows it, and the stick then steers the point
    // it will be handed back to.
    if (!alt_ && !interact_) {
        float dx = 0.f, dy = 0.f;
        stickDelta(c.rx, c.ry, v, &dx, &dy);
        if (castSlot_ >= 0) cursorStep(dx, dy, v, &userX_, &userY_);
        else {
            int px = cx_, py = cy_;
            if (cursorStep(dx, dy, v, &px, &py)) moveTo(px, py, out);
        }
    }

    // ---- hostile target of the moment (overlay + next cast) ----
    // Read the cone off the cursor the PLAYER owns: while a cast holds, cx_
    // sits on its target, so using it would re-target along a direction the
    // player may have left several frames ago.
    float ax = 0.f, ay = 0.f;
    const bool aimed = castSlot_ >= 0 ? aim_from_cursor(v, userX_, userY_, &ax, &ay)
                                      : aim_from_cursor(v, cx_, cy_, &ax, &ay);
    int ti = -1;
    if (cfg_.aim) { ti = pick_hostile(u, n, v, ax, ay, aimed, cfg_, findId(u, n, tgtId_, 1)); tgtId_ = ti >= 0 ? u[ti].id : 0; }

    // ---- cast: faces = slots 1-4, R + faces = 5-8 ----
    if (castSlot_ < 0 && !interact_ && !alt_) {
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) {
            // Cross alone is the context action, not a skill: slots 1-3 sit on
            // the other three faces, 4-7 behind R. That costs slot 8 -- the
            // price of one button that always does the obvious thing.
            const int slot = face_slot(i, layer);
            if (slot < 0) break;
            castSlot_ = slot; castBit_ = kFaceBits[i];
            castAttempt_ = 0; castVerified_ = false;
            out.push(A_KEY, 0x70 + castSlot_);                          // F1..F7 selects the right skill
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }      // movement suspended while casting
            userX_ = cx_; userY_ = cy_;                                 // give this back on release
            // What this slot aims at. The per-frame pick above is the HOSTILE
            // one (it feeds the diamond); a corpse slot re-picks against the
            // dead, and a ground slot deliberately snaps to nothing.
            castKind_ = (TargetKind)cfg_.slotKind[castSlot_];
            int tgtIdx = ti;
            if (castKind_ == T_GROUND) tgtIdx = -1;
            else if (castKind_ == T_CORPSE)
                tgtIdx = cfg_.aim ? pick_hostile(u, n, v, ax, ay, aimed, cfg_, -1, T_CORPSE) : -1;
            const int ti2 = tgtIdx;
            int px, py;
            if (ti2 >= 0) {
                castId_ = u[ti2].id; castType_ = u[ti2].type; castCls_ = u[ti2].cls;
                castH_ = hover_.get(castType_, castCls_, cfg_.hoverH);
                hoverPoint(u[ti2], castH_, v, &px, &py);
            } else if (aimed) {
                castId_ = 0; px = cx_; py = cy_;                        // no target: exactly where they point
            } else {
                // Cursor parked on the player: no direction to read from it,
                // so a ground cast goes out along the last walking direction
                // rather than onto our own feet.
                castId_ = 0; ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py);
            }
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);              // forced move: a new cast always re-places the cursor
            out.push(A_RDOWN, px, py); rmb_ = true;
            break;
        }
    } else if (castSlot_ >= 0) {
        if (!(c.buttons & castBit_)) {
            out.push(A_RUP, cx_, cy_); rmb_ = false;
            castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
            moveTo(userX_, userY_, out);                     // the cursor was only borrowed
        } else {
            int px = cx_, py = cy_;
            int ci = findId(u, n, castId_, castType_);
            const bool stillValid = ci >= 0 && (castKind_ == T_CORPSE ? u[ci].corpse : u[ci].hostile);
            if (castId_ && !stillValid) {                              // target died or left: re-pick
                ci = cfg_.aim ? pick_hostile(u, n, v, ax, ay, aimed, cfg_, -1, castKind_) : -1;
                if (ci >= 0) {
                    castId_ = u[ci].id; castType_ = u[ci].type; castCls_ = u[ci].cls;
                    castAttempt_ = 0; castVerified_ = false;
                    castH_ = hover_.get(castType_, castCls_, cfg_.hoverH);
                } else castId_ = 0;
            }
            if (ci >= 0 && castId_) {
                if (!castVerified_) {
                    if (x.selValid && x.selId == castId_ && x.selType == castType_) {
                        castVerified_ = true; hover_.learn(castType_, castCls_, castH_);
                    } else if (castAttempt_ < 4) {
                        ++castAttempt_;
                        castH_ = HoverTable::try_seq(castAttempt_, hover_.get(castType_, castCls_, cfg_.hoverH));
                    }
                }
                hoverPoint(u[ci], castH_, v, &px, &py);
            } else if (!aimed) ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py);
            moveTo(px, py, out);
        }
    }
    // overlay target
    // A ground slot hits a place, not a unit: the diamond would lie.
    if (castSlot_ >= 0 && castKind_ == T_GROUND) { tgt_ = Target{}; }
    else if (castSlot_ >= 0 && castId_) {
        const int ci = findId(u, n, castId_, castType_);
        tgt_ = Target{};
        if (ci >= 0) { tgt_.has = true; tgt_.id = castId_; tgt_.sx = u[ci].sx; tgt_.sy = u[ci].sy; tgt_.verified = castVerified_; }
    } else if (ti >= 0 && !alt_) { tgt_ = Target{}; tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy; }
    else tgt_ = Target{};

    // ---- Cross: the one action button ----
    // Order: what the player is POINTING at wins, because pointing at a
    // monster can only mean "hit that one". With the cursor idle we fall back
    // to whatever is within arm's reach (chest, door, portal, town NPC) and
    // only then to the nearest enemy -- otherwise a barrel could never be
    // opened with anything alive on screen.
    if ((down & B_CROSS) && !layer && castSlot_ < 0 && !interact_ && !alt_) {
        int ii = (aimed && ti >= 0) ? ti : -1;
        if (ii < 0) ii = pick_interact(u, n, v, cfg_);
        if (ii < 0) ii = ti;
        if (ii >= 0) {
            interact_ = true; interId_ = u[ii].id; interType_ = u[ii].type; interCls_ = u[ii].cls;
            interAttempt_ = 0; interArm_ = 1;
            interH_ = interType_ == 4 ? 6 : hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH);
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }
            // Move ONLY, then press once the game reports the hover -- same rule
            // as the Alt pickup, and for the same reason: a click acts on the
            // hover computed during the PREVIOUS rendered frame, so pressing in
            // this same breath makes the game act on nothing, which it sends as
            // "walk to that spot" instead of interacting. Tapping L repeatedly
            // was the worst case: the release cleared the hover each time.
            int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);
        }
    } else if (interact_ && !lootConfirm_) {
        if (!(c.buttons & B_CROSS)) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            interact_ = false; interId_ = 0; interArm_ = 0;
        } else {
            const int ii = findId(u, n, interId_, interType_);
            if (ii < 0) interArm_ = 0;                        // gone: pressing now would be a walk order
            else {
                int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
                moveTo(px, py, out);
                const bool hovered = x.selValid && x.selId == interId_ && x.selType == interType_;
                // The button is down ONLY while the game confirms it hovers
                // this unit. A click with no hover is not a weaker click, it
                // is a different order -- "walk to that point" -- so holding L
                // on a moving monster had the character chasing tiles: console
                // 20/09 measured the hover valid on just 20 of 237 held ticks.
                // Lifting on loss also re-arms the game's own per-frame hover
                // scan, which is what can re-acquire the unit.
                if (hovered && !lmb_)      { out.push(A_LDOWN, px, py); lmb_ = true; }
                else if (!hovered && lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
                if (hovered) {
                    if (interType_ != 4) hover_.learn(interType_, interCls_, interH_);
                    interArm_ = 0;                                // settled on a height that works
                } else if (interArm_ > 0) {
                    ++interArm_;
                    // Sprites are hit-tested against the cursor itself, so no
                    // hover means the height is wrong -- and on a unit that
                    // MOVES, a height that worked a moment ago can stop
                    // working. Keep cycling rather than giving up after four
                    // tries, one candidate every 3 ticks since each needs a
                    // rendered frame before the game can answer.
                    if (interType_ != 4 && interArm_ % 3 == 0) {
                        interAttempt_ = interAttempt_ >= 4 ? 1 : interAttempt_ + 1;
                        interH_ = HoverTable::try_seq(interAttempt_, hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH));
                    }
                }
            }
        }
    }

    // ---- Alt held: browse ground items (D-pad = cursor, Croix = pick up) ----
    if (alt_) {
        int fromIdx = findId(u, n, lootCursorId_, 4);
        if (fromIdx < 0 && !interact_) { fromIdx = nearest_item(u, n, v); lootCursorId_ = fromIdx >= 0 ? u[fromIdx].id : 0; }
        if (fromIdx >= 0 && !interact_) {
            static const Dir kDirs[4] = { D_UP, D_LEFT, D_DOWN, D_RIGHT };   // matches kDpadBits order
            for (int i = 0; i < 4; ++i) if (down & kDpadBits[i]) {
                const int next = nav_direction(u, n, fromIdx, kDirs[i]);
                if (next >= 0) { lootCursorId_ = u[next].id; fromIdx = next; }
                break;
            }
        }
        if ((down & B_CROSS) && !interact_ && fromIdx >= 0) {
            interact_ = true; lootConfirm_ = true; lootArm_ = 1;
            interId_ = u[fromIdx].id; interType_ = u[fromIdx].type; interCls_ = u[fromIdx].cls;
            interH_ = 6;                                              // items: fixed hover height, same as L
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            // Move ONLY. The button waits for the game to hover the item --
            // see lootArm_ in the header. Aim at the item's NAME (Alt label),
            // the same spot the marker highlights, not the feet/tile: that is
            // what picks the right one when several share a tile.
            int px, py; hoverPoint(u[fromIdx], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);
        } else if (interact_ && lootConfirm_) {
            if (!(c.buttons & B_CROSS)) {
                if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
                interact_ = false; lootConfirm_ = false; interId_ = 0; lootArm_ = 0;
            } else {
                const int ii = findId(u, n, interId_, interType_);
                if (ii < 0) lootArm_ = 0;          // item gone before the press: firing the
                                                   // fallback at its last known spot would be
                                                   // read as "walk there", the very thing the
                                                   // two-step press exists to avoid
                else {
                    int px, py; hoverPoint(u[ii], interH_, v, &px, &py); moveTo(px, py, out);
                    if (lootArm_ > 0) {
                        // The game only writes the label hover as a side effect of
                        // RENDERING a frame (Game+0xc0810 runs from the UI draw
                        // pass), and the click acts on the hover from the frame
                        // before -- so moving the cursor and pressing in the same
                        // breath makes it act on nothing, which it sends as "walk
                        // to that spot". Wait for the report.
                        //
                        // Never press without one: a click with no hover is that
                        // walk order, and the character wanders off instead of
                        // picking anything up. After a few frames, a hover on
                        // ANOTHER item still counts -- it is the one under the
                        // cursor, which is the one the player sees highlighted.
                        if (!x.selValid || x.selType != interType_) ++lootArm_;
                        else if (x.selId == interId_ || lootArm_ >= 5) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; lootArm_ = 0; }
                        else ++lootArm_;
                    }
                }
            }
        }
        lootTgt_ = Target{};
        if (fromIdx >= 0) {
            lootTgt_.has = true; lootTgt_.id = u[fromIdx].id;
            selPos(u[fromIdx], &lootTgt_.sx, &lootTgt_.sy);            // marker frames the label, not the tile
            lootTgt_.w = u[fromIdx].hasLabel ? u[fromIdx].lw : 0;
            lootTgt_.h = u[fromIdx].hasLabel ? u[fromIdx].lh : 0;
        }
    } else {
        lootTgt_ = Target{};
        lootCursorId_ = 0;
        if (lootConfirm_ && interact_) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }   // never lift a button we never pressed
            interact_ = false; lootConfirm_ = false; interId_ = 0; lootArm_ = 0;
        }
    }

    // ---- left stick: move-only orbit, hard stop on release ----
    const float lm = std::sqrt(c.lx * c.lx + c.ly * c.ly);
    const bool on = lsOn_ ? (lm > cfg_.deadzone) : (lm > cfg_.deadzone + 0.05f);
    if (on) { lastDx_ = c.lx / lm; lastDy_ = c.ly / lm; }
    if (castSlot_ < 0 && !interact_ && !alt_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
            moveTo(px, py, out);
            if (!lmb_) { out.push(A_LDOWN, px, py); lmb_ = true; lsClick_ = true; }
        } else if (lsOn_) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }
            int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
            psy += 6; clamp_point(v, cfg_, &psx, &psy);
            cx_ = psx; cy_ = psy; out.push(A_CLICK, psx, psy);            // click at the feet = stop
            // The walk borrowed the cursor the same way a cast does, and the
            // stop click just parked it on our own feet. Hand it back, or
            // every step taken would wipe where the player was aiming.
            moveTo(userX_, userY_, out);
        }
    } else if (lsClick_) { out.push(A_LUP, cx_, cy_); lmb_ = false; lsClick_ = false; }   // only ever lift the
                                                                     // stick's OWN click here: lmb_ now also covers
                                                                     // the interact/pickup press, which this branch
                                                                     // runs alongside and must not cancel
    lsOn_ = on;
}

void Scheme::panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    commonButtons(c, down, up, out);
    // free cursor: both sticks drive it here, same step as the world mode
    {
        float dx = 0.f, dy = 0.f;
        stickDelta(c.lx, c.ly, v, &dx, &dy);
        stickDelta(c.rx, c.ry, v, &dx, &dy);
        int px = cx_, py = cy_;
        if (cursorStep(dx, dy, v, &px, &py)) moveTo(px, py, out);
    }
    // Cross is the left click in EVERY panel, the skill tree included: you
    // still have to click an icon to spend a point on it.
    if ((down & B_CROSS) && !layer) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
    if ((up & B_CROSS) && lmb_ && !(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    if (x.skillTree) {                                                    // faces = bind the hovered icon
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) {
            const int slot = face_slot(i, layer);
            if (slot >= 0) out.push(A_KEY, 0x70 + slot);
        }
    } else {
        if (down & B_TRI) { out.push(A_RDOWN, cx_, cy_); rmb_ = true; }
        if ((up & B_TRI) && rmb_) { out.push(A_RUP, cx_, cy_); rmb_ = false; }
        if (down & B_SQR) shiftOwn(SH_SQR, true, out);
        if (up & B_SQR)   shiftOwn(SH_SQR, false, out);
        if (down & B_CIR) out.push(A_KEY, 0x1B);
    }
    if ((down & B_L) && !layer && !alt_) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
    if ((up & B_L) && lmb_ && !(c.buttons & B_CROSS)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    tgt_ = Target{};
}

} // namespace pad
