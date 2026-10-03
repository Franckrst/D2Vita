// Host test for the title-screen Controls panel: model, touch zones, the
// controls.txt rewrite, and a render smoke test (no console, no game).
#include "platform/controls_help.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace ced;

static State fresh() {
    State s; init(s);
    for (int r = 0; r < NROW; ++r) for (int c = 0; c < NSLOT; ++c) s.def.b[r][c] = Bind{ctl::ACT_NONE, 0};
    s.def.b[0][0] = Bind{ctl::ACT_AIM, 0};
    s.def.b[1][0] = Bind{ctl::ACT_RMB, 0};
    s.def.b[3][0] = Bind{ctl::ACT_KEY, 0x52};
    s.def.b[7][1] = Bind{ctl::ACT_KEY, 0x70};
    for (int i = 0; i < NTUNE; ++i) s.def.v[i] = tune(i).type == T_HEAD ? 0 : (tune(i).lo + tune(i).hi) / 2;
    s.def.v[1] = 35; s.def.v[2] = 800;
    s.cur = s.base = s.def;
    s.open = 1;
    return s;
}

static void test_choices_parse_back_to_themselves() {
    int n; const Choice* c = choices(&n);
    for (int i = 0; i < n; ++i) {
        ctl::ActSpec a{};
        CHECK(ctl::parse_action(c[i].name, &a));
        CHECK(a.kind == c[i].kind && (a.kind != ctl::ACT_KEY || a.vk == c[i].vk));
        for (int j = i + 1; j < n; ++j) CHECK(!(c[i].kind == c[j].kind && c[i].vk == c[j].vk));   // no twin entries
        CHECK(std::strlen(c[i].label) <= 22);                                                   // fits a 250 px cell
    }
}

static void test_ranges_are_the_loaders_ranges() {
    for (int i = 0; i < NTUNE; ++i) {
        const Tune& t = tune(i);
        if (t.type != T_NUM) continue;
        double d, lo, hi; char lov[32], hiv[32];
        std::snprintf(lov, sizeof lov, "%g", t.lo); std::snprintf(hiv, sizeof hiv, "%g", t.hi);
        CHECK(ctl::numeric_key(t.key, lov, &d, &lo, &hi) == 1);
        CHECK(ctl::numeric_key(t.key, hiv, &d, &lo, &hi) == 1);
        CHECK(lo == t.lo && hi == t.hi);
        CHECK(t.step > 0 && t.step <= (t.hi - t.lo));
        CHECK(std::strlen(t.hint) <= 110);                     // two lines of the hint area at most
        CHECK(std::strlen(t.label) <= 27);
    }
}

static void test_cycling_a_binding() {
    State s = fresh();
    s.row = 3; s.col = 0;                                      // Cross, plain: Walk / run
    CHECK(step(s, +1)); CHECK(s.cur.b[3][0].vk == 0x10 && s.cur.b[3][0].kind == ctl::ACT_KEY);
    CHECK(step(s, -1)); CHECK(same(s.cur.b[3][0], s.def.b[3][0]));
    int n; choices(&n);
    for (int i = 0; i < n; ++i) step(s, +1);                   // a full lap lands where it started
    CHECK(same(s.cur.b[3][0], s.def.b[3][0]));
    s.cur.b[3][0] = Bind{ctl::ACT_KEY, 0x4F};                  // a hand-written vk:0x4F
    CHECK(choice_index(s.cur.b[3][0]) < 0);
    CHECK(step(s, +1)); CHECK(choice_index(s.cur.b[3][0]) == 0);
}

