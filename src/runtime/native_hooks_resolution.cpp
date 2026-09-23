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
constexpr uint32_t RVA_SHIFT   = 0x00056ee0;  // D2Client DrawUI(ecx) : ecrit le decalage ecran PUIS dessine
constexpr uint32_t RVA_SHIFT_SUITE = 0x00056f24; // ... la suite, une fois le decalage ecrit
constexpr uint32_t RVA_SHIFT_SORTIE = 0x000572d8; // ... et sa sortie precoce (epilogue)
constexpr uint32_t G_UIOFF = 0x003a2808;       // != 0 : l'interface n'est pas dessinee

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

// ---- LES TABLES DE MISE EN PAGE, RECALEES SUR LE CENTRE ---------------------
//
// inventory.bin et belts.bin portent des coordonnees ECRAN ABSOLUES, en
// 800x600 pour les lignes « 800 » (grille d'inventaire 419..706 x 315..428,
// par exemple). Les cinq accesseurs D2Common ci-dessous les recopient telles
// quelles dans les copies cachees de D2Client (Game+0x835b0), et c'est sur
// ces copies que se font le dessin des cases, celui des objets ET la detection
// de clic. Tout le reste des panneaux (fonds, boutons, onglets, croix, leurs
// survols et leurs clics) est ancre sur ScreenShiftX/Y : gauche en x = SHX,
// droite en x = W - SHX - 320. Avec SHX = W/2-320 (crochet 3) la paire de
// panneaux est donc centree : la disposition 800x600 entiere, translatee de
// ((W-800)/2, (H-600)/2). Les tables doivent suivre du meme vecteur.
//
// Le principe est celui de SGD2FreeRes (mir-diablo-ii-tools, patches/inventory,
// RealignPositionFromCenter) : remplacer les accesseurs et recaler ce qu'ils
// copient sur le centre. La ceinture, elle, vit sur le bandeau (colle en bas,
// centre) : ((W-800)/2, H-600).
// Les entrees vides (-1, ou tout a zero) ne bougent pas.
// Les accesseurs sont remplaces en entier : ce sont de simples copies, dont
// la disassemblee tient en vingt lignes chacune, et le resultat est ce que
// le jeu aurait ecrit si sa table etait faite pour cette taille d'ecran.
constexpr uint32_t RVA_INVPOS  = 0x0025c180;  // GetGlobalInventoryPosition(idx,mode,*rect)        stdcall 0xc
constexpr uint32_t RVA_INVGRID = 0x0025c1f0;  // GetGlobalInventoryGridLayout(idx,mode,*grid)      stdcall 0xc
constexpr uint32_t RVA_INVSLOT = 0x0025c270;  // GetGlobalEquipmentSlotLayout(idx,mode,*slot,n)    stdcall 0x10
constexpr uint32_t RVA_BELTREC = 0x00260cb0;  // GetGlobalBeltRecord(idx,mode,*rec)                stdcall 0xc
constexpr uint32_t RVA_BELTPOS = 0x00260d10;  // GetGlobalBeltSlotPosition(idx,mode,*rect,n)       stdcall 0x10
constexpr uint32_t G_INVTBL  = 0x0056d4f4;    // inventory.bin : lignes de 240 o, idx + 16*mode
constexpr uint32_t G_INVCNT  = 0x0056d4f0;    //   ... et leur nombre
constexpr uint32_t G_BELTTBL = 0x0056d4f8;    // belts.bin : lignes de 264 o, idx + 7*mode
constexpr uint32_t INV_ROW = 0xf0, BELT_ROW = 0x108;
// Signatures SANS adresse absolue : l'image est relogee a l'execution et ses
// operandes .data ne valent plus ce que dit le fichier.
const uint8_t SIG_INVGET [8]  = { 0x55,0x8b,0xec,0x8b,0x45,0x0c,0x8b,0x0d };  // les 3 accesseurs inventaire
const uint8_t SIG_BELTREC[8]  = { 0x55,0x8b,0xec,0x8b,0x4d,0x08,0x8b,0x15 };
const uint8_t SIG_BELTPOS[12] = { 0x55,0x8b,0xec,0x8b,0x4d,0x0c,0x8d,0x04,0xcd,0x00,0x00,0x00 };

