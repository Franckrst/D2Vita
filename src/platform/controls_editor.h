// src/platform/controls_editor.h -- the model behind the title-screen Controls
// panel: what can be edited, how a press / a tap changes it, where things sit
// on screen (for touch hit-testing), and how a change is written back into
// ux0:data/d2vita/controls.txt without losing the player's own comments.
//
// Pure: no SDK, no syscall, no heap in State (it is written by the input tick
// and read by the presentation path, "benign race, single writer" like
// vita_kb.h). The numeric ranges are NOT a second copy of the truth: a host
// test checks every key against ctl::numeric_key(), which stays the single
// authority the loader enforces.
//
// The panel has a third tab, "Game", for the functional switches that live in
// ux0:data/d2vita/env.txt rather than controls.txt (resolution, sound,
// single-player options...). env.txt is only read at boot, so those changes
// apply at the next launch; the model reads the file's own text to show the
// current state and rewrites only the lines the player changed.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "platform/controls_scan.h"

namespace ced {

// ---- actions a button can be given -----------------------------------------
struct Bind { int kind; int vk; };
inline bool same(const Bind& a, const Bind& b) {
    return a.kind == b.kind && (a.kind != ctl::ACT_KEY || a.vk == b.vk);
}

// Order = the order a button cycles through. `name` is the word controls.txt
// takes and MUST parse (ctl::parse_action) to the same kind/vk: a test says so.
struct Choice { const char* name; const char* label; int kind; int vk; };
inline const Choice* choices(int* n) {
    static const Choice T[] = {
        {"none",   "-",                 ctl::ACT_NONE,  0},
        {"lclick", "Left click",        ctl::ACT_LMB,   0},
        {"rclick", "Right click",       ctl::ACT_RMB,   0},
        {"aim",    "Aim assist",        ctl::ACT_AIM,   0},
        {"items",  "Item assist",       ctl::ACT_ITEMS, 0},
        {"run",    "Walk / run",        ctl::ACT_KEY,   0x52},
        {"shift",  "Shift (stand still)", ctl::ACT_KEY, 0x10},
        {"alt",    "Alt (item names)",  ctl::ACT_KEY,   0x12},
        {"swap",   "Weapon swap",       ctl::ACT_KEY,   0x57},
        {"pot1",   "Potion 1",          ctl::ACT_KEY,   0x31},
        {"pot2",   "Potion 2",          ctl::ACT_KEY,   0x32},
        {"pot3",   "Potion 3",          ctl::ACT_KEY,   0x33},
        {"pot4",   "Potion 4",          ctl::ACT_KEY,   0x34},
        {"inv",    "Inventory",         ctl::ACT_KEY,   0x49},
        {"perso",  "Character",         ctl::ACT_KEY,   0x43},
        {"skills", "Skill tree",        ctl::ACT_KEY,   0x54},
        {"quests", "Quest log",         ctl::ACT_KEY,   0x51},
        {"tab",    "Automap",           ctl::ACT_KEY,   0x09},
        {"esc",    "Escape (menu)",     ctl::ACT_KEY,   0x1B},
        {"space",  "Space",             ctl::ACT_KEY,   0x20},
        {"enter",  "Enter",             ctl::ACT_KEY,   0x0D},
        {"f1", "F1", ctl::ACT_KEY, 0x70}, {"f2", "F2", ctl::ACT_KEY, 0x71},
        {"f3", "F3", ctl::ACT_KEY, 0x72}, {"f4", "F4", ctl::ACT_KEY, 0x73},
        {"f5", "F5", ctl::ACT_KEY, 0x74}, {"f6", "F6", ctl::ACT_KEY, 0x75},
        {"f7", "F7", ctl::ACT_KEY, 0x76}, {"f8", "F8", ctl::ACT_KEY, 0x77},
    };
    *n = (int)(sizeof T / sizeof *T);
    return T;
}
inline int choice_index(const Bind& b) {
    int n; const Choice* c = choices(&n);
    for (int i = 0; i < n; ++i) if (same(b, Bind{c[i].kind, c[i].vk})) return i;
    return -1;                                   // a raw `vk:0x..` the file set by hand
}
inline void bind_label(const Bind& b, char* o, size_t n) {
    const int i = choice_index(b); int cn; const Choice* c = choices(&cn);
    if (i >= 0) snprintf(o, n, "%s", c[i].label); else snprintf(o, n, "Key 0x%02X", (unsigned)b.vk);
}
inline void bind_name(const Bind& b, char* o, size_t n) {
    const int i = choice_index(b); int cn; const Choice* c = choices(&cn);
    if (i >= 0) snprintf(o, n, "%s", c[i].name); else snprintf(o, n, "vk:0x%02X", (unsigned)b.vk);
}

// ---- the button grid ---------------------------------------------------------
enum Slot { S_PLAIN = 0, S_R = 1, S_L = 2, NSLOT = 3 };
struct BtnRow { const char* key; const char* label; };
constexpr int NROW = 12;
inline const BtnRow& row_def(int r) {
    static const BtnRow T[NROW] = {
        {"l", "L (held)"}, {"r", "R (held)"}, {"select", "Select"},
        {"cross", "Cross"}, {"circle", "Circle"}, {"square", "Square"}, {"triangle", "Triangle"},
        {"up", "D-pad up"}, {"down", "D-pad down"}, {"left", "D-pad left"}, {"right", "D-pad right"},
        {"start", "Start"},
    };
    return T[r];
}
// nullptr = the cell can be edited; otherwise the text shown in its place
// (the game fixes those bindings, controls.txt cannot move them).
inline const char* cell_lock(int row, int slot) {
    if (row <= 2) {                                    // L, R, Select have no combo layers
        if (row == 2 && slot == S_R) return "Space (fixed)";
        return slot == S_PLAIN ? nullptr : "";
    }
    if (row == 6 && slot == S_R) return "Keyboard (fixed)";
    if (row == 11 && slot == S_L) return "Screenshot (fixed)";
    return nullptr;
}
inline void cell_key(int row, int slot, char* o, size_t n) {
    const char* k = row_def(row).key;
    if (row <= 2 || slot == S_PLAIN) snprintf(o, n, "%s", k);
    else snprintf(o, n, "%s+%s", slot == S_R ? "r" : "l", k);
}

// ---- the tuning list ---------------------------------------------------------
enum { T_HEAD = 0, T_NUM = 1, T_SWITCH = 2 };
struct Tune { int type; const char* key; const char* label; const char* hint; double lo, hi, step; int dec; };
constexpr int NTUNE = 14;
inline const Tune& tune(int i) {
    static const Tune T[NTUNE] = {
        {T_HEAD,   "", "AIMING", "", 0, 0, 0, 0},
        {T_NUM,    "cone",          "Aim cone",
         "Half-angle of the cone around the cursor inside which the aim button picks a monster, in degrees.", 5, 90, 5, 0},
        {T_NUM,    "hostile_reach", "Monster reach",
         "How far the aim button reaches for a monster, in world units. It takes the one nearest the cone axis.", 100, 2000, 50, 0},
        {T_NUM,    "reach",         "Object reach",
         "How far the aim button reaches for a chest, door, portal, NPC or level exit, in world units.", 50, 2000, 25, 0},
        {T_NUM,    "hover_h",       "Hover height",
         "Where on a monster the cursor lands, in px above its feet. Raise it for tall sprites.", 0, 150, 2, 0},
        {T_NUM,    "hud_h",         "HUD band",
         "Bottom band assisted clicks never enter, in px. The game drops clicks in the last 49 px: keep above that.", 0, 300, 5, 0},
        {T_SWITCH, "diamond",       "Aim marker",
         "Show the diamond on the current aim target. The aim itself works either way.", 0, 1, 1, 0},
        {T_HEAD,   "", "STICKS", "", 0, 0, 0, 0},
        {T_NUM,    "sens",          "Cursor speed",
         "Speed of the free cursor on the right stick. Higher is faster.", 1, 100, 1, 0},
        {T_NUM,    "deadzone",      "Stick deadzone",
         "Stick travel ignored around the centre, as a fraction: 0.15 is 15 %.", 0, 0.9, 0.05, 2},
        {T_NUM,    "orbit_min",     "Walk ring, light push",
         "Aim mode: radius of the left-stick walk ring at a light push, in px at 600 lines.", 5, 400, 5, 0},
        {T_NUM,    "orbit_max",     "Walk ring, full push",
         "Aim mode: radius of the left-stick walk ring at full push, in px at 600 lines.", 5, 400, 5, 0},
        {T_NUM,    "orbit",         "Plain-scheme radius",
         "Only when no button is bound to aim: radius the left stick orbits the character, in px.", 10, 400, 5, 0},
        {T_NUM,    "anchor_y",      "Plain-scheme anchor",
         "Only when no button is bound to aim: the character's vertical screen position, in 1/1000 of the height.", 100, 900, 10, 0},
    };
    return T[i];
}

// ---- the game options tab (env.txt) ---------------------------------------------
// One row per option, parallel to game(i): how the boot loader reads the
// value (so the panel shows what the game will do) and what a non-default
// state writes. A state equal to the default REMOVES the line, so an untouched
// install keeps no env.txt entry for it.
enum { E_NONE = 0, E_OFFLIST = 1, E_ONSET = 2, E_INT = 3 };
struct EnvSem {
    const char* key;
    int         kind;
    const char* offs;     // E_OFFLIST: '|'-separated values that mean "off"; an empty token matches an empty value
    const char* wr;       // switches: the value that states the non-default
    double      dflt;     // 1 / 0 for switches, the number for E_INT
};
constexpr int NGAME = 14;
inline const Tune& game(int i) {
    static const Tune T[NGAME] = {
        {T_HEAD,   "", "DISPLAY", "", 0, 0, 0, 0},
        {T_SWITCH, "D2_RES",        "Native 960x544 in game",
         "Off draws the game at its own 800x600 with side bars. Applies at the next launch.", 0, 1, 1, 0},
        {T_SWITCH, "D2_RES640",     "640x480 zoomed to screen",
         "The in-game 640x480 option fills the screen. Off keeps the original bordered 640x480.", 0, 1, 1, 0},
        {T_SWITCH, "D2_ASPECT",     "Menus keep 4:3 shape",
         "Off stretches the menus to the full screen width instead of bordering them.", 0, 1, 1, 0},
        {T_SWITCH, "D2_HUDFILL",    "Fill HUD gaps",
         "Fills the gaps the 800-wide HUD art leaves on a 960-wide screen, and between open panels.", 0, 1, 1, 0},
        {T_HEAD,   "", "SINGLE PLAYER (differs from the original game)", "", 0, 0, 0, 0},
        {T_SWITCH, "D2_RUNEWORDS_LADDER", "Ladder runewords",
         "Lets single-player games make the 23 runewords 1.14d reserves to the ladder. Not in online games.", 0, 1, 1, 0},
        {T_SWITCH, "D2_RESPEC_UNLIMITED", "Unlimited Akara reset",
         "Akara's stat and skill reset stays available once earned (Den of Evil). Single player only.", 0, 1, 1, 0},
        {T_HEAD,   "", "SOUND AND KEYBOARD", "", 0, 0, 0, 0},
        {T_SWITCH, "D2_SON",        "Sound",
         "Off presents the game with no sound card, as a silent machine would. Applies at the next launch.", 0, 1, 1, 0},
        {T_SWITCH, "D2_KBAUTO",     "Keyboard opens by itself",
         "The virtual keyboard opens when a text field gets focus. R + Triangle still opens it by hand.", 0, 1, 1, 0},
        {T_NUM,    "D2_KBALPHA",    "Keyboard opacity",
         "How opaque the virtual keyboard is, in percent. 100 is solid, lower lets the screen show through.", 0, 100, 5, 0},
        {T_HEAD,   "", "NETWORK", "", 0, 0, 0, 0},
        {T_SWITCH, "D2_LOCAL_ONLY", "Private servers only",
         "Blocks official Battle.net: only a private or local server can be reached.", 0, 1, 1, 0},
    };
    return T[i];
}
inline const EnvSem& envsem(int i) {
    static const EnvSem T[NGAME] = {
        {"", E_NONE, "", "", 0},
        {"D2_RES",              E_OFFLIST, "0|non|off|", "0",      1},
        {"D2_RES640",           E_OFFLIST, "0",          "0",      1},
        {"D2_ASPECT",           E_OFFLIST, "etire|stretch|0", "etire", 1},
        {"D2_HUDFILL",          E_OFFLIST, "0|off",      "0",      1},
        {"", E_NONE, "", "", 0},
        {"D2_RUNEWORDS_LADDER", E_ONSET,   "",           "1",      0},
        {"D2_RESPEC_UNLIMITED", E_ONSET,   "",           "1",      0},
        {"", E_NONE, "", "", 0},
        {"D2_SON",              E_OFFLIST, "0",          "0",      1},
        {"D2_KBAUTO",           E_OFFLIST, "0",          "0",      1},
        {"D2_KBALPHA",          E_INT,     "",           "",       80},
        {"", E_NONE, "", "", 0},
        {"D2_LOCAL_ONLY",       E_ONSET,   "",           "1",      0},
    };
    return T[i];
}
constexpr int NITEM = NTUNE + NGAME;     // every editable value: controls.txt tuning, then env.txt options
inline const Tune& item(int i) { return i < NTUNE ? tune(i) : game(i - NTUNE); }

struct Settings {
    Bind   b[NROW][NSLOT];
    double v[NITEM];
};
inline bool aim_bound(const Settings& s) {
    for (int r = 0; r < NROW; ++r)
        for (int c = 0; c < NSLOT; ++c)
            if (!cell_lock(r, c) && s.b[r][c].kind == ctl::ACT_AIM) return true;
    return false;
}
inline double clamp_round(const Tune& t, double v) {
    if (v < t.lo) v = t.lo;
    if (v > t.hi) v = t.hi;
    const double p = std::pow(10.0, t.dec);
    return std::floor(v * p + 0.5) / p;
}

// The defaults of the env.txt options, as the boot code applies them when the
// file says nothing.
inline void env_defaults(Settings& o) {
    for (int i = 0; i < NGAME; ++i) if (game(i).type != T_HEAD) o.v[NTUNE + i] = envsem(i).dflt;
}
inline bool env_is_off(const char* offs, const char* val) {
    const size_t vl = std::strlen(val);
    const char* p = offs;
    for (;;) {
        const char* e = std::strchr(p, '|');
        const size_t n = e ? (size_t)(e - p) : std::strlen(p);
        if (n == vl && std::strncmp(p, val, n) == 0) return true;
        if (!e) return false;
        p = e + 1;
    }
}
inline double env_value(const EnvSem& e, const char* val) {
    switch (e.kind) {
        case E_OFFLIST: return env_is_off(e.offs, val) ? 0.0 : 1.0;
        case E_ONSET:   return (*val && std::strcmp(val, "0") != 0) ? 1.0 : 0.0;
        case E_INT: {   int v = std::atoi(val); return v < 0 ? 0 : v > 100 ? 100 : v; }
    }
    return 0.0;
}
// Reads env.txt the way the boot loader does -- one KEY=VALUE per line, no
// trimming, no trailing comments, a later line wins -- and sets the option
// values the file names. Options it does not name keep what `o` holds.
inline void env_parse(const std::string& text, Settings& o) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        const size_t cut = line.find_first_of("\r");
        if (cut != std::string::npos) line.resize(cut);
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || line[0] == '#') continue;
        for (int i = 0; i < NGAME; ++i) {
            const EnvSem& s = envsem(i);
            if (s.kind != E_NONE && line.compare(0, eq, s.key) == 0 && std::strlen(s.key) == eq)
                o.v[NTUNE + i] = env_value(s, line.c_str() + eq + 1);
        }
    }
}