static void test_locked_cells_are_never_edited_or_focused() {
    State s = fresh();
    s.row = 6; s.col = S_R;                                    // R+Triangle: the keyboard, fixed
    snap_col(s); CHECK(s.col == S_PLAIN || s.col == S_L);
    s.row = 2; s.col = S_L; snap_col(s); CHECK(s.col == S_PLAIN);
    s.col = S_R; CHECK(!step(s, +1));
    s.row = 0; s.col = S_PLAIN; move(s, 0, +1); CHECK(s.col == S_PLAIN);       // L has no layers: nowhere to go
    s.row = 5; s.col = S_PLAIN; move(s, 0, +1); CHECK(s.col == S_R);
    move(s, 0, +1); CHECK(s.col == S_L);
    move(s, 0, +1); CHECK(s.col == S_L);
    s.row = 10; s.col = S_L; move(s, +1, 0); CHECK(s.row == 11 && (s.col == S_PLAIN || s.col == S_R));   // Start: L+ is fixed
    CHECK(!cell_lock(s.row, s.col));
}

static void test_vertical_movement_wraps_and_skips_headings() {
    State s = fresh();
    s.row = 0; move(s, -1, 0); CHECK(s.row == NROW - 1);
    move(s, +1, 0); CHECK(s.row == 0);
    set_tab(s, TAB_TUNE); CHECK(s.tab == TAB_TUNE && tune(s.row).type != T_HEAD);
    for (int i = 0; i < NTUNE * 2; ++i) { move(s, +1, 0); CHECK(tune(s.row).type != T_HEAD); }
    for (int i = 0; i < NTUNE * 2; ++i) { move(s, -1, 0); CHECK(tune(s.row).type != T_HEAD); }
}

static void test_tuning_values_clamp_round_and_toggle() {
    State s = fresh(); set_tab(s, TAB_TUNE);
    s.row = 1;                                                  // cone 35, step 5, 5..90
    CHECK(step(s, +1) && s.cur.v[1] == 40);
    for (int i = 0; i < 50; ++i) step(s, +1);
    CHECK(s.cur.v[1] == 90); CHECK(!step(s, +1));
    for (int i = 0; i < 50; ++i) step(s, -1);
    CHECK(s.cur.v[1] == 5); CHECK(!step(s, -1));
    int dz = -1; for (int i = 0; i < NTUNE; ++i) if (!std::strcmp(tune(i).key, "deadzone")) dz = i;
    s.row = dz; s.cur.v[dz] = 0.15;
    for (int i = 0; i < 3; ++i) step(s, +1);
    CHECK(s.cur.v[dz] == 0.3);                                  // no 0.30000000000000004 drift
    char t[24]; value_text(tune(dz), s.cur.v[dz], t, sizeof t); CHECK(!std::strcmp(t, "0.3"));
    value_text(tune(dz), 0.15, t, sizeof t); CHECK(!std::strcmp(t, "0.15"));
    int dm = -1; for (int i = 0; i < NTUNE; ++i) if (tune(i).type == T_SWITCH) dm = i;
    s.row = dm; s.cur.v[dm] = 1;
    CHECK(step(s, +1) && s.cur.v[dm] == 0); CHECK(step(s, -1) && s.cur.v[dm] == 1);
    value_text(tune(dm), 0, t, sizeof t); CHECK(!std::strcmp(t, "off"));
}

static void test_keys_do_what_the_footer_says() {
    State s = fresh();
    CHECK(press(s, K_DOWN) == R_NONE && s.row == 1);
    s.row = 3; s.col = 0;
    CHECK(press(s, K_RIGHT) == R_NONE && s.col == S_R);          // grid: Right walks
    CHECK(press(s, K_NEXT) == R_CHANGED && s.cur.b[3][S_R].kind != ctl::ACT_NONE);
    CHECK(press(s, K_DEFAULT) == R_CHANGED && same(s.cur.b[3][S_R], s.def.b[3][S_R]));
    CHECK(press(s, K_DEFAULT) == R_NONE);                        // already the default
    CHECK(press(s, K_TAB_R) == R_NONE && s.tab == TAB_TUNE);
    s.row = 2;
    CHECK(press(s, K_RIGHT) == R_CHANGED && s.cur.v[2] == 850);  // tuning: Right changes the value
    CHECK(press(s, K_LEFT) == R_CHANGED && s.cur.v[2] == 800);
    CHECK(press(s, K_DEFAULT) == R_NONE);
    CHECK(press(s, K_TAB_L) == R_NONE && s.tab == TAB_BUTTONS);
    CHECK(press(s, K_CLOSE) == R_CLOSE);
}

