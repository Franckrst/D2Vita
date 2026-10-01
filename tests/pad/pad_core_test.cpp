// tests/pad/pad_core_test.cpp — desktop tests for src/platform/pad_core.*
// Plain asserts, no framework: `./build/pad_core_test` exits 0 when green.
#include "platform/pad_core.h"
#include "platform/controls_scan.h"
#include <cstdio>
#include <cstring>
#include <cmath>

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
    pad::Unit u; u.id = id; u.type = 1; u.cls = 10; u.sx = sx; u.sy = sy; u.hostile = hostile; return u;
}

static void test_pick_hostile() {
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit u[4] = { mkMon(1, 600, 300), mkMon(2, 450, 300), mkMon(3, 400, 100, false), mkMon(4, 400, 320) };
    u[3].hostile = false;                    // corpse, flagged by the glue
    // not aiming: nearest hostile
    CHECK(pad::pick_hostile(u, 4, v, 0.f, 0.f, false, cfg, -1) == 1);
    // aimed to the right: the far right one wins over the near one? no: both in cone, near wins
    CHECK(pad::pick_hostile(u, 4, v, 1.f, 0.f, true, cfg, -1) == 1);
    // aimed up: nothing hostile in the cone (unit 3 is not hostile)
    CHECK(pad::pick_hostile(u, 4, v, 0.f, -1.f, true, cfg, -1) == -1);
    // hysteresis: current target kept while close to best
    pad::Unit w[2] = { mkMon(1, 460, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(w, 2, v, 0.f, 0.f, false, cfg, 0) == 0);
    pad::Unit z[2] = { mkMon(1, 700, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(z, 2, v, 0.f, 0.f, false, cfg, 0) == 1);
    // range and cone limits
    pad::Unit far1[1] = { mkMon(9, 790, 300) };               // 390 px <= 420: taken when not aiming
    CHECK(pad::pick_hostile(far1, 1, v, 0.f, 0.f, false, cfg, -1) == 0);
    pad::Unit far2[1] = { mkMon(9, 400, 100) };               // 200 px straight up
    CHECK(pad::pick_hostile(far2, 1, v, 1.f, 0.f, true, cfg, -1) == -1);   // aimed right: out of the cone
    // an unnormalized direction is normalized for us
    CHECK(pad::pick_hostile(u, 4, v, 250.f, 0.f, true, cfg, -1) == 1);
    // the cursor sitting on the player gives no direction to read
    float ax = 9.f, ay = 9.f;
    CHECK(!pad::aim_from_cursor(v, 400, 300, &ax, &ay));
    CHECK(pad::aim_from_cursor(v, 500, 300, &ax, &ay) && ax == 1.f && ay == 0.f);
}

static void test_pick_interact() {
    pad::View v = mkView(); pad::Config cfg;
    cfg.reach = 220;                                    // pin the radius this test was written against
    pad::Unit u[3];
    u[0].id = 1; u[0].type = 2; u[0].sx = 420; u[0].sy = 300; u[0].interact = true;    // chest, 20 px
    u[1].id = 2; u[1].type = 4; u[1].sx = 560; u[1].sy = 300; u[1].interact = true;    // item, 160 px
    u[2].id = 3; u[2].type = 4; u[2].sx = 700; u[2].sy = 300; u[2].interact = true;    // item, 300 px (too far)
    CHECK(pad::pick_interact(u, 3, v, cfg) == 0);        // items are never L candidates: the chest wins
    u[1].sx = 420; u[1].sy = 300;                          // an item right on top of the chest changes nothing
    CHECK(pad::pick_interact(u, 3, v, cfg) == 0);
    u[0].sx = 600;                                         // chest at 200 px: still in reach
    CHECK(pad::pick_interact(u, 3, v, cfg) == 0);
    u[0].sx = 650;                                         // 250 px: beyond 220 -> nothing
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


static bool hasAct(const pad::Actions& a, pad::ActKind k, int x = -1, int y = -1) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k && (x < 0 || a.v[i].a == x) && (y < 0 || a.v[i].b == y)) return true;
    return false;
}
static int actIndex(const pad::Actions& a, pad::ActKind k) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k) return i;
    return -1;
}

static void test_scheme_move() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Ctl c; pad::Actions a;
    c.lx = 1.f; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_MOVE, 400 + cfg.orbitMax, 300));
    CHECK(hasAct(a, pad::A_LDOWN, 400 + cfg.orbitMax, 300));
    a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);        // held: nothing new (same point)
    CHECK(a.n == 0);
    c.lx = 0.f; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);   // release: LUP then stop click at the feet
    CHECK(hasAct(a, pad::A_LUP));
    CHECK(hasAct(a, pad::A_CLICK, 400, 306));
    CHECK(actIndex(a, pad::A_LUP) < actIndex(a, pad::A_CLICK));
    // walking, then pressing aim: the walk click is lifted for the interaction,
    // and walking resumes in the same tick the button is released
    pad::Unit u[1] = { mkMon(5, 700, 300) };
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP) && s.interacting());
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN) && !s.interacting());
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);              // stick still pushed, point unchanged: steady state
    CHECK(a.n == 0);
}

