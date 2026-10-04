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
    CHECK(run_merge("#sens=18\n", "sens", "25") == "#sens=18\n\n# Set from the in-game Settings panel\nsens=25\n");
    // missing: appended, with the file's own line ending and a final newline
    CHECK(run_merge("l=aim", "cone", "40") == "l=aim\n\n# Set from the in-game Settings panel\ncone=40\n");
    CHECK(run_merge("l=aim\r\nr=rclick\r\n", "cone", "40") == "l=aim\r\nr=rclick\r\n\r\n# Set from the in-game Settings panel\r\ncone=40\r\n");
    CHECK(run_merge("", "cone", "40") == "# Set from the in-game Settings panel\ncone=40\n");
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


// ---- the Game tab (env.txt) ---------------------------------------------------
static Settings env_state(const char* text) {
    Settings o; std::memset(&o, 0, sizeof o);
    env_defaults(o); env_parse(text, o);
    return o;
}
static double ev(const Settings& o, const char* key) {
    for (int i = 0; i < NGAME; ++i) if (envsem(i).kind != E_NONE && !std::strcmp(envsem(i).key, key)) return o.v[NTUNE + i];
    return -999;
}
static int gi(const char* key) {
    for (int i = 0; i < NGAME; ++i) if (envsem(i).kind != E_NONE && !std::strcmp(envsem(i).key, key)) return NTUNE + i;
    return -1;
}

static void test_game_table_is_consistent() {
    int nonhead = 0;
    for (int i = 0; i < NGAME; ++i) {
        const Tune& t = game(i); const EnvSem& e = envsem(i);
        CHECK((t.type == T_HEAD) == (e.kind == E_NONE));
        CHECK((t.type == T_HEAD) == (t.key[0] == 0));
        if (t.type == T_HEAD) continue;
        ++nonhead;
        CHECK(!std::strcmp(t.key, e.key));
        CHECK(!std::strncmp(e.key, "D2_", 3));
        // the boot loader refuses any env.txt name containing these, so the panel could never save it
        std::string up; for (const char* k = e.key; *k; ++k) up += (char)((*k >= 'a' && *k <= 'z') ? *k - 32 : *k);
        CHECK(up.find("KEY") == std::string::npos && up.find("SECRET") == std::string::npos && up.find("PASS") == std::string::npos);
        CHECK(std::strlen(t.label) <= 27);                    // the 220 px label column
        CHECK(std::strlen(t.hint) > 20 && std::strlen(t.hint) <= 112);   // one 912 px line
        if (t.type == T_SWITCH) {
            CHECK(e.kind == E_OFFLIST || e.kind == E_ONSET);
            CHECK(e.dflt == 0.0 || e.dflt == 1.0);
            CHECK((e.kind == E_OFFLIST) == (e.dflt == 1.0));  // an off-list option is on unless told otherwise
            CHECK(e.wr[0]);
            CHECK(env_value(e, e.wr) != e.dflt);               // what the panel writes reads back as the non-default
        } else {
            CHECK(t.type == T_NUM && e.kind == E_INT);
            CHECK(e.dflt >= t.lo && e.dflt <= t.hi);
        }
        for (int j = i + 1; j < NGAME; ++j) CHECK(std::strcmp(e.key, envsem(j).key) != 0);
    }
    CHECK(nonhead == 10);
    CHECK(game(0).type == T_HEAD);
}

