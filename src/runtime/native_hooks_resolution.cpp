// native_hooks_resolution.cpp -- voir native_hooks_resolution.h.
//
// Toutes les adresses sont des RVA de Game.exe 1.14d, relevees au
// desassemblage. exe_identity (taille + TimeDateStamp) refuse deja de demarrer
// sur un autre binaire ; chaque crochet revalide en plus ses octets d'entree et
// REFUSE bruyamment plutot que de s'armer sur du code qu'il ne reconnait pas.
#include "native_hooks_resolution.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
using namespace d2rt;

// La fenetre GXM suit la bascule : c'est elle qui rend la projection 1:1.
// Declare FAIBLE et hors du namespace anonyme, sinon le nom decore ne
// correspondrait pas a celui de vita_gxm.cpp (et le symbole resterait nul).
__attribute__((weak)) void d2gxm_set_window(int w, int h);

namespace {

// Entrees de fonction. Crocheter une cible de saut laisserait le crochet muet :
// la fusion de blocs du dynarec l'avalerait.
constexpr uint32_t RVA_GETSIZE = 0x000f5570;  // GetResolutionSize(mode,*w,*h), stdcall, ret 0xc
constexpr uint32_t RVA_SETRES  = 0x0004ba20;  // D2Client SetResolution(ecx=mode)
constexpr uint32_t RVA_SHIFT   = 0x00056ee0;  // contient set_screen_shift

// Les deux premiers sont la source de verite du moteur (343 et 388 references
// dans .text) : les 126 sites de centrage de l'interface les lisent et suivent
// donc tout seuls. Les trois suivants sont derives, mais activement lus.
constexpr uint32_t G_W    = 0x0031146c;  // GeneralDisplayWidth
constexpr uint32_t G_H    = 0x00311470;  // GeneralDisplayHeight
constexpr uint32_t G_WCPY = 0x00311474;
constexpr uint32_t G_HM40 = 0x003a521c;  // hauteur - 40
constexpr uint32_t G_WCP2 = 0x003a5220;
constexpr uint32_t G_SHX  = 0x003a2858;  // ScreenShiftX
constexpr uint32_t G_SHY  = 0x003a285c;  // ScreenShiftY
constexpr uint32_t G_INV  = 0x003a5218;  // 0 = lignes 640 d'Inventory.txt, 1 = lignes 800
constexpr uint32_t G_GLW  = 0x003c9138;  // largeur vue par le renderer
constexpr uint32_t G_GLH  = 0x003c913c;

const uint8_t SIG_GETSIZE[9] = { 0x55,0x8b,0xec,0x8b,0x45,0x08,0x83,0xf8,0x03 };
const uint8_t SIG_SETRES [4] = { 0x56,0x8b,0xf1,0xe8 };
const uint8_t SIG_SHIFT  [7] = { 0x55,0x8b,0xec,0x83,0xec,0x0c,0x53 };

int g_on = 0, g_applied = 0, g_w = 960, g_h = 544;
uint32_t g_base = 0;

// Ces fonctions ne sont pas reentrantes (un changement de resolution a la
// fois) : un emplacement occupe signalerait qu'on s'est trompe de cible.
struct Slot { bool busy; uint32_t ret; };
Slot s_set{false,0}, s_shift{false,0};
uint32_t s_trapSet = 0, s_trapShift = 0;

bool sig_ok(Cpu* c, uint32_t rva, const uint8_t* want, size_t n, const char* who) {
    uint8_t got[16];
    c->read(g_base + rva, got, (uint32_t)n);
    if (!std::memcmp(got, want, n)) return true;
    jpline("res: REFUS %s Game+0x%x — entree %02x %02x %02x %02x inattendue",
           who, (unsigned)rva, got[0], got[1], got[2], got[3]);
    return false;
}

// Ce que SetResolution aurait ecrit s'il connaissait notre taille. Ce sont des
// ecritures de DONNEES : le jeu fait exactement les memes.
//
// Le decalage ecran : le binaire code +80/-60 pour le mode 2 (800x600). La
// generalisation qui redonne ces constantes est x = w/2-320, y = -(h/2-240).
void apply(Cpu& c) {
    c.write_u32(g_base + G_W,    (uint32_t)g_w);
    c.write_u32(g_base + G_H,    (uint32_t)g_h);
    c.write_u32(g_base + G_WCPY, (uint32_t)g_w);
    c.write_u32(g_base + G_WCP2, (uint32_t)g_w);
    c.write_u32(g_base + G_HM40, (uint32_t)(g_h - 40));
    c.write_u32(g_base + G_SHX,  (uint32_t)(int32_t)(g_w / 2 - 320));
    c.write_u32(g_base + G_SHY,  (uint32_t)(int32_t)(-(g_h / 2 - 240)));
    c.write_u32(g_base + G_INV,  1u);
    c.write_u32(g_base + G_GLW,  (uint32_t)g_w);
    c.write_u32(g_base + G_GLH,  (uint32_t)g_h);
    const bool premier = !g_applied;
    g_applied = 1;
    if (premier && d2gxm_set_window) d2gxm_set_window(g_w, g_h);
    if (premier) {
        jpline("res: %dx%d applique (relecture %ux%u, decalage %d,%d)", g_w, g_h,
               c.read_u32(g_base + G_W), c.read_u32(g_base + G_H),
               g_w / 2 - 320, -(g_h / 2 - 240)); }
}

// Pose un trap de sortie sur l'adresse de retour invitee, puis rejoue le
// prologue et reprend le corps original. La pile invitee est ecrite, jamais
// l'image.
void enter(Cpu& c, Bridge& br, Slot& s, uint32_t trap, uint32_t entry, int reg) {
    const uint32_t E = c.reg(R_ESP);
    if (!s.busy) { s.busy = true; s.ret = c.read_u32(E); c.write_u32(E, trap); }
    c.write_u32(E - 4, c.reg(reg));
    c.set_reg(R_ESP, E - 8);
    br.redirect_next(entry + 1);
}
uint32_t leave(Cpu& c, Bridge& br, Slot& s) {
    const uint32_t ret = s.ret; s.busy = false;
    c.set_reg(R_ESP, c.reg(R_ESP) - 4);
    br.redirect_next(ret);
    return c.reg(R_EAX);
}

}  // namespace

