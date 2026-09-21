// native_hooks_resolution.h -- le jeu dessine a la resolution de l'ecran
// (960x544), sans qu'un seul octet de l'image invitee soit modifie.
//
// D2 ne connait que 640x480 et 800x600. Tant que sa fenetre vaut 800x600, la
// projection doit choisir entre etirer (image 32 % trop large) et border
// (bandes de 117 px). Le faire dessiner en 960x544 supprime le choix : un
// texel = un pixel, plein ecran, aucun filtre.
//
// Quatre crochets `alternate` -- donc poses a la TRADUCTION, jamais en memoire
// invitee -- plus des ecritures de globals `.data`, c'est-a-dire exactement ce
// que le jeu ecrit lui-meme. L'invariant « 0 octet de .text modifie » de
// pristine_audit reste vrai.
//
// ACTIF PAR DEFAUT. D2_RES=0 revient au 800x600 du jeu (et donc aux bandes,
// cf. D2_ASPECT dans vita_gxm.cpp) ; D2_RES=LxH force une autre taille.
//
// Le champ de vision elargi est un avantage en jeu : a reserver au solo et au
// serveur prive.
#pragma once
namespace d2rt { class Cpu; class Bridge; }

void native_hooks_resolution_install(d2rt::Cpu* cpu, d2rt::Bridge& br);

// Consultes par le glide3x maison (src/glide_ring/gx_host.cpp) : grSstWinOpen
// ouvre cette taille quel que soit l'index de mode demande par le jeu -- c'est
// le modele D2DX, legitime ici puisque nous SOMMES le pilote Glide.
extern "C" int d2res_active(void);
extern "C" int d2res_w(void);
extern "C" int d2res_h(void);
