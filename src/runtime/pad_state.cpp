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

void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelNo,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* ui) {
    g_stable.nUnits = g_live.nUnits;
    if (g_live.nUnits > 0) memcpy(g_stable.units, g_live.units, sizeof(Unit) * (size_t)g_live.nUnits);
    g_stable.nLabels = g_live.nLabels;
    if (g_live.nLabels > 0) memcpy(g_stable.labels, g_live.labels, sizeof(Label) * (size_t)g_live.nLabels);
    g_live.frame += 1; g_stable.frame = g_live.frame; g_live.nUnits = 0; g_live.nLabels = 0;
    g_stable.inGame = playerId != 0;
    g_stable.playerId = playerId; g_stable.playerFx = pfx; g_stable.playerFy = pfy;
    g_stable.viewX = vx; g_stable.viewY = vy; g_stable.levelNo = levelNo;
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