static void test_touch_zones() {
    using namespace lay;
    State s = fresh();
    // a tab
    Zone z = hit(s, tab_x(1) + 10, TAB_Y + 5); CHECK(z.kind == Z_TAB && z.a == 1);
    // a grid cell: the middle only focuses on the first tap, then cycles
    const int cx = grid_cell_x(S_PLAIN) + GRID_CELL_W / 2, cy = grid_row_y(3) + 5;
    z = hit(s, cx, cy); CHECK(z.kind == Z_CELL && z.a == 3 && z.b == 0 && z.dir == 0);
    CHECK(touch(s, z) == R_NONE && s.row == 3);                  // focus only
    CHECK(same(s.cur.b[3][0], s.def.b[3][0]));
    CHECK(touch(s, z) == R_CHANGED && !same(s.cur.b[3][0], s.def.b[3][0]));
    // its arrows
    z = hit(s, grid_cell_x(0) + 5, cy); CHECK(z.dir == -1 && repeats(z));
    CHECK(touch(s, z) == R_CHANGED && same(s.cur.b[3][0], s.def.b[3][0]));
    z = hit(s, grid_cell_x(0) + GRID_CELL_W - 5, cy); CHECK(z.dir == +1);
    // a first tap on an ARROW of an unfocused cell changes it straight away
    z = hit(s, grid_cell_x(S_R) + GRID_CELL_W - 5, grid_row_y(5) + 5);
    CHECK(z.kind == Z_CELL && z.dir == +1 && touch(s, z) == R_CHANGED && s.row == 5 && s.col == S_R);
    // locked cells and gaps are not zones
    CHECK(hit(s, grid_cell_x(S_R) + 40, grid_row_y(6) + 5).kind == Z_NONE);
    CHECK(hit(s, grid_cell_x(S_PLAIN) + GRID_CELL_W + 2, grid_row_y(3) + 5).kind == Z_NONE);
    CHECK(hit(s, 400, 300 + 200).kind == Z_NONE);
    // the label column focuses its row
    z = hit(s, PAD + 10, grid_row_y(8) + 5); CHECK(z.kind == Z_CELL && z.a == 8 && z.dir == 0);
    // the footer
    CHECK(hit(s, BTN_SAVE_X + 5, FOOT_Y + 5).kind == Z_SAVE);
    CHECK(touch(s, Zone{Z_SAVE, 0, 0, 0}) == R_CLOSE);
    CHECK(hit(s, BTN_RESET_X + 5, FOOT_Y + 5).kind == Z_RESET);
    // the tuning tab: a heading is not a zone; arrows step
    set_tab(s, TAB_TUNE);
    CHECK(hit(s, TUNE_X0 + 20, tune_row_y(0) + 5).kind == Z_NONE);
    z = hit(s, TUNE_X0 + TUNE_CELL_W - 5, tune_row_y(2) + 5);
    CHECK(z.kind == Z_CELL && z.a == 2 && z.dir == +1 && touch(s, z) == R_CHANGED && s.cur.v[2] == 850);
    z = hit(s, TUNE_X0 + TUNE_CELL_W / 2, tune_row_y(3) + 5);
    CHECK(touch(s, z) == R_NONE && s.row == 3);                  // the middle never changes a number
    // every cell is at least a finger tall and wide
    CHECK(ROW_H >= 24 && ARROW_W >= 28);
}

static void test_layout_fits_the_screen() {
    using namespace lay;
    CHECK(grid_row_y(NROW - 1) + ROW_H <= HINT_Y - 6);
    CHECK(tune_row_y(NTUNE - 1) + ROW_H <= HINT_Y - 6);
    CHECK(grid_cell_x(NSLOT - 1) + GRID_CELL_W <= W - PAD);
    CHECK(BAR_X0 + BAR_W <= W - PAD);
    CHECK(NOTE_Y + 16 <= FOOT_Y && FOOT_Y + FOOT_H <= H);
    CHECK(BTN_SAVE_X + BTN_SAVE_W < W);
}