// ---- LE CADRE DECORATIF 800BorderFrame -------------------------------------
//
// Deux fonctions de D2Client (Game+0x98630 a gauche, +0x98700 a droite)
// dessinent les dix morceaux du cadre a des coordonnees IMMEDIATES, celles
// d'un ecran de 800x600 : le cadre reste donc cale sur 0..800 quand les
// panneaux, eux, ont suivi le centre. Les immediats ne se reecrivent pas
// (0 octet de .text modifie) : on rejoue la fonction depuis l'hote, avec les
// memes appels invites — chargement du cel s'il manque, puis les cinq blits —
// et les memes coordonnees, translatees de ((W-800)/2, (H-600)/2) comme les
// panneaux qu'elles encadrent. C'est la lecture que SGD2FreeRes fait du cadre
// d'origine (DrawOriginal*ScreenBorderFrame).
constexpr uint32_t RVA_BORDER_L  = 0x00098630;
constexpr uint32_t RVA_BORDER_R  = 0x00098700;
constexpr uint32_t RVA_DRAWIMG   = 0x000f6480;  // D2Gfx DrawImage(ctx,x,y,-1,5,0), stdcall 0x18
constexpr uint32_t RVA_LOADCEL   = 0x000520c0;  // thiscall(ecx = chemin) -> cel*
constexpr uint32_t RVA_BORDERSTR = 0x002da034;  // "Panel\\800BorderFrame"
constexpr uint32_t G_BORDERCEL   = 0x003bef18;  // le cel, charge a la premiere demande
const uint8_t SIG_BORDER_L[13] = { 0x55,0x8b,0xec,0x83,0xec,0x48,0x6a,0x48,0x8d,0x45,0xb8,0x6a,0x00 };
const uint8_t SIG_BORDER_R[9]  = { 0x55,0x8b,0xec,0x83,0xec,0x48,0x56,0x8b,0x35 };
const uint8_t SIG_DRAWIMG [6]  = { 0x55,0x8b,0xec,0x8b,0x45,0x1c };
const uint8_t SIG_LOADCEL [9]  = { 0x55,0x8b,0xec,0x81,0xec,0x04,0x01,0x00,0x00 };
struct BorderPiece { int frame, x, y; };
// Releves au desassemblage : (image, x, y) de chaque appel DrawImage.
const BorderPiece BORDER_L[5] = { {0,0,253}, {1,256,63}, {2,0,484}, {3,0,553}, {4,256,553} };
const BorderPiece BORDER_R[5] = { {5,400,63}, {6,544,253}, {7,713,484}, {8,544,553}, {9,400,553} };

int g_on = 0, g_applied = 0, g_w = 960, g_h = 544;
// Ancrage de la disposition 800x600 sur notre ecran (D2_RES_PANNEAUX) — cette
// bascule ne pilote plus que l'axe X ; l'axe Y est toujours centre (voir
// dy_centre plus bas, et pourquoi juste avant apply()) :
//   0 (defaut, « bords ») : decalage ecran X 80, celui du jeu en mode 2. Le
//     panneau de gauche reste colle a gauche (cadre 0..80, art 80..400), celui
//     de droite est colle a droite (art W-400..W-80, cadre W-80..W) ; entre
//     les deux, le monde. C'est l'aspect voulu sur une dalle de 960.
//   1 (« centre ») : decalage X W/2-320, la disposition 800 entiere centree
//     d'un bloc en X, panneaux jointifs (modele SGD2FreeRes) ; laisse une
//     bande de 80 px entre le cadre et le bord de l'ecran.
// Dans les deux cas le MEME decalage sert au dessin et aux clics (crochet 3),
// et tables (crochet 4) comme cadre (crochet 5) suivent ce meme ancrage.
int g_centre = 0;
uint32_t g_base = 0;

// Ces fonctions ne sont pas reentrantes (un changement de resolution a la
// fois) : un emplacement occupe signalerait qu'on s'est trompe de cible.
struct Slot { bool busy; uint32_t ret; };
Slot s_set{false,0};
uint32_t s_trapSet = 0;

