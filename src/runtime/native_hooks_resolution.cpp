// native_hooks_resolution.cpp -- voir native_hooks_resolution.h.
//
// TOUTES les adresses ci-dessous sont des RVA de Game.exe 1.14d, relevées au
// désassemblage et vérifiées octet par octet. Elles sont ancrées à cette
// version par exe_identity (taille + TimeDateStamp), qui refuse de démarrer
// sur un autre binaire ; chaque crochet revérifie en plus ses octets d'entrée
// et REFUSE bruyamment plutôt que de s'armer sur du code qu'il ne reconnaît
// pas.
#include "native_hooks_resolution.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
using namespace d2rt;

namespace {

// ---- les cibles, en RVA 1.14d ---------------------------------------------
// Fonctions (toutes des ENTRÉES de fonction : crocheter une cible de saut
// laisserait le crochet muet, la fusion de blocs du dynarec l'avalerait).
constexpr uint32_t RVA_GETSIZE = 0x000f5570;  // GetResolutionSize(mode,*w,*h) stdcall, ret 0xc
constexpr uint32_t RVA_SETRES  = 0x0004ba20;  // D2Client SetResolution(ecx=mode)
constexpr uint32_t RVA_SHIFT   = 0x00056ee0;  // contient set_screen_shift (le site est en +0x1d)
constexpr uint32_t RVA_GLIDE   = 0x00109bc0;  // Glide SetMode : choisit l'index 7/8 et appelle grSstWinOpen

// Globals. Les deux premiers sont la source de vérité du moteur (343 et 388
// références absolues dans .text) ; les 126 sites de centrage de l'interface
// les lisent et suivent donc tout seuls.
constexpr uint32_t G_W    = 0x0031146c;  // GeneralDisplayWidth
constexpr uint32_t G_H    = 0x00311470;  // GeneralDisplayHeight
constexpr uint32_t G_WCPY = 0x00311474;  // copie de la largeur
constexpr uint32_t G_SHX  = 0x003a2858;  // ScreenShiftX
constexpr uint32_t G_SHY  = 0x003a285c;  // ScreenShiftY
constexpr uint32_t G_HM40 = 0x003a521c;  // hauteur - 40 (limite basse de la zone de panneaux)
constexpr uint32_t G_WCP2 = 0x003a5220;  // copie de la largeur (61 refs)
constexpr uint32_t G_GLW  = 0x003c9138;  // largeur vue par le renderer
constexpr uint32_t G_GLH  = 0x003c913c;  // hauteur vue par le renderer

// Empreintes d'entrée. Sans elles, un binaire légèrement différent ferait
// exécuter nos corps à la place d'autre chose, en silence.
const uint8_t SIG_GETSIZE[9] = { 0x55,0x8b,0xec,0x8b,0x45,0x08,0x83,0xf8,0x03 };
const uint8_t SIG_SETRES [4] = { 0x56,0x8b,0xf1,0xe8 };
const uint8_t SIG_SHIFT  [7] = { 0x55,0x8b,0xec,0x83,0xec,0x0c,0x53 };
const uint8_t SIG_GLIDE  [7] = { 0x55,0x8b,0xec,0x83,0xec,0x14,0x53 };

int g_on = 0, g_w = 0, g_h = 0;
uint32_t g_base = 0;

// Compteurs de passage. Un crochet pose sur autre chose qu'une ENTREE de
// fonction est SILENCIEUSEMENT muet (la fusion de blocs du dynarec l'avale) :
// sans ces compteurs, "muet" et "jamais appele" se presentent pareil.
unsigned g_nGetSize = 0, g_nSetRes = 0, g_nShift = 0, g_nGlide = 0;
void hit(const char* who, unsigned& n) {
    if (++n == 1u) jpline("res960: 1er passage dans %s", who);
}

// Un seul emplacement de trap de sortie par crochet : ces fonctions ne sont
// pas réentrantes (changement de résolution, une à la fois), et un
// emplacement occupé signifierait qu'on s'est trompé de cible.
struct Slot { bool busy; uint32_t ret; };
Slot s_slotSet{false,0}, s_slotShift{false,0}, s_slotGlide{false,0};
uint32_t s_trapSetExit = 0, s_trapShiftExit = 0, s_trapGlideExit = 0;

bool sig_ok(Cpu* c, uint32_t rva, const uint8_t* want, size_t n, const char* who) {
    uint8_t got[16];
    c->read(g_base + rva, got, (uint32_t)n);
    if (!std::memcmp(got, want, n)) return true;
    jpline("res960: REFUS %s Game+0x%x — entree %02x %02x %02x %02x inattendue",
           who, (unsigned)rva, got[0], got[1], got[2], got[3]);
    return false;
}

// Les globals que SetResolution aurait remplis s'il connaissait notre taille.
// Ce sont des écritures de DONNÉES : le jeu fait exactement les mêmes.
void write_globals(Cpu& c) {
    // Ce que le jeu venait d'y mettre : la preuve que le crochet a bien vu
    // passer la fonction, et sur quel mode. Une fois.
    static bool dit = false;
    const uint32_t wasW = c.read_u32(g_base + G_W), wasH = c.read_u32(g_base + G_H);
    c.write_u32(g_base + G_W,    (uint32_t)g_w);
    c.write_u32(g_base + G_H,    (uint32_t)g_h);
    c.write_u32(g_base + G_WCPY, (uint32_t)g_w);
    c.write_u32(g_base + G_WCP2, (uint32_t)g_w);
    c.write_u32(g_base + G_HM40, (uint32_t)(g_h - 40));
    if (!dit) { dit = true;
        jpline("res960: globals %ux%u -> %dx%d (relecture %ux%u)",
               wasW, wasH, g_w, g_h,
               c.read_u32(g_base + G_W), c.read_u32(g_base + G_H)); }
}

// ScreenShift : le binaire code +80/-60 pour le mode 2 (800x600). La
// généralisation qui redonne EXACTEMENT ces constantes à 800x600 est
// x = w/2 - 320 et y = -(h/2 - 240) — vérifié : 400-320=80, -(300-240)=-60.
// À 960x544 : 160 et -32.
void write_shift(Cpu& c) {
    c.write_u32(g_base + G_SHX, (uint32_t)(int32_t)(g_w / 2 - 320));
    c.write_u32(g_base + G_SHY, (uint32_t)(int32_t)(-(g_h / 2 - 240)));
}

}  // namespace