static void test_aim_interact() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[2] = { mkMon(5, 500, 300) };
    u[1].id = 8; u[1].type = 2; u[1].cls = 7; u[1].sx = 430; u[1].sy = 310; u[1].interact = true;
    pad::Ctl c; pad::Actions a;
    c.aim = true; s.tick(c, x, v, u, 2, a);              // cursor idle: the chest wins over the monster
    CHECK(hasAct(a, pad::A_MOVE, 430, 290) && !hasAct(a, pad::A_LDOWN));   // move first, press next tick
    x.selValid = 1; x.selId = 8; x.selType = 2;
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LDOWN, 430, 290));
    x.selValid = 0; x.selId = 0; x.selType = 0;
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LUP));
    // aim with nothing around and no target: no click at all
    a = pad::Actions{}; c.aim = true; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
}

// Every A_LDOWN must have a lift on EVERY exit, not just the button's own
// release. Console-visible failure: hold Cross on a town NPC, the dialogue opens,
// the mode switches to PANEL -- and the left button stays down forever, so
// every later cursor move is a drag.
static void test_left_button_always_released() {
    pad::View v = mkView();
    pad::Unit chest[1]; chest[0].id = 51; chest[0].type = 2; chest[0].cls = 7;
    chest[0].sx = 400; chest[0].sy = 300; chest[0].interact = true;
    auto countUp = [](const pad::Actions& a) {
        int k = 0; for (int i = 0; i < a.n; ++i) if (a.v[i].k == pad::A_LUP) ++k; return k; };

    // A: a Cross interaction pressed and held, then a panel opens
    { pad::Config cfg; pad::Assist s(cfg); pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
      c.aim = true; s.tick(c, x, v, chest, 1, a);
      x.selValid = 1; x.selId = 51; x.selType = 2;
      a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(hasAct(a, pad::A_LDOWN));
      x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(countUp(a) == 1); }

    // B: a Cross interaction pressed and held, then the game is left
    { pad::Config cfg; pad::Assist s(cfg); pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
      c.aim = true; s.tick(c, x, v, chest, 1, a);
      x.selValid = 1; x.selId = 51; x.selType = 2;
      a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(hasAct(a, pad::A_LDOWN));
      a = pad::Actions{}; s.leave(a);
      CHECK(countUp(a) == 1); }
}

// A chest or NPC is hit-tested against the cursor's own position, so the
// cursor height matters -- and each candidate height needs a RENDERED frame
// before the game can answer. Escalating every tick burned all four
// candidates before a single frame had been drawn. The button goes down
// after one tick either way: it is HELD, and the game re-reads the hover on
// every frame while it is, so it corrects itself as soon as a height lands.
static void test_interact_height_retry_paces_itself() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1]; u[0].id = 60; u[0].type = 2; u[0].cls = 7;      // a chest
    u[0].sx = 400; u[0].sy = 300; u[0].interact = true;
    c.aim = true; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 400, 280) && !hasAct(a, pad::A_LDOWN));   // move first, default height 20
    int moves = 0, downs = 0;
    for (int i = 0; i < 9; ++i) {                                   // still not hovered: keep hunting heights
        a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
        if (hasAct(a, pad::A_MOVE)) ++moves;
        if (hasAct(a, pad::A_LDOWN)) ++downs;
    }
    CHECK(downs == 0);                                              // never presses without a hover
    CHECK(moves >= 1 && moves <= 4);                                // paced, not one per tick
    // it finally reports the hover: the working height is learned and kept
    x.selValid = 1; x.selId = 60; x.selType = 2;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    const int settled = s.cy();
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.cy() == settled && !hasAct(a, pad::A_MOVE));            // stops moving once it sticks
}

// Holding Cross on a MOVING unit: the button follows the game's hover, going down
// when it confirms the unit and lifting when it loses it -- never issuing the
// hover-less click, which the game reads as "walk to that point".
static void test_hold_follows_the_hover() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(70, 460, 300) };
    c.aim = true; s.tick(c, x, v, u, 1, a);             // no interactable: falls back to the target
    x.selValid = 1; x.selId = 70; x.selType = 1;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
    x.selValid = 0;                                                 // the game loses it (cursor off the sprite)
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP));
    for (int i = 0; i < 4; ++i) {                                   // still lost: no click on empty ground
        a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
        CHECK(!hasAct(a, pad::A_LDOWN));
    }
    x.selValid = 1;                                                 // re-acquired: press again
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
    // and the cursor tracked the unit as it moved
    u[0].sx = 520; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE) && s.cx() == 520);
}

static void test_scheme_panel_and_leave() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    pad::Ctl c; pad::Actions a;
    // an interaction is held when a panel opens: released on the mode change
    c.aim = true; s.tick(c, x, v, u, 1, a);
    CHECK(s.interacting());
    x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(!s.interacting());
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // panel: aim = click at the cursor
    s.setCursor(200, 200);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN, 200, 200));
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP, 200, 200));
    // left stick moves the free cursor in a panel
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE) && s.cx() > 200);
    c.lx = 0.f;
    // leave() while the click is held lifts it, once
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    a = pad::Actions{}; s.leave(a);
    CHECK(hasAct(a, pad::A_LUP));
}

// --- right stick = free cursor, assist fires at the moment of the cast -------

