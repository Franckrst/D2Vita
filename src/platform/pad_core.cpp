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

int pick_hostile(const Unit* u, int n, const View& v, const Ctl& c, const Config& cfg, int current) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    float ax = c.rx, ay = c.ry;
    const float am = std::sqrt(ax * ax + ay * ay);
    const bool aimed = am > cfg.deadzone;
    if (aimed) { ax /= am; ay /= am; }
    const float cosMin = std::cos(cfg.coneDeg * kPi / 180.f);
    const float maxD = aimed ? 500.f : 420.f;
    auto score = [&](int i, float* s) -> bool {
        const Unit& t = u[i];
        if (!t.hostile) return false;
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
    for (int pass = 0; pass < 2 && best < 0; ++pass) {          // pass 0: items ; pass 1: the rest
        const float maxD = pass == 0 ? 220.f : 160.f;
        for (int i = 0; i < n; ++i) {
            const Unit& t = u[i];
            if (!t.interact) continue;
            if ((pass == 0) != (t.type == 4)) continue;
            const float dx = (float)(t.sx - psx), dy = (float)(t.sy - psy);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > maxD) continue;
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
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

int nav_direction(const Unit* u, int n, int fromIdx, Dir d) {
    if (!u || fromIdx < 0 || fromIdx >= n) return -1;
    const Unit& from = u[fromIdx];
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (i == fromIdx || u[i].type != 4) continue;
        const float dx = (float)(u[i].sx - from.sx), dy = (float)(u[i].sy - from.sy);
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
static const uint32_t kDpadBits[4] = { B_UP, B_LEFT, B_DOWN, B_RIGHT };   // belt 1..4, same order as the legacy table

void Scheme::moveTo(int x, int y, Actions& out) {
    if (x != cx_ || y != cy_) { cx_ = x; cy_ = y; out.push(A_MOVE, x, y); }
}

int Scheme::findId(const Unit* u, int n, uint32_t id) const {
    if (!id) return -1;
    for (int i = 0; i < n; ++i) if (u[i].id == id) return i;
    return -1;
}

void Scheme::hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const {
    *px = t.sx; *py = t.sy - h;
    clamp_point(v, cfg_, px, py);
}

void Scheme::releaseAll(Actions& out) {
    if (rmb_)    { out.push(A_RUP, cx_, cy_); rmb_ = false; }
    if (lmb_)    { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    if (alt_)    { out.push(A_KEYUP, 0x12); alt_ = false; }
    if (shiftSq_){ out.push(A_KEYUP, 0x10); shiftSq_ = false; }
    if (esc_)    { out.push(A_KEYUP, 0x1B); esc_ = false; }
    if (wkey_)   { out.push(A_KEYUP, 0x57); wkey_ = false; }
    for (Held& h : dpad_) if (h.vk) { out.push(A_KEYUP, h.vk); if (h.shift) out.push(A_KEYUP, 0x10); h = Held{}; }
    castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
    interact_ = false; interNoop_ = false; interId_ = 0; interVerified_ = false;
    lootConfirm_ = false; lootCursorId_ = 0; lootTgt_ = Target{};
    lsOn_ = false; tgt_ = Target{}; tgtId_ = 0; aimActive_ = false;
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
            if (layer) out.push(A_KEYDOWN, 0x10);                     // Shift + belt key = potion to the mercenary
            out.push(A_KEYDOWN, 0x31 + i);
        }
        if ((up & kDpadBits[i]) && dpad_[i].vk) {
            out.push(A_KEYUP, dpad_[i].vk);
            if (dpad_[i].shift) out.push(A_KEYUP, 0x10);
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

    // ---- hostile target of the moment (overlay + next cast) ----
    int ti = -1;
    if (cfg_.aim) { ti = pick_hostile(u, n, v, c, cfg_, findId(u, n, tgtId_)); tgtId_ = ti >= 0 ? u[ti].id : 0; }
    {
        const float am = std::sqrt(c.rx * c.rx + c.ry * c.ry);
        aimActive_ = !alt_ && am > cfg_.deadzone;
        if (aimActive_) ground_point(v, c, cfg_, lastDx_, lastDy_, &aimX_, &aimY_);
    }

    // ---- cast: faces = slots 1-4, R + faces = 5-8 ----
    if (castSlot_ < 0 && !interact_ && !alt_) {
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) {
            castSlot_ = i + (layer ? 4 : 0); castBit_ = kFaceBits[i];
            castAttempt_ = 0; castVerified_ = false;
            out.push(A_KEY, 0x70 + castSlot_);                          // F1..F8 selects the right skill
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }      // movement suspended while casting
            int px, py;
            if (ti >= 0) {
                castId_ = u[ti].id; castType_ = u[ti].type; castCls_ = u[ti].cls;
                castH_ = hover_.get(castType_, castCls_, cfg_.hoverH);
                hoverPoint(u[ti], castH_, v, &px, &py);
            } else { castId_ = 0; ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py); }
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);              // forced move: a new cast always re-places the cursor
            out.push(A_RDOWN, px, py); rmb_ = true;
            break;
        }
    } else if (castSlot_ >= 0) {
        if (!(c.buttons & castBit_)) {
            out.push(A_RUP, cx_, cy_); rmb_ = false;
            castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
        } else {
            int px = cx_, py = cy_;
            int ci = findId(u, n, castId_);
            if (castId_ && (ci < 0 || !u[ci].hostile)) {               // target died or left: re-pick
                ci = cfg_.aim ? pick_hostile(u, n, v, c, cfg_, -1) : -1;
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
            } else ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py);
            moveTo(px, py, out);
        }
    }
    // overlay target
    if (castSlot_ >= 0 && castId_) {
        const int ci = findId(u, n, castId_);
        tgt_ = Target{};
        if (ci >= 0) { tgt_.has = true; tgt_.id = castId_; tgt_.sx = u[ci].sx; tgt_.sy = u[ci].sy; tgt_.verified = castVerified_; }
    } else if (ti >= 0 && !alt_) { tgt_ = Target{}; tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy; }
    else tgt_ = Target{};

    // ---- L: interact (items > objects / town NPCs > the hostile target) ----
    if ((down & B_L) && !layer && castSlot_ < 0 && !interact_ && !alt_) {
        int ii = pick_interact(u, n, v, cfg_);
        if (ii < 0 && ti >= 0) ii = ti;
        if (ii >= 0) {
            interact_ = true; interId_ = u[ii].id; interType_ = u[ii].type; interCls_ = u[ii].cls;
            interAttempt_ = 0; interVerified_ = false;
            interH_ = interType_ == 4 ? 6 : hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH);
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);              // forced move, same reason as the cast
            out.push(A_LDOWN, px, py);
        } else interNoop_ = true;
    } else if (interact_ && !lootConfirm_) {
        if (!(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); interact_ = false; interId_ = 0; }
        else {
            const int ii = findId(u, n, interId_);
            if (ii >= 0) {
                if (!interVerified_) {
                    if (x.selValid && x.selId == interId_ && x.selType == interType_) {
                        interVerified_ = true;
                        if (interType_ != 4) hover_.learn(interType_, interCls_, interH_);
                    } else if (interAttempt_ < 4 && interType_ != 4) {
                        ++interAttempt_;
                        interH_ = HoverTable::try_seq(interAttempt_, hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH));
                    }
                }
                int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
                moveTo(px, py, out);
            }
        }
    }
    if ((up & B_L) && interNoop_) interNoop_ = false;

    // ---- Alt held: browse ground items (D-pad = cursor, Croix = pick up) ----
    if (alt_) {
        int fromIdx = findId(u, n, lootCursorId_);
        if (fromIdx < 0) { fromIdx = nearest_item(u, n, v); lootCursorId_ = fromIdx >= 0 ? u[fromIdx].id : 0; }
        if (fromIdx >= 0 && !interact_) {
            static const Dir kDirs[4] = { D_UP, D_LEFT, D_DOWN, D_RIGHT };   // matches kDpadBits order
            for (int i = 0; i < 4; ++i) if (down & kDpadBits[i]) {
                const int next = nav_direction(u, n, fromIdx, kDirs[i]);
                if (next >= 0) { lootCursorId_ = u[next].id; fromIdx = next; }
                break;
            }
        }
        if ((down & B_CROSS) && !interact_ && fromIdx >= 0) {
            interact_ = true; lootConfirm_ = true;
            interId_ = u[fromIdx].id; interType_ = u[fromIdx].type; interCls_ = u[fromIdx].cls;
            interH_ = 6;                                              // items: fixed hover height, same as L
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int px, py; hoverPoint(u[fromIdx], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);
            out.push(A_LDOWN, px, py);
        } else if (interact_ && lootConfirm_) {
            if (!(c.buttons & B_CROSS)) { out.push(A_LUP, cx_, cy_); interact_ = false; lootConfirm_ = false; interId_ = 0; }
            else {
                const int ii = findId(u, n, interId_);
                if (ii >= 0) { int px, py; hoverPoint(u[ii], interH_, v, &px, &py); moveTo(px, py, out); }
            }
        }
        lootTgt_ = Target{};
        if (fromIdx >= 0) { lootTgt_.has = true; lootTgt_.id = u[fromIdx].id; lootTgt_.sx = u[fromIdx].sx; lootTgt_.sy = u[fromIdx].sy; }
    } else {
        lootTgt_ = Target{};
        if (lootConfirm_ && interact_) { out.push(A_LUP, cx_, cy_); interact_ = false; lootConfirm_ = false; interId_ = 0; }
    }

    // ---- left stick: move-only orbit, hard stop on release ----
    const float lm = std::sqrt(c.lx * c.lx + c.ly * c.ly);
    const bool on = lsOn_ ? (lm > cfg_.deadzone) : (lm > cfg_.deadzone + 0.05f);
    if (on) { lastDx_ = c.lx / lm; lastDy_ = c.ly / lm; }
    if (castSlot_ < 0 && !interact_ && !alt_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
            moveTo(px, py, out);
            if (!lmb_) { out.push(A_LDOWN, px, py); lmb_ = true; }
        } else if (lsOn_) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
            psy += 6; clamp_point(v, cfg_, &psx, &psy);
            cx_ = psx; cy_ = psy; out.push(A_CLICK, psx, psy);            // click at the feet = stop
        }
    } else if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }        // suspended while casting / interacting
    lsOn_ = on;
}