// ---- the panel's state --------------------------------------------------------
enum { TAB_BUTTONS = 0, TAB_TUNE = 1, TAB_GAME = 2, NTAB = 3 };
struct State {
    int      open;
    int      tab, row, col;           // focus; on the list tabs `row` indexes item()
    Settings cur;                      // what is applied right now
    Settings base;                     // what was applied when the panel opened (diff = to save)
    Settings def;                      // the built-in defaults (Triangle puts these back)
    char     note[96];                 // one status line ("Saved...", "Restart...")
};

inline void init(State& s) { std::memset(&s, 0, sizeof s); }

inline void snap_col(State& s) {
    if (s.col < 0) s.col = 0;
    if (s.col >= NSLOT) s.col = NSLOT - 1;
    if (!cell_lock(s.row, s.col)) return;
    for (int d = 1; d < NSLOT; ++d) {
        if (s.col - d >= 0 && !cell_lock(s.row, s.col - d)) { s.col -= d; return; }
        if (s.col + d < NSLOT && !cell_lock(s.row, s.col + d)) { s.col += d; return; }
    }
}
// The rows a list tab shows: item() indices [first_item, first_item + n_items).
inline int first_item(int tab) { return tab == TAB_GAME ? NTUNE : 0; }
inline int n_items(int tab) { return tab == TAB_BUTTONS ? NROW : tab == TAB_TUNE ? NTUNE : NGAME; }
inline void set_tab(State& s, int t) {
    t = ((t % NTAB) + NTAB) % NTAB;
    if (t == s.tab && s.row >= 0) return;
    s.tab = t; s.col = 0;
    s.row = t == TAB_BUTTONS ? 0 : first_item(t) + 1;      // the first row of a list is a heading
}
inline void focus_row(State& s, int r) {
    if (s.tab == TAB_BUTTONS) { if (r >= 0 && r < NROW) { s.row = r; snap_col(s); } }
    else if (r >= first_item(s.tab) && r < first_item(s.tab) + n_items(s.tab) && item(r).type != T_HEAD) s.row = r;
}
inline void move(State& s, int dr, int dc) {
    if (dr) {
        const int lo = first_item(s.tab), n = n_items(s.tab);
        int r = s.row;
        for (int i = 0; i < n; ++i) {
            r = lo + ((r - lo + dr) % n + n) % n;
            if (s.tab == TAB_BUTTONS || item(r).type != T_HEAD) break;
        }
        s.row = r;
        if (s.tab == TAB_BUTTONS) snap_col(s);
    }
    if (dc && s.tab == TAB_BUTTONS) {
        for (int c = s.col + dc; c >= 0 && c < NSLOT; c += dc)
            if (!cell_lock(s.row, c)) { s.col = c; break; }
    }
}

