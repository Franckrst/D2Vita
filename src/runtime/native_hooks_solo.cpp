// native_hooks_solo.cpp -- voir native_hooks_solo.h.
//
// Deux options de confort solo, chacune opt-in, qui choisissent une branche du
// code serveur de jeu sans ecrire un octet du jeu ni de ses donnees. Les deux
// n'agissent que si la partie est de type solo.
//
// 1. Runewords ladder (D2_RUNEWORDS_LADDER). Game+0x162660 (ecx = la partie)
//    cherche le runeword de l'objet serti, puis le refuse s'il est reserve au
//    ladder hors ladder :
//      162802  call 0x22bed0          ; eax = ligne de Runes (0x120 o) ou 0
//      16280b  mov al,[eax+0x81]      ; drapeau server
//      162815  cmp [ebx+0x74],0       ; ebx = la partie : ladder ?
//      16281d  jne ... (ne rien faire)
//      16281f  ...                    ; appliquer le runeword
//    Le test tombe au milieu d'un bloc traduit : on ne peut pas l'accrocher.
//    On accroche donc 0x22bed0 (cible d'un call, fiable) et, seulement quand
//    l'adresse de retour est 0x162807, on pose un trap de retour qui reprend
//    en 0x16281f au lieu de 0x162807 quand le runeword est reserve au ladder.
//
// 2. Reinitialisation d'Akara reutilisable (D2_RESPEC_UNLIMITED). Le
//    gestionnaire serveur des PNJ (Game+0x179d60, edi = la partie) reinitialise
//    stats et competences, puis appelle 0x18fd50 (« deactivated respec quest »,
//    a1q1.cpp) : quete 41 bit 0 (utilisee) a 1, bit 1 (disponible) a 0. Le
//    client n'affiche l'entree du menu que si bit 0 = 0 et bit 1 = 1. En solo,
//    cet appel-la (retour 0x17a266) est saute : la reinitialisation reste
//    disponible.
#include "native_hooks_solo.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>

using namespace d2rt;