static std::string run_merge(const char* in, const char* k, const char* v) {
    KV kv; std::snprintf(kv.key, sizeof kv.key, "%s", k); std::snprintf(kv.val, sizeof kv.val, "%s", v);
    return merge(in, &kv, 1);
}
static void test_merge_keeps_what_it_does_not_touch() {
    const char* f = "# my notes\nsens=18   # fast enough\n\ncross=run\nl=aim\n";
    CHECK(run_merge(f, "sens", "25") == "# my notes\nsens=25  # fast enough\n\ncross=run\nl=aim\n");
    // an alias, another case and a duplicate are the same key
    CHECK(run_merge("croix=rclick\nCROSS = lclick\nup=pot1\n", "cross", "run") == "cross=run\nup=pot1\n");
    CHECK(run_merge("R+Croix=f5\n", "r+cross", "none") == "r+cross=none\n");
    CHECK(run_merge("l+cross=inv\nr+cross=f5\n", "r+cross", "none") == "l+cross=inv\nr+cross=none\n");
    // a commented-out line is a comment, not the key
    CHECK(run_merge("#sens=18\n", "sens", "25") == "#sens=18\n\n# Set from the in-game Controls panel\nsens=25\n");
    // missing: appended, with the file's own line ending and a final newline
    CHECK(run_merge("l=aim", "cone", "40") == "l=aim\n\n# Set from the in-game Controls panel\ncone=40\n");
    CHECK(run_merge("l=aim\r\nr=rclick\r\n", "cone", "40") == "l=aim\r\nr=rclick\r\n\r\n# Set from the in-game Controls panel\r\ncone=40\r\n");
    CHECK(run_merge("", "cone", "40") == "# Set from the in-game Controls panel\ncone=40\n");
    // CRLF edit in place stays CRLF; a BOM on the first line survives
    CHECK(run_merge("sens=18\r\ncone=35\r\n", "cone", "40") == "sens=18\r\ncone=40\r\n");
    CHECK(run_merge("\xEF\xBB\xBF" "sens=18\n", "sens", "20") == "\xEF\xBB\xBF" "sens=20\n");
    // a value that is the key of nothing else, spaces around '='
    CHECK(run_merge("hud_h = 60\n", "hud_h", "70") == "hud_h=70\n");
}

static void test_the_loader_reads_back_what_the_panel_writes() {
    State s = fresh();
    s.cur.b[3][0] = Bind{ctl::ACT_KEY, 0x4F};                       // not in the table: vk:0x4F
    s.cur.b[7][2] = Bind{ctl::ACT_AIM, 0};
    s.cur.b[2][0] = Bind{ctl::ACT_RMB, 0};
    s.cur.v[1] = 45; s.cur.v[2] = 1200;
    s.cur.v[tune(6).type == T_SWITCH ? 6 : 6] = 0;                  // diamond off
    KV kv[40]; const int n = diff(s, kv, 40);
    CHECK(n == 6);
    std::string text = "cone=35\n";
    text = merge(text, kv, n);
    CHECK(text.find("cone=45") != std::string::npos);
    CHECK(text.find("hostile_reach=1200") != std::string::npos);
    CHECK(text.find("diamond=off") != std::string::npos);
    CHECK(text.find("cross=vk:0x4F") != std::string::npos);
    CHECK(text.find("l+up=aim") != std::string::npos);
    CHECK(text.find("select=rclick") != std::string::npos);
    // every written line is accepted by the loader's own rules
    size_t p = 0; int lines = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(p, e - p); p = e + 1;
        char* k; char* v;
        if (!ctl::split_line(&ln[0], &k, &v)) continue;
        double d, lo, hi; ctl::ActSpec a;
        const int nk = ctl::numeric_key(k, v, &d, &lo, &hi);
        if (nk) CHECK(nk == 1);
        else if (!strcasecmp(k, "diamond")) { bool b; CHECK(ctl::parse_switch(v, &b)); }
        else {
            CHECK(ctl::parse_action(v, &a));
            const char* kk = k; if (!strncasecmp(kk, "r+", 2) || !strncasecmp(kk, "l+", 2)) kk += 2;
            CHECK(!strcasecmp(kk, "select") || ctl::button_bit(kk) != 0);
        }
        ++lines;
    }
    CHECK(lines == 6);
    CHECK(dirty(s));
    s.base = s.cur; CHECK(!dirty(s)); CHECK(diff(s, kv, 40) == 0);
}

