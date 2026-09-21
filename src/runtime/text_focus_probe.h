// text_focus_probe.h -- sonde « un champ texte a le focus » : lit la globale
// « contrôle focalisé » de D2Win (lecture seule, jamais le texte du champ) et
// publie l'identifiant du contrôle pour l'ouverture automatique du clavier
// (vita_present.cpp). Appelée depuis la porte 16 ms de PeekMessageA, sur le
// fil du jeu, avant d2vita_input_tick(). D2_KBAUTO=0 la coupe.
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; }

void text_focus_poll(d2rt::Cpu& c);
extern "C" uint32_t d2vita_text_focus(void);   // edit box focalisée (0 = aucune)
