// tools/tests/item_assist_test.cpp — host-side oracle for item_assist.h.
//
// Pure logic: what still needs a console is whether the label rects read
// from the guest are the ones the game hit-tests, and how the hover lag
// plays out at the real frame rate.
#include "platform/item_assist.h"
#include <cstdio>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace d2ia;

// 60x12 label centred on (x,y).
static Label L(uint32_t id, int x, int y) { return Label{x - 30, y - 6, x + 30, y + 6, id}; }

static In mk(const std::vector<Label>& v, int cx = 400, int cy = 300) {
    In i{}; i.labels = v.data(); i.n = (int)v.size();
    i.hover = Hover{false, 0, 0}; i.cx = cx; i.cy = cy; i.refX = 400; i.refY = 280;
    return i;
}

static void t_nav_direction() {
    std::vector<Label> v = { L(1,400,300), L(2,500,300), L(3,300,300), L(4,400,200), L(5,400,400), L(6,420,500) };
    CHECK(v[nav_direction(v.data(), 6, 0, DIR_RIGHT)].id == 2, "right");
    CHECK(v[nav_direction(v.data(), 6, 0, DIR_LEFT)].id == 3, "left");
    CHECK(v[nav_direction(v.data(), 6, 0, DIR_UP)].id == 4, "up");
    CHECK(v[nav_direction(v.data(), 6, 0, DIR_DOWN)].id == 5, "down: nearest below, not the far one");
    CHECK(nav_direction(v.data(), 6, 1, DIR_RIGHT) == -1, "nothing right of the rightmost");
    // |minor| > |major| is not "in that direction"
    std::vector<Label> d = { L(1,400,300), L(2,420,400) };
    CHECK(nav_direction(d.data(), 2, 0, DIR_RIGHT) == -1, "steep diagonal is not right");
    CHECK(nav_direction(d.data(), 2, 0, DIR_DOWN) == 1, "steep diagonal is down");
    // tie on distance -> smaller id, whatever the array order
    std::vector<Label> t1 = { L(9,400,300), L(7,500,300), L(3,500,300) };
    CHECK(t1[nav_direction(t1.data(), 3, 0, DIR_RIGHT)].id == 3, "tie -> smaller id");
    CHECK(nav_direction(v.data(), 6, -1, DIR_UP) == -1 && nav_direction(v.data(), 6, 6, DIR_UP) == -1, "bad index");
}

static void t_warmup_ignores_stale_labels() {
    std::vector<Label> v = { L(1,100,100) };
    State s; s.reset(3);
    for (int k = 0; k < 3; ++k) { Out o = s.tick(mk(v)); CHECK(!o.move && !s.warm() == (k < 2), "warm-up tick %d silent", k); }
    CHECK(s.warm(), "warm after 3 ticks");
    Out o = s.tick(mk(v));
    CHECK(o.move && o.mx == 100 && o.my == 100 && s.focus() == 1, "focus acquired after warm-up");
}

static void t_initial_focus_nearest_reference() {
    std::vector<Label> v = { L(1,100,100), L(2,410,290), L(3,700,500) };
    State s; s.reset(0);
    Out o = s.tick(mk(v, 50, 50));
    CHECK(s.focus() == 2 && o.move && o.mx == 410 && o.my == 290, "starts at the item nearest the character, not the cursor");
    // no movement while nothing changes: the stick stays usable
    In i = mk(v, 410, 290); Out o2 = s.tick(i);
    CHECK(!o2.move && !o2.lmb, "idle tick is silent");
}

static void t_dpad_moves_focus_and_cursor() {
    std::vector<Label> v = { L(1,400,300), L(2,500,300), L(3,400,400) };
    State s; s.reset(0); s.tick(mk(v));            // focus -> nearest to (400,280) = id 1
    CHECK(s.focus() == 1, "focus 1");
    In i = mk(v, 400, 300); i.dirEdge = DIR_RIGHT; Out o = s.tick(i);
    CHECK(s.focus() == 2 && o.move && o.mx == 500 && o.my == 300, "right -> id 2");
    i = mk(v, 500, 300); i.dirEdge = DIR_RIGHT; o = s.tick(i);
    CHECK(s.focus() == 2 && !o.move, "no candidate: focus and cursor stay");
    i = mk(v, 500, 300); i.dirEdge = DIR_LEFT | DIR_DOWN; o = s.tick(i);
    CHECK(s.focus() == 1, "several directions in one tick: one step, fixed priority (left before down)");
}

