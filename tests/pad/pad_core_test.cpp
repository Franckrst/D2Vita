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
    pad::Unit u; u.id = id; u.type = 1; u.cls = 10; u.sx = sx; u.sy = sy; u.hostile = hostile; return u;
}

static void test_pick_hostile() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    pad::Unit u[4] = { mkMon(1, 600, 300), mkMon(2, 450, 300), mkMon(3, 400, 100, false), mkMon(4, 400, 320) };
    u[3].hostile = false;                    // corpse, flagged by the glue
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

static pad::Unit mkItem(uint32_t id, int sx, int sy) {
    pad::Unit u; u.id = id; u.type = 4; u.sx = sx; u.sy = sy; u.interact = true; return u;
}

static void test_nav_direction() {
    // "plus" layout centered on Z (400,300): one item per cardinal direction,
    // all at distance 100 so no direction's pick is ambiguous with another.
    pad::Unit u[6] = {
        mkItem(20, 400, 300),   // Z: the cursor start
        mkItem(21, 500, 300),   // right of Z, dist 100
        mkItem(23, 300, 300),   // left of Z, dist 100
        mkItem(24, 400, 200),   // above Z, dist 100
        mkItem(25, 400, 400),   // below Z, dist 100
    };
    CHECK(pad::nav_direction(u, 5, 0, pad::D_RIGHT) == 1);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_LEFT)  == 2);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_UP)    == 3);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_DOWN)  == 4);

    // no candidate in a direction: cursor does not move (returns -1)
    pad::Unit only[2] = { mkItem(30, 400, 300), mkItem(31, 500, 300) };
    CHECK(pad::nav_direction(only, 2, 0, pad::D_LEFT) == -1);
    CHECK(pad::nav_direction(only, 2, 0, pad::D_UP)   == -1);
    CHECK(pad::nav_direction(only, 1, 0, pad::D_RIGHT) == -1);   // single item: no candidates at all

    // non-item units are never candidates, even when nearer than the real item
    pad::Unit withMon[3] = { mkItem(40, 400, 300), mkItem(41, 500, 300) };
    withMon[2] = mkItem(42, 450, 300); withMon[2].type = 1; withMon[2].hostile = true;
    CHECK(pad::nav_direction(withMon, 3, 0, pad::D_RIGHT) == 1);   // finds item 41, not the closer monster

    // a target at exactly 45 degrees qualifies for BOTH adjacent directions
    // (both tests use an inclusive >=), with nothing else around to compete
    pad::Unit diag[2] = { mkItem(50, 400, 300), mkItem(51, 500, 400) };
    CHECK(pad::nav_direction(diag, 2, 0, pad::D_RIGHT) == 1);
    CHECK(pad::nav_direction(diag, 2, 0, pad::D_DOWN)  == 1);

    // tie in distance: lower id wins, regardless of array order
    pad::Unit tie[3] = { mkItem(60, 400, 300), mkItem(41, 500, 250), mkItem(40, 500, 350) };
    CHECK(pad::nav_direction(tie, 3, 0, pad::D_RIGHT) == 2);   // index 2 = id 40, the lower id

    // two items on the SAME ground tile (identical sx,sy): without a label
    // position neither dx nor dy is ever nonzero, so no direction is
    // "dominant" and the D-pad can never reach the second one at all.
    pad::Unit sameTile[2] = { mkItem(70, 400, 300), mkItem(71, 400, 300) };
    CHECK(pad::nav_direction(sameTile, 2, 0, pad::D_UP)   == -1);
    CHECK(pad::nav_direction(sameTile, 2, 0, pad::D_DOWN) == -1);

    // the game draws their Alt labels stacked (same x, one above the other):
    // once hasLabel/lx/ly are set, navigation follows the labels, not the
    // shared tile — reachable both ways, symmetric with each as the origin.
    pad::Unit labelled[2] = { mkItem(70, 400, 300), mkItem(71, 400, 300) };
    labelled[0].hasLabel = true; labelled[0].lx = 400; labelled[0].ly = 260;   // name line (topmost)
    labelled[1].hasLabel = true; labelled[1].lx = 400; labelled[1].ly = 280;   // affix line, below it
    CHECK(pad::nav_direction(labelled, 2, 0, pad::D_DOWN) == 1);
    CHECK(pad::nav_direction(labelled, 2, 1, pad::D_UP)   == 0);
}

