// tools/tests/controls_help_test.cpp — host-side oracle for controls_help.h.
//
// controls_help.h never touches VitaSDK or a syscall, so everything below
// builds and runs on a dev machine. What still needs on-device checking is
// whether the icon/panel actually render above the frame, and the touch
// coordinates it receives are truly confined to the left letterbox band.
#include "platform/controls_help.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace d2ch;

static void t_icon_hit_rect_confined_to_left_band() {
    // The band is x in [0,117), full height (960x544 screen, design section 2).
    CHECK(icon_hit(50, 10) == true,   "inside the band should hit");
    CHECK(icon_hit(0, 0) == true,     "top-left corner of the band should hit");
    CHECK(icon_hit(116, 543) == true, "bottom-right corner of the band should hit");
    CHECK(icon_hit(117, 10) == false, "one pixel past the band must NOT hit");
    CHECK(icon_hit(400, 10) == false, "inside D2's own 800x600 canvas must NOT hit");
    CHECK(icon_hit(50, 544) == false, "one row past the bottom must NOT hit");
}

static void t_open_close_toggle() {
    State s; std::memset(&s, 0, sizeof s);
    CHECK(s.open == false, "starts closed");
    tap(s, 50, 10);
    CHECK(s.open == true, "tap inside the icon opens the panel");
    close(s);
    CHECK(s.open == false, "close() closes it");
    // A tap outside the band, while closed, must not open it.
    State s2; std::memset(&s2, 0, sizeof s2);
    tap(s2, 400, 10);
    CHECK(s2.open == false, "tap outside the band does not open the panel");
}

static void t_scroll_dpad_step() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 20; s.visible_rows = 6;
    CHECK(s.scroll == 0, "starts at top");
    scroll_dpad(s, +1);
    CHECK(s.scroll == 1, "one row down");
    for (int i = 0; i < 30; ++i) scroll_dpad(s, +1);
    CHECK(s.scroll == 14, "clamped to row_count - visible_rows (20-6=14)");
    for (int i = 0; i < 30; ++i) scroll_dpad(s, -1);
    CHECK(s.scroll == 0, "clamped at 0, never negative");
}

static void t_scroll_drag_delta() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 20; s.visible_rows = 6; s.row_px = 20;
    // Dragging UP (finger moves toward smaller y) scrolls the list DOWN
    // (higher row index) — same convention as a phone list.
    scroll_drag(s, /*dy_px=*/-45);
    CHECK(s.scroll == 2, "45px of upward drag at 20px/row => 2 rows (45/20 truncated)");
    scroll_drag(s, /*dy_px=*/+200);
    CHECK(s.scroll == 0, "large downward drag clamps at 0");
}

static void t_content_shorter_than_view_never_scrolls() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 3; s.visible_rows = 6;
    scroll_dpad(s, +5);
    CHECK(s.scroll == 0, "fewer rows than the view: max scroll is 0");
}

int main() {
    t_icon_hit_rect_confined_to_left_band();
    t_open_close_toggle();
    t_scroll_dpad_step();
    t_scroll_drag_delta();
    t_content_shorter_than_view_never_scrolls();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