extern "C" int d2res_active(void) { return g_on; }
extern "C" int d2res_w(void)      { return g_w; }
extern "C" int d2res_h(void)      { return g_h; }

void native_hooks_resolution_install(Cpu* cpu, Bridge& br) {
    const char* e = getenv("D2_RES");
    if (!e || !*e) return;
    g_w = 960; g_h = 544;                       // D2_RES=1 -> la taille de l'ecran
    if (std::strchr(e, 'x')) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 1280 && h <= 1024) {
            g_w = w; g_h = h;
        } else {
            jpline("res960: D2_RES=%s ignore (attendu LxH, L>=640, H>=480)", e);
            return;
        }
    }
    // Plancher dur, dérivé d'Inventory.txt : le panneau d'inventaire est
    // centré avec un ecart constant de -19,5 px, donc son bord haut vaut
    // h/2 - 240. En dessous de 480 de haut il sort de l'ecran.
    if (g_h < 480) { jpline("res960: hauteur %d < 480 — l'inventaire sortirait de l'ecran", g_h); return; }
    if (!g_114 || !g_d2base) { jpline("res960: REFUS — 1.14d non detecte"); return; }
    g_base = g_d2base;

    if (!sig_ok(cpu, RVA_GETSIZE, SIG_GETSIZE, sizeof SIG_GETSIZE, "GetResolutionSize") ||
        !sig_ok(cpu, RVA_SETRES,  SIG_SETRES,  sizeof SIG_SETRES,  "SetResolution")     ||
        !sig_ok(cpu, RVA_SHIFT,   SIG_SHIFT,   sizeof SIG_SHIFT,   "set_screen_shift")  ||
        !sig_ok(cpu, RVA_GLIDE,   SIG_GLIDE,   sizeof SIG_GLIDE,   "Glide SetMode"))
        return;                                  // tout ou rien : un crochet sur deux donnerait une image incoherente

    // ---- 1. GetResolutionSize : remplacement TOTAL -------------------------
    // Table de saut a 4 entrees, taille en dur dans chaque branche : c'est la
    // seule des trois fonctions mode->taille du binaire qui n'a aucun chemin
    // passant par les globals. 6 appelants, tous internes a D2GFX
    // (dimensionnement de la fenetre et de la surface).
    {
        Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "native!d2_getressize_114";
        s.fn = [](Cpu& c) -> uint32_t {
            hit("GetResolutionSize", g_nGetSize);
            const uint32_t E = c.reg(R_ESP);
            const uint32_t mode = c.read_u32(E + 4);
            const uint32_t pW = c.read_u32(E + 8), pH = c.read_u32(E + 12);
            uint32_t w, h;
            switch (mode) {
                case 0:  w = 640;  h = 480;  break;   // le "petit" mode reste intact
                case 1:
                case 2:  w = (uint32_t)g_w; h = (uint32_t)g_h; break;   // <- notre taille
                case 3:  w = 1344; h = 700;  break;   // mode mort du binaire, laisse tel quel
                default: w = 0;    h = 0;    break;   // l'original appelle un assert ici ; renvoyer 0x0 suffit
            }
            if (pW) c.write_u32(pW, w);
            if (pH) c.write_u32(pH, h);
            return pH;   // la branche d'origine laisse pH dans EAX ; aucun appelant ne le lit
        };
        br.register_shim("native.hook", "d2_getressize_114", s);
        cpu->set_alternate(g_base + RVA_GETSIZE, br.shim_trap("native.hook", "d2_getressize_114"));
    }

    // ---- 2. SetResolution : trap de SORTIE ---------------------------------
    // On laisse la fonction s'executer normalement (elle ecrit 800x600 pour le
    // mode 2, plus toute sa plomberie : recalcul des panneaux, notification
    // aux sous-systemes), PUIS on corrige les globals. Corriger avant ne
    // servirait a rien : la branche mode 2 les ecrase.
    {
        Shim xe; xe.argc = 0; xe.stdcall_cleanup = false; xe.tag = "native!d2_setres_exit";
        xe.fn = [&br](Cpu& c) -> uint32_t {
            write_globals(c);
            write_shift(c);          // le shift est pose par une AUTRE fonction, qui peut
                                     // ne pas etre appelee : on le garantit ici aussi
            const uint32_t ret = s_slotSet.ret; s_slotSet.busy = false;
            c.set_reg(R_ESP, c.reg(R_ESP) - 4); br.redirect_next(ret);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_setres_exit", xe);
        s_trapSetExit = br.shim_trap("native.hook", "d2_setres_exit");

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_setres_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            hit("SetResolution", g_nSetRes);
            const uint32_t E = c.reg(R_ESP), entry = g_base + RVA_SETRES;
            if (!s_slotSet.busy) { s_slotSet.busy = true; s_slotSet.ret = c.read_u32(E);
                                   c.write_u32(E, s_trapSetExit); }
            c.write_u32(E - 4, c.reg(R_ESI));      // repli fidele : le `push esi` d'entree
            c.set_reg(R_ESP, E - 8);
            br.redirect_next(entry + 1);           // reprend sur `mov esi,ecx`
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_setres_114", s);
        cpu->set_alternate(g_base + RVA_SETRES, br.shim_trap("native.hook", "d2_setres_114"));
    }

    // ---- 3. set_screen_shift : trap de SORTIE ------------------------------
    // La fonction code +80/-60 pour le mode 2 et 0/0 sinon, et une branche
    // precoce peut sauter l'ecriture entierement. On ecrit donc sans
    // condition en sortie : quelle que soit la branche prise, la valeur juste
    // pour notre taille est la meme.
    {
        Shim xe; xe.argc = 0; xe.stdcall_cleanup = false; xe.tag = "native!d2_shift_exit";
        xe.fn = [&br](Cpu& c) -> uint32_t {
            write_shift(c);
            const uint32_t ret = s_slotShift.ret; s_slotShift.busy = false;
            c.set_reg(R_ESP, c.reg(R_ESP) - 4); br.redirect_next(ret);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_shift_exit", xe);
        s_trapShiftExit = br.shim_trap("native.hook", "d2_shift_exit");

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_shift_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            hit("set_screen_shift", g_nShift);
            const uint32_t E = c.reg(R_ESP), entry = g_base + RVA_SHIFT;
            if (!s_slotShift.busy) { s_slotShift.busy = true; s_slotShift.ret = c.read_u32(E);
                                     c.write_u32(E, s_trapShiftExit); }
            c.write_u32(E - 4, c.reg(R_EBP));      // repli fidele : `push ebp`
            c.set_reg(R_ESP, E - 8);
            br.redirect_next(entry + 1);           // reprend sur `mov ebp,esp`
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_shift_114", s);
        cpu->set_alternate(g_base + RVA_SHIFT, br.shim_trap("native.hook", "d2_shift_114"));
    }

    // ---- 4. Glide SetMode : trap de SORTIE ---------------------------------
    // Cette fonction choisit l'index Glide (7 = 640x480, 8 = 800x600), ecrit
    // les globals du renderer en dur, puis appelle grSstWinOpen. L'index nous
    // est indifferent (notre glide3x ouvre d2res_w x d2res_h quel que soit
    // l'index, cf. gx_host.cpp) ; ce qui compte est de corriger les deux
    // globals du renderer apres coup.
    {
        Shim xe; xe.argc = 0; xe.stdcall_cleanup = false; xe.tag = "native!d2_glidemode_exit";
        xe.fn = [&br](Cpu& c) -> uint32_t {
            c.write_u32(g_base + G_GLW, (uint32_t)g_w);
            c.write_u32(g_base + G_GLH, (uint32_t)g_h);
            const uint32_t ret = s_slotGlide.ret; s_slotGlide.busy = false;
            c.set_reg(R_ESP, c.reg(R_ESP) - 4); br.redirect_next(ret);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_glidemode_exit", xe);
        s_trapGlideExit = br.shim_trap("native.hook", "d2_glidemode_exit");

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_glidemode_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            hit("Glide SetMode", g_nGlide);
            const uint32_t E = c.reg(R_ESP), entry = g_base + RVA_GLIDE;
            if (!s_slotGlide.busy) { s_slotGlide.busy = true; s_slotGlide.ret = c.read_u32(E);
                                     c.write_u32(E, s_trapGlideExit); }
            c.write_u32(E - 4, c.reg(R_EBP));      // repli fidele : `push ebp`
            c.set_reg(R_ESP, E - 8);
            br.redirect_next(entry + 1);
            return c.reg(R_EAX); };
        br.register_shim("native.hook", "d2_glidemode_114", s);
        cpu->set_alternate(g_base + RVA_GLIDE, br.shim_trap("native.hook", "d2_glidemode_114"));
    }

    g_on = 1;
    // LIMITE CONNUE : seul le renderer Glide est couvert. Le renderer GDI
    // (mode fenetre, `-w`) fabrique sa taille en RVA 0x2c7b39 puis l'utilise
    // immediatement pour son BITMAPINFO, DANS la meme fonction : un trap de
    // sortie arriverait trop tard, et un alternate ne sait pas reecrire un
    // immediat. Les bancs hote tournent tous en `-w`, donc ils continueront
    // de voir 800x600 — ce n'est pas une panne, c'est ce perimetre.
    if (const char* a = getenv("D2ARGS"))
        if (!std::strstr(a, "-3dfx"))
            jpline("res960: ATTENTION — D2ARGS ne contient pas -3dfx : le renderer GDI "
                   "n'est PAS couvert, l'image restera en 800x600");
    jpline("res960: ARME %dx%d — 4 alternates, 0 octet de l'image touche "
           "(decalage ecran %d,%d)", g_w, g_h, g_w / 2 - 320, -(g_h / 2 - 240));
    std::printf("res960: armed %dx%d (alternates only, no .text patch)\n", g_w, g_h);
}