static void test_diff_only_lists_what_changed_and_never_locked_cells() {
    State s = fresh();
    KV kv[64]; CHECK(diff(s, kv, 64) == 0);
    s.cur.b[6][S_R] = Bind{ctl::ACT_KEY, 0x74};                     // locked: ignored even if someone set it
    s.cur.b[0][S_R] = Bind{ctl::ACT_KEY, 0x74};
    CHECK(diff(s, kv, 64) == 0 && !dirty(s));
    s.cur.b[0][0] = Bind{ctl::ACT_NONE, 0};
    CHECK(diff(s, kv, 64) == 1 && !std::strcmp(kv[0].key, "l") && !std::strcmp(kv[0].val, "none"));
    CHECK(aim_bound(s.def) && !aim_bound(s.cur));
}

static int lit(const std::vector<uint32_t>& fb, int x0, int y0, int x1, int y1, uint32_t c) {
    int n = 0; for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) n += fb[(size_t)y * 960 + x] == c;
    return n;
}
static void test_render_paints_every_pixel_and_stays_inside() {
    State s = fresh();
    std::vector<uint32_t> fb(960 * 544, 0xDEADBEEFu);
    d2ch::draw(s, fb.data(), 960, 544);                                // open: the whole screen is repainted
    CHECK(lit(fb, 0, 0, 960, 544, 0xDEADBEEFu) == 0);
    set_tab(s, TAB_TUNE);
    std::fill(fb.begin(), fb.end(), 0xDEADBEEFu);
    d2ch::draw(s, fb.data(), 960, 544);
    CHECK(lit(fb, 0, 0, 960, 544, 0xDEADBEEFu) == 0);
    s.open = 0;                                                        // closed: only the little tab in the left band
    std::fill(fb.begin(), fb.end(), 0xDEADBEEFu);
    d2ch::draw(s, fb.data(), 960, 544);
    CHECK(lit(fb, d2ch::BAND_X1, 0, 960, 544, 0xDEADBEEFu) == (960 - d2ch::BAND_X1) * 544);
    CHECK(lit(fb, 0, 0, d2ch::BAND_X1, 500, 0xDEADBEEFu) == d2ch::BAND_X1 * 500);
    // a state full of long values and a long note must not draw outside the buffer (ASan would trap)
    s.open = 1; set_tab(s, TAB_BUTTONS);
    std::snprintf(s.note, sizeof s.note, "%095d", 7);
    for (int r = 0; r < NROW; ++r) for (int c = 0; c < NSLOT; ++c) s.cur.b[r][c] = Bind{ctl::ACT_KEY, 0xFE};
    for (int i = 0; i < NTUNE; ++i) s.cur.v[i] = tune(i).hi * 3;
    d2ch::draw(s, fb.data(), 960, 544);
    set_tab(s, TAB_TUNE); d2ch::draw(s, fb.data(), 960, 544);
}

int main() {
    test_choices_parse_back_to_themselves();
    test_ranges_are_the_loaders_ranges();
    test_cycling_a_binding();
    test_locked_cells_are_never_edited_or_focused();
    test_vertical_movement_wraps_and_skips_headings();
    test_tuning_values_clamp_round_and_toggle();
    test_keys_do_what_the_footer_says();
    test_touch_zones();
    test_layout_fits_the_screen();
    test_merge_keeps_what_it_does_not_touch();
    test_the_loader_reads_back_what_the_panel_writes();
    test_diff_only_lists_what_changed_and_never_locked_cells();
    test_render_paints_every_pixel_and_stays_inside();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
