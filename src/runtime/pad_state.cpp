// src/runtime/pad_state.cpp — see pad_state.h.
#include "runtime/pad_state.h"
#include <cstdlib>
#include <cstring>

namespace padst {

static Snapshot g_live, g_stable;
static bool g_written = false;

bool on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("D2_PAD"); v = (e && *e == '0') ? 0 : 1; }
    return v != 0;
}

static uint32_t g_levelPtr = 0;
void set_level_ptr(uint32_t p) { g_levelPtr = p; }

void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelNo,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* ui) {
    // An EMPTY live list means no draw pass ran, not "nothing is on screen":
    // the game skips rendering under load (Game+0x44f27e) while this hook,
    // which sits before that check, fires every frame regardless. In game the
    // player's own unit is always drawn, so zero units can only mean a skipped
    // frame. Publishing that emptiness made the reader lose every unit for a
    // frame -- and with it the unit it was following, so the cursor froze
    // while the game kept repeating the held click at the stale screen point,
    // landing on tiles once the camera had scrolled (console, 20/09).
    // The camera moves with them or not at all: on a skipped frame the kept
    // units stay paired with the camera they were drawn under.
    static int32_t s_vx = 0, s_vy = 0, s_pfx = 0, s_pfy = 0; static bool s_have = false;
    if (g_live.nUnits > 0) {
        g_stable.nUnits = g_live.nUnits;
        memcpy(g_stable.units, g_live.units, sizeof(Unit) * (size_t)g_live.nUnits);
        g_stable.viewX = s_have ? s_vx : vx; g_stable.viewY = s_have ? s_vy : vy;
        g_stable.playerFx = s_have ? s_pfx : pfx; g_stable.playerFy = s_have ? s_pfy : pfy;
    }
    s_vx = vx; s_vy = vy; s_pfx = pfx; s_pfy = pfy; s_have = true;
    g_stable.nLabels = g_live.nLabels;
    if (g_live.nLabels > 0) memcpy(g_stable.labels, g_live.labels, sizeof(Label) * (size_t)g_live.nLabels);
    g_live.frame += 1; g_stable.frame = g_live.frame; g_live.nUnits = 0; g_live.nLabels = 0;
    g_stable.inGame = playerId != 0;
    g_stable.playerId = playerId; g_stable.levelNo = levelNo; g_stable.levelPtr = g_levelPtr;
    g_stable.selValid = selValid; g_stable.selId = selId; g_stable.selType = selType;
    memcpy(g_stable.uiVars, ui, sizeof g_stable.uiVars);
    g_written = true;
}

void add_unit(const Unit& u) {
    if (g_live.nUnits < MAX_UNITS) g_live.units[g_live.nUnits++] = u;
}

void set_labels(const Label* l, int n) {
    if (n < 0) n = 0;
    if (n > MAX_LABELS) n = MAX_LABELS;
    g_live.nLabels = n;
    if (n > 0) memcpy(g_live.labels, l, sizeof(Label) * (size_t)n);
}

bool read(Snapshot& out) {
    if (!g_written) return false;
    out = g_stable;
    return true;
}

} // namespace padst