static void test_right_stick_is_a_free_cursor() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Ctl c; pad::Actions a;
    c.rx = 1.f;                                      // full right
    s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_MOVE, 410, 300));         // sens 10 at 800 px wide, quadratic response
    CHECK(s.cx() == 410 && s.cy() == 300);
    CHECK(!hasAct(a, pad::A_LDOWN));                 // aiming never clicks
    a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(s.cx() == 420);                            // keeps travelling while held
}

static void test_free_cursor_reaches_the_hud() {
    // clamp_point keeps ASSISTED points out of the bottom band (the game drops
    // a click resolved there). The cursor the PLAYER drives must still reach
    // the belt and the skill buttons.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Ctl c; pad::Actions a; c.ry = 1.f;
    for (int i = 0; i < 40; ++i) { a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a); }
    CHECK(s.cy() > v.h - cfg.hudH);
    CHECK(s.cy() <= v.h - 1);
}

static void test_aim_cone_follows_the_cursor() {
    // The cone direction comes from the player->cursor vector, not from the
    // stick: both sticks stay idle here.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[2] = { mkMon(1, 600, 300), mkMon(2, 400, 150) };   // right 200 px, up 150 px
    pad::Ctl c; pad::Actions a;
    s.setCursor(500, 300);                           // pointing right: the FAR one wins
    s.tick(c, x, v, u, 2, a);
    CHECK(s.target().has && s.target().id == 1);
    s.setCursor(400, 200);                           // pointing up
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.target().has && s.target().id == 2);
}

static void test_the_walk_leaves_the_cursor_alone() {
    // Console, 21/09: "the cursor should stay where it is once the stick is
    // released". Any teleport of the cursor is disorienting, including the
    // well-meant one that tried to restore the previous aim -- the walk's own
    // stop click already parks it on the character, and that is where it stays.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Ctl c; pad::Actions a;
    s.setCursor(600, 200);
    c.lx = 1.f; s.tick(c, x, v, nullptr, 0, a);
    c.lx = 0.f; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_CLICK, 400, 306));         // stop click on the feet
    CHECK(!hasAct(a, pad::A_MOVE));                   // and nothing after it
    CHECK(s.cx() == 400 && s.cy() == 306);
}

static void test_repick_follows_where_the_player_now_aims() {
    // While a cast holds, cx_ sits on the TARGET, so reading the cone off it
    // would re-target along a direction the player left long ago. The stick
    // steers userX_/userY_ during the cast: that is what the cone must use.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit right[1] = { mkMon(1, 600, 300) };                 // 200 px to the right
    pad::Unit up[1]    = { mkMon(2, 400, 150) };                 // 150 px straight up
    pad::Ctl c; pad::Actions a;
    s.setCursor(500, 300);                                        // aiming right
    s.tick(c, x, v, right, 1, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, right, 1, a);
    CHECK(s.interacting() && s.target().id == 1);
    c.ry = -1.f;                                                  // swing the aim upwards, still holding
    for (int i = 0; i < 20; ++i) { a = pad::Actions{}; s.tick(c, x, v, right, 1, a); }
    a = pad::Actions{}; s.tick(c, x, v, up, 1, a);                // target 1 gone: re-pick
    CHECK(s.target().has && s.target().id == 2);
}

static void test_panel_cursor_sums_both_sticks_before_rounding() {
    // Both sticks drive the panel cursor. Their steps are summed and rounded
    // once: rounding each stick on its own loses a pixel per axis per tick,
    // which is a drift the player feels over a stash full of items.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.panelOpen = true;
    pad::Ctl c; pad::Actions a;
    c.lx = 0.5f; c.rx = 0.5f;                     // 2.5 px each, 5.0 px together
    s.tick(c, x, v, nullptr, 0, a);
    CHECK(s.cx() == 405);
}

// --- the aim button is the context action ---------------------------------

static void test_cross_is_the_context_button() {
    // The aim button is the one action button. Pointed at a monster it
    // attacks it, through the move-then-wait-for-the-hover path.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[1] = { mkMon(5, 600, 300) };
    pad::Ctl c; pad::Actions a;
    s.setCursor(550, 300);                                // pointing at it
    s.tick(c, x, v, u, 1, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 600, 272));              // moved onto it
    CHECK(!hasAct(a, pad::A_LDOWN));                      // and waits for the game's hover
    x.selValid = 1; x.selId = 5; x.selType = 1;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
}

static void test_cross_prefers_a_close_object_when_not_pointing_at_anything() {
    // Cursor idle on the player: no aim to read, so a chest within reach wins
    // over a monster the player never pointed at.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[2] = { mkMon(5, 600, 300) };
    u[1].id = 8; u[1].type = 2; u[1].cls = 7; u[1].sx = 430; u[1].sy = 310; u[1].interact = true;
    pad::Ctl c; pad::Actions a;
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 430, 290));              // the chest, not the monster
}

// --- per-slot target kind (point 8) ---------------------------------------

// --- L is a modifier now: stand still held, walk/run on a clean tap -------

// --- feedback from discussion #16, v3-test1 --------------------------------

static void test_distance_is_measured_in_the_world_not_on_screen() {
    // The projection squashes y by 2 (x*16, y*8 per subtile), so a unit due
    // north looked HALF as far as one due east at the same world distance.
    // That is what put Lysander ahead of Fara on console.
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit u[2] = { mkMon(1, 600, 300),      // due east, 200 px of screen
                       mkMon(2, 400, 180) };    // due north, 120 px of screen
    // On screen the northern one is nearer; in the world it is 240 against 200.
    CHECK(pad::pick_hostile(u, 2, v, 0.f, 0.f, false, cfg, -1) == 0);
}

