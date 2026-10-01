// Pure, host-testable reading of controls.txt: the button-name table shared
// with the loader in vita_present.cpp, and the one question the hook
// installer needs answered before that loader has run -- does the file bind
// the `aim` action at all? (The camera/unit hooks that feed aim assist are
// only armed when it does: they cost frame time and are useless otherwise.)
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace ctl {

// SCE_CTRL_* bit values (kept literal so the header stays SDK-free).
constexpr uint32_t BIT_SELECT = 0x000001, BIT_START = 0x000008, BIT_UP = 0x000010,
                   BIT_RIGHT = 0x000020, BIT_DOWN = 0x000040, BIT_LEFT = 0x000080,
                   BIT_TRI = 0x001000, BIT_CIR = 0x002000, BIT_CROSS = 0x004000,
                   BIT_SQR = 0x008000;

inline uint32_t button_bit(const char* n) {
    struct E { const char* n; uint32_t b; };
    static const E N[] = { {"cross",BIT_CROSS},{"croix",BIT_CROSS},{"circle",BIT_CIR},{"rond",BIT_CIR},
        {"square",BIT_SQR},{"carre",BIT_SQR},{"triangle",BIT_TRI},
        {"up",BIT_UP},{"down",BIT_DOWN},{"left",BIT_LEFT},{"right",BIT_RIGHT},
        {"start",BIT_START},{"select",BIT_SELECT} };
    for (const E& x : N) if (!strcasecmp(n, x.n)) return x.b;
    return 0;
}

// ---- one table, one line reader, shared with the loader ---------------------
// The hook installer (binds_aim, below) runs BEFORE the loader and reads the
// same file. Two private readers drifted apart before: `l=aim_assist` was
// aim for the loader but not for the scan, and a typo such as `l=lclik` left
// the loader on its default (aim) while the scan switched the hooks off --
// the button then did nothing at all. Everything that decides what a line
// MEANS lives here, once.
enum ActKind { ACT_NONE = 0, ACT_LMB = 1, ACT_RMB = 2, ACT_KEY = 3, ACT_ITEMS = 4, ACT_AIM = 5 };
struct ActSpec { int kind; int vk; };

inline bool parse_action(const char* v, ActSpec* out) {
    struct E { const char* n; ActSpec a; };
    static const E T[] = {
        {"lclick",{ACT_LMB,0}},{"rclick",{ACT_RMB,0}},{"none",{ACT_NONE,0}},
        {"items",{ACT_ITEMS,0}},{"item_assist",{ACT_ITEMS,0}},{"assist",{ACT_ITEMS,0}},
        {"aim",{ACT_AIM,0}},{"aim_assist",{ACT_AIM,0}},
        {"alt",{ACT_KEY,0x12}},{"shift",{ACT_KEY,0x10}},{"tab",{ACT_KEY,0x09}},{"automap",{ACT_KEY,0x09}},
        {"esc",{ACT_KEY,0x1B}},{"echap",{ACT_KEY,0x1B}},{"inv",{ACT_KEY,0x49}},{"perso",{ACT_KEY,0x43}},
        {"skills",{ACT_KEY,0x54}},{"quests",{ACT_KEY,0x51}},{"swap",{ACT_KEY,0x57}},{"space",{ACT_KEY,0x20}},
        {"run",{ACT_KEY,0x52}},{"enter",{ACT_KEY,0x0D}},
        {"pot1",{ACT_KEY,0x31}},{"pot2",{ACT_KEY,0x32}},{"pot3",{ACT_KEY,0x33}},{"pot4",{ACT_KEY,0x34}},
        {"f1",{ACT_KEY,0x70}},{"f2",{ACT_KEY,0x71}},{"f3",{ACT_KEY,0x72}},{"f4",{ACT_KEY,0x73}},
        {"f5",{ACT_KEY,0x74}},{"f6",{ACT_KEY,0x75}},{"f7",{ACT_KEY,0x76}},{"f8",{ACT_KEY,0x77}} };
    for (const E& t : T) if (!strcasecmp(v, t.n)) { *out = t.a; return true; }
    if (!strncasecmp(v, "vk:", 3) && v[3]) {
        char* e = nullptr; const long k = strtol(v + 3, &e, 0);
        if (*e || k < 1 || k > 0xFE) return false;       // a Windows virtual-key code
        *out = {ACT_KEY, (int)k}; return true;
    }
    return false;
}

inline char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

// One raw line, in place: cut at '#', split at the first '=', trim both
// sides. False for blank lines, comments and lines with no key.
inline bool split_line(char* line, char** k, char** v) {
    if (char* h = strchr(line, '#')) *h = 0;
    char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
    char* eq = strchr(line, '=');
    if (!eq) return false;
    *eq = 0;
    *k = trim(line); *v = trim(eq + 1);
    return (*k)[0] != 0;
}

// Windows editors prepend a BOM; without skipping it the first key of the
// file (often `l=` or `sens=`) was silently unknown.
inline size_t bom_len(const char* t, size_t len) {
    return len >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF ? 3 : 0;
}

// The numeric tuning keys, with the range that makes sense for each. A value
// outside it is REFUSED (and reported) rather than applied: `orbit=1000`,
// carried over from an older build, put the walk point a thousand pixels from
// the character and nothing could be controlled any more.
//   0 = not a numeric key, 1 = accepted (*out set), -1 = refused (*lo/*hi set).
inline int numeric_key(const char* k, const char* v, double* out, double* lo, double* hi) {
    struct E { const char* n; double lo, hi; };
    static const E T[] = {
        {"orbit",10,400},{"sens",1,100},{"deadzone",0,0.9},{"anchor_y",100,900},
        {"orbit_min",5,400},{"orbit_max",5,400},{"cone",5,90},{"hover_h",0,150},
        {"hud_h",0,300},{"reach",50,2000} };
    for (const E& t : T) if (!strcasecmp(k, t.n)) {
        *lo = t.lo; *hi = t.hi;
        char* e = nullptr; const double d = strtod(v, &e);
        if (!v[0] || *e || d < t.lo || d > t.hi) return -1;
        *out = d; return 1;
    }
    return 0;
}

// On/off values for the display switches (`diamond=off`).
inline bool parse_switch(const char* v, bool* out) {
    static const char* const on[]  = {"1","on","yes","true","oui"};
    static const char* const off[] = {"0","off","no","false","non"};
    for (const char* s : on)  if (!strcasecmp(v, s)) { *out = true;  return true; }
    for (const char* s : off) if (!strcasecmp(v, s)) { *out = false; return true; }
    return false;
}

// True when a key that really binds an action (l, r, select, [r+|l+]<button>)
// carries `aim`. `l=aim` is the built-in default: an absent file or one that
// never sets `l=` binds aim, and only an explicit, VALID `l=<other>` takes it
// away (unless another key still binds it).
inline bool binds_aim(const char* text, size_t len) {
    bool lAim = true, other = false;
    size_t i = bom_len(text, len);
    while (i < len) {
        char line[1024]; size_t n = 0;
        while (i < len && text[i] != '\n') { if (n + 1 < sizeof line) line[n++] = text[i]; ++i; }
        ++i;
        line[n] = 0;
        char *k, *v;
        if (!split_line(line, &k, &v)) continue;
        ActSpec a;
        if (!parse_action(v, &a)) continue;                    // the loader rejects it too
        const bool isAim = a.kind == ACT_AIM;
        if (!strcasecmp(k, "l")) { lAim = isAim; continue; }
        if (!isAim) continue;
        if (!strcasecmp(k, "r") || !strcasecmp(k, "select")) { other = true; continue; }
        const bool layer = !strncasecmp(k, "r+", 2) || !strncasecmp(k, "l+", 2);
        const uint32_t bit = button_bit(layer ? k + 2 : k);
        if (bit && bit != BIT_SELECT) other = true;
    }
    return lAim || other;
}

}  // namespace ctl
