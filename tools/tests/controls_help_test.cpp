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
    // Only the drawn tab (bottom of the band) plus a finger margin hits, not the whole band.
    CHECK(icon_hit(60, 528) == true,  "centre of the tab should hit");
    CHECK(icon_hit(ICON_X0 - ICON_HIT_PAD, ICON_Y0 - ICON_HIT_PAD) == true, "finger margin corner should hit");
    CHECK(icon_hit(50, 10) == false,  "top of the band must NOT hit");
    CHECK(icon_hit(60, 300) == false, "middle of the band must NOT hit");
    CHECK(icon_hit(0, 0) == false,    "top-left corner of the band must NOT hit");
    CHECK(icon_hit(60, ICON_Y0 - ICON_HIT_PAD - 1) == false, "just above the margin must NOT hit");
    CHECK(icon_hit(117, 528) == false, "one pixel past the band must NOT hit");
    CHECK(icon_hit(400, 528) == false, "inside D2's own 800x600 canvas must NOT hit");
    CHECK(icon_hit(60, 544) == false, "one row past the bottom must NOT hit");
}

static void t_open_close_toggle() {
    State s; std::memset(&s, 0, sizeof s);
    CHECK(s.open == false, "starts closed");
    tap(s, 50, 10);
    CHECK(s.open == false, "tap elsewhere in the band does not open the panel");
    tap(s, 60, 528);
    CHECK(s.open == true, "tap on the icon opens the panel");
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

static void t_open_with_content_draws_something() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 2; s.visible_rows = 6;
    const char* lines[2] = {"Cross: Test", "Circle: Shift"};
    std::vector<uint32_t> fb(960 * 544, 0xDEADBEEFu), ref = fb;
    draw(s, fb.data(), 960, 544, lines);
    CHECK(std::memcmp(fb.data(), ref.data(), fb.size() * 4) != 0,
          "panel open with content: framebuffer must change");
}

// t_open_with_content_draws_something (above) only proves the framebuffer
// changed from an un-drawn 0xDEADBEEF reference — the panel's own opaque
// background fill (rect() over the whole screen, before any row text is
// touched) is ALONE enough to make that pass, regardless of whether row
// text is ever drawn. It would have passed against the pre-Task-8 stub, and
// would pass again if the row-text call were ever deleted by accident.
// This test instead draws two DIFFERENT sets of lines into two separate
// framebuffers and asserts the two results differ from EACH OTHER, not
// just from a blank reference — a regression that stops rendering row text
// collapses both to the same background-only image, which this catches.
static void t_open_with_different_content_draws_differently() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 1; s.visible_rows = 6;
    const char* lines_a[1] = {"Cross: Test"};
    const char* lines_b[1] = {"Circle: Shift"};
    std::vector<uint32_t> fb_a(960 * 544, 0xDEADBEEFu);
    std::vector<uint32_t> fb_b(960 * 544, 0xDEADBEEFu);
    draw(s, fb_a.data(), 960, 544, lines_a);
    draw(s, fb_b.data(), 960, 544, lines_b);
    CHECK(std::memcmp(fb_a.data(), fb_b.data(), fb_a.size() * 4) != 0,
          "different content must render differently, not just 'something changed from blank'");
}

// The panel background must be a translucent blend against whatever was
// already in the framebuffer (design spec section 3: "translucent overlay
// ... title screen art beneath it stays visible"), not a flat opaque
// overwrite. Pre-fill with a known color, open the panel with no lines (it
// returns right after the background fill, so nothing else touches the
// pixel we check), and confirm the result is neither the original color
// (background fill did nothing) nor a fully opaque rgb(10,10,10) (background
// fill ignored alpha and overwrote solid).
static void t_panel_background_is_translucent_not_opaque() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 0; s.visible_rows = 6;
    std::vector<uint32_t> fb(960 * 544, 0xFFFFFFFFu);
    draw(s, fb.data(), 960, 544, nullptr);
    const uint32_t after = fb[0];
    CHECK(after != 0xFFFFFFFFu, "background fill must change the pre-existing pixel");
    CHECK(after != 0xFF0A0A0Au, "background fill must NOT be a fully opaque overwrite (not translucent)");
}

int main() {
    t_icon_hit_rect_confined_to_left_band();
    t_open_close_toggle();
    t_scroll_dpad_step();
    t_scroll_drag_delta();
    t_content_shorter_than_view_never_scrolls();
    t_open_with_content_draws_something();
    t_open_with_different_content_draws_differently();
    t_panel_background_is_translucent_not_opaque();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