static void test_interact_reach_is_round_in_the_world() {
    pad::View v = mkView(); pad::Config cfg;
    cfg.reach = 220;                                         // pin the radius: this tests the METRIC
    pad::Unit u[1];
    u[0].id = 1; u[0].type = 2; u[0].interact = true;
    u[0].sx = 400; u[0].sy = 400;                            // 100 px below = 200 in the world
    CHECK(pad::pick_interact(u, 1, v, cfg) == 0);
    u[0].sy = 420;                                           // 120 px below = 240: out of reach
    CHECK(pad::pick_interact(u, 1, v, cfg) == -1);
    u[0].sx = 600; u[0].sy = 300;                            // 200 px east = 200: in reach again
    CHECK(pad::pick_interact(u, 1, v, cfg) == 0);
}

static void test_cross_takes_what_is_under_the_cursor() {
    // Console report: "the stash does not get detected at all; I can even
    // highlight it with the right stick, press Cross, and the character
    // neither walks to it nor opens it." The chain read the CONE, never the
    // unit actually under the cursor, so any targetable unit beat it.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[2] = { mkMon(5, 560, 300) };                 // a hostile, right in the cone
    u[1].id = 9; u[1].type = 2; u[1].cls = 7;                // the stash, under the cursor
    u[1].sx = 500; u[1].sy = 300; u[1].interact = true;
    pad::Ctl c; pad::Actions a;
    s.setCursor(500, 296);                                   // inside the stash's box
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 280));                 // the stash (type 2 hover height 20)
}

static void test_pick_at_prefers_the_unit_under_the_cursor() {
    pad::Unit u[2] = { mkMon(1, 400, 300) };
    u[1].id = 2; u[1].type = 2; u[1].sx = 600; u[1].sy = 300; u[1].interact = true;
    CHECK(pad::pick_at(u, 2, 600, 296) == 1);
    CHECK(pad::pick_at(u, 2, 400, 280) == 0);
    CHECK(pad::pick_at(u, 2, 100, 100) == -1);
}

static void test_a_unit_the_game_never_hovers_is_given_up_on() {
    // Console: "tiny animals (scorpions) that are not attackable get detected
    // but char does not run to them, essentially blocking the attack to an
    // attackable target". We cannot tell a critter apart up front, but we can
    // notice that the game refuses to hover it and stop offering it.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 440, 300), mkMon(2, 520, 300) };   // the critter is nearer
    c.aim = true; s.tick(c, x, v, u, 2, a);
    CHECK(s.interacting());
    for (int i = 0; i < 30; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 2, a); }
    CHECK(!s.interacting());                                  // gave up, never pressed
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    // pressing again must now reach the one behind it
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 520, 272));
}

static void test_a_hovered_unit_is_never_given_up_on() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(1, 440, 300) };
    c.aim = true; s.tick(c, x, v, u, 1, a);
    x.selValid = 1; x.selId = 1; x.selType = 1;
    for (int i = 0; i < 40; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 1, a); }
    CHECK(s.interacting());                                   // held on a unit the game confirms
}

// --- second console round, 21/09 ------------------------------------------

static void test_scenery_the_game_never_hovers_is_learned_by_class() {
    // Console, 21/09: "X cible encore des torches". Every type-2 object is
    // flagged interactable by the glue, torches included. The game never
    // hovers one, and scenery of a given class never will -- so learn the
    // CLASS, not the instance, and learn it just by sweeping the cursor over
    // it rather than by wasting a press.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2];
    u[0].id = 1; u[0].type = 2; u[0].cls = 55; u[0].sx = 500; u[0].sy = 300; u[0].interact = true; u[0].selectable = false;  // torch
    u[1].id = 2; u[1].type = 2; u[1].cls = 55; u[1].sx = 560; u[1].sy = 300; u[1].interact = true; u[1].selectable = false;  // another torch
    s.setCursor(500, 296);                              // hovering the first one, game says nothing
    for (int i = 0; i < 10; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 2, a); }
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(!s.interacting());                            // learned: not a target
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    s.setCursor(560, 296);                              // the OTHER torch of the same class
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(!s.interacting());                            // no second press wasted
}

static void test_a_hovered_object_is_never_learned_as_scenery() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1];
    u[0].id = 1; u[0].type = 2; u[0].cls = 7; u[0].sx = 500; u[0].sy = 300; u[0].interact = true;
    s.setCursor(500, 296);
    x.selValid = 1; x.selId = 1; x.selType = 2;         // the game does hover this chest
    for (int i = 0; i < 20; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 1, a); }
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.interacting());
}

static void test_left_stick_switches_target_while_cross_is_held() {
    // Restored. It was taken away to fix "stuck in a melee", on a theory that
    // turned out to be wrong -- the real cause was the walk clicking against
    // the previous frame's hover, which is fixed independently. Movement is
    // gated while Cross is held either way, so the left stick is free to mean
    // "that one instead", which is what was asked for.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 500, 300),      // east
                       mkMon(2, 400, 220) };    // north
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.interacting() && hasAct(a, pad::A_MOVE, 500, 272));   // the nearer one, east
    c.ly = -1.f;                                                   // swing the stick north
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.interacting());                                        // still attacking, not fleeing
    CHECK(hasAct(a, pad::A_MOVE, 400, 192));                       // now the northern one
}