bool sig_ok(Cpu* c, uint32_t rva, const uint8_t* want, size_t n, const char* who) {
    uint8_t got[16];
    c->read(g_base + rva, got, (uint32_t)n);
    if (!std::memcmp(got, want, n)) return true;
    jpline("res: REFUS %s Game+0x%x — entree %02x %02x %02x %02x inattendue",
           who, (unsigned)rva, got[0], got[1], got[2], got[3]);
    return false;
}

// Le vecteur de translation de la disposition 800x600 (640x480 en mode 0)
// vers notre ecran. Mesure sur console (23/09/2026) : DrawUI (Game+0x56ee0)
// reecrit le decalage ecran a chaque image AVANT de dessiner ; l'ancien trap
// de sortie ne le remettait qu'apres, si bien que l'interface etait dessinee
// et survolee avec un decalage, et les clics (evalues hors du dessin) avec un
// autre — 80 px d'ecart en X entre le bouton vu et sa zone de clic. Le
// crochet 3 ecrit maintenant le decalage A L'ENTREE de DrawUI, et tout suit
// le meme decalage.
//
// X et Y sont ancres independamment. En X, D2_RES_PANNEAUX choisit (voir plus
// haut). En Y, un panneau haut de 600 px doit tenir dans les 544 px de la
// dalle : rester colle en bas, comme le fait le jeu nativement (SHY=-60),
// laisse le HAUT du panneau — le cadre ouvrage — tronque de 56 px, en plein
// milieu du motif des coins (mesure sur console le 23/09/2026, capture
// zoomee : rivets dores coupes a ras en haut, bandeau intact en bas). Le
// centrage vertical repartit ce manque des deux cotes (28 px en haut, 28 px
// en bas) au lieu de tout prendre en haut — d'ou dy_centre() plus bas, seule
// source du decalage Y : DrawUI (SHY, ce que le jeu lit pour dessiner le
// panneau lui-meme) ET dy_tab/dy_cadre (nos propres tables de cases et le
// rejeu du cadre 800BorderFrame) en derivent tous les deux, pour ne plus
// jamais pouvoir diverger comme dessin et clic ont pu diverger en X avant ce
// commit.
int dx_centre(uint32_t mode) { return mode ? (g_w - 800) / 2 : (g_w - 640) / 2; }
int dy_centre(uint32_t mode) { return mode ? (g_h - 600) / 2 : (g_h - 480) / 2; }
int dx_droite(uint32_t mode) { return mode ? (g_w - 800)     : (g_w - 640); }
int dy_bas(uint32_t mode)    { return mode ? (g_h - 600)     : (g_h - 480); }
// SHY qui correspond a un decalage de table dy donne : inverse de "haut du
// panneau = H + SHY - 480", verifie contre les deux ancrages X existants
// (bords : SHY=-60, dy=H-600 ; centre : SHY=-(H/2-240), dy=(H-600)/2 — les
// deux donnent le meme haut de panneau par cette relation).
int shy_de_dy(int dy) { return dy - (int)g_h + 540; }