// Cycle the focused binding / step the focused value. True when it changed.
inline bool step(State& s, int dir) {
    if (s.tab == TAB_BUTTONS) {
        if (cell_lock(s.row, s.col)) return false;
        int n; choices(&n);
        Bind& b = s.cur.b[s.row][s.col];
        int i = choice_index(b);
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : ((i + dir) % n + n) % n;
        const Choice& c = choices(&n)[i];
        b = Bind{c.kind, c.vk};
        return true;
    }
    const Tune& t = item(s.row);
    if (t.type == T_HEAD) return false;
    double& v = s.cur.v[s.row];
    const double nv = t.type == T_SWITCH ? (v != 0.0 ? 0.0 : 1.0) : clamp_round(t, v + dir * t.step);
    if (nv == v) return false;
    v = nv;
    return true;
}
inline bool reset_focus(State& s) {
    if (s.tab == TAB_BUTTONS) {
        if (cell_lock(s.row, s.col)) return false;
        Bind& b = s.cur.b[s.row][s.col];
        const Bind d = s.def.b[s.row][s.col];
        if (same(b, d)) return false;
        b = d; return true;
    }
    if (item(s.row).type == T_HEAD || s.cur.v[s.row] == s.def.v[s.row]) return false;
    s.cur.v[s.row] = s.def.v[s.row];
    return true;
}