static void t_manual_cursor_follows_label() {
    std::vector<Label> v = { L(1,400,300), L(2,600,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v, 610, 302); i.cursorMoved = true; Out o = s.tick(i);
    CHECK(s.focus() == 2 && !o.move, "stick onto a label -> focus follows, cursor not yanked");
    i = mk(v, 500, 100); i.cursorMoved = true; s.tick(i);
    CHECK(s.focus() == 2, "stick over empty space keeps the focus");
}

static void t_pickup_waits_for_hover() {
    std::vector<Label> v = { L(1,400,300), L(2,500,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v, 400, 300); i.confirmEdge = true; i.confirmHeld = true;
    Out o = s.tick(i);
    CHECK(o.move && o.mx == 400 && !o.lmb && s.pressing(), "press tick: move only, no button");
    // cursor arrived, game has not reported a hover yet
    i = mk(v, 400, 300); i.confirmHeld = true; o = s.tick(i);
    CHECK(!o.lmb, "no hover yet -> still no button");
    i.hover = Hover{true, UNIT_ITEM, 1}; o = s.tick(i);
    CHECK(o.lmb, "hover on the target -> button down");
    i = mk(v, 400, 300); i.confirmHeld = true; i.hover = Hover{true, UNIT_ITEM, 1}; o = s.tick(i);
    CHECK(o.lmb, "stays down while held");
    i.confirmHeld = false; o = s.tick(i);
    CHECK(!o.lmb && !s.pressing(), "release lifts the button");
}

static void t_pickup_never_fires_without_hover() {
    std::vector<Label> v = { L(1,400,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v); i.confirmEdge = true; i.confirmHeld = true; s.tick(i);
    for (int k = 0; k < 40; ++k) {
        In j = mk(v, 400, 300); j.confirmHeld = true;
        j.hover = Hover{true, 1 /*monster*/, 1};
        CHECK(!s.tick(j).lmb, "a non-item hover never counts (tick %d)", k);
    }
}

static void t_pickup_accepts_other_item_after_a_few_ticks() {
    std::vector<Label> v = { L(1,400,300), L(2,405,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v); i.confirmEdge = true; i.confirmHeld = true; s.tick(i);
    bool fired = false; int at = -1;
    for (int k = 0; k < 10 && !fired; ++k) {
        In j = mk(v, 400, 300); j.confirmHeld = true; j.hover = Hover{true, UNIT_ITEM, 2};
        fired = s.tick(j).lmb; at = k;
    }
    CHECK(fired && at >= 3, "another item's hover is accepted, but not immediately (fired at %d)", at);
}

static void t_item_vanishes_before_press() {
    std::vector<Label> v = { L(1,400,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v); i.confirmEdge = true; i.confirmHeld = true; s.tick(i);
    std::vector<Label> gone;
    In j = mk(gone, 400, 300); j.confirmHeld = true; j.hover = Hover{true, UNIT_ITEM, 1};
    Out o = s.tick(j);
    CHECK(!o.lmb && !o.move, "item gone: no button, no move to its old spot");
    v = { L(3,300,300) };
    j = mk(v, 400, 300); j.confirmHeld = true; j.hover = Hover{true, UNIT_ITEM, 3};
    CHECK(!s.tick(j).lmb, "and a new item appearing does not inherit the press");
}

static void t_after_pickup_focus_continues_nearby() {
    std::vector<Label> v = { L(1,400,300), L(2,200,300), L(3,420,320) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v); i.confirmEdge = true; i.confirmHeld = true; s.tick(i);
    i = mk(v, 400, 300); i.confirmHeld = true; i.hover = Hover{true, UNIT_ITEM, 1}; s.tick(i);
    // item 1 picked up: its label is gone, button still held
    std::vector<Label> v2 = { L(2,200,300), L(3,420,320) };
    i = mk(v2, 400, 300); i.confirmHeld = true; CHECK(s.tick(i).lmb, "button held until release");
    i.confirmHeld = false; s.tick(i);
    Out o = s.tick(mk(v2, 400, 300));
    CHECK(s.focus() == 3 && o.move, "focus re-acquired next to where the last one was");
}

static void t_reset_drops_everything() {
    std::vector<Label> v = { L(1,400,300) };
    State s; s.reset(0); s.tick(mk(v));
    In i = mk(v); i.confirmEdge = true; i.confirmHeld = true; s.tick(i);
    s.reset(2);
    CHECK(!s.pressing() && s.focus() == 0 && !s.warm(), "reset clears the press and the focus");
}

int main() {
    t_nav_direction(); t_warmup_ignores_stale_labels(); t_initial_focus_nearest_reference();
    t_dpad_moves_focus_and_cursor(); t_manual_cursor_follows_label();
    t_pickup_waits_for_hover(); t_pickup_never_fires_without_hover();
    t_pickup_accepts_other_item_after_a_few_ticks(); t_item_vanishes_before_press();
    t_after_pickup_focus_continues_nearby(); t_reset_drops_everything();
    std::printf("item_assist_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