static void test_env_parse_reads_the_way_the_boot_loader_does() {
    Settings d = env_state("");
    CHECK(ev(d, "D2_SON") == 1 && ev(d, "D2_KBAUTO") == 1 && ev(d, "D2_HUDFILL") == 1 && ev(d, "D2_RES") == 1);
    CHECK(ev(d, "D2_RES640") == 1 && ev(d, "D2_ASPECT") == 1);
    CHECK(ev(d, "D2_RUNEWORDS_LADDER") == 0 && ev(d, "D2_RESPEC_UNLIMITED") == 0 && ev(d, "D2_LOCAL_ONLY") == 0);
    CHECK(ev(d, "D2_KBALPHA") == 80);
    CHECK(ev(env_state("D2_SON=0\n"), "D2_SON") == 0);
    CHECK(ev(env_state("D2_SON=1\n"), "D2_SON") == 1);
    CHECK(ev(env_state("D2_SON=\n"), "D2_SON") == 1);             // empty = unset for the sound code
    CHECK(ev(env_state("D2_RES=\n"), "D2_RES") == 0);             // but empty turns D2_RES off
    CHECK(ev(env_state("D2_RES=0\n"), "D2_RES") == 0 && ev(env_state("D2_RES=off\n"), "D2_RES") == 0 && ev(env_state("D2_RES=non\n"), "D2_RES") == 0);
    CHECK(ev(env_state("D2_RES=1280x720\n"), "D2_RES") == 1);
    CHECK(ev(env_state("D2_RES640=0\n"), "D2_RES640") == 0 && ev(env_state("D2_RES640=900x500\n"), "D2_RES640") == 1);
    CHECK(ev(env_state("D2_ASPECT=etire\n"), "D2_ASPECT") == 0 && ev(env_state("D2_ASPECT=stretch\n"), "D2_ASPECT") == 0);
    CHECK(ev(env_state("D2_ASPECT=0\n"), "D2_ASPECT") == 0 && ev(env_state("D2_ASPECT=iso\n"), "D2_ASPECT") == 1);
    CHECK(ev(env_state("D2_HUDFILL=0\n"), "D2_HUDFILL") == 0 && ev(env_state("D2_HUDFILL=off\n"), "D2_HUDFILL") == 0);
    CHECK(ev(env_state("D2_LOCAL_ONLY=1\n"), "D2_LOCAL_ONLY") == 1 && ev(env_state("D2_LOCAL_ONLY=0\n"), "D2_LOCAL_ONLY") == 0);
    CHECK(ev(env_state("D2_LOCAL_ONLY=\n"), "D2_LOCAL_ONLY") == 0);
    CHECK(ev(env_state("D2_RUNEWORDS_LADDER=yes\n"), "D2_RUNEWORDS_LADDER") == 1);
    CHECK(ev(env_state("D2_KBALPHA=150\n"), "D2_KBALPHA") == 100 && ev(env_state("D2_KBALPHA=-5\n"), "D2_KBALPHA") == 0);
    CHECK(ev(env_state("D2_KBALPHA=35\n"), "D2_KBALPHA") == 35);
    CHECK(ev(env_state("D2_SON=0\nD2_SON=1\n"), "D2_SON") == 1);   // setenv: the later line wins
    CHECK(ev(env_state("#D2_SON=0\n"), "D2_SON") == 1);            // a comment line is not an assignment
    CHECK(ev(env_state("D2_SON =0\n"), "D2_SON") == 1);            // "D2_SON " is another variable
    CHECK(ev(env_state("D2_SON=0\r\nD2_KBAUTO=0\r\n"), "D2_KBAUTO") == 0 && ev(env_state("D2_SON=0\r\n"), "D2_SON") == 0);
    CHECK(ev(env_state("D2_SON=0"), "D2_SON") == 0);                // no trailing newline
    CHECK(ev(env_state("D2SCHED=native\nD2_SON=0\nD2WRITE=ux0:x\n"), "D2_SON") == 0);
}

static void test_game_tab_navigation() {
    State s = fresh();
    CHECK(press(s, K_TAB_R) == R_NONE && press(s, K_TAB_R) == R_NONE && s.tab == TAB_GAME);
    CHECK(s.row == NTUNE + 1 && game(s.row - NTUNE).type != T_HEAD);
    for (int i = 0; i < NGAME * 3; ++i) { move(s, +1, 0); CHECK(s.row >= NTUNE && s.row < NITEM && item(s.row).type != T_HEAD); }
    for (int i = 0; i < NGAME * 3; ++i) { move(s, -1, 0); CHECK(s.row >= NTUNE && s.row < NITEM && item(s.row).type != T_HEAD); }
    CHECK(press(s, K_TAB_R) == R_NONE && s.tab == TAB_BUTTONS);      // three tabs wrap
    CHECK(press(s, K_TAB_L) == R_NONE && s.tab == TAB_GAME);
    CHECK(press(s, K_TAB_L) == R_NONE && s.tab == TAB_TUNE && s.row == 1);
    focus_row(s, NTUNE + 1); CHECK(s.row == 1);                      // a game row cannot be focused from the tuning tab
    set_tab(s, TAB_GAME);
    focus_row(s, 2); CHECK(s.row == NTUNE + 1);                      // nor a tuning row from the game tab
}