enum Key { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_NEXT, K_PREV, K_DEFAULT, K_TAB_L, K_TAB_R, K_CLOSE };
enum Res { R_NONE = 0, R_CHANGED = 1, R_CLOSE = 2 };
// What each pad key does. Left/Right walk the grid on the buttons tab and
// adjust the value on the tuning tab, where there is no second axis to walk.
inline Res press(State& s, Key k) {
    switch (k) {
        case K_UP:    move(s, -1, 0); return R_NONE;
        case K_DOWN:  move(s, +1, 0); return R_NONE;
        case K_LEFT:  if (s.tab == TAB_BUTTONS) { move(s, 0, -1); return R_NONE; }
                      return step(s, -1) ? R_CHANGED : R_NONE;
        case K_RIGHT: if (s.tab == TAB_BUTTONS) { move(s, 0, +1); return R_NONE; }
                      return step(s, +1) ? R_CHANGED : R_NONE;
        case K_NEXT:  return step(s, +1) ? R_CHANGED : R_NONE;
        case K_PREV:  return step(s, -1) ? R_CHANGED : R_NONE;
        case K_DEFAULT: return reset_focus(s) ? R_CHANGED : R_NONE;
        case K_TAB_L: set_tab(s, s.tab - 1); return R_NONE;
        case K_TAB_R: set_tab(s, s.tab + 1); return R_NONE;
        case K_CLOSE: return R_CLOSE;
    }
    return R_NONE;
}