static void test_nearest_item() {
    pad::View v = mkView();   // player projects to (400,300)
    pad::Unit u[3] = { mkItem(70, 450, 350), mkItem(71, 400, 250) };
    // dist(70) = sqrt(50^2+50^2) ~= 70.7 ; dist(71) = 50 -> 71 is nearest
    CHECK(pad::nearest_item(u, 2, v) == 1);

    // no cap: an item far outside L's 220px catchment is still found when it's
    // the only one present
    pad::Unit far[1] = { mkItem(72, 400, 900) };
    CHECK(pad::nearest_item(far, 1, v) == 0);

    // non-item units are ignored
    pad::Unit withMon[2]; withMon[0] = mkItem(73, 500, 500);
    withMon[1].id = 74; withMon[1].type = 1; withMon[1].sx = 401; withMon[1].sy = 301;
    CHECK(pad::nearest_item(withMon, 2, v) == 0);

    // no items at all
    pad::Unit mon[1]; mon[0].id = 75; mon[0].type = 1; mon[0].sx = 400; mon[0].sy = 300;
    CHECK(pad::nearest_item(mon, 1, v) == -1);
    CHECK(pad::nearest_item(nullptr, 0, v) == -1);
}

static bool hasAct(const pad::Actions& a, pad::ActKind k, int x = -1, int y = -1) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k && (x < 0 || a.v[i].a == x) && (y < 0 || a.v[i].b == y)) return true;
    return false;
}
static int actIndex(const pad::Actions& a, pad::ActKind k) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k) return i;
    return -1;
}

static void test_scheme_cast() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    pad::Ctl c; pad::Actions a;
    s.tick(c, x, v, u, 1, a);                                   // settle, nothing pressed
    CHECK(a.n == 0);
    CHECK(s.target().has && s.target().id == 5 && !s.target().verified);
    // press Cross: F1, cursor on the target (28 px above its feet), right button down
    c.buttons = pad::B_CROSS; a = pad::Actions{};
    s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x70));
    CHECK(hasAct(a, pad::A_MOVE, 500, 272));
    CHECK(hasAct(a, pad::A_RDOWN, 500, 272));
    CHECK(actIndex(a, pad::A_KEY) < actIndex(a, pad::A_RDOWN));
    CHECK(s.casting() && s.cursorOwned());
    // held, game does not hover it yet: second attempt height (14)
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 286));
    // now the game hovers it: verified, learned
    x.selValid = 1; x.selId = 5; x.selType = 1;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.target().verified);
    // release: right button up, no more cast
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP) && !s.casting());
    // next cast of the same class starts at the learned height (14)
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 286));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // R + Triangle = slot 8 (F8)
    c.buttons = pad::B_R | pad::B_TRI; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x77));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP));
    // no hostile: ground cast along the fallback direction
    c.buttons = pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEY, 0x71) && hasAct(a, pad::A_RDOWN));
    CHECK(!s.target().has);
}

static void test_scheme_move() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
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
    // moving then casting: the left button is released for the cast, re-pressed after
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(actIndex(a, pad::A_LUP) >= 0 && actIndex(a, pad::A_LUP) < actIndex(a, pad::A_RDOWN));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // spec 5.1 "reprend au relachement": release ends the cast AND resumes movement
    // in the same tick (worldTick runs the cast section before the left-stick one).
    CHECK(hasAct(a, pad::A_RUP) && hasAct(a, pad::A_LDOWN));
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);              // stick still pushed, point unchanged: steady state
    CHECK(a.n == 0);
}