void Scheme::panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    commonButtons(c, down, up, out);
    // free cursor: both sticks, relative, quadratic response
    {
        const float sc = cfg_.sens * ((float)v.w / 800.f);
        float dx = 0.f, dy = 0.f;
        if (std::fabs(c.lx) > cfg_.deadzone || std::fabs(c.ly) > cfg_.deadzone) { dx += c.lx * std::fabs(c.lx) * sc; dy += c.ly * std::fabs(c.ly) * sc; }
        if (std::fabs(c.rx) > cfg_.deadzone || std::fabs(c.ry) > cfg_.deadzone) { dx += c.rx * std::fabs(c.rx) * sc; dy += c.ry * std::fabs(c.ry) * sc; }
        if (dx != 0.f || dy != 0.f) {
            int px = cx_ + (int)dx, py = cy_ + (int)dy;
            if (px < 0) px = 0; if (px > v.w - 1) px = v.w - 1;
            if (py < 0) py = 0; if (py > v.h - 1) py = v.h - 1;
            moveTo(px, py, out);
        }
    }
    if (x.skillTree) {                                                    // faces = hotkeys on the hovered icon
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) out.push(A_KEY, 0x70 + i + (layer ? 4 : 0));
    } else {
        if (down & B_CROSS) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
        if ((up & B_CROSS) && lmb_ && !(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
        if (down & B_TRI) { out.push(A_RDOWN, cx_, cy_); rmb_ = true; }
        if ((up & B_TRI) && rmb_) { out.push(A_RUP, cx_, cy_); rmb_ = false; }
        if (down & B_SQR) { out.push(A_KEYDOWN, 0x10); shiftSq_ = true; }
        if ((up & B_SQR) && shiftSq_) { out.push(A_KEYUP, 0x10); shiftSq_ = false; }
        if (down & B_CIR) out.push(A_KEY, 0x1B);
    }
    if ((down & B_L) && !layer && !alt_) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
    if ((up & B_L) && lmb_ && !(c.buttons & B_CROSS)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    tgt_ = Target{}; aimActive_ = false;
}

} // namespace pad