static void test_right_stick_switches_target_while_cross_is_held() {
    // Target steering, on the stick that is free during an interaction.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 500, 300),      // east
                       mkMon(2, 400, 220) };    // north
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.interacting() && hasAct(a, pad::A_MOVE, 500, 272));   // the nearer one, east
    c.ry = -1.f;                                                   // swing the aim north
    for (int i = 0; i < 12; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 2, a); }
    CHECK(hasAct(a, pad::A_MOVE, 400, 192) || s.target().id == 2);
}

static void test_own_corpse_outranks_everything() {
    // Console, 21/09: "after a death, walking back to my corpse, I cannot
    // target it with X -- that should be an absolute priority". A player
    // corpse is a type-0 unit and the glue set neither interact nor hostile
    // on those, so nothing ever offered it.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 470, 300) };
    u[1].id = 7; u[1].type = 0; u[1].cls = 4; u[1].sx = 620; u[1].sy = 300;   // 220, within reach
    u[1].interact = true; u[1].ownCorpse = true;
    s.setCursor(470, 280);                            // cursor parked ON the monster
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 620, 272));          // the corpse wins anyway
}

static void test_interact_reach_is_configurable() {
    // Measuring in world units halved the vertical reach that the old screen
    // metric gave, so the default has to be re-stated rather than inherited.
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit u[1];
    u[0].id = 1; u[0].type = 2; u[0].interact = true;
    u[0].sx = 400; u[0].sy = 440;                   // 140 px below = 280 in the world
    CHECK(pad::pick_interact(u, 1, v, cfg) == 0);   // inside the new default reach
    cfg.reach = 220;
    CHECK(pad::pick_interact(u, 1, v, cfg) == -1);  // and the knob really bites
}

static void test_offscreen_units_are_not_targets() {
    // Console log, 21/09: targets at ecran=(-86,281) and (864,225) on an
    // 800-wide screen. You cannot click what is not drawn, and reaching for
    // one of those beat the monster actually in front of the player.
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit off[2] = { mkMon(1, -86, 281), mkMon(2, 864, 225) };
    CHECK(pad::pick_hostile(off, 2, v, 0.f, 0.f, false, cfg, -1) == -1);
    pad::Unit mixed[2] = { mkMon(1, 864, 225), mkMon(2, 470, 300) };
    CHECK(pad::pick_hostile(mixed, 2, v, 0.f, 0.f, false, cfg, -1) == 1);
    pad::Unit obj[1];
    obj[0].id = 3; obj[0].type = 2; obj[0].sx = -20; obj[0].sy = 300; obj[0].interact = true;
    CHECK(pad::pick_interact(obj, 1, v, cfg) == -1);
}

static void test_untargetable_scenery_is_not_offered_by_proximity() {
    // Console, 21/09: "in Lut Gholein it is the little fires or shadows that
    // sometimes get targeted by X". Those are offered by PROXIMITY, so the
    // cursor never passed over them and the passive learner never saw them.
    // They cost one press to learn -- make that press short: an object is
    // static, so a few frames without a hover is already the answer, unlike a
    // creature whose sprite has to be hunted for.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2];
    u[0].id = 1; u[0].type = 2; u[0].cls = 99; u[0].sx = 430; u[0].sy = 300; u[0].interact = true; u[0].selectable = false;  // a fire
    u[1].id = 2; u[1].type = 2; u[1].cls = 99; u[1].sx = 460; u[1].sy = 300; u[1].interact = true; u[1].selectable = false;  // another
    c.aim = true; s.tick(c, x, v, u, 2, a);
    CHECK(!s.interacting());                              // the game's own flag says untargetable: never offered
}

static void test_a_selectable_object_is_never_written_off_by_a_slow_hover() {
    // Console, 29/09: stash, waypoint, chests and town portal were not
    // targeted. The game only hovers after a rendered frame (~12 fps), so a
    // few ticks without hover on an object the game flags targetable is a
    // race, not a statement: it must stay offerable, even after a long miss.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1];
    u[0].id = 1; u[0].type = 2; u[0].cls = 60; u[0].sx = 500; u[0].sy = 300; u[0].interact = true;   // selectable
    s.setCursor(500, 296);
    for (int i = 0; i < 20; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 1, a); }   // cold learner window
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.interacting());
    for (int i = 0; i < 40; ++i) { a = pad::Actions{}; s.tick(c, x, v, u, 1, a); }   // gives up the press...
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.interacting());                                                          // ...but is offered again
}

static void test_the_game_hover_on_an_object_wins_over_our_box() {
    // The cursor is on the visible sprite of a big object (far above the
    // feet-anchored box), the game says so: press acts on it, cursor stays.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2];
    u[0] = mkMon(1, 420, 300);                                                       // a nearer NPC-ish unit
    u[1].id = 2; u[1].type = 2; u[1].cls = 61; u[1].sx = 560; u[1].sy = 340; u[1].interact = true;
    s.setCursor(560, 250);                                                           // 90 px above the feet
    x.selValid = 1; x.selId = 2; x.selType = 2;
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.interacting());
    CHECK(!hasAct(a, pad::A_MOVE, 420, 272));
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LDOWN, 560, 250));                                        // pressed where it already hovers
}