// Ce que SetResolution aurait ecrit s'il connaissait notre taille. Ce sont des
// ecritures de DONNEES : le jeu fait exactement les memes.
void apply(Cpu& c) {
    const int shx = g_centre ? g_w / 2 - 320 : 80;
    const int shy = shy_de_dy(dy_centre(1));
    c.write_u32(g_base + G_W,    (uint32_t)g_w);
    c.write_u32(g_base + G_H,    (uint32_t)g_h);
    c.write_u32(g_base + G_WCPY, (uint32_t)g_w);
    c.write_u32(g_base + G_WCP2, (uint32_t)g_w);
    c.write_u32(g_base + G_HM40, (uint32_t)(g_h - 40));
    c.write_u32(g_base + G_SHX,  (uint32_t)(int32_t)shx);
    c.write_u32(g_base + G_SHY,  (uint32_t)(int32_t)shy);
    c.write_u32(g_base + G_INV,  1u);
    c.write_u32(g_base + G_GLW,  (uint32_t)g_w);
    c.write_u32(g_base + G_GLH,  (uint32_t)g_h);
    const bool premier = !g_applied;
    g_applied = 1;
    if (premier && d2gxm_set_window) d2gxm_set_window(g_w, g_h);
    if (premier) {
        jpline("res: %dx%d applique (relecture %ux%u, decalage %d,%d)", g_w, g_h,
               c.read_u32(g_base + G_W), c.read_u32(g_base + G_H), shx, shy); }
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
// Rend la main au corps original sans trap de sortie : `push ebp` rejoue,
// puis reprise a entry+1. Sert de repli quand un remplacement ne veut pas
// s'appliquer (bascule pas encore faite, table absente).
uint32_t original(Cpu& c, Bridge& br, uint32_t entry) {
    const uint32_t E = c.reg(R_ESP);
    c.write_u32(E - 4, c.reg(R_EBP));
    c.set_reg(R_ESP, E - 8);
    br.redirect_next(entry + 1);
    return c.reg(R_EAX);
}

// Le vecteur d'une ligne de table selon l'ancrage horizontal (dx_centre,
// dy_centre, dx_droite, dy_bas et shy_de_dy sont definis avant apply(), qui
// en a besoin) : au centre, toutes les lignes bougent de (W-800)/2 en x ;
// aux bords, une ligne de gauche (coffre, echange, PNJ : invLeft 80) ne
// bouge pas en x, une ligne de droite (le personnage : invLeft 400 ; 320 en
// mode 640) suit le bord droit. En Y, toujours dy_centre — voir le
// commentaire au-dessus d'apply().
int dx_tab(Cpu& c, uint32_t rec, uint32_t mode) {
    if (g_centre) return dx_centre(mode);
    const int32_t l = (int32_t)c.read_u32(rec);
    if (l < 0) return 0;
    return l >= (mode ? 400 : 320) ? dx_droite(mode) : 0;
}
int dy_tab(uint32_t mode) { return dy_centre(mode); }
// Le cadre : chaque moitie suit son panneau en X ; en Y, meme centre que le reste.
int dx_cadre(bool droite) { return g_centre ? dx_centre(1) : (droite ? dx_droite(1) : 0); }
int dy_cadre()            { return dy_centre(1); }

// Un rectangle de table : gauche, droite, haut, bas.
void copie_rect(Cpu& c, uint32_t src, uint32_t dst, int dx, int dy, bool vide_si_moins1) {
    int32_t v[4];
    for (int i = 0; i < 4; ++i) v[i] = (int32_t)c.read_u32(src + 4u * i);
    bool vide = vide_si_moins1 ? (v[0] == -1 || v[1] == -1 || v[2] == -1 || v[3] == -1)
                               : (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0);
    if (!vide) { v[0] += dx; v[1] += dx; v[2] += dy; v[3] += dy; }
    for (int i = 0; i < 4; ++i) c.write_u32(dst + 4u * i, (uint32_t)v[i]);
}

// Ligne d'inventory.bin demandee par un accesseur, ou 0 s'il faut laisser
// l'original faire (et lever son erreur).
uint32_t inv_ligne(Cpu& c, uint32_t idx, uint32_t mode) {
    const uint32_t tbl = c.read_u32(g_base + G_INVTBL), n = c.read_u32(g_base + G_INVCNT);
    const uint32_t row = idx + mode * 16u;
    if (!tbl || row > n) return 0;
    return tbl + row * INV_ROW;
}
uint32_t belt_ligne(Cpu& c, uint32_t idx, uint32_t mode) {
    const uint32_t tbl = c.read_u32(g_base + G_BELTTBL);
    if (!tbl) return 0;
    return tbl + (idx + mode * 7u) * BELT_ROW;
}

uint32_t inv_pos(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    const uint32_t idx = c.read_u32(E + 4), mode = c.read_u32(E + 8), out = c.read_u32(E + 12);
    const uint32_t rec = g_applied ? inv_ligne(c, idx, mode) : 0;
    if (!rec) { original(c, br, g_base + RVA_INVPOS); return 0; }
    copie_rect(c, rec, out, dx_tab(c, rec, mode), dy_tab(mode), true);
    return 1;
}
uint32_t inv_grille(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    const uint32_t idx = c.read_u32(E + 4), mode = c.read_u32(E + 8), out = c.read_u32(E + 12);
    const uint32_t rec = g_applied ? inv_ligne(c, idx, mode) : 0;
    if (!rec) { original(c, br, g_base + RVA_INVGRID); return 0; }
    // [0] colonnes|lignes (octets), [1..4] rectangle, [5] taille de case.
    const uint32_t dims = c.read_u32(rec + 0x10);
    c.write_u32(out, dims);
    const bool vide = !(dims & 0xff) || !((dims >> 8) & 0xff);
    copie_rect(c, rec + 0x14, out + 4, vide ? 0 : dx_tab(c, rec, mode), vide ? 0 : dy_tab(mode), true);
    c.write_u32(out + 0x14, c.read_u32(rec + 0x24));
    return 1;
}
uint32_t inv_case(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    const uint32_t idx = c.read_u32(E + 4), mode = c.read_u32(E + 8), out = c.read_u32(E + 12), n = c.read_u32(E + 16);
    const uint32_t rec = g_applied ? inv_ligne(c, idx, mode) : 0;
    if (!rec) { original(c, br, g_base + RVA_INVSLOT); return 0; }
    const uint32_t slot = rec + 0x28 + n * 0x14;   // dix emplacements de 5 mots : rectangle + largeur|hauteur
    copie_rect(c, slot, out, dx_tab(c, rec, mode), dy_tab(mode), true);
    c.write_u32(out + 0x10, c.read_u32(slot + 0x10));
    return 1;
}
uint32_t belt_rec(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    const uint32_t idx = c.read_u32(E + 4), mode = c.read_u32(E + 8), out = c.read_u32(E + 12);
    const uint32_t rec = g_applied ? belt_ligne(c, idx, mode) : 0;
    if (!rec) { original(c, br, g_base + RVA_BELTREC); return 0; }
    // 264 o : nombre de cases, un mot, puis 16 rectangles.
    c.write_u32(out, c.read_u32(rec)); c.write_u32(out + 4, c.read_u32(rec + 4));
    const uint32_t n = c.read_u32(rec);
    for (uint32_t i = 0; i < 16; ++i)
        copie_rect(c, rec + 8 + i * 16, out + 8 + i * 16, i < n ? dx_centre(mode) : 0, i < n ? dy_bas(mode) : 0, false);
    return 1;
}
uint32_t belt_pos(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    const uint32_t idx = c.read_u32(E + 4), mode = c.read_u32(E + 8), out = c.read_u32(E + 12), n = c.read_u32(E + 16);
    const uint32_t rec = g_applied ? belt_ligne(c, idx, mode) : 0;
    if (!rec) { original(c, br, g_base + RVA_BELTPOS); return 0; }
    copie_rect(c, rec + 8 + n * 16, out, dx_centre(mode), dy_bas(mode), false);
    return 1;
}

// ---- Le cadre : rejoue depuis l'hote ------------------------------------
//
// Le shim d'entree prend la place de la fonction. Chaque appel invite
// (chargement du cel, puis un blit par morceau) se fait en posant ses
// arguments et un trap de retour sur la pile, puis en redirigeant vers la
// fonction du jeu ; le trap rappelle border_suite, qui enchaine. Au bout,
// ESP est remis comme apres le `ret` de l'original et l'on reprend a
// l'adresse de retour. La pile sous ESP est du brouillon (pas de zone rouge
// en x86) : le contexte de dessin (0x48 o) et les arguments y vivent.
struct BorderRun {
    bool busy = false, chargement = false;
    uint32_t esp0 = 0, ret = 0, ctx = 0;
    int etape = 0;
    const BorderPiece* pieces = nullptr;
};
BorderRun s_bord;
uint32_t s_trapBord = 0;

void border_suite(Cpu& c, Bridge& br) {
    BorderRun& s = s_bord;
    if (s.chargement) {           // retour du chargeur : eax = cel
        s.chargement = false;
        c.write_u32(g_base + G_BORDERCEL, c.reg(R_EAX));
    }
    const uint32_t cel = c.read_u32(g_base + G_BORDERCEL);
    const uint32_t A = s.ctx - 0x1c;          // trap + 6 arguments, juste sous le contexte
    if (!cel) {                               // premiere fois : charger le cel
        s.chargement = true;
        c.set_reg(R_ECX, g_base + RVA_BORDERSTR);
        c.write_u32(A, s_trapBord);
        c.set_reg(R_ESP, A - 4);              // +4 au retour du shim -> ESP = A
        br.redirect_next(g_base + RVA_LOADCEL);
        return;
    }
    if (s.etape < 5) {
        const BorderPiece& p = s.pieces[s.etape++];
        for (uint32_t i = 0; i < 0x48; i += 4) c.write_u32(s.ctx + i, 0);
        c.write_u32(s.ctx + 0x00, (uint32_t)p.frame);
        c.write_u32(s.ctx + 0x34, cel);
        c.write_u32(A + 0x00, s_trapBord);
        c.write_u32(A + 0x04, s.ctx);
        c.write_u32(A + 0x08, (uint32_t)(p.x + dx_cadre(s.pieces == BORDER_R)));
        c.write_u32(A + 0x0c, (uint32_t)(p.y + dy_cadre()));
        c.write_u32(A + 0x10, 0xffffffffu);
        c.write_u32(A + 0x14, 5u);
        c.write_u32(A + 0x18, 0u);
        c.set_reg(R_ESP, A - 4);
        br.redirect_next(g_base + RVA_DRAWIMG);
        return;
    }
    s.busy = false;                           // fini : comme apres le `ret` de l'original
    c.set_reg(R_ESP, s.esp0);
    br.redirect_next(s.ret);
}
uint32_t border_entree(Cpu& c, Bridge& br, uint32_t entry, const BorderPiece* pieces) {
    // DIAG (D2_TRACE_CLIC) : le cadre est dessine a chaque image tant que le
    // panneau est ouvert ; une ligne toutes les ~1.5 s dit s'il l'est encore.
    { static int tr=-1; if(tr<0) tr=getenv("D2_TRACE_CLIC")?1:0;
      if(tr){ static uint64_t lastL=0,lastR=0; uint64_t now=rt_now_ms(); uint64_t& last=(pieces==BORDER_R)?lastR:lastL;
          if(now-last>1500){ last=now; jpline("  [cadre] %s dessine SHX=%d SHY=%d W=%u H=%u", pieces==BORDER_R?"DROIT":"GAUCHE",
              (int)c.read_u32(g_base+G_SHX),(int)c.read_u32(g_base+G_SHY),c.read_u32(g_base+G_W),c.read_u32(g_base+G_H)); } } }
    if (!g_applied || s_bord.busy) return original(c, br, entry);
    BorderRun& s = s_bord;
    s.busy = true; s.chargement = false; s.etape = 0; s.pieces = pieces;
    s.esp0 = c.reg(R_ESP); s.ret = c.read_u32(s.esp0);
    s.ctx = s.esp0 - 0x50;
    border_suite(c, br);
    return 0;
}

}  // namespace