static void test_game_values_toggle_and_reset() {
    State s = fresh(); env_defaults(s.def); env_defaults(s.cur); s.base = s.cur;
    set_tab(s, TAB_GAME);
    const int son = gi("D2_SON"), alpha = gi("D2_KBALPHA");
    s.row = son;
    CHECK(press(s, K_RIGHT) == R_CHANGED && s.cur.v[son] == 0);
    CHECK(press(s, K_RIGHT) == R_CHANGED && s.cur.v[son] == 1);     // a switch flips either way
    CHECK(press(s, K_LEFT) == R_CHANGED && s.cur.v[son] == 0);
    CHECK(press(s, K_DEFAULT) == R_CHANGED && s.cur.v[son] == 1);
    CHECK(press(s, K_DEFAULT) == R_NONE);
    s.row = alpha;
    CHECK(press(s, K_RIGHT) == R_CHANGED && s.cur.v[alpha] == 85);
    for (int i = 0; i < 10; ++i) press(s, K_RIGHT);
    CHECK(s.cur.v[alpha] == 100 && press(s, K_RIGHT) == R_NONE);
    for (int i = 0; i < 30; ++i) press(s, K_LEFT);
    CHECK(s.cur.v[alpha] == 0 && press(s, K_LEFT) == R_NONE);
    CHECK(dirty(s) && env_dirty(s));
    KV c[64]; CHECK(diff(s, c, 64) == 0);                           // the controls.txt list never carries a game option
    value_text(game(alpha - NTUNE), 75, c[0].val, sizeof c[0].val); CHECK(!std::strcmp(c[0].val, "75"));
    State t = fresh(); set_tab(t, TAB_BUTTONS); CHECK(!env_dirty(t) && !dirty(t));
}

static std::string env_round_trip(const std::string& file, State s, KV* ev_out = nullptr, int* n_out = nullptr) {
    KV kv[32]; const int n = env_diff(s, kv, 32);
    if (ev_out) std::memcpy(ev_out, kv, sizeof(KV) * n);
    if (n_out) *n_out = n;
    return env_merge(file, kv, n);
}
static void test_env_changes_are_written_minimally_and_read_back() {
    // every option, flipped from its default and from the opposite state, survives a save and a re-open
    for (int start = 0; start < 2; ++start)
        for (int i = NTUNE; i < NITEM; ++i) {
            if (item(i).type == T_HEAD) continue;
            State s = fresh(); env_defaults(s.def);
            const char* seed[2] = { "", "D2SCHED=native\n# mine\nD2_RES=1280x720\n" };
            std::string file = seed[start];
            env_defaults(s.cur); env_parse(file, s.cur); s.base = s.cur;
            const double was = s.cur.v[i];
            s.cur.v[i] = item(i).type == T_SWITCH ? (was != 0 ? 0 : 1) : (was == 40 ? 45 : 40);
            KV kv[32]; int n;
            const std::string out = env_round_trip(file, s, kv, &n);
            CHECK(n == 1);
            Settings back; std::memset(&back, 0, sizeof back); env_defaults(back); env_parse(out, back);
            CHECK(back.v[i] == s.cur.v[i]);
            for (int j = NTUNE; j < NITEM; ++j) if (j != i && item(j).type != T_HEAD) CHECK(back.v[j] == s.base.v[j]);
            CHECK(out.find("D2SCHED=native") == (start ? 0u : std::string::npos));   // the rest of the file is untouched
            // flipping it back removes the line again (or restores the file's value) and leaves nothing to save
            State s2 = s; s2.base = back; s2.cur = back; s2.cur.v[i] = was;
            const std::string out2 = env_round_trip(out, s2);
            Settings back2; std::memset(&back2, 0, sizeof back2); env_defaults(back2); env_parse(out2, back2);
            CHECK(back2.v[i] == was);
        }
}

