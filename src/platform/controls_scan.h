// Pure, host-testable reading of controls.txt: the button-name table shared
// with the loader in vita_present.cpp, and the one question the hook
// installer needs answered before that loader has run -- does the file bind
// the `aim` action at all? (The camera/unit hooks that feed aim assist are
// only armed when it does: they cost frame time and are useless otherwise.)
#pragma once
#include <cstddef>
#include <cstdint>
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

// Same line rules as load_controls_txt(): cut at '#', split at the first '=',
// trim both sides. True when a key that really binds an action (l, r,
// select, [r+|l+]<button>) carries the value `aim`. `l=aim` is the built-in
// default: an absent file or one that never sets `l=` binds aim, and only an
// explicit `l=<other>` takes it away (unless another key still binds it).
inline bool binds_aim(const char* text, size_t len) {
    bool lAim = true, other = false;
    size_t i = 0;
    while (i < len) {
        char line[256]; size_t n = 0;
        while (i < len && text[i] != '\n') { if (n + 1 < sizeof line) line[n++] = text[i]; ++i; }
        ++i;
        line[n] = 0;
        if (char* h = strchr(line, '#')) *h = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        auto trim = [](char* s) {
            while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
            char* e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
            return s;
        };
        char* k = trim(line);
        char* v = trim(eq + 1);
        if (!k[0]) continue;
        const bool isAim = strcasecmp(v, "aim") == 0;
        if (!strcasecmp(k, "l")) { if (isAim || v[0]) lAim = isAim; continue; }
        if (!isAim) continue;
        if (!strcasecmp(k, "r") || !strcasecmp(k, "select")) { other = true; continue; }
        const bool layer = !strncasecmp(k, "r+", 2) || !strncasecmp(k, "l+", 2);
        const uint32_t bit = button_bit(layer ? k + 2 : k);
        if (bit && bit != BIT_SELECT) other = true;
    }
    return lAim || other;
}

}  // namespace ctl