static void test_a_crowd_of_untargetable_critters_does_not_hide_the_real_monster() {
    // Console, 01/10: "in act 3 it keeps tracking the un-targetable small
    // gecko". Rejection is per instance in a ring: with more critters than
    // slots the oldest was forgotten while the nearest critters were still
    // there, and the real monster behind them was never reached.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[13];
    for (int i = 0; i < 12; ++i) u[i] = mkMon(100 + i, 440 + i * 6, 300);          // 12 critters, the game never hovers them
    u[12] = mkMon(1, 650, 300);                                                    // the real one, farther
    bool reached = false;
    for (int press = 0; press < 20 && !reached; ++press) {
        c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 13, a);
        if (s.interacting() && s.interactId() == 1) { reached = true; break; }
        for (int i = 0; i < 40 && s.interacting(); ++i) {
            if (s.interactId() == 1) { reached = true; break; }
            a = pad::Actions{}; s.tick(c, x, v, u, 13, a);                          // never hovered: it gives up
        }
        c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 13, a);
    }
    CHECK(reached);
}

static void test_walking_out_of_a_crowd_finds_clear_ground() {
    // Console, 21/09: "in a group of monsters, impossible to get out with the
    // stick, my barbarian keeps launching attacks". The walk click landed ON a
    // monster, and D2 reads a left click on a monster as ATTACK, not move --
    // so the stick stopped being a way out. Plug every point the search used
    // to consider and require it to find clear ground regardless.
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c; c.lx = 1.f;
    static const float kAng[7] = { 0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f };
    pad::Unit u[32]; int n = 0;
    for (int shrink = 0; shrink < 3; ++shrink) {
        const float rr = (float)cfg.orbitMax - shrink * 20.f;
        for (int k = 0; k < 7 && n < 32; ++k) {
            const float a = kAng[k] * 3.14159265f / 180.f;
            u[n] = mkMon((uint32_t)(n + 1),
                         400 + (int)(std::cos(a) * rr), 300 + (int)(std::sin(a) * rr));
            ++n;
        }
    }
    int px, py;
    CHECK(pad::orbit_point(v, c, cfg, u, n, &px, &py));
    bool blocked = false;
    for (int i = 0; i < n; ++i) if (pad::in_unit_box(u[i], px, py)) blocked = true;
    CHECK(!blocked);                                  // a way out exists and it was found
    CHECK(px > 400);                                  // and it still goes where the stick points
}

static void test_a_far_corpse_does_not_outrank_what_is_on_us() {
    // "Absolute priority" for our own body was taken literally and it is
    // wrong at range: console, 21/09, the corpse won while it was far across
    // the screen and a monster was in our face. It wins within reach, where
    // pressing Cross can only mean "pick my gear back up".
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 440, 300) };                  // 40 in the world
    u[1].id = 7; u[1].type = 0; u[1].cls = 4; u[1].sx = 780; u[1].sy = 300;   // 380 away
    u[1].interact = true; u[1].ownCorpse = true;
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 440, 272));                  // the monster, not the far body
    // brought within reach, it wins again
    pad::Config cfg2; pad::Assist s2(cfg2); pad::Ctx x2; x2.inGame = true;
    pad::Ctl c2; pad::Actions a2;
    u[1].sx = 600;                                            // 200 away, inside reach
    s2.tick(c2, x2, v, u, 2, a2);
    c2.aim = true; a2 = pad::Actions{}; s2.tick(c2, x2, v, u, 2, a2);
    CHECK(hasAct(a2, pad::A_MOVE, 600, 272));
}

static void test_the_nearest_wins_between_an_object_and_a_monster() {
    // The chain used to put ANY interactable within reach ahead of the
    // nearest enemy, by fixed precedence. With the targetable bit now
    // filtering the scenery out, what is left are real doors and chests --
    // and opening one instead of hitting the monster on top of us is not
    // what the button should mean. Compare distances instead.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(1, 430, 300) };                  // monster, 30 in the world
    u[1].id = 8; u[1].type = 2; u[1].cls = 7; u[1].sx = 600; u[1].sy = 300;   // door, 200
    u[1].interact = true;
    s.tick(c, x, v, u, 2, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_MOVE, 430, 272));                  // the monster is nearer: hit it
    // and the other way round, the door wins
    pad::Config cfg2; pad::Assist s2(cfg2); pad::Ctx x2; x2.inGame = true;
    pad::Ctl c2; pad::Actions a2;
    pad::Unit w[2] = { mkMon(1, 700, 300) };                  // monster, 300
    w[1] = u[1]; w[1].sx = 450;                               // door, 50
    s2.tick(c2, x2, v, w, 2, a2);
    c2.aim = true; a2 = pad::Actions{}; s2.tick(c2, x2, v, w, 2, a2);
    CHECK(hasAct(a2, pad::A_MOVE, 450, 280));
}

