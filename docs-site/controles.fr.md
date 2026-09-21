# Manette et clavier

!!! note "Autopilote"
    Le VPK (v5+) embarque un autopilote (menus → partie) qui se coupe
    automatiquement à la **première action physique** du joueur : ne touche
    à rien et il conduit jusqu'en jeu ; touche n'importe quoi et la main est
    rendue immédiatement. Il reste actif en continu pour les tests headless.

## En jeu

| Entrée Vita | Action Diablo II |
|---|---|
| Stick gauche | **Déplacement direct** (curseur en orbite autour du personnage + clic gauche maintenu) |
| Stick droit | Souris libre, sans clic (visée, menus, survol) |
| Écran tactile | Curseur absolu ; tap bref = clic gauche |
| **L (maintenu)** | **Clic gauche** |
| **R (maintenu)** | **Clic droit** (sert aussi de couche combo, voir plus bas) |
| Croix | R — bascule marche/course |
| Rond | Shift (maintenu) — attaque sur place / cast forcé |
| Carré (maintenu) | Alt — affiche les objets au sol |
| Triangle | W — échange d'armes |
| D-pad ↑ / ← / ↓ / → | Potions ceinture 1 / 2 / 3 / 4 |
| Start | Échap (menu / fermer) |
| **Select** | **Menu radial** (voir plus bas) |

## Couche R (R maintenu + …)

| Combo | Action |
|---|---|
| R + Triangle | **Clavier virtuel** (ouvrir ; fermer avec Select — il s'ouvre aussi de lui-même sur les champs texte, voir plus bas) |
| R + D-pad | F1 / F2 / F3 / F4 — compétences rapides |
| R + Select | Espace — fermer tous les panneaux |

## Menu radial (Select)

Appuyer sur **Select** ouvre immédiatement un menu à 7 secteurs centré à
l'écran. Le **stick gauche** pointe vers un secteur (il s'illumine en doré) ;
**relâcher Select** valide le secteur pointé à cet instant précis. Ramener le
stick au centre avant de relâcher Select **annule** (aucun secteur n'est
alors sélectionné, même si un secteur avait été survolé juste avant).

| Secteur | Action D2 |
|---|---|
| Compétences (haut) | T — arbre de compétences |
| Quêtes | Q — journal de quêtes |
| Carte | Tab — automap |
| Équipement | I — inventaire |
| Chat | Entrée — ligne de discussion |
| Guilde | P — écran de groupe |
| Personnage | C — feuille de personnage |

Ce menu remplace les anciens raccourcis dédiés à chacune de ces actions
(personnage, compétences, quêtes, automap, inventaire) : un seul geste pour
les sept, plutôt que sept combinaisons à mémoriser. Le clavier virtuel n'y
est volontairement pas inclus : il s'ouvre de lui-même dès qu'un champ texte
prend le focus, et garde son propre geste dédié (R + Triangle) pour le reste.

## Clavier virtuel

Le clavier **s'ouvre de lui-même** quand un champ texte prend le focus — nom
de personnage, compte et mot de passe Battle.net, nom et mot de passe de
partie. La fermeture reste manuelle (Select ou la touche FERMER —
Start/Entrée valide le champ mais laisse le clavier ouvert), et un clavier
fermé ne se rouvre pas tant qu'un autre champ n'a pas pris le focus (ou que
le même ne l'a pas perdu puis repris). R + Triangle l'ouvre toujours, sur
n'importe quel écran. `D2_KBAUTO=0` dans `ux0:data/d2vita/env.txt` coupe
l'ouverture automatique. Le clavier se dessine à 80 % d'opacité par défaut —
le personnage/menu reste visible en filigrane derrière — réglable avec
`D2_KBALPHA=0-100` (100 = ancien rendu opaque). En dessous de 100, le mélange
se fait sur le GPU (une couche texturée à part, composée aux côtés du menu
radial) quand ce chemin est armé ; sinon le clavier revient à un mélange CPU,
qui relit l'écran et coûte nettement plus cher par image — opaque (100) reste
dans tous les cas le chemin rapide. Le runtime lit pour cela l'état « contrôle
focalisé » du jeu lui-même (lecture seule, l'en-tête du contrôle seulement —
jamais le texte). Chaque transition est journalisée dans
`boot_progress.txt` en `clavier: focus champ ON/OFF` (plafonné à 64 lignes
par session). Le chat en jeu n'est
pas encore couvert (phase 2).

| Entrée | Action |
|---|---|
| D-pad | Naviguer sur les touches |
| Croix | Taper la touche sélectionnée |
| Rond | Effacer (backspace) |
| Start | Entrée |
| Select (ou touche FERMER) | Fermer le clavier |
| Tap tactile sur une touche | La taper directement |

Le rendu du clavier virtuel lui-même (police, disposition) vient du moteur
générique winx86 (`src/platform/vita_kb.h`/`vita_kb_font.h`). Le
déclenchement (R + Triangle), lui, est spécifique à d2vita.

## Remappage sans rebuild

Fichier `ux0:data/d2vita/controls.txt`, une ligne par entrée :

```
cross=rclick
r+triangle=f5
orbit=70         # rayon du déplacement direct (px)
sens=10          # vitesse du stick droit
deadzone=0.25
anchor_y=470     # ancre verticale du personnage (pour une résolution de 1000)
```

**Boutons** : `cross`/`croix`, `circle`/`rond`, `square`/`carre`,
`triangle`, `up`, `down`, `left`, `right`, `start`, `select` (préfixer `r+`
pour la couche R).

**Actions** : `lclick`, `rclick`, `alt`, `shift`, `tab`/`automap`,
`esc`/`echap`, `inv`, `perso`, `skills`, `quests`, `swap`, `space`, `run`,
`enter`, `pot1`-`pot4`, `f1`-`f8`, `vk:0xNN` (code de touche brut), `none`.

## Diagnostic

`D2_INPUTLOG=1` dans `env.txt` journalise les mots de boutons bruts reçus —
utile pour déboguer un remappage qui ne se comporte pas comme attendu.
N'activer que pour du diagnostic, pas en jeu normal (coût de journalisation).

!!! note "Gotcha connu"
    Si la boîte de chat est ouverte, Échap la ferme d'abord (un deuxième
    appui est nécessaire pour ouvrir le menu).