// ---- what is different from when the panel opened ----------------------------
inline bool dirty(const State& s) {
    for (int r = 0; r < NROW; ++r)
        for (int c = 0; c < NSLOT; ++c)
            if (!cell_lock(r, c) && !same(s.cur.b[r][c], s.base.b[r][c])) return true;
    for (int i = 0; i < NITEM; ++i)
        if (item(i).type != T_HEAD && s.cur.v[i] != s.base.v[i]) return true;
    return false;
}
inline bool env_dirty(const State& s) {
    for (int i = NTUNE; i < NITEM; ++i)
        if (item(i).type != T_HEAD && s.cur.v[i] != s.base.v[i]) return true;
    return false;
}

struct KV { char key[24]; char val[24]; };
inline void value_text(const Tune& t, double v, char* o, size_t n) {
    if (t.type == T_SWITCH) { snprintf(o, n, "%s", v != 0.0 ? "on" : "off"); return; }
    if (t.dec == 0) { snprintf(o, n, "%d", (int)std::floor(v + 0.5)); return; }
    snprintf(o, n, "%.*f", t.dec, v);
    char* e = o + strlen(o);
    while (e > o + 1 && e[-1] == '0' && std::strchr(o, '.')) *--e = 0;
}
inline int diff(const State& s, KV* out, int max) {
    int n = 0;
    for (int r = 0; r < NROW; ++r)
        for (int c = 0; c < NSLOT; ++c) {
            if (cell_lock(r, c) || same(s.cur.b[r][c], s.base.b[r][c]) || n >= max) continue;
            cell_key(r, c, out[n].key, sizeof out[n].key);
            bind_name(s.cur.b[r][c], out[n].val, sizeof out[n].val);
            ++n;
        }
    for (int i = 0; i < NTUNE; ++i) {
        const Tune& t = tune(i);
        if (t.type == T_HEAD || s.cur.v[i] == s.base.v[i] || n >= max) continue;
        snprintf(out[n].key, sizeof out[n].key, "%s", t.key);
        value_text(t, s.cur.v[i], out[n].val, sizeof out[n].val);
        ++n;
    }
    return n;
}

