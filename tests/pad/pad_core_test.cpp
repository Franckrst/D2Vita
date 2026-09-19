// tests/pad/pad_core_test.cpp — desktop tests for src/platform/pad_core.*
// Plain asserts, no framework: `./build/pad_core_test` exits 0 when green.
#include "platform/pad_core.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// A view where the player projects exactly to (400,300): viewX/Y absorb the
// player's absolute position.
static pad::View mkView(int32_t pfx = 100 << 16, int32_t pfy = 50 << 16) {
    pad::View v; v.w = 800; v.h = 600; v.playerFx = pfx; v.playerFy = pfy;
    v.viewX = (int32_t)(((int64_t)(pfx - pfy) * 16) / 65536) - 400;
    v.viewY = (int32_t)(((int64_t)(pfx + pfy) * 8) / 65536) - 300;
    return v;
}

static void test_projection() {
    pad::View v = mkView();
    int sx, sy;
    pad::world_to_screen(v, v.playerFx, v.playerFy, &sx, &sy);
    CHECK(sx == 400 && sy == 300);
    // +1 subtile in x -> +16 px right, +8 px down ; +1 in y -> -16, +8
    pad::world_to_screen(v, v.playerFx + (1 << 16), v.playerFy, &sx, &sy);
    CHECK(sx == 416 && sy == 308);
    pad::world_to_screen(v, v.playerFx, v.playerFy + (1 << 16), &sx, &sy);
    CHECK(sx == 384 && sy == 308);
    // inverse: a screen direction maps back to the same screen direction
    float wx, wy; pad::screen_dir_to_world(1.f, 0.f, &wx, &wy);
    int sx2, sy2;
    pad::world_to_screen(v, v.playerFx + (int32_t)(wx * 65536.f * 10.f), v.playerFy + (int32_t)(wy * 65536.f * 10.f), &sx2, &sy2);
    CHECK(sx2 > 400 && sy2 == 300);
}

static void test_clamp_and_box() {
    pad::View v = mkView(); pad::Config cfg;
    int x = -20, y = 590;
    pad::clamp_point(v, cfg, &x, &y);
    CHECK(x == 4 && y == 600 - cfg.hudH - 1);
    pad::Unit m; m.type = 1; m.sx = 500; m.sy = 300;
    CHECK(pad::in_unit_box(m, 500, 280));
    CHECK(pad::in_unit_box(m, 484, 252));
    CHECK(!pad::in_unit_box(m, 500, 240));
    CHECK(!pad::in_unit_box(m, 530, 290));
    pad::Unit it; it.type = 4; it.sx = 100; it.sy = 100;
    CHECK(pad::in_unit_box(it, 110, 105));
    CHECK(!pad::in_unit_box(it, 130, 100));
    pad::Unit ms; ms.type = 3; ms.sx = 100; ms.sy = 100;
    CHECK(!pad::in_unit_box(ms, 100, 100));
}

static pad::Unit mkMon(uint32_t id, int sx, int sy, bool hostile = true) {
    pad::Unit u; u.id = id; u.type = 1; u.cls = 10; u.mode = 1; u.sx = sx; u.sy = sy; u.hostile = hostile; return u;
}

