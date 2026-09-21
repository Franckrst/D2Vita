// native_hooks_resolution.h -- résolution de jeu native (960x544 par défaut),
// obtenue SANS écrire un seul octet de l'image invitée.
//
// Le jeu ne connaît que 640x480 et 800x600 ; sur Vita l'écran fait 960x544.
// Tant que la fenêtre de jeu vaut 800x600, la projection doit choisir entre
// étirer (image 32 % trop large) et border (bandes de 117 px, voir D2_ASPECT
// dans vita_gxm.cpp). Faire dessiner le JEU en 960x544 supprime le choix : un
// texel = un pixel, plein écran, aucun filtre.
//
// Mécanique : quatre crochets `alternate` (donc posés à la TRADUCTION, jamais
// en mémoire invitée) plus des écritures de GLOBALS `.data` — c'est-à-dire
// exactement ce que le jeu écrit lui-même. L'invariant « 0 octet de .text
// modifié » de pristine_audit reste vrai, ce qui garde ce chemin hors de
// portée d'un MEM_CHECK ou d'un PAGE_CHECK.
//
// ARMEMENT : D2_RES=960x544 (ou D2_RES=1 pour la taille de l'écran). ABSENT
// PAR DÉFAUT. Le champ de vision élargi est un avantage en jeu : à réserver
// au solo et au serveur privé, jamais au Battle.net officiel.
#pragma once
#include <cstdint>
namespace d2rt { class Cpu; class Bridge; }
using d2rt::Cpu; using d2rt::Bridge;

void native_hooks_resolution_install(Cpu* cpu, Bridge& br);

// Consultés par le glide3x maison (src/glide_ring/gx_host.cpp) : quand
// l'override est armé, grSstWinOpen ouvre cette taille quel que soit l'index
// de mode que le jeu demande — c'est le modèle D2DX, et c'est légitime ici
// puisque nous SOMMES le pilote Glide.
extern "C" int d2res_active(void);
extern "C" int d2res_w(void);
extern "C" int d2res_h(void);