static void test_scheme_interact_and_keys() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[2] = { mkMon(5, 500, 300) };
    u[1].id = 8; u[1].type = 2; u[1].cls = 7; u[1].sx = 430; u[1].sy = 310; u[1].interact = true;
    pad::Ctl c; pad::Actions a;
    c.buttons = pad::B_L; s.tick(c, x, v, u, 2, a);                  // L: the chest wins over the monster
    CHECK(hasAct(a, pad::A_MOVE, 430, 290) && !hasAct(a, pad::A_LDOWN));   // move first, press next tick
    x.selValid = 1; x.selId = 8; x.selType = 2;
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LDOWN, 430, 290));
    x.selValid = 0; x.selId = 0; x.selType = 0;
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LUP));
    // L with nothing around and no target: no click at all
    a = pad::Actions{}; c.buttons = pad::B_L; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
    // R then L = Alt held ; released with either
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x12) && !hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_L; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x12));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    // D-pad up = potion 1 ; R + D-pad left = Shift + potion 2 (merc)
    c.buttons = pad::B_UP; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x31));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x31));
    c.buttons = pad::B_R | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x10) && hasAct(a, pad::A_KEYDOWN, 0x32));
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x32) && hasAct(a, pad::A_KEYUP, 0x10));
    // R + Start = W ; Start = Escape
    c.buttons = pad::B_R | pad::B_START; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x57));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x57));
    c.buttons = pad::B_START; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x1B));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x1B));
}

static void test_scheme_loot_browse() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[3];
    u[0] = mkItem(50, 480, 300);                                 // dist 80 from player (400,300): nearest
    u[1] = mkItem(51, 300, 300);                                 // dist 100: farther, to the LEFT of u[0]
    u[2].id = 52; u[2].type = 1; u[2].sx = 470; u[2].sy = 300; u[2].hostile = true;
    // ^ monster: closer to u[1] (dist 170) than u[0] is (dist 180) -- must never be selected
    pad::Ctl c; pad::Actions a;

    // R then L: Alt activates; cursor defaults to the item nearest the player
    c.buttons = pad::B_R; s.tick(c, x, v, u, 3, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x12));
    CHECK(s.lootCursor().has && s.lootCursor().id == 50);
    CHECK(!s.target().has);   // hostile-target diamond suppressed while browsing loot

    // D-pad left: moves to item 51; does NOT also drink a belt potion
    c.buttons = pad::B_R | pad::B_L | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 51);
    CHECK(!hasAct(a, pad::A_KEYDOWN, 0x32));
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);

    // D-pad right: back to item 50 -- the closer monster must never win
    c.buttons = pad::B_R | pad::B_L | pad::B_RIGHT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 50);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);

    // left stick pushed during Alt: no movement click, character stays put (spec 3.6)
    c.buttons = pad::B_R | pad::B_L; c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(!hasAct(a, pad::A_LDOWN) && !s.cursorOwned());
    c.lx = 0.f;

    // right stick pushed during Alt: no aim reticle (spec 3.6)
    c.buttons = pad::B_R | pad::B_L; c.rx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(!s.aimActive());
    c.rx = 0.f;

    // Croix: MOVES onto the item, and deliberately does NOT press yet. Clicking
    // in the same breath made the game act on whatever it hovered last, which
    // it reads as "walk there" -- console 20/09: the label lit up and the
    // character walked off without picking anything up.
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_MOVE, 480, 294) && !hasAct(a, pad::A_LDOWN));
    CHECK(!s.casting());                                          // did NOT also start a cast (any slot)

    // next tick, the game reports it hovers item 50: NOW the button goes down
    x.selValid = 1; x.selId = 50; x.selType = 4;
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_LDOWN, 480, 294));
    x.selValid = 0; x.selId = 0; x.selType = 0;

    // D-pad is locked out while the pickup is held
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 50);

    // the picked-up item vanishes from the unit list (normal case: it was just
    // grabbed) while Croix is still held -- the reported cursor must NOT drift
    // to a different item; the reacquire-fallback must stay locked out by
    // !interact_ until Croix is released
    pad::Unit withoutPickedItem[2] = { u[1], u[2] };   // item 50 gone, item 51 + the monster remain
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, withoutPickedItem, 2, a);
    CHECK(!s.lootCursor().has);                        // gone from view -- critically NOT drifted to item 51

    // release Croix: LUP, browsing resumes
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_LUP));

    // move to item 51 before Alt ends, so re-entry below can prove it re-derives the
    // nearest item rather than resuming this stale selection
    c.buttons = pad::B_R | pad::B_L | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 51);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);

    // release L (R still held): Alt ends, selection forgotten
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x12));
    CHECK(!s.lootCursor().has);

    // re-entering Alt after it fully ended re-derives nearest-to-player (50), rather
    // than resuming the stale selection (51) from before Alt ended
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 50);
}