static void test_pick_hostile() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    pad::Unit u[4] = { mkMon(1, 600, 300), mkMon(2, 450, 300), mkMon(3, 400, 100, false), mkMon(4, 400, 320) };
    u[3].mode = 12;  u[3].hostile = false;   // corpse flagged by the glue
    // idle stick: nearest hostile
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == 1);
    // aimed to the right: the far right one wins over the near one? no: both in cone, near wins
    c.rx = 1.f; c.ry = 0.f;
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == 1);
    // aimed up: nothing hostile in the cone (unit 3 is not hostile)
    c.rx = 0.f; c.ry = -1.f;
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == -1);
    // hysteresis: current target kept while close to best
    c.rx = 0.f; c.ry = 0.f;
    pad::Unit w[2] = { mkMon(1, 460, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(w, 2, v, c, cfg, 0) == 0);
    pad::Unit z[2] = { mkMon(1, 700, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(z, 2, v, c, cfg, 0) == 1);
    // range and cone limits
    pad::Unit far1[1] = { mkMon(9, 790, 300) };               // 390 px <= 420: taken when idle
    c.rx = 0.f; c.ry = 0.f;
    CHECK(pad::pick_hostile(far1, 1, v, c, cfg, -1) == 0);
    pad::Unit far2[1] = { mkMon(9, 400, 100) };               // 200 px straight up
    c.rx = 1.f; c.ry = 0.f;                                   // aimed right: out of the cone
    CHECK(pad::pick_hostile(far2, 1, v, c, cfg, -1) == -1);
}

static void test_pick_interact() {
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit u[3];
    u[0].id = 1; u[0].type = 2; u[0].sx = 420; u[0].sy = 300; u[0].interact = true;    // chest, 20 px
    u[1].id = 2; u[1].type = 4; u[1].sx = 560; u[1].sy = 300; u[1].interact = true;    // item, 160 px
    u[2].id = 3; u[2].type = 4; u[2].sx = 700; u[2].sy = 300; u[2].interact = true;    // item, 300 px (too far)
    CHECK(pad::pick_interact(u, 3, v, cfg) == 1);        // items first, within 220
    u[1].sx = 700;                                         // both items too far -> the chest
    CHECK(pad::pick_interact(u, 3, v, cfg) == 0);
    u[0].sx = 600;                                         // chest at 200 px: beyond 160 -> nothing
    CHECK(pad::pick_interact(u, 3, v, cfg) == -1);
}

static void test_hover_table() {
    pad::HoverTable t;
    CHECK(t.get(1, 42, 28) == 28);
    t.learn(1, 42, 44);
    CHECK(t.get(1, 42, 28) == 44);
    CHECK(t.get(2, 42, 20) == 20);
    t.learn(1, 42, 64);
    CHECK(t.get(1, 42, 28) == 64 && t.n == 1);
    CHECK(pad::HoverTable::try_seq(0, 28) == 28);
    CHECK(pad::HoverTable::try_seq(1, 28) == 14);
    CHECK(pad::HoverTable::try_seq(4, 28) == 90);
    CHECK(pad::HoverTable::try_seq(5, 28) == 28);
}

static void test_orbit() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    int px = 0, py = 0;
    CHECK(!pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));           // idle
    c.lx = 1.f; c.ly = 0.f;                                               // full right
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(px == 400 + cfg.orbitMax && py == 300);
    c.lx = 0.5f;                                                          // half tilt
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(px > 400 + cfg.orbitMin && px < 400 + cfg.orbitMax && py == 300);
    // a monster standing exactly on the ring: the point moves off its box
    c.lx = 1.f;
    pad::Unit m = mkMon(7, 400 + cfg.orbitMax, 300 + 10);
    CHECK(pad::orbit_point(v, c, cfg, &m, 1, &px, &py));
    CHECK(!pad::in_unit_box(m, px, py));
    // never below the HUD band
    c.lx = 0.f; c.ly = 1.f; v.viewY -= 250;                               // player low on screen
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(py <= 600 - cfg.hudH - 1);
}

static void test_ground_point() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    int px, py;
    c.rx = 1.f; c.ry = 0.f;                                               // full right -> rangeMax subtiles
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    CHECK(py == 300 && px > 400);
    const int farX = px;
    c.rx = 0.5f;                                                          // half tilt -> closer
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    CHECK(px > 400 && px < farX);
    c.rx = 0.f; c.ry = 0.f;                                               // idle -> fallback direction (down), rangeMin
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    // straight down on screen = world diagonal: ~11.3 px per subtile
    CHECK(px == 400 && py > 300 && py < 300 + 12 * cfg.rangeMin + 2);
}

int main() {
    test_projection();
    test_clamp_and_box();
    test_pick_hostile();
    test_pick_interact();
    test_hover_table();
    test_orbit();
    test_ground_point();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
