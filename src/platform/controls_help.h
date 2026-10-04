// src/platform/controls_help.h -- title-screen Controls panel, drawing side:
// a small tab in D2's left letterbox band, and the full editor it opens (the
// model, the geometry and the touch zones live in controls_editor.h).
//
// No VitaSDK, no syscall. The panel is OPAQUE on purpose: a translucent one
// has to read the frame back to blend, and reading CDRAM costs ~814 ns per
// pixel on this console (see the keyboard's alpha notes in vita_present.cpp);
// a full-screen blend would stall the title screen. Everything here only
// writes pixels.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "platform/controls_editor.h"
#include "platform/vita_kb.h"   // d2kb::draw_detail -- the one pixel font, not a second copy

namespace d2ch {

// Left letterbox band on the title screen (960x544 physical screen, D2's own
// 800x600 canvas centered at x in [117,842)).
constexpr int BAND_X0 = 0, BAND_X1 = 117, BAND_Y0 = 0, BAND_Y1 = 544;

// The drawn "Controls" tab, and the touch zone around it (a finger is far
// bigger than the 16 px tab). The tap must land here, not anywhere in the band.
constexpr int ICON_X0 = BAND_X0 + 8, ICON_X1 = BAND_X1 - 8;
constexpr int ICON_Y0 = BAND_Y1 - 24, ICON_Y1 = BAND_Y1 - 8;
constexpr int ICON_HIT_PAD = 8;

inline bool icon_hit(int x, int y) {
    return x >= ICON_X0 - ICON_HIT_PAD && x < ICON_X1 + ICON_HIT_PAD &&
           y >= ICON_Y0 - ICON_HIT_PAD && y < ICON_Y1 + ICON_HIT_PAD;
}

namespace pal {
using d2kb::draw_detail::rgb;          // framebuffer is A8B8G8R8: rgb() puts red in the LOW byte
inline uint32_t bg()      { return rgb(0x16, 0x12, 0x0D); }
inline uint32_t band()    { return rgb(0x1F, 0x19, 0x12); }
inline uint32_t alt()     { return rgb(0x1B, 0x16, 0x10); }
inline uint32_t gold()    { return rgb(0xC8, 0xA1, 0x4A); }
inline uint32_t text()    { return rgb(0xE0, 0xD4, 0xBA); }
inline uint32_t dim()     { return rgb(0x8A, 0x7E, 0x68); }
inline uint32_t lock()    { return rgb(0x5E, 0x56, 0x48); }
inline uint32_t focus()   { return rgb(0x44, 0x34, 0x18); }
inline uint32_t chip()    { return rgb(0x2A, 0x22, 0x16); }
inline uint32_t edited()  { return rgb(0x8F, 0xD0, 0x6A); }
inline uint32_t custom()  { return rgb(0xE8, 0xC0, 0x6A); }
inline uint32_t track()   { return rgb(0x30, 0x28, 0x1B); }
}

namespace draw_detail {
using d2kb::draw_detail::rect;
using d2kb::draw_detail::frame;
using d2kb::draw_detail::text;

inline int text_w(const char* t, int sc = 1) { return (int)std::strlen(t) * 8 * sc; }
inline void text_c(uint32_t* fb, int W, int H, const char* t, int x, int w, int y, int sc, uint32_t c) {
    text(fb, W, H, t, x + (w - text_w(t, sc)) / 2, y, sc, c);
}
inline void button(uint32_t* fb, int W, int H, int x, int y, int w, int h, const char* label, bool on) {
    rect(fb, W, H, x, y, w, h, on ? pal::focus() : pal::chip());
    frame(fb, W, H, x, y, w, h, on ? pal::gold() : pal::lock());
    text_c(fb, W, H, label, x, w, y + (h - 16) / 2, 1, on ? pal::text() : pal::dim());
}
// The value line of a cell: arrows only on the focused one.
inline void cell(uint32_t* fb, int W, int H, int x, int y, int w, const char* v, uint32_t c, bool focused) {
    using namespace ced::lay;
    if (focused) {
        rect(fb, W, H, x, y + 1, w, ROW_H - 2, pal::focus());
        frame(fb, W, H, x, y + 1, w, ROW_H - 2, pal::gold());
        text(fb, W, H, "<", x + 10, y + 5, 1, pal::gold());
        text(fb, W, H, ">", x + w - 18, y + 5, 1, pal::gold());
    }
    text_c(fb, W, H, v, x, w, y + 5, 1, c);
}
} // namespace draw_detail

// The closed state: just the tab in the left band.
inline void draw_icon(uint32_t* fb, int W, int H) {
    using namespace draw_detail;
    rect(fb, W, H, ICON_X0, ICON_Y0, ICON_X1 - ICON_X0, ICON_Y1 - ICON_Y0, pal::chip());
    frame(fb, W, H, ICON_X0, ICON_Y0, ICON_X1 - ICON_X0, ICON_Y1 - ICON_Y0, pal::gold());
    text_c(fb, W, H, "Settings", ICON_X0, ICON_X1 - ICON_X0, ICON_Y0, 1, pal::gold());
}

inline uint32_t bind_color(const ced::State& s, int r, int c) {
    const ced::Bind& cur = s.cur.b[r][c];
    if (!ced::same(cur, s.base.b[r][c])) return pal::edited();
    return ced::same(cur, s.def.b[r][c]) ? pal::text() : pal::custom();
}
inline uint32_t tune_color(const ced::State& s, int i) {
    if (s.cur.v[i] != s.base.v[i]) return pal::edited();
    return s.cur.v[i] == s.def.v[i] ? pal::text() : pal::custom();
}

inline void draw_buttons_tab(const ced::State& s, uint32_t* fb, int W, int H) {
    using namespace draw_detail; using namespace ced::lay;
    static const char* const head[ced::NSLOT] = { "PLAIN", "WITH R HELD", "WITH L HELD" };
    text(fb, W, H, "BUTTON", PAD + 8, GRID_HEAD_Y, 1, pal::gold());
    for (int c = 0; c < ced::NSLOT; ++c) text_c(fb, W, H, head[c], grid_cell_x(c), GRID_CELL_W, GRID_HEAD_Y, 1, pal::gold());
    rect(fb, W, H, PAD, GRID_Y0 - 3, W - 2 * PAD, 1, pal::lock());
    for (int r = 0; r < ced::NROW; ++r) {
        const int y = grid_row_y(r);
        if (r & 1) rect(fb, W, H, PAD, y, W - 2 * PAD, ROW_H, pal::alt());
        const bool rowFocus = s.row == r;
        text(fb, W, H, ced::row_def(r).label, PAD + 8, y + 5, 1, rowFocus ? pal::gold() : pal::text());
        for (int c = 0; c < ced::NSLOT; ++c) {
            char v[48];
            if (const char* lk = ced::cell_lock(r, c)) {
                text_c(fb, W, H, lk, grid_cell_x(c), GRID_CELL_W, y + 5, 1, pal::lock());
                continue;
            }
            if (r == 2 && c == ced::S_PLAIN && s.cur.b[r][c].kind == ctl::ACT_NONE) std::snprintf(v, sizeof v, "Radial menu");
            else ced::bind_label(s.cur.b[r][c], v, sizeof v);
            cell(fb, W, H, grid_cell_x(c), y, GRID_CELL_W, v, bind_color(s, r, c), rowFocus && s.col == c);
        }
    }
}

// One list tab: the tuning values (controls.txt) or the game options (env.txt).
inline void draw_list_tab(const ced::State& s, uint32_t* fb, int W, int H, int first, int count) {
    using namespace draw_detail; using namespace ced::lay;
    for (int i = first; i < first + count; ++i) {
        const ced::Tune& t = ced::item(i);
        const int y = ced::lay::item_row_y(i);
        if (t.type == ced::T_HEAD) {
            text(fb, W, H, t.label, PAD + 8, y + 5, 1, pal::gold());
            rect(fb, W, H, PAD + 8 + text_w(t.label) + 10, y + 13, W - 2 * PAD - text_w(t.label) - 18, 1, pal::lock());
            continue;
        }
        if (i & 1) rect(fb, W, H, PAD, y, W - 2 * PAD, ROW_H, pal::alt());
        const bool f = s.row == i;
        text(fb, W, H, t.label, PAD + 8, y + 5, 1, f ? pal::gold() : pal::text());
        char v[24]; ced::value_text(t, s.cur.v[i], v, sizeof v);
        cell(fb, W, H, TUNE_X0, y, TUNE_CELL_W, v, tune_color(s, i), f);
        if (t.type == ced::T_NUM) {
            const int by = y + 11;
            rect(fb, W, H, BAR_X0, by, BAR_W, 4, pal::track());
            const double span = t.hi - t.lo;
            const int fw = (int)((s.cur.v[i] - t.lo) / span * BAR_W);
            rect(fb, W, H, BAR_X0, by, fw < 0 ? 0 : fw > BAR_W ? BAR_W : fw, 4, f ? pal::gold() : pal::dim());
            const int dx = BAR_X0 + (int)((s.def.v[i] - t.lo) / span * BAR_W);
            rect(fb, W, H, dx, by - 3, 2, 10, pal::text());          // tick: where the default sits
        }
    }
}

// Drawn once per presented frame while the panel is open. Cheap: a handful of
// rectangles and ~600 glyphs, no read of the frame.
inline void draw_panel(const ced::State& s, uint32_t* fb, int W, int H) {
    using namespace draw_detail; using namespace ced::lay;
    rect(fb, W, H, 0, 0, W, H, pal::bg());
    rect(fb, W, H, 0, 0, W, 38, pal::band());
    text(fb, W, H, "SETTINGS", PAD, 3, 2, pal::gold());
    const char* path = s.tab == ced::TAB_GAME ? "ux0:data/d2vita/env.txt" : "ux0:data/d2vita/controls.txt";
    text(fb, W, H, path, W - PAD - text_w(path), 12, 1, pal::dim());
    rect(fb, W, H, 0, 38, W, 2, pal::gold());

    static const char* const tabs[ced::NTAB] = { "Buttons", "Tuning", "Game" };
    for (int i = 0; i < ced::NTAB; ++i) button(fb, W, H, tab_x(i), TAB_Y, TAB_W, TAB_H, tabs[i], s.tab == i);
    text(fb, W, H, "L / R", tab_x(ced::NTAB) + 6, TAB_Y + 6, 1, pal::lock());

    if (s.tab == ced::TAB_BUTTONS) draw_buttons_tab(s, fb, W, H);
    else draw_list_tab(s, fb, W, H, ced::first_item(s.tab), ced::n_items(s.tab));

    // The line under the list: what the focused item is, and its default.
    rect(fb, W, H, PAD, HINT_Y - 6, W - 2 * PAD, 1, pal::lock());
    char l1[160] = "", l2[160] = "";
    if (s.tab == ced::TAB_BUTTONS) {
        char key[24], d[48];
        ced::cell_key(s.row, s.col, key, sizeof key);
        ced::bind_label(s.def.b[s.row][s.col], d, sizeof d);
        if (ced::cell_lock(s.row, s.col)) std::snprintf(l1, sizeof l1, "%s: fixed by the game", key);
        else if (s.row == 2) std::snprintf(l1, sizeof l1, "select: any action, or '-' for the radial menu");
        else if (s.row <= 1) std::snprintf(l1, sizeof l1, "%s: what the shoulder button does by itself (it is also the layer prefix)", key);
        else std::snprintf(l1, sizeof l1, "%s: the action this button fires", key);
        std::snprintf(l2, sizeof l2, "Default: %s", s.row == 2 && s.def.b[2][0].kind == ctl::ACT_NONE ? "Radial menu" : d);
    } else {
        const ced::Tune& t = ced::item(s.row);
        std::snprintf(l1, sizeof l1, "%s", t.hint);
        char dv[24], lo[24], hi[24];
        ced::value_text(t, s.def.v[s.row], dv, sizeof dv);
        ced::value_text(t, t.lo, lo, sizeof lo); ced::value_text(t, t.hi, hi, sizeof hi);
        if (s.tab == ced::TAB_GAME && t.type == ced::T_SWITCH)
            std::snprintf(l2, sizeof l2, "Default: %s   (takes effect at the next launch)", dv);
        else if (s.tab == ced::TAB_GAME)
            std::snprintf(l2, sizeof l2, "Default: %s   (range %s to %s, takes effect at the next launch)", dv, lo, hi);
        else if (t.type == ced::T_SWITCH) std::snprintf(l2, sizeof l2, "Default: %s", dv);
        else std::snprintf(l2, sizeof l2, "Default: %s   (range %s to %s)", dv, lo, hi);
    }
    text(fb, W, H, l1, PAD, HINT_Y, 1, pal::text());
    text(fb, W, H, l2, PAD, HINT_Y + 18, 1, pal::dim());
    if (s.note[0]) text(fb, W, H, s.note, PAD, NOTE_Y, 1, pal::edited());
    else text(fb, W, H, "white: default   gold: customised   green: changed, not saved yet", PAD, NOTE_Y, 1, pal::lock());

    button(fb, W, H, BTN_RESET_X, FOOT_Y, BTN_RESET_W, FOOT_H, "Triangle: default", false);
    button(fb, W, H, BTN_SAVE_X, FOOT_Y, BTN_SAVE_W, FOOT_H, ced::dirty(s) ? "Circle: save & close" : "Circle: close", true);
    const char* keys = s.tab == ced::TAB_BUTTONS ? "D-pad move   Cross / Square next / previous action"
                                                  : "Up / Down pick   Left / Right change   hold to repeat";
    text(fb, W, H, keys, BTN_SAVE_X + BTN_SAVE_W + 20, FOOT_Y + 6, 1, pal::dim());
}

inline void draw(const ced::State& s, uint32_t* fb, int W, int H) {
    if (s.open) draw_panel(s, fb, W, H); else draw_icon(fb, W, H);
}

} // namespace d2ch
