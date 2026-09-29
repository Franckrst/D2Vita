// src/platform/item_assist.h — "item assist": hold a bound button to show the
// ground-item name labels (the game's own Alt display), browse them with the
// D-pad and pick the focused one up with Cross.
//
// Pure logic, no VitaSDK and no guest access: vita_present.cpp reads the
// game's label array and hover globals, hands them in through In, and applies
// the Out. That keeps this file host-testable (tools/oracle_item_assist.sh).
//
// The two-step pick-up is the whole point. Moving the cursor onto a label and
// pressing in the same tick makes the game act on the PREVIOUS frame's hover
// (none), which it turns into "walk to that spot" — the character wanders off
// instead of picking anything up. So: move, wait until the game itself
// reports the item as hovered, and only then press the left button.
#pragma once
#include <cmath>
#include <cstdint>

namespace d2ia {

constexpr int MAX_LABELS = 32;
constexpr uint32_t UNIT_ITEM = 4;      // hover type of an item on the ground

struct Label { int32_t x1, y1, x2, y2; uint32_t id; };
struct Hover { bool valid; uint32_t type; uint32_t id; };

enum : unsigned { DIR_UP = 1, DIR_LEFT = 2, DIR_DOWN = 4, DIR_RIGHT = 8 };

struct In {
    const Label* labels; int n;
    Hover hover;
    unsigned dirEdge;          // directions pressed this tick, consumed by us
    bool confirmEdge;          // Cross pressed this tick, consumed by us
    bool confirmHeld;          // Cross still held AND the press was ours
    bool cursorMoved;          // stick/touch moved the cursor this tick
    int cx, cy;                // current cursor, game coordinates
    int refX, refY;            // where to start when nothing is focused yet
};

struct Out {
    bool move; int mx, my;     // put the cursor here
    bool lmb;                  // desired state of the held left button
};

inline int cx_of(const Label& l) { return (l.x1 + l.x2) / 2; }
inline int cy_of(const Label& l) { return (l.y1 + l.y2) / 2; }

inline int find_id(const Label* l, int n, uint32_t id) {
    for (int i = 0; i < n; ++i) if (l[i].id == id) return i;
    return -1;
}

// Nearest label centre lying in direction `d` from label `from`: the
// candidate must be in that half-plane with |major| >= |minor|; ties on
// distance go to the smaller id, so the choice never depends on array order.
inline int nav_direction(const Label* l, int n, int from, unsigned d) {
    if (!l || from < 0 || from >= n) return -1;
    const int fx = cx_of(l[from]), fy = cy_of(l[from]);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (i == from) continue;
        const float dx = (float)(cx_of(l[i]) - fx), dy = (float)(cy_of(l[i]) - fy);
        bool ok = false;
        switch (d) {
            case DIR_RIGHT: ok = dx > 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case DIR_LEFT:  ok = dx < 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case DIR_DOWN:  ok = dy > 0.f && std::fabs(dy) >= std::fabs(dx); break;
            case DIR_UP:    ok = dy < 0.f && std::fabs(dy) >= std::fabs(dx); break;
        }
        if (!ok) continue;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd || (dist == bd && l[i].id < l[best].id)) { best = i; bd = dist; }
    }
    return best;
}

inline int nearest_to(const Label* l, int n, int x, int y) {
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        const float dx = (float)(cx_of(l[i]) - x), dy = (float)(cy_of(l[i]) - y);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd || (dist == bd && l[i].id < l[best].id)) { best = i; bd = dist; }
    }
    return best;
}

inline int label_at(const Label* l, int n, int x, int y) {
    for (int i = 0; i < n; ++i)
        if (x >= l[i].x1 && x < l[i].x2 && y >= l[i].y1 && y < l[i].y2) return i;
    return -1;
}

class State {
public:
    // The label array is only refreshed while the game draws with Alt held, so
    // right after activation it can still hold the previous session's rows:
    // ignore it for `warmupTicks`.
    void reset(int warmupTicks) {
        warm_ = warmupTicks < 0 ? 0 : warmupTicks;
        focus_ = 0; pressing_ = false; arm_ = 0; target_ = 0; lmb_ = false;
        haveLast_ = false;
    }
    bool warm() const { return warm_ == 0; }
    uint32_t focus() const { return focus_; }
    bool pressing() const { return pressing_; }

    Out tick(const In& in) {
        Out o{false, 0, 0, lmb_};
        if (warm_ > 0) { --warm_; return o; }
        const Label* l = in.labels; const int n = in.n;

        if (pressing_) {
            if (!in.confirmHeld) {
                pressing_ = false; arm_ = 0; target_ = 0; lmb_ = false;
                o.lmb = false;
                return o;
            }
            const int ti = find_id(l, n, target_);
            if (ti < 0) { arm_ = 0; return o; }            // gone: never fire at its old spot
            const int px = cx_of(l[ti]), py = cy_of(l[ti]);
            if (in.cx != px || in.cy != py) { o.move = true; o.mx = px; o.my = py; }
            if (arm_ > 0) {
                // Wait for the game's own hover report. After a few ticks a
                // hover on ANOTHER item counts: it is the one under the cursor.
                if (!in.hover.valid || in.hover.type != UNIT_ITEM) ++arm_;
                else if (in.hover.id == target_ || arm_ >= kArmMax) { lmb_ = true; arm_ = 0; }
                else ++arm_;
            }
            o.lmb = lmb_;
            return o;
        }

        if (n <= 0) return o;

        int fi = focus_ ? find_id(l, n, focus_) : -1;
        bool changed = false;
        if (fi < 0) {
            // First focus, or the focused item was picked up: continue from
            // where the last one was, else from the character.
            const int rx = haveLast_ ? lastX_ : in.refX, ry = haveLast_ ? lastY_ : in.refY;
            fi = nearest_to(l, n, rx, ry);
            changed = true;
        }
        static const unsigned kOrder[4] = { DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT };
        for (unsigned d : kOrder) if (in.dirEdge & d) {
            const int nx = nav_direction(l, n, fi, d);
            if (nx >= 0) { fi = nx; changed = true; }
            break;
        }
        if (!changed && in.cursorMoved) {
            // The player steered the cursor by hand onto a label: follow it.
            const int under = label_at(l, n, in.cx, in.cy);
            if (under >= 0 && l[under].id != focus_) { fi = under; focus_ = l[fi].id; }
        }
        if (changed) {
            focus_ = l[fi].id;
            o.move = true; o.mx = cx_of(l[fi]); o.my = cy_of(l[fi]);
        }
        lastX_ = cx_of(l[fi]); lastY_ = cy_of(l[fi]); haveLast_ = true;

        if (in.confirmEdge) {
            pressing_ = true; target_ = l[fi].id; arm_ = 1;
            o.move = true; o.mx = cx_of(l[fi]); o.my = cy_of(l[fi]);
        }
        return o;
    }

private:
    static constexpr int kArmMax = 5;
    int warm_ = 0;
    uint32_t focus_ = 0, target_ = 0;
    bool pressing_ = false, lmb_ = false, haveLast_ = false;
    int arm_ = 0, lastX_ = 0, lastY_ = 0;
};

} // namespace d2ia
