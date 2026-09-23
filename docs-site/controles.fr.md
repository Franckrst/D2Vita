# Manette et clavier

!!! note "Autopilote"
    Le VPK (v5+) embarque un autopilote (menus → partie) qui se coupe
    automatiquement à la **première action physique** du joueur : ne touche
    à rien et il conduit jusqu'en jeu ; touche n'importe quoi et la main est
    rendue immédiatement. Il reste actif en continu pour les tests headless.

## En jeu (schéma « visée assistée », défaut depuis la snapshot manette v2)

| Entrée Vita | Action Diablo II |
|---|---|
| Stick gauche | **Déplacement seul** : le personnage marche dans la direction du stick (rayon selon l'inclinaison), sans jamais attaquer, parler ni ramasser par accident ; relâcher = arrêt net |
| Stick droit | **Curseur libre**, comme sur PC : il ne clique rien tout seul, et il atteint le HUD (ceinture, boutons de compétences) |
| Croix | **Le bouton d'action** : l'objet au sol en cours de parcours, sinon ce que vous visez, sinon un coffre / une porte / un portail / un PNJ à portée, sinon l'ennemi le plus proche |
| Rond / Carré / Triangle | **Compétences 1 à 3**. Le lancer saute sur l'ennemi dans un cône de ±35° autour du curseur (le plus proche si le curseur est posé sur le personnage), puis rend le curseur aussitôt ; maintenir = répéter |
| R maintenu + faces | Compétences 4 à 7 |
| L maintenu | **Sur place** (Maj) : lancer et attaquer sans bouger |
| L, pression brève | Marche/course (bascule) |
| L + D-pad → | Clavier virtuel |
| L + D-pad ↓ | **Clic droit** au curseur — où qu'il soit, quoi qu'il y ait dessous |
| L + D-pad ↑ | **Échange d'armes** |
| R puis L (maintenus) | Alt — étiquettes des objets au sol ; D-pad = déplacer le curseur d'objet en objet (vers le plus proche du curseur dans cette direction), Croix = ramasser l'objet sélectionné (repère cyan) |
| D-pad ↑ / ← / ↓ / → | Potions ceinture 1 / 2 / 3 / 4 |
| R + D-pad | Potion au mercenaire |
| Start | Échap |
| Select | Menu radial (voir plus bas) ; R + Select : Espace |
| Écran tactile | Curseur absolu ; tap bref = clic gauche |

Un losange marque l'ennemi ciblé (doré dès que le jeu confirme le survol). La
visée assistée ne fait qu'emprunter le curseur : un lancer le déplace sur sa
cible et le remet où vous l'aviez laissé au relâchement, et le clic d'arrêt
qui termine une marche fait de même. Sans ennemi dans le cône, la compétence
part exactement là où pointe le curseur — c'est ainsi qu'on vise un sort au
sol (téléport, météore…). Curseur posé sur les pieds du personnage, elle part
plutôt dans la direction de votre dernier déplacement.

`aim=0` dans `controls.txt` désactive le saut et laisse un curseur simple.

**Le curseur prime sur tout.** Ce qu'il survole est ce sur quoi Croix agit,
avant n'importe quel cône. Les distances se mesurent dans le monde et non en
pixels écran : la projection rend un pixel vers le bas deux fois plus « cher »
qu'un pixel de côté, donc une unité au nord n'est pas la plus proche qu'elle
paraît. Et une unité que le jeu refuse de survoler — une bestiole, un vautour
encore en vol — est abandonnée au bout d'un moment au lieu de capturer tous
les appuis.

**Assigner une compétence à un bouton** : ouvrir l'arbre de compétences
(menu radial), survoler l'icône, puis faire exactement le geste qui la
lancera — Rond pour l'emplacement 1, R + Croix pour le 4, etc. **Croix y
clique** comme partout ailleurs : c'est toujours elle qui dépense un point.

**Panneaux ouverts** (inventaire, coffre, marchand…) : les sticks déplacent le
curseur, L ou Croix cliquent, Triangle = clic droit, Carré maintenu = Shift
(déplacement rapide), Rond ferme.

**Ce que vise un emplacement.** Par défaut une compétence cherche un ennemi
vivant. Deux familles veulent autre chose, et `controls.txt` le dit
emplacement par emplacement : `ground` ne saute jamais et part là où pointe
le curseur (téléportation, météore, blizzard — sauter sur le monstre avec
une téléportation vous met *dessus*), et `corpse` cherche un mort à la place
(explosion de cadavre, résurrection, relever un squelette, que le filtre
« ennemi vivant » exclut par construction).

Hors partie (menus, sélection de personnage), l'ancien schéma souris reste
actif. `scheme=mouse` dans `controls.txt` le rétablit partout (mapping de la
0.1.6 : L/R = clics, Croix = marche/course, Rond = Shift, Carré = Alt,
Triangle = W, R + D-pad = F1-F4).

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
est volontairement pas inclus — trop utilisé pour justifier de passer par un
menu à chaque fois, il reste sur son propre geste dédié (L + D-pad droite ;
R + Triangle dans le schéma `scheme=mouse` legacy, où sous le schéma de visée
c'est la compétence 7).

## Clavier virtuel (L + D-pad droite)

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
déclenchement, lui, est spécifique à d2vita : L + D-pad droite sous le schéma
de visée, R + Triangle sous `scheme=mouse`.

## Remappage sans rebuild

Fichier `ux0:data/d2vita/controls.txt`, une ligne par entrée :

```
scheme=aim        # aim (défaut, visée assistée) | mouse (schéma legacy intégral)
aim=1              # 0 = pas de choix automatique de cible (schéma aim seulement)
orbit_min=40       # px (à 600 de haut), rayon d'orbite stick à peine poussé
orbit_max=110      # px, stick à fond
range_min=6        # sous-tuiles, sort au sol curseur posé sur le personnage
range_max=20
cone=35            # demi-angle du cône autour du curseur, degrés
hover_h=28         # hauteur de survol par défaut, px
hud_h=60           # bande basse interdite aux clics ASSISTÉS, px (le curseur
                   # que vous pilotez y va, sinon la ceinture serait hors de portée)
slot1=hostile      # slot1..slot7 : hostile (défaut) | ground | corpse
slot3=ground       #   ex. téléportation sur le 3, explosion de cadavre sur le 5
slot5=corpse
deadzone=0.25
sens=10            # vitesse du curseur libre (stick droit en jeu, deux sticks dans les panneaux)
```

`D2_PAD=0` (ou `1`) dans `env.txt` a priorité sur `scheme=` — pratique pour
un A/B sans toucher à `controls.txt`.

Les clés suivantes, et les remaps `cross=…`/`r+triangle=…` ci-dessous, ne
s'appliquent qu'au schéma `scheme=mouse` (legacy) :

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
