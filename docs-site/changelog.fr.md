# Journal des versions

Changements côté joueur uniquement — refactors internes, commits de test
et commits de documentation seule sont omis, sauf quand une release ne
contient que ça. Historique complet des commits :
[vue comparative GitHub](https://github.com/Franckrst/D2Vita/commits/main).

## v0.1.11-beta3 — 2026-09-23

- Étiquette de build à l'écran (coin haut-droit : version + hash de
  commit), pour qu'une capture d'écran de rapport de bug dise exactement
  de quelle build elle vient. `D2VITA_BUILDTAG=0` dans `env.txt` la masque.
- Panneaux en 960x544 centrés verticalement, en plus du centrage
  horizontal déjà livré en v0.1.11-beta.

## v0.1.11-beta2 — 2026-09-23

- **Crash d'épuisement de la piscine JIT corrigé (signature
  `SMRD2J34ZXU2A55I`, crash n°1 en volume — 143 rapports / 42 consoles au
  22/09).** Le mécanisme d'éviction-et-réessai (déjà livré en v0.1.10) ne
  se déclenchait que si l'allocateur bas niveau refusait explicitement une
  demande. Il se déclenche désormais aussi directement sur l'occupation de
  la piscine (≥95% pleine), fermant un second chemin vers le même crash
  que le seul compteur de l'allocateur ne pouvait pas voir. Budget de
  réessai relevé (8→24 tours d'éviction, 64→192 sorties de secours) — un
  pari à faible risque, pas encore prouvé par un rapport de terrain, mais
  les réessais concernés étaient déjà bornés et sûrs côté verrouillage.
- Fenêtre VA invitée réduite de 220 à 216 Mio pour redonner un peu de
  marge à la piscine JIT (elle manquait d'à peu près ça, selon deux
  estimations indépendantes).
- Pas encore validé sur console — à surveiller sur le nombre de rapports
  de cette signature dans les jours qui viennent.

## v0.1.11-beta — 2026-09-23

- Corrigé : en 960x544, les clics à l'écran (croix de fermeture des
  panneaux, boutons, appuis tactiles, L/R) tombaient sur des coordonnées
  périmées au lieu de ce qui était dessiné. Le décalage écran est
  désormais écrit là où le jeu lui-même pose son interface, donc clics et
  dessin sont toujours d'accord.
- Les panneaux s'ancrent aux bords de l'écran par défaut ; l'ancienne
  disposition centrée façon SGD2 devient une option (`controls.txt`).

## v0.1.10 — 2026-09-22

- L'image n'est plus étirée : Glide dessine en 800x600 et l'ancienne
  projection le plaquait de façon affine sur tout l'écran (×1,2 en X,
  ×0,907 en Y — 32 % trop large). Échelle isotrope par défaut désormais
  (725×544, bandes noires) ; `D2_ASPECT=etire` restaure l'ancien rendu
  étiré.
- Résolution native 960x544 en option (`D2_RES`, absent par défaut) : le
  jeu dessine directement à la résolution de l'écran Vita au lieu
  d'agrandir du 800x600 — un texel pour un pixel, sans filtrage.
- `d2vGlideFlush` déporté du fil de jeu vers son propre cœur, et six
  réglages qui dormaient dans la config de référence depuis début
  septembre deviennent le défaut compilé : **+13 % en jeu réel, a-coups
  divisés par 29** (mesuré sur console, passes entrelacées).
- Les derniers trous de la disposition 960x544 comblés : les panneaux
  suivent maintenant leur fond (tables, clics, cadre, ceinture), et la
  colonne/ligne noire entre deux panneaux ouverts a disparu.
- La piscine de code traduit (JIT) de box86 et le tas RW de métadonnées
  rendus visibles sur la ligne de battement de cœur de la console et dans
  les diagnostics de plantage, en amont du travail d'éviction livré en
  v0.1.11-beta2.
