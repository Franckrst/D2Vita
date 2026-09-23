// src/platform/controls_help.h — title-screen "controls help" overlay:
// a persistent icon in D2's left letterbox band, and the panel it opens.
//
// No VitaSDK, no syscall — same "benign race, single writer" state model as
// vita_kb.h/radial_menu.h: State is written from the input tick, drawn from
// the presentation path.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace d2ch {

// Left letterbox band on the title screen (960x544 physical screen, D2's own
// 800x600 canvas centered at x in [117,842) — see design doc section 2).
constexpr int BAND_X0 = 0, BAND_X1 = 117, BAND_Y0 = 0, BAND_Y1 = 544;

inline bool icon_hit(int x, int y) {
    return x >= BAND_X0 && x < BAND_X1 && y >= BAND_Y0 && y < BAND_Y1;
}

struct State {
    volatile int open;        // panel shown (vs. just the persistent icon)
    volatile int row_count;   // total bindings to show
    volatile int visible_rows;// how many fit on screen at once
    volatile int scroll;      // topmost visible row index
    volatile int row_px;      // pixel height of one row, for drag-to-scroll
};

inline int max_scroll(const State& s) {
    int m = s.row_count - s.visible_rows;
    return m > 0 ? m : 0;
}

inline void clamp_scroll(State& s) {
    if (s.scroll < 0) s.scroll = 0;
    int m = max_scroll(s);
    if (s.scroll > m) s.scroll = m;
}

inline void tap(State& s, int x, int y) {
    if (!s.open) {
        if (icon_hit(x, y)) s.open = true;
        return;
    }
}

inline void close(State& s) { s.open = false; }

inline void scroll_dpad(State& s, int rows) {
    s.scroll += rows;
    clamp_scroll(s);
}

inline void scroll_drag(State& s, int dy_px) {
    // Upward finger motion (negative dy_px) scrolls forward through the list.
    s.scroll += (-dy_px) / (s.row_px > 0 ? s.row_px : 1);
    clamp_scroll(s);
}

namespace draw_detail {
inline uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b); }
inline void rect(uint32_t* fb, int fw, int fh, int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            fb[y * fw + x] = color;
}
} // namespace draw_detail

// Draws the persistent icon (when !s.open) or the full panel (when s.open).
// `labels`/`bindings` are `row_count` parallel arrays of already-formatted,
// null-terminated strings (built by the caller from g_btn[] + the fixed
// non-remappable list — see controls_help_format.h, Task 6).
inline void draw(const State& s, uint32_t* fb, int fw, int fh,
                  const char* const* labels, const char* const* bindings) {
    (void)labels; (void)bindings;   // text rendering: same primitive kit as vita_kb.h's draw_detail::text/ch, reused as-is
    using namespace draw_detail;
    if (!s.open) {
        rect(fb, fw, fh, BAND_X0 + 8, BAND_Y1 - 24, BAND_X1 - 8, BAND_Y1 - 8, rgb(40, 40, 40));
        return;
    }
    // Full-screen translucent panel background.
    rect(fb, fw, fh, 0, 0, fw, fh, rgb(10, 10, 10));
}

} // namespace d2ch