// Every A_LDOWN must have a lift on EVERY exit, not just the button's own
// release. Console-visible failure: hold L on a town NPC, the dialogue opens,
// the mode switches to PANEL -- and the left button stays down forever, so
// every later cursor move is a drag.
static void test_left_button_always_released() {
    pad::View v = mkView();
    pad::Unit u[1] = { mkItem(50, 400, 300) };
    pad::Unit chest[1]; chest[0].id = 51; chest[0].type = 2; chest[0].cls = 7;
    chest[0].sx = 400; chest[0].sy = 300; chest[0].interact = true;
    auto countUp = [](const pad::Actions& a) {
        int k = 0; for (int i = 0; i < a.n; ++i) if (a.v[i].k == pad::A_LUP) ++k; return k; };

    // A: L-interact pressed and held, then a panel opens
    { pad::Config cfg; pad::Scheme s(cfg); pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
      c.buttons = pad::B_L; s.tick(c, x, v, chest, 1, a);
      x.selValid = 1; x.selId = 51; x.selType = 2;
      a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(hasAct(a, pad::A_LDOWN));
      x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(countUp(a) == 1); }

    // B: L-interact pressed and held, then the game is left
    { pad::Config cfg; pad::Scheme s(cfg); pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
      c.buttons = pad::B_L; s.tick(c, x, v, chest, 1, a);
      x.selValid = 1; x.selId = 51; x.selType = 2;
      a = pad::Actions{}; s.tick(c, x, v, chest, 1, a);
      CHECK(hasAct(a, pad::A_LDOWN));
      a = pad::Actions{}; s.leave(a);
      CHECK(countUp(a) == 1); }

    // C: Alt pickup pressed, then a panel opens
    { pad::Config cfg; pad::Scheme s(cfg); pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
      c.buttons = pad::B_R; s.tick(c, x, v, u, 1, a);
      c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
      c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
      x.selValid = 1; x.selId = 50; x.selType = 4;
      a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
      CHECK(hasAct(a, pad::A_LDOWN));
      x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
      CHECK(countUp(a) == 1); }
}

// D2 unit ids are unique per type only: a monster and an item can share one.
// Matching on the id alone made the loot marker jump onto the monster, and
// Croix then clicked it -- an attack, never a pick-up.
static void test_id_collision_across_types() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[2] = { mkMon(50, 300, 250), mkItem(50, 480, 300) };   // same id, monster listed FIRST
    c.buttons = pad::B_R; s.tick(c, x, v, u, 2, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(s.lootCursor().has && s.lootCursor().sx == 480);
    a = pad::Actions{}; s.tick(c, x, v, u, 2, a);                     // held: must stay on the ITEM
    CHECK(s.lootCursor().sx == 480 && s.lootCursor().sy == 300);
}