static void test_the_walk_waits_for_the_hover_to_clear() {
    // Console, 21/09: "when X is not held we still attack -- the left stick
    // moves the mouse and clicks, and if there are mobs under the cursor it
    // should not click, because there it clicks from the very start."
    // Exactly right: a click resolves against the hover of the PREVIOUS
    // rendered frame, so moving and pressing in the same breath applies the
    // click to whatever the cursor was over before -- a monster, which D2
    // reads as ATTACK. The pickup and interact paths already wait for this;
    // the walk did not.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(1, 500, 300) };
    x.selValid = 1; x.selId = 1; x.selType = 1;        // the game is hovering a monster
    c.lx = 1.f; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE));                      // the cursor goes to the walk point
    CHECK(!hasAct(a, pad::A_LDOWN));                    // but nothing is pressed yet
    x.selValid = 0;                                     // the game re-reads: clear ground
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));                     // now it is a walk order
}

static void test_the_walk_still_starts_at_once_on_clear_ground() {
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    c.lx = 1.f; s.tick(c, x, v, nullptr, 0, a);         // nothing hovered: no reason to wait
    CHECK(hasAct(a, pad::A_LDOWN));
}

static void test_the_walk_never_stalls_for_long() {
    // If the hover never clears, walk anyway rather than stand there.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(1, 500, 300) };
    x.selValid = 1; x.selId = 1; x.selType = 1;
    c.lx = 1.f;
    int ticks = 0; bool pressed = false;
    while (ticks < 12 && !pressed) {
        a = pad::Actions{}; s.tick(c, x, v, u, 1, a); ++ticks;
        if (hasAct(a, pad::A_LDOWN)) pressed = true;
    }
    CHECK(pressed && ticks <= 6);
}

static void test_the_cursor_overrides_the_targetable_filter() {
    // Console, 21/09: shrines and the stash can no longer be approached or
    // activated, "even hovering above them via right-stick". The targetable
    // bit says they are not selectable, whatever the reason, and that filter
    // was applied everywhere. Pointing at something is an explicit statement
    // of intent, so the cursor overrides it -- the filter keeps the PROXIMITY
    // list clean, which is all it was ever needed for.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1];
    u[0].id = 1; u[0].type = 2; u[0].cls = 7; u[0].sx = 500; u[0].sy = 300;
    u[0].interact = true; u[0].selectable = false;    // a shrine the bit disowns
    CHECK(pad::pick_interact(u, 1, v, cfg) == -1);    // not offered by proximity
    CHECK(pad::pick_at(u, 1, 500, 296) == 0);         // but reachable by pointing at it
    s.setCursor(500, 296);
    s.tick(c, x, v, u, 1, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 280));
}

static void test_cross_clicks_the_hud() {
    // Console, 21/09: Cross does not work as a left click on the stats and
    // skills buttons, the menu bar or the belt. Those are not panels, so the
    // scheme was in world mode and Cross meant "act on the world". Down in
    // the HUD band there is no world to act on, and the only way the cursor
    // got there is the player driving it.
    pad::Config cfg; pad::Assist s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(1, 430, 300) };           // a monster right next to us
    s.setCursor(300, 560);                             // cursor on the HUD
    s.tick(c, x, v, u, 1, a);
    c.aim = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN, 300, 560));          // a plain click, not an attack
    CHECK(!s.interacting());
    c.aim = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP));
}


static bool scan(const char* s) { return ctl::binds_aim(s, std::strlen(s)); }
static void test_controls_scan() {
    CHECK(scan(""));                                   // built-in default: l=aim
    CHECK(scan("square=items\nr+up=f5\n"));
    CHECK(scan("cross=aim\n"));
    CHECK(scan("  Croix = AIM  # comment\n"));
    CHECK(scan("square=items\r\nr+triangle=aim\r\n"));
    CHECK(scan("l+down=aim"));
    CHECK(scan("l=aim\n"));
    CHECK(scan("r = aim\n"));
    CHECK(scan("select=aim\n"));
    CHECK(scan("#l=lclick\n"));                        // a comment changes nothing
    CHECK(scan("l=lclick # aim\n") == false);          // explicit opt-out
    CHECK(!scan("l = none\n"));
    CHECK(!scan("l=lclick\nl+cross=inv\n"));
    CHECK(scan("l=lclick\ncross=aim\n"));             // another key still binds it
    CHECK(scan("l=lclick\nl=aim\n"));                 // last l= wins
    CHECK(!scan("l=aim\nl=lclick\n"));
    CHECK(scan("l=\n"));                               // empty value: ignored, default stays
    CHECK(scan("cross=none # aim\n"));
    CHECK(scan("scheme=aim\n"));
    CHECK(scan("aim=1\n"));
    CHECK(scan("r+select=aim\n"));
    CHECK(scan("bogus=aim\n"));
    CHECK(ctl::button_bit("Cross") == 0x4000 && ctl::button_bit("rond") == 0x2000);
    CHECK(ctl::button_bit("nope") == 0);

    // The scan and the loader share ONE action table: what one calls aim, so does the other.
    CHECK(scan("l=aim_assist\n"));
    CHECK(scan("l=lclick\ncross=aim_assist\n"));
    CHECK(!scan("l=lclick\ncross=aimm\n"));            // a typo binds nothing
    CHECK(scan("l=lclik\n"));                            // typo: loader keeps the default, so must the scan
    CHECK(scan("l=leftclick\n"));
    CHECK(scan("l=vk:0x1FF\n"));                         // not a virtual key: refused
    CHECK(!scan("l=vk:0x4F\n"));                         // a real one is a deliberate opt-out
    CHECK(scan("\xEF\xBB\xBFl=lclick\ncross=aim\n"));   // BOM does not hide the first key
    CHECK(!scan("\xEF\xBB\xBFl=lclick\n"));
}