namespace {

// Partie : type, mesure sur console (28/09/2026) en relevant la structure au
// moment du sertissage : 3 en solo, 2 en TCP/IP heberge. Liste blanche : tout
// autre type garde le comportement du jeu.
constexpr uint32_t OFF_TYPE = 0x6a;
constexpr uint8_t  TYPE_SOLO = 3;

constexpr uint32_t RVA_RUNE_LOOKUP = 0x0022bed0;   // runeword de l'objet (stdcall, 1 arg)
constexpr uint32_t RVA_RET_SITE    = 0x00162807;   // retour de l'appel dans le test ladder
constexpr uint32_t RVA_APPLY       = 0x0016281f;   // branche « appliquer le runeword »
constexpr uint32_t OFF_SERVER = 0x81;              // Runes.txt : colonne server
constexpr uint32_t OFF_LADDER = 0x74;              // partie : drapeau ladder

constexpr uint32_t RVA_RESPEC_OFF  = 0x0018fd50;   // desactive la reinitialisation (fastcall)
constexpr uint32_t RVA_RESPEC_CALL = 0x0017a25d;   // son appel apres la reinitialisation
constexpr uint32_t RVA_RESPEC_RET  = 0x0017a266;

const uint8_t SIG_LOOKUP[10] = { 0x55,0x8b,0xec,0x83,0xec,0x2c,0x53,0x8b,0x5d,0x08 };
// 0x162807..0x162822 : test eax / mov al,[eax+81] / test / cmp [ebx+74],0 /
// jne / test / jne / push 0 / push edi / push esi.
const uint8_t SIG_SITE[27] = { 0x85,0xc0,0x74,0x44,0x8a,0x80,0x81,0x00,0x00,0x00,0x84,0xc0,
                               0x74,0x0a,0x83,0x7b,0x74,0x00,0x75,0x04,0x84,0xc0,0x75,0x30,
                               0x6a,0x00,0x57 };
// 0x18fd50 : push esi / push edi / push edx / mov edi,ecx (puis, en +0x12 :
// push 0 / push 0x29 / push esi, le « utilisee » de la quete 41).
const uint8_t SIG_RESPEC_OFF[5]  = { 0x56,0x57,0x52,0x8b,0xf9 };
const uint8_t SIG_RESPEC_Q41[5]  = { 0x6a,0x00,0x6a,0x29,0x56 };
// 0x17a25d : mov edx,esi / mov ecx,edi / call 0x18fd50 / jmp 0x17a7db.
const uint8_t SIG_RESPEC_CALL[14] = { 0x8b,0xd6,0x8b,0xcf,0xe8,0xea,0x5a,0x01,0x00,0xe9,0x70,0x05,0x00,0x00 };

uint32_t g_base = 0, s_trap = 0;
struct { bool busy; uint32_t ret; } s_slot{false, 0};

uint8_t rd8(Cpu& c, uint32_t va) { uint8_t b = 0; c.read(va, &b, 1); return b; }

// Rend la main au corps original : la premiere instruction (`push reg`) est
// rejouee, puis reprise a entry+1.
uint32_t original(Cpu& c, Bridge& br, uint32_t entry, int reg) {
    const uint32_t E = c.reg(R_ESP);
    c.write_u32(E - 4, c.reg(reg));
    c.set_reg(R_ESP, E - 8);
    br.redirect_next(entry + 1);
    return c.reg(R_EAX);
}

uint32_t rune_return(Cpu& c, Bridge& br) {
    const uint32_t rec = c.reg(R_EAX), game = c.reg(R_EBX);
    const bool reserve = rec && rd8(c, rec + OFF_SERVER);
    const bool ladder  = reserve && c.read_u32(game + OFF_LADDER) != 0;
    const uint8_t type = reserve && !ladder ? rd8(c, game + OFF_TYPE) : 0;
    const bool force   = reserve && !ladder && type == TYPE_SOLO;
    if (reserve && !ladder) {
        static int dits = 0;
        if (dits++ < 4) {
            char nom[65] = {0}; c.read(rec, nom, 64);
            jpline("solo: runeword ladder '%s' %s (type de partie %u)", nom,
                   force ? "applique en solo" : "laisse au jeu, hors solo", type);
        }
    }
    const uint32_t ret = s_slot.ret;
    s_slot.busy = false;
    c.set_reg(R_ESP, c.reg(R_ESP) - 4);
    br.redirect_next(force ? g_base + RVA_APPLY : ret);
    return c.reg(R_EAX);
}

uint32_t rune_entry(Cpu& c, Bridge& br) {
    const uint32_t E = c.reg(R_ESP);
    if (!s_slot.busy && c.read_u32(E) == g_base + RVA_RET_SITE) {
        s_slot.busy = true; s_slot.ret = c.read_u32(E); c.write_u32(E, s_trap);
    }
    return original(c, br, g_base + RVA_RUNE_LOOKUP, R_EBP);
}

uint32_t respec_off(Cpu& c, Bridge& br) {
    if (c.read_u32(c.reg(R_ESP)) == g_base + RVA_RESPEC_RET) {
        const uint8_t type = rd8(c, c.reg(R_ECX) + OFF_TYPE);
        static int dits = 0;
        if (dits++ < 4)
            jpline("solo: reinitialisation d'Akara %s (type de partie %u)",
                   type == TYPE_SOLO ? "gardee disponible" : "consommee, hors solo", type);
        if (type == TYPE_SOLO) return c.reg(R_EAX);    // retour direct a l'appelant
    }
    return original(c, br, g_base + RVA_RESPEC_OFF, R_ESI);
}

bool sig(Cpu* c, uint32_t rva, const uint8_t* want, size_t n) {
    uint8_t got[32];
    c->read(g_base + rva, got, (uint32_t)n);
    return !std::memcmp(got, want, n);
}

bool env_on(const char* name) {
    const char* e = getenv(name);
    return e && *e && std::strcmp(e, "0");
}

void install_runewords(Cpu* cpu, Bridge& br) {
    if (!sig(cpu, RVA_RUNE_LOOKUP, SIG_LOOKUP, sizeof SIG_LOOKUP) ||
        !sig(cpu, RVA_RET_SITE, SIG_SITE, sizeof SIG_SITE)) {
        jpline("solo: REFUS runewords — code inattendu a Game+0x%x / 0x%x",
               (unsigned)RVA_RUNE_LOOKUP, (unsigned)RVA_RET_SITE);
        return;
    }
    Shim x; x.argc = 0; x.stdcall_cleanup = false; x.tag = "native!d2_runes_ret";
    x.fn = [&br](Cpu& c) -> uint32_t { return rune_return(c, br); };
    br.register_shim("native.hook", "d2_runes_ret", x);
    s_trap = br.shim_trap("native.hook", "d2_runes_ret");

    Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_runes_lookup";
    s.fn = [&br](Cpu& c) -> uint32_t { return rune_entry(c, br); };
    br.register_shim("native.hook", "d2_runes_lookup", s);
    cpu->set_alternate(g_base + RVA_RUNE_LOOKUP, br.shim_trap("native.hook", "d2_runes_lookup"));
    jpline("solo: runewords ladder autorises en solo (D2_RUNEWORDS_LADDER)");
}

void install_respec(Cpu* cpu, Bridge& br) {
    if (!sig(cpu, RVA_RESPEC_OFF, SIG_RESPEC_OFF, sizeof SIG_RESPEC_OFF) ||
        !sig(cpu, RVA_RESPEC_OFF + 0x12, SIG_RESPEC_Q41, sizeof SIG_RESPEC_Q41) ||
        !sig(cpu, RVA_RESPEC_CALL, SIG_RESPEC_CALL, sizeof SIG_RESPEC_CALL)) {
        jpline("solo: REFUS reinitialisation — code inattendu a Game+0x%x / 0x%x",
               (unsigned)RVA_RESPEC_OFF, (unsigned)RVA_RESPEC_CALL);
        return;
    }
    Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!d2_respec_off";
    s.fn = [&br](Cpu& c) -> uint32_t { return respec_off(c, br); };
    br.register_shim("native.hook", "d2_respec_off", s);
    cpu->set_alternate(g_base + RVA_RESPEC_OFF, br.shim_trap("native.hook", "d2_respec_off"));
    jpline("solo: reinitialisation d'Akara reutilisable en solo (D2_RESPEC_UNLIMITED)");
}

}  // namespace

void native_hooks_solo_install(Cpu* cpu, Bridge& br) {
    const bool runes = env_on("D2_RUNEWORDS_LADDER"), respec = env_on("D2_RESPEC_UNLIMITED");
    if (!runes && !respec) return;
    if (!g_114 || !g_d2base) { jpline("solo: REFUS — 1.14d non detecte"); return; }
    g_base = g_d2base;
    if (runes)  install_runewords(cpu, br);
    if (respec) install_respec(cpu, br);
}
