// text_focus_probe.cpp -- see text_focus_probe.h.
#include "text_focus_probe.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdlib>
#include <cstring>
using namespace d2rt;

namespace {
// Game.exe 1.14d, D2Win : contrôle qui a le focus (écrit par SetFocus
// Game+0xf91c0, remis à 0 quand le contrôle est détruit, Game+0xf9310). Sa
// jumelle +0x3d55cc (focus effectif) n'est recopiée que par la pompe des
// menus, d'où le choix de celle-ci. Calibrée par diff d'instantanés mémoire
// (3 passes concordantes, 2026-09-21) et confirmée au désassemblage.
constexpr uint32_t RVA_FOCUSED = 0x3d55c8u;
// Premier dword du contrôle : 1 = edit box (asserté par le jeu, Game+0xfeae0).
// Seul ce dword est lu : le texte du champ (+0x5c) porte le mot de passe
// Battle.net et ne doit jamais sortir de la mémoire invitée.
constexpr uint32_t CTL_TYPE_EDIT = 1;

uint32_t g_focus = 0;
int g_enabled = -1;
int g_lines = 0;                   // journal plafonné à 64 lignes par session
const char* g_lastWhy = nullptr;   // une ligne par épisode anormal

// Cpu::read recopie sans broncher une page non mappée à l'intérieur de
// l'arène : vérifier les deux bouts avant de lire.
bool rd32(Cpu& c, uint32_t va, uint32_t& v) {
    if (va + 3 < va || !c.mapped(va) || !c.mapped(va + 3)) return false;
    return c.read(va, &v, 4);
}
bool may_log() {
    if (g_lines >= 64) return false;
    if (++g_lines == 64) { jpline("clavier: journal plafonne (64 lignes), suite muette"); return false; }
    return true;
}
} // namespace

void text_focus_poll(Cpu& c) {
    if (g_enabled < 0) {
        const char* e = std::getenv("D2_KBAUTO");
        g_enabled = (e && *e && std::strcmp(e, "0") == 0) ? 0 : 1;
        if (!g_enabled) jpline("clavier: ouverture automatique coupee (D2_KBAUTO=0)");
    }
    if (!g_enabled || !g_d2base) return;
    uint32_t p = 0, type = 0, id = 0;
    const char* why = nullptr;
    if (!rd32(c, g_d2base + RVA_FOCUSED, p)) why = "globale illisible";
    else if (p != 0) {
        if (!rd32(c, p, type)) why = "controle illisible";
        else if (type != CTL_TYPE_EDIT) why = "controle focalise qui n'est pas une edit box";
        else id = p;
    }
    if (why != g_lastWhy) {
        g_lastWhy = why;
        if (why && may_log()) jpline("clavier: focus champ ignore -- %s (type=%u)", why, (unsigned)type);
    }
    if (id != g_focus) {
        if (may_log()) {
            if (id) jpline("clavier: focus champ ON 0x%08x (type=%u)", (unsigned)id, (unsigned)type);
            else    jpline("clavier: focus champ OFF");
        }
        g_focus = id;
    }
}
extern "C" uint32_t d2vita_text_focus(void) { return g_focus; }
