# Manette v2 — navigation du butin au sol (Phase 2) : conception

Date : 2026-09-19. Branche : `wt/manette` (worktree `/home/doudou/repos/wt-manette`).
Construit sur la Phase 1, déjà livrée et validée sur console réelle :
`docs/superpowers/specs/2026-09-19-manette-ciblage-design.md`.

## 1. Objectif

Aujourd'hui, R puis L maintenus affichent les étiquettes de tous les objets au sol
visibles à l'écran (« Alt »), mais L seul ne peut interagir qu'avec **le seul objet le
plus proche du personnage**. Objectif : pouvoir **choisir précisément lequel** parmi
plusieurs objets affichés, avec le D-pad pour naviguer et Croix pour valider — sans
toucher au stick droit (déjà utilisé pour la visée de compétence, et la Phase 1 a déjà
montré via l'exemple Path of Exile 1/2 que réutiliser le même stick pour deux usages
finit en conflit).

Contrainte fondatrice inchangée (CLAUDE.md, héritée de la Phase 1) : on n'écrit jamais
dans la mémoire du jeu, on n'appelle aucune fonction du jeu. Ce mécanisme reste un
curseur de souris simulé + clics.

## 2. Comportement actuel (ce qui change)

| Entrée | Aujourd'hui |
|---|---|
| R puis L maintenus | Alt : étiquettes affichées, aucune sélection possible |
| L seul | Interagit avec le seul objet/PNJ le plus proche (priorité item ≤ 220 px, puis objet/PNJ ≤ 160 px, puis cible hostile) |

L seul (sans R) ne change pas : reste le raccourci rapide vers « le plus proche »,
inchangé par cette Phase 2.

## 3. Nouveau comportement

### 3.1 Déclenchement et périmètre

Identique à l'Alt actuel : R maintenu en premier, puis L. Portée : **tous les objets
actuellement affichés à l'écran par Alt** — la même liste qui alimente déjà l'affichage
des étiquettes, aucune limite de distance ajoutée (contrairement à L seul). Filtré aux
objets au sol (`type==4`) uniquement ; les coffres/portes/PNJ restent exclusivement
accessibles via L seul, comme aujourd'hui — Alt n'a jamais montré que des objets, ce
mode ne change pas cette portée.

### 3.2 Curseur et sélection initiale

À l'entrée du mode (L pressé après R) : le curseur se place sur l'objet le plus proche
du personnage. Si aucune flèche n'est utilisée, Croix reproduit exactement le
comportement actuel de L seul.

### 3.3 Navigation (D-pad)

Chaque flèche déplace le curseur vers l'objet le plus proche **du curseur** (pas du
personnage) dans la direction pressée — navigation par focus directionnel, motif
standard d'UI console.

Algorithme, pour une flèche `D` et une position écran de curseur `(cx, cy)` : un objet
candidat `I` en `(ix, iy)` est retenu si `(dx, dy) = (ix-cx, iy-cy)` a le signe attendu
pour `D` et si la composante de `D` est **dominante** :

- droite : `dx > 0 && |dx| >= |dy|` — gauche : `dx < 0 && |dx| >= |dy|`
- bas : `dy > 0 && |dy| >= |dx|` — haut : `dy < 0 && |dy| >= |dx|`

Parmi les candidats retenus, le plus proche en distance euclidienne du curseur est
choisi. Égalité de distance : id d'unité le plus petit (déterministe, pas de
scintillement). Aucun candidat dans cette direction ⇒ le curseur ne bouge pas.

### 3.4 Affichage

Les noms de tous les objets restent affichés (Alt actuel, inchangé). Un repère visuel
distinct marque l'objet actuellement sélectionné par le curseur — même famille que le
losange de ciblage hostile de la Phase 1, mais visuellement différent (couleur/forme)
pour ne jamais le confondre avec une cible de compétence.

### 3.5 Validation (Croix)