// The env.txt lines to change. An empty val means "remove the line": the
// option went back to what the game does without one.
inline int env_diff(const State& s, KV* out, int max) {
    int n = 0;
    for (int i = 0; i < NGAME && n < max; ++i) {
        const Tune& t = game(i);
        const EnvSem& e = envsem(i);
        const double cur = s.cur.v[NTUNE + i];
        if (t.type == T_HEAD || cur == s.base.v[NTUNE + i]) continue;
        snprintf(out[n].key, sizeof out[n].key, "%s", e.key);
        if (cur == e.dflt) out[n].val[0] = 0;
        else if (e.kind == E_INT) snprintf(out[n].val, sizeof out[n].val, "%d", (int)cur);
        else snprintf(out[n].val, sizeof out[n].val, "%s", e.wr);
        ++n;
    }
    return n;
}

// ---- writing it back ------------------------------------------------------------
// `croix=`, `Cross=` and `cross=` are the same key to the loader, and so are
// `r+cross` / `R+Cross`: compare the meaning, not the spelling, or an old
// alias line would survive next to the new one and silently win or lose.
inline std::string canon_key(const char* k) {
    std::string s;
    for (; *k; ++k) s += (char)((*k >= 'A' && *k <= 'Z') ? *k + 32 : *k);
    std::string pre;
    if (s.size() > 2 && (s[0] == 'r' || s[0] == 'l') && s[1] == '+') { pre = s.substr(0, 2); s = s.substr(2); }
    static const struct { uint32_t b; const char* n; } N[] = {
        {ctl::BIT_CROSS, "cross"}, {ctl::BIT_CIR, "circle"}, {ctl::BIT_SQR, "square"}, {ctl::BIT_TRI, "triangle"},
        {ctl::BIT_UP, "up"}, {ctl::BIT_DOWN, "down"}, {ctl::BIT_LEFT, "left"}, {ctl::BIT_RIGHT, "right"},
        {ctl::BIT_START, "start"}, {ctl::BIT_SELECT, "select"} };
    const uint32_t bit = ctl::button_bit(s.c_str());
    if (bit) for (const auto& e : N) if (e.b == bit) { s = e.n; break; }
    return pre + s;
}

