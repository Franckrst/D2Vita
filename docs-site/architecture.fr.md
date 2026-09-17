# Architecture

## Relation avec winx86

```
d2vita (ce dépôt)
  │
  ├── third_party/winx86/  (sous-module git, moteur générique)
  │     chargeur PE32, dynarec ARMv7, Cpu/Bridge, ordonnanceurs
  │
  └── tout le reste (spécifique à Diablo II)
```

Le moteur — chargeur PE32, dynarec x86→ARMv7 dérivé de Box86, interface
`Cpu`, `Bridge` (pont d'imports Win32), ordonnanceurs de fils invités — vit
dans [winx86](https://winx86-136891.gitlab.io/) et n'a **aucune**
connaissance de Diablo II. d2vita l'ajoute comme sous-module git
(`third_party/winx86`) et lie son propre exécutable de boot
(`tools/rt_boot.cpp`) contre `libwinx86.a`.

Cette séparation a été faite en extrayant de ce dépôt tout ce qui n'avait
aucune connaissance du jeu, validée à chaque étape par `tools/shim_seq.sh`
(aucune table de shims n'a bougé) et l'oracle qemu-arm rejoué deux fois
(verdict déterministe identique avant/après).

## Les couches spécifiques à Diablo II

### `tools/rt_boot.cpp` — le boot

Le point d'entrée du runtime : charge les DLL de D2 1.14d, enregistre les
shims Win32 via l'API d'extension de winx86, démarre l'ordonnanceur.

La **liste complète, avec qui sert quoi**, est sur la page
[Liste des shims Win32](shims.md) — générée depuis les sources, donc jamais
périmée.

!!! note "Le tri est fait fonction par fonction, sur le corps réel"
    Un nom d'API parfaitement générique (`SHGetFolderPathA`...) peut porter du
    contenu propre au jeu — un chemin `C:\Diablo II` codé en dur, par exemple.
    Migrer un shim vers le dépôt générique sur la seule foi de son nom serait
    malhonnête : la classification se fait **fonction par fonction, sur le
    corps réel et non sur le nom d'API** (SHELL32/ADVAPI32, USER32,
    DDRAW/Bink/Smacker/ijl11, IMM32, GDI32 et la fenêtre/curseur, VERSION et
    PSAPI, WSOCK32/WS2_32). Chaque déplacement est couvert par
    `tools/shim_seq.sh` (ensemble, corps gagnant, ordre) et par l'oracle
    qemu-arm rejoué deux fois.

    Ce qui était réellement spécifique est resté, et c'est assumé : le chemin
    `C:\Diablo II` de `SHGetFolderPathA` est toujours ici.

Le gros morceau non trié reste **KERNEL32** : la grande majorité de ses clés
sont encore côté jeu, sans qu'un audit ait dit lesquelles sont réellement
spécifiques. C'est le prochain chantier évident.

### Les hooks natifs de point chaud

Trois fichiers extraits de `rt_boot.cpp`, sur le même patron
que l'API d'extension de winx86 (`Cpu::set_alternate` + `Bridge::shim_trap`) :

- **`src/runtime/phase_hooks.cpp`** — crochets de phase de rendu
  (`D2_PHASEPROF`, `D2_ONEDRAW`) et de marquage du ring Glide
  (`D2_REPLAY60`/`D2_RINGTAG`).
- **`src/runtime/native_hooks_codec.cpp`** — décodeurs (compression PKWARE,
  codecs audio Huffman/ADPCM), décodage de sprites DCC, `fog_raise_snap`.
  Groupe volontairement limité aux hooks **autonomes** — sans dépendance sur
  le moteur de parallélisme ci-dessous.
- **`src/runtime/native_hooks_cellengine.cpp`** — le moteur fork-join de
  portage de la boucle de cellules (`NATIVECELLLOOP`), avec le lightgrid, le
  blend, le RLE et la lookup de collision qui en dépendent. C'est le bloc
  qui porte l'essentiel des gains mesurés (voir
  [Portages natifs et gains mesurés](gains.md)).

Ce qui reste dans `rt_boot.cpp` : le câblage de boot lui-même et tout ce qui
n'a pas encore été jugé assez isolé pour être extrait sans risque.

### Rendu et plateforme

- **`src/glide_ring/*`** — pipeline de rendu Glide de D2 (tampon annulaire,
  atlas de textures, rejeu) rendu sur le GPU (sceGxm) plutôt qu'en logiciel.
- **`src/runtime/vita_present.cpp`** — présentation à l'écran (mélange
  générique/spécifique, pas encore audité en détail).
- **`src/platform/vita_net.cpp`** — ce qui reste de la couche réseau côté
  jeu. Les primitives (table de sockets, résolution de noms, mise en
  non-bloquant, attente GIL-aware) sont parties dans winx86 ;
  ce fichier garde l'auto-test, qui embarque des vérifications du protocole
  Battle.net.
- **`src/runtime/dcc_native.cpp`** — décodeur du format de sprite
  propriétaire de Blizzard (DCC).
- **`src/runtime/ds_emul.cpp`** — émulation DirectSound (vtable COM
  fabriquée + choix de mixage spécifiques au jeu).

## Comment la frontière est tenue

Le motif est toujours le même, et le réseau en est l'exemple le plus abouti :
**le moteur fournit le mécanisme, le jeu fournit la politique.**

- winx86 implémente les primitives Winsock→POSIX (table de sockets,
  `connect`, `recv`, `send`, et de vraies implémentations d'`accept`/`bind`/
  `listen` qui n'étaient jusque-là que des bouchons). Aucun vocabulaire
  Diablo II n'y apparaît : ni numéro de port, ni notion de passerelle.
- winx86 expose un **observateur passif** : il raconte ce qui passe
  (connexion, octets émis, octets reçus), il ne demande jamais d'avis.
- d2vita enregistre **un seul** observateur, en **un seul** endroit, qui
  porte tout le spécifique : décodage du protocole Battle.net, drapeaux
  d'état de connexion, bascules d'horloge.

Le « un seul endroit » n'est pas une préférence de style. `register_shim`
**écrase** la clé : une fonction inscrite deux fois voit la dernière
inscription gagner, silencieusement. Ce motif a causé une vraie régression de
connexion Battle.net. La page [Liste des shims Win32](shims.md) liste en
permanence les clés inscrites plus d'une fois, pour que le cas se voie au
lieu de se découvrir en production.

Deux corollaires appris à la dure :

- **Un nom d'API ne dit rien.** La classification se fait sur le corps réel
  de la fonction, jamais sur sa signature Windows.
- **Une surcouche n'est pas une couche de traduction.** Le shim qui traduit
  un appel Win32 en appel POSIX est obligatoire — il n'existe aucun noyau
  Windows sous la Vita. Ce qui doit disparaître, c'est la logique *métier*
  glissée dedans. Exemple : brancher le jeu sur un serveur privé se fait par
  la **liste de passerelles du registre**, comme sur PC — pas en réécrivant
  l'adresse de destination dans `connect`.

## Les trois niveaux de test

Voir [Validation à trois niveaux](validation.md) pour la méthode complète —
c'est ce qui rend crédible n'importe quelle affirmation de performance ou de
correction faite sur ce projet.
