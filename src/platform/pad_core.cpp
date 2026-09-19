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

} // namespace pad