static void test_env_merge_keeps_what_it_does_not_touch() {
    auto run = [](const std::string& in, const char* k, const char* v) { KV kv{}; std::snprintf(kv.key, 24, "%s", k); std::snprintf(kv.val, 24, "%s", v); return env_merge(in, &kv, 1); };
    CHECK(run("", "D2_SON", "0") == "# Set from the in-game Settings panel\nD2_SON=0\n");
    CHECK(run("D2SCHED=native\n", "D2_SON", "0") == "D2SCHED=native\n\n# Set from the in-game Settings panel\nD2_SON=0\n");
    CHECK(run("D2SCHED=native", "D2_SON", "0") == "D2SCHED=native\n\n# Set from the in-game Settings panel\nD2_SON=0\n");
    CHECK(run("D2SCHED=native\r\n", "D2_SON", "0") == "D2SCHED=native\r\n\r\n# Set from the in-game Settings panel\r\nD2_SON=0\r\n");
    CHECK(run("# keep\nD2_SON=1\nX=1\n", "D2_SON", "0") == "# keep\nD2_SON=0\nX=1\n");
    CHECK(run("D2_SON=1\r\nX=1\r\n", "D2_SON", "0") == "D2_SON=0\r\nX=1\r\n");
    CHECK(run("D2_SON=1\nX=1\nD2_SON=0\n", "D2_SON", "1") == "D2_SON=1\nX=1\n");              // a duplicate is dropped
    CHECK(run("D2_SON=0\nX=1\n", "D2_SON", "") == "X=1\n");                                    // empty value: the line goes
    CHECK(run("X=1\n", "D2_SON", "") == "X=1\n");                                              // nothing to remove, nothing appended, no header
    CHECK(run("#D2_SON=0\n", "D2_SON", "0") == "#D2_SON=0\n\n# Set from the in-game Settings panel\nD2_SON=0\n");   // a comment is not the line
    CHECK(run("D2_SON2=0\nD2_SO=1\n", "D2_SON", "0") == "D2_SON2=0\nD2_SO=1\n\n# Set from the in-game Settings panel\nD2_SON=0\n");   // exact names only
    CHECK(run("D2_SON=0 # old\n", "D2_SON", "1") == "D2_SON=1\n");                             // env.txt has no trailing comments: the value was "0 # old"
}

static void test_game_tab_touch_and_layout() {
    State s = fresh();
    set_tab(s, TAB_BUTTONS);
    Zone z = hit(s, lay::tab_x(2) + 5, lay::TAB_Y + 5);
    CHECK(z.kind == Z_TAB && z.a == TAB_GAME);
    CHECK(touch(s, z) == R_NONE && s.tab == TAB_GAME);
    CHECK(lay::item_row_y(NITEM - 1) + lay::ROW_H <= lay::HINT_Y - 6);
    const int son = gi("D2_SON");
    z = hit(s, lay::TUNE_X0 + lay::TUNE_CELL_W - 5, lay::item_row_y(son) + 5);
    CHECK(z.kind == Z_CELL && z.a == son && z.dir == +1);
    env_defaults(s.cur); s.base = s.cur;
    CHECK(touch(s, z) == R_CHANGED && s.cur.v[son] == 0 && s.row == son);
    CHECK(repeats(z));
    z = hit(s, lay::TUNE_X0 + lay::TUNE_CELL_W / 2, lay::item_row_y(son) + 5);
    CHECK(z.kind == Z_CELL && z.dir == 0 && touch(s, z) == R_NONE);          // the middle only focuses
    CHECK(hit(s, lay::TUNE_X0 + 20, lay::item_row_y(0) + 5).kind == Z_NONE);  // a heading
    // the controls tab's rows are not reachable through the game tab's geometry and vice versa
    set_tab(s, TAB_TUNE);
    z = hit(s, lay::TUNE_X0 + lay::TUNE_CELL_W - 5, lay::tune_row_y(2) + 5);
    CHECK(z.kind == Z_CELL && z.a == 2);
}

static void test_render_game_tab() {
    State s = fresh(); env_defaults(s.def); env_defaults(s.cur); s.base = s.cur; set_tab(s, TAB_GAME);
    std::vector<uint32_t> fb(960 * 544, 0xDEADBEEFu);
    d2ch::draw(s, fb.data(), 960, 544);
    CHECK(lit(fb, 0, 0, 960, 544, 0xDEADBEEFu) == 0);
    for (int i = NTUNE; i < NITEM; ++i) s.cur.v[i] = item(i).hi * 3;     // out-of-range values must still draw safely
    std::snprintf(s.note, sizeof s.note, "%095d", 7);
    for (int r = NTUNE; r < NITEM; ++r) { s.row = r; d2ch::draw(s, fb.data(), 960, 544); }
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
    test_game_table_is_consistent();
    test_env_parse_reads_the_way_the_boot_loader_does();
    test_game_tab_navigation();
    test_game_values_toggle_and_reset();
    test_env_changes_are_written_minimally_and_read_back();
    test_env_merge_keeps_what_it_does_not_touch();
    test_game_tab_touch_and_layout();
    test_render_game_tab();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