static void test_controls_parse() {
    ctl::ActSpec a;
    CHECK(ctl::parse_action("AIM", &a) && a.kind == ctl::ACT_AIM);
    CHECK(ctl::parse_action("aim_assist", &a) && a.kind == ctl::ACT_AIM);
    CHECK(ctl::parse_action("vk:0x4F", &a) && a.kind == ctl::ACT_KEY && a.vk == 0x4F);
    CHECK(ctl::parse_action("vk:79", &a) && a.vk == 79);
    CHECK(!ctl::parse_action("vk:", &a) && !ctl::parse_action("vk:zz", &a) && !ctl::parse_action("vk:0", &a));
    CHECK(!ctl::parse_action("", &a) && !ctl::parse_action("lclik", &a));
    char l1[] = "  Sens = 18   # right stick  ", *k, *v;
    CHECK(ctl::split_line(l1, &k, &v) && !strcmp(k, "Sens") && !strcmp(v, "18"));
    char l2[] = "# l=aim", l3[] = "   ", l4[] = "=5", l5[] = "nokey";
    CHECK(!ctl::split_line(l2, &k, &v) && !ctl::split_line(l3, &k, &v) && !ctl::split_line(l4, &k, &v) && !ctl::split_line(l5, &k, &v));
    char l6[] = "l=aim\r\n";
    CHECK(ctl::split_line(l6, &k, &v) && !strcmp(v, "aim"));
    double d, lo, hi;
    CHECK(ctl::numeric_key("orbit", "70", &d, &lo, &hi) == 1 && d == 70);
    CHECK(ctl::numeric_key("ORBIT", "1000", &d, &lo, &hi) == -1 && lo == 10 && hi == 400);   // the old per-mille value
    CHECK(ctl::numeric_key("deadzone", "15", &d, &lo, &hi) == -1);                          // a percentage, not 0..1
    CHECK(ctl::numeric_key("deadzone", "0.15", &d, &lo, &hi) == 1);
    CHECK(ctl::numeric_key("sens", "40", &d, &lo, &hi) == 1);
    CHECK(ctl::numeric_key("sens", "abc", &d, &lo, &hi) == -1);
    CHECK(ctl::numeric_key("sens", "", &d, &lo, &hi) == -1);
    CHECK(ctl::numeric_key("sens", "18px", &d, &lo, &hi) == -1);
    CHECK(ctl::numeric_key("aim", "1", &d, &lo, &hi) == 0);
    bool b = true;
    CHECK(ctl::parse_switch("off", &b) && !b && ctl::parse_switch("1", &b) && b && ctl::parse_switch("Non", &b) && !b);
    CHECK(!ctl::parse_switch("maybe", &b) && !ctl::parse_switch("", &b));
    CHECK(ctl::bom_len("\xEF\xBB\xBFx", 4) == 3 && ctl::bom_len("x", 1) == 0 && ctl::bom_len("\xEF", 1) == 0);
}

int main() {
    test_controls_scan();
    test_controls_parse();
    test_projection();
    test_clamp_and_box();
    test_pick_hostile();
    test_pick_interact();
    test_hover_table();
    test_orbit();
    test_scheme_move();
    test_aim_interact();
    test_left_button_always_released();
    test_interact_height_retry_paces_itself();
    test_hold_follows_the_hover();
    test_scheme_panel_and_leave();
    test_right_stick_is_a_free_cursor();
    test_free_cursor_reaches_the_hud();
    test_aim_cone_follows_the_cursor();
    test_the_walk_leaves_the_cursor_alone();
    test_the_walk_waits_for_the_hover_to_clear();
    test_a_selectable_object_is_never_written_off_by_a_slow_hover();
    test_the_game_hover_on_an_object_wins_over_our_box();
    test_a_crowd_of_untargetable_critters_does_not_hide_the_real_monster();
    test_the_walk_still_starts_at_once_on_clear_ground();
    test_the_walk_never_stalls_for_long();
    test_repick_follows_where_the_player_now_aims();
    test_panel_cursor_sums_both_sticks_before_rounding();
    test_cross_is_the_context_button();
    test_cross_prefers_a_close_object_when_not_pointing_at_anything();
    test_distance_is_measured_in_the_world_not_on_screen();
    test_interact_reach_is_round_in_the_world();
    test_cross_takes_what_is_under_the_cursor();
    test_pick_at_prefers_the_unit_under_the_cursor();
    test_a_unit_the_game_never_hovers_is_given_up_on();
    test_a_hovered_unit_is_never_given_up_on();
    test_scenery_the_game_never_hovers_is_learned_by_class();
    test_a_hovered_object_is_never_learned_as_scenery();
    test_untargetable_scenery_is_not_offered_by_proximity();
    test_walking_out_of_a_crowd_finds_clear_ground();
    test_left_stick_switches_target_while_cross_is_held();
    test_right_stick_switches_target_while_cross_is_held();
    test_own_corpse_outranks_everything();
    test_a_far_corpse_does_not_outrank_what_is_on_us();
    test_the_nearest_wins_between_an_object_and_a_monster();
    test_the_cursor_overrides_the_targetable_filter();
    test_cross_clicks_the_hud();
    test_interact_reach_is_configurable();
    test_offscreen_units_are_not_targets();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
