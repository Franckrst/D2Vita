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
| Stick droit | **Visée** : choisit l'ennemi dans un cône de ±35° ; pour un sort au sol (téléport, météore…), la distance suit l'inclinaison |
| Croix / Rond / Carré / Triangle | **Compétences 1 à 4**, lancer immédiat sur l'ennemi ciblé (le plus proche si le stick droit est au repos) ; maintenir = répéter |
| R maintenu + faces | Compétences 5 à 8 |
| L | **Interagir** : objet au sol le plus proche, sinon coffre / porte / PNJ, sinon attaque de base sur la cible |
| R puis L (maintenus) | Alt — étiquettes des objets au sol ; D-pad = déplacer le curseur d'objet en objet (vers le plus proche du curseur dans cette direction), Croix = ramasser l'objet sélectionné (repère cyan) |
| D-pad ↑ / ← / ↓ / → | Potions ceinture 1 / 2 / 3 / 4 |
| L + D-pad ↓ | Marche/course (bascule) |
| R + D-pad | Potion au mercenaire |
| Start | Échap ; R + Start : échange d'armes |
| Select | Menu radial (voir plus bas) ; R + Select : Espace |
| Écran tactile | Curseur absolu ; tap bref = clic gauche |

Un losange marque l'ennemi ciblé (doré dès que le jeu confirme le survol) et un
point le lieu d'impact d'un sort au sol quand le stick droit est poussé.

**Assigner une compétence à un bouton** : ouvrir l'arbre de compétences
(menu radial), toucher l'icône, presser le bouton voulu (R + bouton pour les
emplacements 5 à 8). Dépenser un point : L ou tap.

**Panneaux ouverts** (inventaire, coffre, marchand…) : les sticks déplacent le
curseur, L ou Croix cliquent, Triangle = clic droit, Carré maintenu = Shift
(déplacement rapide), Rond ferme.

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
menu à chaque fois, il reste sur son propre geste dédié (R + Triangle).

## Clavier virtuel (R + Triangle)

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
scheme=aim        # aim (défaut, visée assistée) | mouse (schéma legacy intégral)
aim=1              # 0 = pas de choix automatique de cible (schéma aim seulement)
orbit_min=40       # px (à 600 de haut), rayon d'orbite stick à peine poussé
orbit_max=110      # px, stick à fond
range_min=6        # sous-tuiles, sort au sol stick à peine poussé
range_max=20
cone=35            # demi-angle du cône de visée, degrés
hover_h=28         # hauteur de survol par défaut, px
hud_h=60           # bande basse interdite au curseur, px
deadzone=0.25
sens=10            # curseur libre (panneaux)
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