// Rewrites `in` so each key in kv[] has exactly one line carrying its new
// value: an existing line is replaced in place (its trailing comment kept),
// later duplicates are dropped, and keys the file never had are appended.
// Every other byte -- the player's comments, other keys, the line endings --
// is left alone.
inline std::string merge(const std::string& in, const KV* kv, int nkv) {
    std::string ck[64]; bool done[64] = {};
    if (nkv > 64) nkv = 64;
    for (int i = 0; i < nkv; ++i) ck[i] = canon_key(kv[i].key);
    const bool crlf = in.find("\r\n") != std::string::npos;
    std::string out;
    size_t pos = 0; bool first = true;
    while (pos < in.size()) {
        size_t e = in.find('\n', pos);
        const bool hasNl = e != std::string::npos;
        std::string line = in.substr(pos, hasNl ? e - pos : std::string::npos);
        pos = hasNl ? e + 1 : in.size();
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        const std::string orig = line;
        size_t bom = first ? ctl::bom_len(line.data(), line.size()) : 0;
        first = false;
        std::string work = line.substr(bom);
        char* k; char* v;
        std::string buf = work;
        int hit = -1;
        if (ctl::split_line(&buf[0], &k, &v)) {
            const std::string c = canon_key(k);
            for (int i = 0; i < nkv; ++i) if (ck[i] == c) { hit = i; break; }
        }
        if (hit < 0) { out += orig; if (hasNl) out += cr ? "\r\n" : "\n"; continue; }
        if (done[hit]) continue;                                   // a duplicate: drop it
        done[hit] = true;
        out += orig.substr(0, bom);
        out += kv[hit].key; out += '='; out += kv[hit].val;
        const size_t h = work.find('#');
        if (h != std::string::npos) { out += "  "; out += work.substr(h); }
        if (hasNl) out += cr ? "\r\n" : "\n";
    }
    bool any = false;
    for (int i = 0; i < nkv; ++i) if (!done[i]) any = true;
    if (any) {
        const char* eol = crlf ? "\r\n" : "\n";
        if (!out.empty() && out.back() != '\n') out += eol;
        if (!out.empty()) out += eol;
        out += "# Set from the in-game Settings panel"; out += eol;
        for (int i = 0; i < nkv; ++i) if (!done[i]) { out += kv[i].key; out += '='; out += kv[i].val; out += eol; }
    }
    return out;
}

// The env.txt counterpart of merge(): keys match exactly (they are
// environment variable names), a line is `KEY=VALUE` to its end with no
// trailing comment (the boot loader keeps everything after the `=`), a key
// with an empty value loses its line, and the rest of the file is untouched.
inline std::string env_merge(const std::string& in, const KV* kv, int nkv,
                             const char* header = "# Set from the in-game Settings panel") {
    bool done[32] = {};
    if (nkv > 32) nkv = 32;
    const bool crlf = in.find("\r\n") != std::string::npos;
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
        const size_t e = in.find('\n', pos);
        const bool hasNl = e != std::string::npos;
        std::string line = in.substr(pos, hasNl ? e - pos : std::string::npos);
        pos = hasNl ? e + 1 : in.size();
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        int hit = -1;
        const size_t eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos && eq != 0)
            for (int i = 0; i < nkv; ++i)
                if (std::strlen(kv[i].key) == eq && line.compare(0, eq, kv[i].key) == 0) { hit = i; break; }
        if (hit < 0) { out += line; if (hasNl) out += cr ? "\r\n" : "\n"; continue; }
        if (done[hit]) continue;
        done[hit] = true;
        if (!kv[hit].val[0]) continue;
        out += kv[hit].key; out += '='; out += kv[hit].val;
        if (hasNl) out += cr ? "\r\n" : "\n";
    }
    bool any = false;
    for (int i = 0; i < nkv; ++i) if (!done[i] && kv[i].val[0]) any = true;
    if (any) {
        const char* eol = crlf ? "\r\n" : "\n";
        if (!out.empty() && out.back() != '\n') out += eol;
        if (!out.empty()) out += eol;
        out += header; out += eol;
        for (int i = 0; i < nkv; ++i) if (!done[i] && kv[i].val[0]) { out += kv[i].key; out += '='; out += kv[i].val; out += eol; }
    }
    return out;
}

// ---- screen geometry (960x544), shared by the drawing and the hit test --------
namespace lay {
constexpr int W = 960, H = 544;
constexpr int PAD = 24;
constexpr int TAB_Y = 44, TAB_H = 28, TAB_W = 150, TAB_GAP = 10;
constexpr int ROW_H = 26;
constexpr int GRID_HEAD_Y = 80, GRID_Y0 = 102;            // buttons tab
constexpr int GRID_LABEL_W = 130, GRID_X0 = PAD + GRID_LABEL_W + 8, GRID_CELL_W = 250, GRID_GAP = 10;
constexpr int TUNE_Y0 = 80;                               // tuning tab
constexpr int TUNE_LABEL_W = 220, TUNE_X0 = PAD + TUNE_LABEL_W + 8, TUNE_CELL_W = 220;
constexpr int BAR_X0 = TUNE_X0 + TUNE_CELL_W + 24, BAR_W = 300;
constexpr int ARROW_W = 32;                               // finger-sized arrow zones at each end of a cell
constexpr int HINT_Y = 450, NOTE_Y = 486;
constexpr int FOOT_Y = 504, FOOT_H = 28;
constexpr int BTN_RESET_X = PAD, BTN_RESET_W = 190;
constexpr int BTN_SAVE_X = BTN_RESET_X + BTN_RESET_W + 12, BTN_SAVE_W = 210;

inline int tab_x(int i) { return PAD + i * (TAB_W + TAB_GAP); }
inline int grid_cell_x(int slot) { return GRID_X0 + slot * (GRID_CELL_W + GRID_GAP); }
inline int grid_row_y(int r) { return GRID_Y0 + r * ROW_H; }
inline int tune_row_y(int i) { return TUNE_Y0 + i * ROW_H; }
inline int item_row_y(int i) { return tune_row_y(i < NTUNE ? i : i - NTUNE); }
}