Réutilise tel quel le mécanisme de ramassage déjà existant pour L (Phase 1 §5.3,
`Scheme::worldTick`, bloc interact) : déplacement du curseur jeu au point de survol
appris pour ce type d'objet (`HoverTable`), clic gauche, vérification via l'unité
survolée par le jeu (`selValid`/`selId`/`selType`), ajustement de la hauteur sur 4
tentatives si besoin. Aucun nouveau code de ramassage — seule la sélection de **quel**
objet cibler change.

Maintenu = le ramassage suit la même logique multi-tick que L aujourd'hui ; relâché
avant complétion = annulé proprement (LUP, pas de ramassage forcé). **Pendant que
Croix est maintenu, le D-pad ne change plus la sélection** — la cible du ramassage en
cours est verrouillée, cohérent avec le principe « une seule action à la fois » déjà en
place pour les compétences (Phase 1 §5.1).

### 3.6 Stick gauche et droit pendant ce mode

- Stick gauche (déplacement) : suspendu, comme pendant toute interaction maintenue en
  Phase 1 (§5.1 : « pendant un lancer ou une interaction maintenue, le déplacement est
  suspendu »).
- Stick droit (visée) : inerte — pas de réticule de visée de compétence pendant
  l'affichage du butin ; ce mode remplace temporairement la visée, il ne s'y ajoute pas.

### 3.7 Relâchement

Relâcher R ou L, à tout moment (y compris pendant un Croix maintenu), referme le mode
proprement : tout ce qui est maintenu est relâché (LUP si un ramassage était en cours),
la sélection est oubliée — la prochaine ouverture repart de « objet le plus proche du
personnage ».

## 4. Architecture

Vit dans `pad::Scheme` (Phase 1, `src/platform/pad_core.h/.cpp`), pas un nouveau
composant séparé : nouvel état analogue à `interact_`/`castSlot_` (ex. `lootBrowse_`,
`lootCursorId_`). La liste de candidats (objets au sol affichés) provient de la liste
d'unités déjà construite par les crochets existants (`padst`, Phase 1 Task 5) — aucun
nouveau crochet nécessaire, juste un filtre `type==4` sur une liste déjà peuplée.

Nouvelle fonction pure, testable sur PC comme le reste de la Phase 1 :

```cpp
// Direction focus navigation among on-screen ground items. Returns the index
// in `u` of the nearest item to `u[fromIdx]` in screen direction `d`, or -1
// if none qualifies (dominant-axis test, see design §3.3).
enum Dir { D_UP, D_DOWN, D_LEFT, D_RIGHT };
int nav_direction(const Unit* u, int n, int fromIdx, Dir d);
```

## 5. Configuration

Aucune nouvelle clé `controls.txt` identifiée pour l'instant — contrairement à
`orbit_min/max`, `cone`, etc. (Phase 1), ce comportement n'a pas de paramètre
ergonomique évident à exposer.

## 6. Validation

- **PC** : `nav_direction` sur plusieurs configurations (grille régulière, objets
  alignés en diagonale — donc jamais dominants dans aucune direction, un seul objet,
  aucun objet, égalité de distance).
- **Console** (seule source de vérité, comme la Phase 1) : une scène avec 3+ objets au
  sol dispersés ; vérifier que les 4 directions déplacent le curseur comme attendu et
  que Croix ramasse bien l'objet actuellement sélectionné, pas un autre.

## 7. Risques et replis

| Risque | Repli |
|---|---|
| Beaucoup d'objets à l'écran (drop massif, boss) : le calcul par direction devient coûteux | La liste est déjà bornée par ce qui est dessiné à l'écran (même liste qu'Alt) — pas un nouveau coût, un tri de plus sur une liste déjà petite |
| Deux objets à égale distance dans la même direction | Départage déterministe par id, pas de scintillement |
| Objet à exactement 45° du curseur | Aucune ambiguïté réelle : les deux tests (`|dx|>=|dy|` et `|dy|>=|dx|`) utilisent une comparaison large, donc un objet en diagonale pure est atteignable à la fois par la flèche horizontale et la verticale — couvert par un test PC dédié |