// Le glide3x ne doit ouvrir la grande fenetre qu'APRES la bascule : les
// MENUS ont un art en 800x600 fixe et ne peuvent pas remplir 960x544.
extern "C" int d2res_active(void) { return g_on && g_applied; }
extern "C" int d2res_w(void)      { return g_w; }
extern "C" int d2res_h(void)      { return g_h; }
extern "C" int d2res_centre(void) { return g_centre; }

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

    // 3. DrawUI (Game+0x56ee0) : son prologue ecrit +80/-60 pour le mode 2
    //    (0/0 sinon) puis dessine toute l'interface avec. Un trap de sortie ne
    //    suffit pas : le dessin a deja eu lieu (mesure console, 23/09/2026).
    //    On remplace le PROLOGUE depuis l'hote — memes ecritures de pile,
    //    memes registres, notre decalage a la place du sien — et l'on reprend
    //    le corps a l'instruction qui suit ses deux ecritures. La sortie
    //    precoce (interface coupee, G_UIOFF != 0) est rejouee telle quelle.
    //    Zero octet de .text modifie.
    {
        // Le prologue entier est revalide, operandes absolus exclus (l'image
        // peut etre relogee) : opcodes en 0x00..0x44, voir le desassemblage.
        static const struct { uint8_t off, val; } K[] = {
            {0x00,0x55},{0x01,0x8b},{0x02,0xec},{0x03,0x83},{0x04,0xec},{0x05,0x0c},
            {0x06,0x53},{0x07,0x33},{0x08,0xdb},{0x09,0x39},{0x0a,0x1d},
            {0x0f,0x89},{0x10,0x4d},{0x11,0xf4},{0x12,0x0f},{0x13,0x85},
            {0x18,0xe8},{0x1d,0x83},{0x1e,0xf8},{0x1f,0x02},{0x20,0x75},{0x21,0x16},
            {0x22,0xc7},{0x23,0x05},{0x28,0x50},{0x2c,0xc7},{0x2d,0x05},{0x32,0xc4},
            {0x36,0xeb},{0x37,0x0c},{0x38,0x89},{0x39,0x1d},{0x3e,0x89},{0x3f,0x1d},
            {0x44,0x53} };
        uint8_t got[0x48]; cpu->read(g_base + RVA_SHIFT, got, sizeof got);
        bool ok = true;
        for (const auto& k : K) if (got[k.off] != k.val) ok = false;
        if (!ok) { jpline("res: REFUS DrawUI Game+0x%x — prologue inattendu", (unsigned)RVA_SHIFT); return; }

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_drawui_114";
        s.fn = [&br](Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(R_ESP);            // -> adresse de retour
            c.write_u32(E - 4, c.reg(R_EBP));           // push ebp
            const uint32_t ebp = E - 4;                 // mov ebp,esp
            c.set_reg(R_EBP, ebp);                      // sub esp,0xc
            c.write_u32(ebp - 0x10, c.reg(R_EBX));      // push ebx
            c.set_reg(R_EBX, 0);                        // xor ebx,ebx
            c.write_u32(ebp - 0xc, c.reg(R_ECX));       // mov [ebp-0xc],ecx
            c.set_reg(R_ESP, ebp - 0x10 - 4);           // = ebp-0x10 apres le +4 du trap
            if (c.read_u32(g_base + G_UIOFF) != 0) {    // cmp [G_UIOFF],ebx ; jne epilogue
                br.redirect_next(g_base + RVA_SHIFT_SORTIE); return 0; }
            apply(c);                                   // notre decalage, pas +80/-60
            br.redirect_next(g_base + RVA_SHIFT_SUITE);
            return 0; };
        br.register_shim("native.hook", "d2_drawui_114", s);
        cpu->set_alternate(g_base + RVA_SHIFT, br.shim_trap("native.hook", "d2_drawui_114"));
    }

    // D2_RES_PANNEAUX=0 : garde la bascule (crochets 1-3) mais laisse les
    // panneaux tels que le jeu les place — la jambe temoin d'un A/B a meme
    // eboot, et un repli si un jour une table modifiee ne se recale pas bien.
    const char* pn = getenv("D2_RES_PANNEAUX");
    const bool panneaux = !(pn && *pn && (!std::strcmp(pn, "0") || !std::strcmp(pn, "non") || !std::strcmp(pn, "off")));
    g_centre = (pn && (!std::strcmp(pn, "centre") || !std::strcmp(pn, "center"))) ? 1 : 0;
    int poses = 3;
    if (!panneaux) jpline("res: panneaux laisses au jeu (D2_RES_PANNEAUX=%s)", pn);
    else jpline("res: panneaux ancres %s", g_centre ? "au centre (D2_RES_PANNEAUX=centre)" : "aux bords");

    // 4. Les cinq accesseurs de table : remplaces, recales sur le centre.
    //    Tout ou rien, comme ci-dessus.
    if (panneaux) {
        auto get_ok = [&](uint32_t rva, const uint8_t* want, size_t n, const char* who, uint8_t b4b) {
            if (!sig_ok(cpu, rva, want, n, who)) return false;
            uint8_t k; cpu->read(g_base + rva + 0x4b, &k, 1);   // le mot qui distingue les 3 accesseurs inventaire
            if (b4b && k != b4b) { jpline("res: REFUS %s Game+0x%x — corps inattendu", who, (unsigned)rva); return false; }
            return true; };
        if (get_ok(RVA_INVPOS,  SIG_INVGET,  sizeof SIG_INVGET,  "GetGlobalInventoryPosition",   0x8b) &&
            get_ok(RVA_INVGRID, SIG_INVGET,  sizeof SIG_INVGET,  "GetGlobalInventoryGridLayout", 0x8b) &&
            get_ok(RVA_INVSLOT, SIG_INVGET,  sizeof SIG_INVGET,  "GetGlobalEquipmentSlotLayout", 0x8b) &&
            get_ok(RVA_BELTREC, SIG_BELTREC, sizeof SIG_BELTREC, "GetGlobalBeltRecord",          0)    &&
            get_ok(RVA_BELTPOS, SIG_BELTPOS, sizeof SIG_BELTPOS, "GetGlobalBeltSlotPosition",    0)) {
            struct G { uint32_t rva; uint32_t argc; const char* nom; uint32_t (*fn)(Cpu&, Bridge&); };
            static const G gs[5] = {
                { RVA_INVPOS,  3, "d2_invpos_114",   inv_pos    },
                { RVA_INVGRID, 3, "d2_invgrid_114",  inv_grille },
                { RVA_INVSLOT, 4, "d2_invslot_114",  inv_case   },
                { RVA_BELTREC, 3, "d2_beltrec_114",  belt_rec   },
                { RVA_BELTPOS, 4, "d2_beltpos_114",  belt_pos   } };
            for (const G& g : gs) {
                // Pas de nettoyage stdcall par le trap : quand le remplacement
                // s'applique, c'est lui qui remet ESP ; quand il rend la main a
                // l'original, c'est le `ret 0xc` de celui-ci qui le fait.
                Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = g.nom;
                auto fn = g.fn; const uint32_t argc = g.argc;
                s.fn = [&br, fn, argc](Cpu& c) -> uint32_t {
                    const uint32_t E = c.reg(R_ESP), ret = c.read_u32(E);
                    // 1 = remplacement fait : simuler `ret 4*argc` (le trap
                    // ajoute 4). 0 = repli sur l'original, deja redirige.
                    if (fn(c, br)) { c.set_reg(R_ESP, E + 4u * argc); br.redirect_next(ret); }
                    return 0; };
                br.register_shim("native.hook", g.nom, s);
                cpu->set_alternate(g_base + g.rva, br.shim_trap("native.hook", g.nom));
            }
            poses += 5;
        }
    }

    // 5. Le cadre decoratif : les deux fonctions rejouees depuis l'hote.
    if (panneaux &&
        sig_ok(cpu, RVA_BORDER_L, SIG_BORDER_L, sizeof SIG_BORDER_L, "border_frame_gauche") &&
        sig_ok(cpu, RVA_BORDER_R, SIG_BORDER_R, sizeof SIG_BORDER_R, "border_frame_droite") &&
        sig_ok(cpu, RVA_DRAWIMG,  SIG_DRAWIMG,  sizeof SIG_DRAWIMG,  "DrawImage")           &&
        sig_ok(cpu, RVA_LOADCEL,  SIG_LOADCEL,  sizeof SIG_LOADCEL,  "LoadCel")) {
        Shim xt; xt.argc = 0; xt.stdcall_cleanup = false; xt.tag = "native!d2_border_suite";
        xt.fn = [&br](Cpu& c) -> uint32_t { border_suite(c, br); return 0; };
        br.register_shim("native.hook", "d2_border_suite", xt);
        s_trapBord = br.shim_trap("native.hook", "d2_border_suite");

        Shim sl; sl.argc = 0; sl.stdcall_cleanup = false; sl.tag = "native!d2_border_l_114";
        sl.fn = [&br](Cpu& c) -> uint32_t { return border_entree(c, br, g_base + RVA_BORDER_L, BORDER_L); };
        br.register_shim("native.hook", "d2_border_l_114", sl);
        cpu->set_alternate(g_base + RVA_BORDER_L, br.shim_trap("native.hook", "d2_border_l_114"));

        Shim sr; sr.argc = 0; sr.stdcall_cleanup = false; sr.tag = "native!d2_border_r_114";
        sr.fn = [&br](Cpu& c) -> uint32_t { return border_entree(c, br, g_base + RVA_BORDER_R, BORDER_R); };
        br.register_shim("native.hook", "d2_border_r_114", sr);
        cpu->set_alternate(g_base + RVA_BORDER_R, br.shim_trap("native.hook", "d2_border_r_114"));
        poses += 2;
    }

    g_on = 1;
    jpline("res: %dx%d arme — %d alternates, bascule a l'entree en partie", g_w, g_h, poses);
}