- Suppression de `grab_checkrevision.py` et de l'échappatoire
  `D2_ALLOW_OFFICIAL` qui allait avec.

## v0.1.9 — 2026-09-21

- Le clavier virtuel s'ouvre désormais tout seul quand un champ texte
  prend le focus (nom de personnage, compte/mot de passe Battle.net, nom
  de partie) — aucun signal Win32 n'existe pour ce focus, donc il est lu
  directement dans l'état d'interface de D2. L'ouverture manuelle
  (R+Triangle) fonctionne toujours ; `D2_KBAUTO=0` dans `env.txt` coupe
  l'automatisme.
- Opacité du clavier réglable (`D2_KBALPHA=0-100`, défaut 80 % —
  translucide, le personnage/menu reste visible derrière), composée sur
  le GPU plutôt que relue depuis le CPU.

## v0.1.8 — 2026-09-20

- Correction des huit signatures de plantage encore actives en 0.1.6
  (suivi dans la PR #15) : lectures/écritures de fichiers échouées non
  rapportées, dossier d'écriture non validé, archives non vérifiées au
  boot, mauvais dossier de rapport de plantage.
- Plafond du tas invité relevé à ~32,9 Mio (des `ALLOC FAIL` et un Halt
  904 sur les grosses allocations — l'atlas de glyphes des installations
  en chinois traditionnel était le déclencheur le plus fréquent).
- Rendu du menu radial (Select) entièrement déporté sur le GPU : il
  coûtait ~81 ms par image tant qu'ouvert (24,5 → 8,2 img/s sur console),
  dont ~70 % rien qu'à relire la CDRAM depuis le CPU pour mélanger.
  Composité désormais en quads texturés, sans aucune relecture du
  framebuffer.
- Premier lot de mitigations contre l'épuisement de la piscine JIT, repris
  de winx86 (la demande de segment décroît au lieu d'abandonner net ;
  occupation publiée sur le battement de cœur) — pas encore le correctif
  complet, voir v0.1.11-beta2.

## v0.1.7 — 2026-09-20

- Release surtout documentation et graphismes : disposition de
  `keys.txt` documentée (EN/FR), habillage LiveArea issu d'une
  contribution communautaire (issue #7).

## v0.1.6 — 2026-09-19

- Plafond du tas invité relevé à ~25 Mio (un `HeapAlloc` de 4,2 Mo était
  refusé au plafond précédent de 20 Mio alors que plus de 200 Mo d'autre
  mémoire restaient libres) — la même famille de correctif poussée plus
  loin en v0.1.8.
- Les rapports de faute dynarec dumpent désormais les octets et la région
  de l'opcode fautif, rendant toute cette famille de plantages traçable
  au lieu de générique.

## v0.1.5 — 2026-09-18

- Correctif du processus de release uniquement (marge d'empaquetage VPK
  après le premier segment) — aucun changement côté joueur.

## v0.1.4 — 2026-09-20 *(étiquette corrigée après coup — voir note)*

- Le jeu refuse désormais de charger autre chose que le `Game.exe` 1.14d
  officiel **avant** de l'exécuter, plutôt que d'échouer plus tard avec le
  message cryptique de Diablo lui-même « Unsupported graphics mode ».
  (Cause racine d'un vrai rapport : un exécutable 1.14b suffisamment
  proche pour passer la vérification de version.)
- Outil d'administration des rapports de plantage : dernière version vue
  par signature, filtres déroulants.

!!! note "Pourquoi v0.1.4 est datée après v0.1.5–v0.1.7"
    Sa première tentative de release a échoué en CI (un problème
    d'empaquetage VPK) ; l'étiquette a été corrigée et repoussée après la
    sortie de v0.1.5, v0.1.6 et v0.1.7, donc son historique git et sa date
    de publication tombent après ce que son numéro de version suggère. Le
    contenu ci-dessus est ce que v0.1.4 a réellement livré, indépendamment
    du moment où l'étiquette a été corrigée.