// Le glide3x ne doit ouvrir la grande fenetre qu'APRES la bascule : les
// MENUS ont un art en 800x600 fixe et ne peuvent pas remplir 960x544.
extern "C" int d2res_active(void) { return g_on && g_applied; }
extern "C" int d2res_w(void)      { return g_w; }
extern "C" int d2res_h(void)      { return g_h; }

void native_hooks_resolution_install(Cpu* cpu, Bridge& br) {
    if (const char* e = getenv("D2_RES")) {
        if (!*e || !std::strcmp(e, "0") || !std::strcmp(e, "non") || !std::strcmp(e, "off")) return;
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2) { g_w = w; g_h = h; }
        else { jpline("res: D2_RES=%s ignore (attendu LxH, ou 0)", e); return; }
    }
    // Plancher derive d'Inventory.txt : le panneau d'inventaire est centre avec
    // un ecart constant, donc son bord haut vaut h/2-240. En dessous de 480 de
    // haut il sortirait de l'ecran. 544 passe avec 64 px de marge.
    if (g_w < 640 || g_h < 480 || g_w > 1280 || g_h > 1024) {
        jpline("res: %dx%d hors bornes (640x480 a 1280x1024)", g_w, g_h); return; }
    if (!g_114 || !g_d2base) { jpline("res: REFUS — 1.14d non detecte"); return; }
    g_base = g_d2base;

    // Tout ou rien : un crochet sur deux donnerait une image incoherente.
    if (!sig_ok(cpu, RVA_GETSIZE, SIG_GETSIZE, sizeof SIG_GETSIZE, "GetResolutionSize") ||
        !sig_ok(cpu, RVA_SETRES,  SIG_SETRES,  sizeof SIG_SETRES,  "SetResolution")     ||
        !sig_ok(cpu, RVA_SHIFT,   SIG_SHIFT,   sizeof SIG_SHIFT,   "set_screen_shift"))
        return;

    // 1. GetResolutionSize : remplacement total. Table de saut a 4 entrees,
    //    taille en dur dans chaque branche -- c'est la seule des trois
    //    fonctions mode->taille du binaire a n'avoir aucun chemin par les
    //    globals. Ses 6 appelants dimensionnent la fenetre et la surface.
    {
        Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "native!d2_getressize_114";
        s.fn = [](Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(R_ESP);
            const uint32_t mode = c.read_u32(E + 4);
            const uint32_t pW = c.read_u32(E + 8), pH = c.read_u32(E + 12);
            uint32_t w, h;
            switch (mode) {
                case 0:  w = 640;  h = 480;  break;   // le « petit » mode reste intact
                case 1:
                case 2:  if (g_applied) { w = (uint32_t)g_w; h = (uint32_t)g_h; }
                         else           { w = 800; h = 600; }
                         break;
                case 3:  w = 1344; h = 700;  break;   // mode mort du binaire
                default: w = 0;    h = 0;    break;
            }
            if (pW) c.write_u32(pW, w);
            if (pH) c.write_u32(pH, h);
            return pH; };
        br.register_shim("native.hook", "d2_getressize_114", s);
        cpu->set_alternate(g_base + RVA_GETSIZE, br.shim_trap("native.hook", "d2_getressize_114"));
    }

    // 2. SetResolution : appelee a l'entree en partie, elle REECRIT 800x600
    //    pour le mode 2. On la laisse s'executer (elle fait toute sa plomberie)
    //    puis on repose nos valeurs.
    {
        Shim xe; xe.argc = 0; xe.stdcall_cleanup = false; xe.tag = "native!d2_setres_exit";
        xe.fn = [&br](Cpu& c) -> uint32_t { apply(c); return leave(c, br, s_set); };
        br.register_shim("native.hook", "d2_setres_exit", xe);
        s_trapSet = br.shim_trap("native.hook", "d2_setres_exit");

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_setres_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            enter(c, br, s_set, s_trapSet, g_base + RVA_SETRES, R_ESI);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_setres_114", s);
        cpu->set_alternate(g_base + RVA_SETRES, br.shim_trap("native.hook", "d2_setres_114"));
    }

    // 3. set_screen_shift : ecrit +80/-60 pour le mode 2, 0/0 sinon, et une
    //    branche precoce peut sauter l'ecriture. On repose donc sans condition
    //    en sortie : la valeur juste pour notre taille est la meme dans tous
    //    les cas.
    {
        Shim xe; xe.argc = 0; xe.stdcall_cleanup = false; xe.tag = "native!d2_shift_exit";
        xe.fn = [&br](Cpu& c) -> uint32_t { apply(c); return leave(c, br, s_shift); };
        br.register_shim("native.hook", "d2_shift_exit", xe);
        s_trapShift = br.shim_trap("native.hook", "d2_shift_exit");

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_shift_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            enter(c, br, s_shift, s_trapShift, g_base + RVA_SHIFT, R_EBP);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_shift_114", s);
        cpu->set_alternate(g_base + RVA_SHIFT, br.shim_trap("native.hook", "d2_shift_114"));
    }

    g_on = 1;
    jpline("res: %dx%d arme — 3 alternates, bascule a l'entree en partie", g_w, g_h);
}