enum ZoneKind { Z_NONE = 0, Z_TAB, Z_CELL, Z_RESET, Z_SAVE };
// a = tab / row, b = column, dir = -1 / +1 on an arrow zone, 0 elsewhere.
struct Zone { int kind, a, b, dir; };
inline Zone hit(const State& s, int x, int y) {
    using namespace lay;
    Zone z{Z_NONE, 0, 0, 0};
    if (y >= TAB_Y && y < TAB_Y + TAB_H)
        for (int i = 0; i < NTAB; ++i)
            if (x >= tab_x(i) && x < tab_x(i) + TAB_W) { z.kind = Z_TAB; z.a = i; return z; }
    if (y >= FOOT_Y && y < FOOT_Y + FOOT_H) {
        if (x >= BTN_RESET_X && x < BTN_RESET_X + BTN_RESET_W) { z.kind = Z_RESET; return z; }
        if (x >= BTN_SAVE_X && x < BTN_SAVE_X + BTN_SAVE_W) { z.kind = Z_SAVE; return z; }
        return z;
    }
    auto arrow = [&](int cx, int cw) { return x < cx + ARROW_W ? -1 : x >= cx + cw - ARROW_W ? +1 : 0; };
    if (s.tab == TAB_BUTTONS) {
        for (int r = 0; r < NROW; ++r) {
            if (y < grid_row_y(r) || y >= grid_row_y(r) + ROW_H) continue;
            for (int c = 0; c < NSLOT; ++c) {
                if (x < grid_cell_x(c) || x >= grid_cell_x(c) + GRID_CELL_W) continue;
                if (cell_lock(r, c)) return z;
                z.kind = Z_CELL; z.a = r; z.b = c; z.dir = arrow(grid_cell_x(c), GRID_CELL_W);
                return z;
            }
            // the label column focuses the row on its plain cell
            if (x >= PAD && x < GRID_X0) { z.kind = Z_CELL; z.a = r; z.b = 0; }
            return z;
        }
        return z;
    }
    for (int i = first_item(s.tab); i < first_item(s.tab) + n_items(s.tab); ++i) {
        if (item(i).type == T_HEAD || y < item_row_y(i) || y >= item_row_y(i) + ROW_H) continue;
        if (x >= PAD && x < BAR_X0 + BAR_W) {
            z.kind = Z_CELL; z.a = i; z.b = 0;
            if (x >= TUNE_X0 && x < TUNE_X0 + TUNE_CELL_W) z.dir = arrow(TUNE_X0, TUNE_CELL_W);
        }
        return z;
    }
    return z;
}

// A touch that lands on a zone. Arrows step the value; the middle of an
// already-focused grid cell cycles it forward (a first tap only focuses, so
// brushing the screen never rewrites a binding).
inline Res touch(State& s, const Zone& z) {
    switch (z.kind) {
        case Z_TAB:   set_tab(s, z.a); return R_NONE;
        case Z_RESET: return reset_focus(s) ? R_CHANGED : R_NONE;
        case Z_SAVE:  return R_CLOSE;
        case Z_CELL: {
            const bool was = s.row == z.a && (s.tab != TAB_BUTTONS || s.col == z.b);
            focus_row(s, z.a);
            if (s.tab == TAB_BUTTONS) { s.col = z.b; snap_col(s); }
            if (z.dir) return step(s, z.dir) ? R_CHANGED : R_NONE;
            if (s.tab == TAB_BUTTONS && was) return step(s, +1) ? R_CHANGED : R_NONE;
            return R_NONE;
        }
    }
    return R_NONE;
}
// True when holding the finger there should keep stepping (an arrow zone).
inline bool repeats(const Zone& z) { return z.kind == Z_CELL && z.dir != 0; }

} // namespace ced
