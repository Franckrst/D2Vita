# Manette et clavier

!!! note "Autopilote"
    Le VPK (v5+) embarque un autopilote (menus → partie) qui se coupe
    automatiquement à la **première action physique** du joueur : ne touche
    à rien et il conduit jusqu'en jeu ; touche n'importe quoi et la main est
    rendue immédiatement. Il reste actif en continu pour les tests headless.

## Contrôles par défaut

Le mapping ci-dessous est ce que remappent `controls.txt`, les couches
`R + …` / `L + …` et l'aide aux objets plus bas. Depuis la 0.1.18-beta, le
défaut intégré lie l'[action `aim`](#visee-assistee) à **L**, donc les sticks
sont en mode visée. Ce défaut est une bêta, publiée pour recueillir les
retours de la communauté. Pour retrouver l'ancien schéma, mettez `l=lclick`
dans `controls.txt` (sans autre liaison `aim`) : voir [Visée assistée](#visee-assistee).

| Entrée Vita | Action Diablo II |
|---|---|
| Stick gauche | **Déplacement seul** en jeu (n'attaque ni ne ramasse rien) ; hors partie, comportement simple des sticks |
| Stick droit | **Curseur libre** (visée, menus, panneaux, HUD), sans clic tout seul |
| Écran tactile | Curseur absolu ; tap bref = clic gauche |
| **L (maintenu)** | **Visée assistée** en jeu (agit sur la meilleure cible) ; **clic gauche** simple dans les menus, les panneaux ouverts et hors partie — remappable, voir plus bas (sert aussi de couche combo, voir plus bas) |
| **R (maintenu)** | **Clic droit** — remappable, voir plus bas (sert aussi de couche combo, voir plus bas) |
| Croix | R — bascule marche/course |
| Rond | Shift (maintenu) — attaque sur place / cast forcé |
| Carré (maintenu) | [Aide aux objets](#aide-aux-objets) — affiche les objets au sol, les parcourt, les ramasse (`square=alt` rend l'Alt simple) |
| Triangle | W — échange d'armes |
| D-pad ↑ / ← / ↓ / → | Potions ceinture 1 / 2 / 3 / 4 |
| Start | Échap (menu / fermer) |
| **Select** | **Menu radial** — remappable, voir plus bas |

Croix seule ne clique plus dans les menus : c'est **L** qui clique.

Remapper L et R ne change jamais leur second rôle : R continue d'armer
chaque combo « R + … » ci-dessous, et L sa propre couche « L + … »,
quelle que soit l'action assignée à l'appui seul.

## Couche R (R maintenu + …) et couche L (L maintenu + …)

| Combo | Action |
|---|---|
| R + Triangle | **Clavier virtuel** (ouvrir ; fermer avec Select — il s'ouvre aussi de lui-même sur les champs texte, voir plus bas) |
| R + D-pad | F1 / F2 / F3 / F4 — compétences rapides |
| L + Croix | I — inventaire |
| L + Rond | Tab — carte |
| L + Carré | O — inventaire du mercenaire |
| R + Select | Espace — fermer tous les panneaux (fixe, non remappable) |
| L + Start | Capture d'écran (fixe, non remappable) |

R + Triangle, R + Select et L + Start l'emportent toujours : ils sont
vérifiés avant tout remappage de `controls.txt`, donc leur assigner
autre chose via ce fichier n'a aucun effet (le panneau d'aide à
l'écran-titre, voir plus bas, et `controls.reference.txt` le rappellent
tous les deux). Le reste de la couche « R + … » est lié par défaut ; la
couche miroir « L + … » démarre avec trois liaisons (inventaire, carte,
inventaire du mercenaire) et toutes les autres cases « L + … » restent libres
pour `controls.txt`. Attention : L est aussi le bouton de visée, donc les
combos « L + … » déclenchent aussi l'action `aim` tant que L est enfoncé
(comme `l=lclick` cliquait pendant les combos L auparavant).

## Aide aux objets

Sur Carré par défaut (`square=items` ; `square=alt` rend l'Alt simple).
Assignez l'action `items` à un autre bouton si vous préférez, et **maintenez-la** :

- le jeu affiche les noms des objets au sol, exactement comme avec Alt ;
- la croix directionnelle saute d'un nom à l'autre (le plus proche dans la
  direction pressée), et l'objet surligné est celui que le jeu lui-même
  déclare survolé ;
- **L** (le bouton de visée) ou Croix ramasse l'objet surligné — avec `aim`
  sur L, maintenez Carré et appuyez sur L, inutile d'aller chercher Croix. Le
  curseur se place sur le nom, et le clic n'est envoyé qu'une fois que le jeu a signalé l'objet comme survolé :
  cliquer avant est lu par le jeu comme « aller là-bas » ;
- sans objet au sol, la croix directionnelle, Croix et L gardent leurs
  fonctions habituelles (potions, marche/course, visée) : maintenir le bouton ne
  fait jamais perdre une potion ;
- le stick droit déplace toujours le curseur à la main — se poser sur un
  nom le sélectionne ; le stick gauche (déplacement direct) annule tout
  ramassage en cours.

`alt` reste disponible comme simple Alt pour qui le préfère. `items`
s'assigne comme n'importe quelle action : boutons de face, croix
directionnelle, couches `r+`/`l+`, `l=`, `r=`, `select=` (éviter de
l'assigner à Croix ou à la croix directionnelle elles-mêmes, qu'elle
prend en charge tant qu'elle est maintenue).

Validée côté hôte (tests de logique) ; sur console, seulement par le jeu du
mainteneur pour l'instant.

## Visée assistée

Activée par défaut, liée à **L** (`l=aim`). Elle coûte du temps d'image : les
hooks caméra et unités qui l'alimentent sont installés dès que les liaisons
portent `aim`, et leur coût n'est pas encore mesuré. Liez `aim` à un autre
bouton — par exemple `cross=aim` — et **maintenez-le**. Pour la désactiver,
mettez `l=lclick` dans `controls.txt` (sans autre liaison `aim`) : les hooks ne
sont alors pas installés et les sticks reviennent au schéma simple ci-dessous.

Tant qu'`aim` est lié (le défaut), les **sticks** sont en mode visée, en jeu :

| Entrée Vita | Avec `aim` lié |
|---|---|
| Stick gauche | **Déplacement seul** : le personnage marche dans la direction du stick (le rayon suit l'inclinaison), sans jamais attaquer, parler ni ramasser quoi que ce soit par accident ; relâcher = arrêt net |
| Stick droit | **Curseur libre**, comme sur PC : il ne clique rien tout seul, et il atteint le HUD (ceinture, boutons de compétences) |
| Bouton `aim` (maintenu) | Agit sur la meilleure cible : votre propre cadavre à portée, sinon ce que survole le curseur, sinon l'ennemi dans un cône de ±35° autour du curseur, sinon le coffre / la porte / le portail / le PNJ le plus proche (ou, hors ville, l'ennemi le plus proche) |

Sans `aim` lié (désactivation), les sticks gardent l'ancien comportement simple
(stick gauche = déplacement direct : le curseur orbite autour du personnage
avec un clic gauche maintenu ; stick droit = souris libre, sans clic) et L est
un simple clic gauche. Hors partie (menus, sélection de personnage), le
comportement simple des sticks s'applique toujours, et le bouton `aim` y est un
simple **clic gauche** — c'est pourquoi L clique désormais dans les menus.

Un losange marque la cible hostile courante (doré une fois que le jeu confirme
le survol). On peut le masquer : `diamond=off` dans `controls.txt` (la visée
elle-même ne change pas ; `diamond=on` par défaut). La visée assistée ne fait qu'emprunter le curseur : il se place sur
la cible, maintient le clic une fois que le jeu signale le survol, puis revient
là où vous l'aviez laissé au relâchement — comme le clic d'arrêt qui termine
une marche. **Le curseur prime sur tout** : ce qu'il survole est ce sur quoi
`aim` agit, avant tout cône. Les distances se mesurent dans le monde, pas en
pixels d'écran, et une unité que le jeu refuse de survoler — un critter, un
vautour encore en l'air — est abandonnée au bout d'un moment au lieu de
capturer chaque appui (jusqu'à 32 unités de ce genre sont mémorisées : une
meute de critters ne cache plus le monstre derrière elle).

**Panneaux ouverts** (inventaire, coffre, marchand, menus de PNJ, fenêtre
d'ajout de socket de Larzuk…) : les deux sticks déplacent le curseur et `aim`
est un clic gauche. La liste des panneaux vient des drapeaux d'interface du
jeu ; la fenêtre d'ajout de socket (indice `0x0E`) a été ajoutée en
0.1.18-beta et n'a eu qu'un premier essai sur console.

Tout le reste — potions, compétences, Alt, aide aux objets, menu radial — reste
au mapping ordinaire de `controls.txt`, et les deux se combinent : le repère
cyan de l'aide aux objets montre l'objet au sol ciblé que `aim` soit lié ou non.

Clés de réglage (toutes optionnelles, `controls.txt`) :

```
orbit_min=40       # px (à 600 lignes), rayon de l'anneau de marche à faible inclinaison
orbit_max=110      # px, stick à fond
cone=35            # demi-angle du cône de visée autour du curseur, en degrés
hover_h=28         # hauteur de survol par défaut au-dessus des pieds d'une unité, px
diamond=on         # off = masque le losange de visée (la visée marche toujours)
hud_h=60           # bande basse où les clics ASSISTÉS n'entrent jamais, px (le curseur
                   # que vous pilotez, lui, y va, sinon la ceinture serait inatteignable)
reach=300          # portée de `aim` vers un coffre/une porte/un PNJ, unités du monde
```

**Non repris de l'ancienne expérience `scheme=aim`** : les emplacements de
compétence qui accrochent le lancer sur une cible (et le choix
`slot1..7` = hostile/ground/corpse), les boutons de face dédiés aux panneaux,
L maintenu = rester sur place, et le parcours du butin à la croix directionnelle
avec étiquettes natives (l'aide aux objets le remplace). Tout cela vit dans
l'historique git et la branche `manette/pad-core-port`. `scheme=` et `D2_PAD`
n'existent plus.

Validation : tests de logique hôte (pad_core), plus les essais du mainteneur sur
console (menus de PNJ, tactile, menus de l'écran-titre). Le coût en temps
d'image des hooks n'est **pas** mesuré, et le changement de défaut n'a pas
encore été testé par la communauté.

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
prend le focus, et garde sinon son propre geste dédié — R + Triangle.

## Clavier virtuel (R + Triangle)

Le clavier **s'ouvre de lui-même** quand un champ texte prend le focus — nom
de personnage, compte et mot de passe Battle.net, nom et mot de passe de
partie. La fermeture reste manuelle (Select ou la touche FERMER —
Start/Entrée valide le champ mais laisse le clavier ouvert), et un clavier
fermé ne se rouvre pas tant qu'un autre champ n'a pas pris le focus (ou que
le même ne l'a pas perdu puis repris). R + Triangle l'ouvre toujours. `D2_KBAUTO=0` dans
`ux0:data/d2vita/env.txt` coupe l'ouverture automatique. Le clavier se
dessine à 80 % d'opacité par défaut —
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
déclenchement, lui, est spécifique à d2vita : R + Triangle.

## Panneau d'aide aux contrôles (écran-titre)

Un petit onglet « Controls » se trouve dans la bande gauche de
l'écran-titre. Tap ou clic dessus ouvre un panneau plein écran listant
tous les bindings **réellement en vigueur** (défauts + ce que
`controls.txt` a changé) — D-pad ou glisser pour défiler, Rond/Start pour
fermer. Il relit `controls.txt` plutôt que de répéter cette page : il ne
peut donc jamais être en décalage avec votre propre fichier.

## Remappage sans rebuild

Fichier `ux0:data/d2vita/controls.txt`. Une copie de référence entièrement
commentée y est déposée automatiquement au tout premier démarrage du jeu
(ne remplace jamais un fichier déjà présent — supprimez le vôtre et il
revient au démarrage suivant) ; le tour d'horizon ci-dessous couvre le
même terrain.

```
cross=rclick
r+triangle=f5
l+circle=perso   # couche L (défauts : l+cross=inv, l+circle=automap, l+square=vk:0x4F)
l=lclick         # action de L seul (défaut : aim ; lclick = désactive la visée assistée)
r=none           # action de R seul (défaut : rclick) — "none" la désactive
select=inv       # action de Select seul (défaut : ouvre le menu radial)
cross=aim        # tout bouton, couche ou l=/r=/select= peut porter `aim` (voir Visée assistée)
orbit=70         # rayon du déplacement direct (px), mode simple
sens=18          # vitesse du stick droit (défaut 18)
deadzone=0.15    # défaut 0.15
anchor_y=470     # ancre verticale du personnage (pour une résolution de 1000)
diamond=off      # masque le losange de la visée assistée (défaut on)
```

**Plages acceptées** pour les clés numériques — une valeur hors plage est
refusée (le défaut reste) et signalée dans `boot_progress.txt` : `orbit`
10–400, `sens` 1–100, `deadzone` 0–0,9 (une fraction : `0.15`, pas `15`),
`anchor_y` 100–900, `orbit_min`/`orbit_max` 5–400, `cone` 5–90, `hover_h`
0–150, `hud_h` 0–300, `reach` 50–2000. En particulier `orbit=1000`, utilisé
par d'anciennes versions, est refusé : il enverrait le point de marche hors
de l'écran et plus rien ne répondrait. Un BOM UTF-8 en début de fichier
(ajouté par certains éditeurs Windows) est ignoré.

**Boutons** : `cross`/`croix`, `circle`/`rond`, `square`/`carre`,
`triangle`, `up`, `down`, `left`, `right`, `start` (préfixer `r+` pour la
couche R, `l+` pour la couche L — jamais les deux sur la même ligne).
`l=`, `r=` et `select=` sont des clés de premier niveau à part (sans
préfixe de bouton) : elles fixent ce que L, R et Select font **seuls**,
pas en combo.

**Actions** : `lclick`, `rclick`, `alt`, `items`, `aim`, `shift`, `tab`/`automap`,
`esc`/`echap`, `inv`, `perso`, `skills`, `quests`, `swap`, `space`, `run`,
`enter`, `pot1`-`pot4`, `f1`-`f8`, `vk:0xNN` (code de touche brut), `none`.

Une ligne que le jeu ne reconnaît pas — clé ou bouton inconnu, action inconnue,
nombre hors plage,
ou (`r+select=`/`l+select=`) un combo que Select n'a pas — n'est jamais
appliquée, et le dit désormais : regarder `boot_progress.txt` pour les
lignes `controls.txt ignore "..."`, une par ligne rejetée (plafonné à 8).
Avant cela, une ligne fautive ne faisait simplement rien, sans indiquer
pourquoi — le cas le plus fréquent étant un `controls.txt` copié depuis
un build qui ne reconnaît pas exactement les mêmes clés que celui
réellement lancé.

**Pas encore possible** : un seul bouton déclenchant plusieurs actions à
la suite (par exemple « ouvrir la carte et courir »).

## Diagnostic

`D2_INPUTLOG=1` dans `env.txt` journalise les mots de boutons bruts reçus —
utile pour déboguer un remappage qui ne se comporte pas comme attendu.
N'activer que pour du diagnostic, pas en jeu normal (coût de journalisation).

!!! note "Gotcha connu"
    Si la boîte de chat est ouverte, Échap la ferme d'abord (un deuxième
    appui est nécessaire pour ouvrir le menu).