// The item disappears before the press (someone else grabbed it, it scrolled
// off): the delayed press must be cancelled, not fired at the last known spot
// -- the game would read that as "walk there", which is what the two-step
// press exists to avoid in the first place.
static void test_pickup_cancelled_when_item_vanishes() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkItem(50, 480, 300) };
    c.buttons = pad::B_R; s.tick(c, x, v, u, 1, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(!hasAct(a, pad::A_LDOWN));                                  // move only, as designed
    for (int i = 0; i < 8; ++i) {                                     // item gone, well past the 5-tick fallback
        a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
        CHECK(!hasAct(a, pad::A_LDOWN));
    }
}

// A click with no hover at all is a walk order: the character wanders off
// instead of picking anything up. The press must never fire on the timeout
// alone -- only once the game reports it hovers an item.
static void test_pickup_never_presses_without_hover() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkItem(50, 480, 300) };
    c.buttons = pad::B_R; s.tick(c, x, v, u, 1, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    for (int i = 0; i < 12; ++i) {                    // the game never reports a hover
        a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
        CHECK(!hasAct(a, pad::A_LDOWN));
    }
    // it hovers a DIFFERENT item (our id went stale): that one is under the
    // cursor and highlighted, so after the grace ticks the press goes through
    x.selValid = 1; x.selId = 77; x.selType = 4;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
}

// A chest or NPC is hit-tested against the cursor's own position, so the
// cursor height matters -- and each candidate height needs a RENDERED frame
// before the game can answer. Escalating every tick burned all four
// candidates before a single frame had been drawn. The button goes down
// after one tick either way: it is HELD, and the game re-reads the hover on
// every frame while it is, so it corrects itself as soon as a height lands.
static void test_interact_height_retry_paces_itself() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1]; u[0].id = 60; u[0].type = 2; u[0].cls = 7;      // a chest
    u[0].sx = 400; u[0].sy = 300; u[0].interact = true;
    c.buttons = pad::B_L; s.tick(c, x, v, u, 1, a);
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

// Holding L on a MOVING unit: the button follows the game's hover, going down
// when it confirms the unit and lifting when it loses it -- never issuing the
// hover-less click, which the game reads as "walk to that point".
static void test_hold_follows_the_hover() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; pad::Ctl c; pad::Actions a;
    pad::Unit u[1] = { mkMon(70, 460, 300) };
    c.buttons = pad::B_L; s.tick(c, x, v, u, 1, a);                 // no interactable: falls back to the target
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
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true;
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    pad::Ctl c; pad::Actions a;
    // a cast is held when a panel opens: it is released on the mode change
    c.buttons = pad::B_CROSS; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RDOWN));
    x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP) && !s.casting());
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // panel: Circle = Escape ; Cross = click at the cursor ; Square = Shift held
    s.setCursor(200, 200);
    c.buttons = pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x1B));
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN, 200, 200));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP, 200, 200));
    c.buttons = pad::B_SQR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x10));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x10));
    // left stick moves the free cursor in a panel
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE) && s.cx() > 200);
    c.lx = 0.f;
    // skill tree: faces are hotkeys, not clicks
    x.skillTree = true;
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x70) && !hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_R | pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x75));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // leave() while a Shift is held releases it
    c.buttons = pad::B_SQR; x.skillTree = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    a = pad::Actions{}; s.leave(a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x10));
}

int main() {
    test_projection();
    test_clamp_and_box();
    test_pick_hostile();
    test_pick_interact();
    test_hover_table();
    test_orbit();
    test_ground_point();
    test_nav_direction();
    test_nearest_item();
    test_scheme_cast();
    test_scheme_move();
    test_scheme_interact_and_keys();
    test_scheme_loot_browse();
    test_left_button_always_released();
    test_id_collision_across_types();
    test_pickup_cancelled_when_item_vanishes();
    test_pickup_never_presses_without_hover();
    test_interact_height_retry_paces_itself();
    test_hold_follows_the_hover();
    test_scheme_panel_and_leave();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
