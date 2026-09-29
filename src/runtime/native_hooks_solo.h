// native_hooks_solo.h -- options de confort, en partie solo seulement (opt-in).
//
// D2_RUNEWORDS_LADDER=1 : un runeword marque `server` dans Runes.txt (reserve
// au ladder Battle.net) se cree comme les autres.
// D2_RESPEC_UNLIMITED=1 : la reinitialisation stats/competences d'Akara n'est
// pas consommee, elle reste proposee.
// Aucun octet du jeu ni de ses donnees n'est modifie : des traps choisissent
// une branche du code serveur, seulement depuis des sites d'appel verifies et
// seulement si la partie est de type solo.
#pragma once
namespace d2rt { class Cpu; class Bridge; }
void native_hooks_solo_install(d2rt::Cpu* cpu, d2rt::Bridge& br);
