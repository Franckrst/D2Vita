# Manette v2 — ciblage assisté (phase 1) : conception

Date : 2026-09-19. Branche : `wt/manette` (worktree `/home/doudou/repos/wt-manette`).
Recherche d'origine : `/home/doudou/sessions/doudou/d2vita-manette-recherche-2026-09-19.md`
(D2R, PD2, DevilutionX, mods LoD, offsets 1.14d publics).

## 1. Objectif

Remplacer l'émulation souris actuelle (stick gauche = curseur en orbite + clic
maintenu, compétences par F1-F4 puis clic droit) par le schéma que les joueurs
de Project Diablo 2 jugent supérieur à D2R : **déplacement seul au stick
gauche, visée au stick droit, compétences en lancer immédiat sur les boutons,
cible choisie automatiquement**. Livrable de la phase 1 : une **snapshot VPK de
test** jouable en solo sur console, le nouveau schéma actif par défaut, l'ancien
schéma conservé en repli (`scheme=mouse` dans `controls.txt`).

Contrainte fondatrice inchangée (CLAUDE.md) : **on n'écrit jamais dans la
mémoire du jeu, on n'appelle aucune fonction du jeu**. Le runtime lit l'état
invité (lecture seule, dans des crochets déjà en place) et ne synthétise que des
messages Win32 (souris, clavier). C'est le jeu qui fait son hit-test, son
pathfinding et ses paquets réseau. Rien d'observable par Warden, online intact.

## 2. Hors périmètre (phases suivantes)

- Phase 2 : butin à lettres (R+L maintenu → lettres sur les 4 objets les plus
  proches), navigation d'objet en objet dans l'inventaire sans tactile (PS TV),
  potion au mercenaire, secteurs radiaux supplémentaires (échange d'armes,
  marche/course), ouverture automatique du clavier virtuel sur le secteur Chat.
- Phase 3 : boutons étendus PS TV (L2/R2/L3/R3), tactile arrière, icônes de
  boutons sur la barre de compétences, réglages par classe, menu de réglages.
- Jamais (sauf décision explicite) : appel de `ClickMap`/`GetSelectedUnit`
  du jeu, paquets forgés, écriture mémoire.

## 3. Ce que le runtime sait déjà lire (vérifié au désassemblage de Game.exe 1.14d)

Base `Game.exe` = `g_d2base` (0x400000, jamais relogé). RVA ci-dessous.

| Donnée | Où | Vérification |
|---|---|---|
| Dessin d'une unité | crochet d'ENTRÉE `Game+0xdc7b0`, `ECX = UnitAny*` (déjà posé par `ringtag_hooks_install`, `src/runtime/phase_hooks.cpp`) | prouvé console (replay60) |
| Caméra / début du monde | crochet de SORTIE `Game+0x5b440` (idem) : `[0x3a520c]` = ViewportX, `[0x3a5208]` = ViewportY, `[0x3a6a70]` = UnitAny* joueur | prouvé console |
| UnitAny | `+0` type (0 joueur, 1 monstre, 2 objet, 3 missile, 4 item, 5 tuile), `+4` classe (txtFileNo), `+0xC` id, `+0x10` mode, `+0x14` pMonsterData, `+0x2C` pPath, `+0x94` dwOwnerType, `+0x98` dwOwnerId | +0/+4/+0xC/+0x10/+0x2C prouvés ; +0x94/+0x98 = struct D2BS 1.13c/1.14d, **à confirmer par journal** |
| Position fine 16.16 (ce que `GetUnitX/Y` renvoient) | types 0/1/3 : `[pPath+8]`, `[pPath+0xC]` ; types 2/4/5 : `[pPath+4]`, `[pPath+8]` | désassemblage `0x620650` → `0x6489c0` (`[+8]`) et branche statique (`[+4]`) |
| Unité sous le curseur (résultat du hit-test du jeu) | `[0x3a6a94]` ≠ 0 ⇒ une unité est survolée ; `[0x3a6a78]` = id ; `[0x3a6a8c]` = type | désassemblage de `GetSelectedUnit` `0x467a10` (`ecx=id, edx=type` vers `0x463990`) |
| Panneaux ouverts | tableau `UiVar[38]` à `0x3a27c0` (dword par index) ; 1 inventaire, 2 feuille perso, 4 arbre de compétences, 8 menu PNJ, 9 menu Échap, 0x0A automap, 0x0C boutique, 0x14 waypoint, 0x19 coffre, 0x1A cube, 0x21 aide, 0x24 mercenaire | désassemblage `GetUiVar` `0x4538d0` (`mov eax,[0x7a27c0+ecx*4]`) ; index = kolbot `sdk.js` ; AutomapOn mir = `0x3a27e8` = index 0x0A ✓ |
| Niveau courant (ville ?) | `pPath+0x1C` → Room1 ; `Room1+0x10` → Room2 ; `Room2+0x58` → Level ; `Level+0x1F8` = dwLevelNo | struct D2BS, **à confirmer par journal** (attendu 1 au camp des Rogues) |
| Projection monde → écran | `sx = (fx − fy)·16/65536 − ViewportX`, `sy = (fx + fy)·8/65536 − ViewportY` | la partie delta est prouvée (replay60, erreur < 1 px) ; la soustraction du viewport = recette D2BS `ClickMap`, **à confirmer par journal** (le joueur doit se projeter à une position écran constante) |

Tout ce qui est marqué « à confirmer » a une **tâche de journalisation dédiée
et un repli** dans le plan ; aucun de ces points ne bloque la snapshot.

## 4. Détection du contexte (chaque tick d'entrée, 30 Hz)

- `inGame` : le crochet caméra a tiré pendant la dernière image avec un joueur
  non nul (même signal que replay60 « veille hors jeu »).
- `panelOpen` : un des `UiVar` {1, 2, 4, 8, 9, 0x0C, 0x14, 0x19, 0x1A, 0x21,
  0x24} est non nul. L'automap (0x0A) n'est **pas** un panneau.
- `skillTree` : `UiVar[4]` non nul.
- `town` : `levelNo ∈ {1, 40, 75, 103, 109}` (ou `levelNo == 0` = inconnu ⇒
  traité comme ville, donc pas de ciblage hostile).
- Hors `inGame` (menus, chargement, sélection de perso) : **schéma legacy
  inchangé**, code existant.

## 5. Mapping phase 1 (Vita : 4 faces, L, R, D-pad, Start, Select, 2 sticks, tactile)

### 5.1 Monde (`inGame && !panelOpen`)

| Entrée | Action |
|---|---|
| Stick gauche | **Déplacement seul** : clic gauche maintenu sur un point en orbite autour du perso, rayon 40→110 px selon l'inclinaison, point **déplacé hors des sprites** (aucune attaque, aucun dialogue, aucun ramassage par accident). Relâcher = clic aux pieds du perso ⇒ **arrêt net**. |
| Stick droit | **Visée** : cône de ±35° pour choisir la cible ; pour un sort au sol, distance 6→20 sous-tuiles selon l'inclinaison. Ne déplace plus le curseur. |
| Croix / Rond / Carré / Triangle | **Compétences 1-4** en lancer immédiat : touche F1-F4 puis clic droit sur la cible (ou au point de visée). Maintenu = répétition (comportement natif du clic droit maintenu), la cible est suivie. |
| R maintenu + faces | Compétences 5-8 (F5-F8). |
| L | **Interaction** : clic gauche sur l'objet au sol le plus proche (≤ 220 px), sinon l'objet/PNJ interactif le plus proche (≤ 160 px), sinon l'ennemi ciblé (attaque avec la compétence gauche). Maintenu = suivi. Rien à portée = rien. |
| R + L (R d'abord) | Alt maintenu (étiquettes des objets au sol). |
| D-pad | Potions ceinture 1-4 (inchangé). |
| R + D-pad | Potion au mercenaire (Shift + 1-4). |
| Start | Échap. R + Start : W (échange d'armes). |
| Select | Menu radial (inchangé). R + Select : Espace (inchangé). |
| L + Start | Capture d'écran (inchangé, prioritaire). L + Haut : marqueur de ralentissement (inchangé). |
| Tactile avant | Curseur absolu + tap = clic (inchangé). |
| Non mappés par défaut | marche/course (`run`), attaque sur place (`shift`) — disponibles dans `controls.txt`. |

Une seule compétence à la fois : un deuxième bouton pressé pendant un maintien
est ignoré. Pendant un lancer ou une interaction maintenue, le déplacement est
suspendu (clic gauche relâché) et reprend au relâchement si le stick est
toujours poussé.

### 5.2 Choix de la cible hostile (`aim=1`, défaut)

Candidats = unités **dessinées cette image** (crochet dessin), `type == 1`,
`mode ∉ {0 (mort en cours), 12 (cadavre)}`, `ownerId ≠ id du joueur` (invocations),
classe ∉ {271, 338, 359, 560} (mercenaires), et **jamais en ville**.
Score en espace écran depuis la position du joueur :

- stick droit poussé : cône ±35° autour de sa direction, ≤ 500 px,
  score = distance + 300·(1 − cos(angle)) ;
- stick droit au repos : la plus proche ≤ 420 px.

Hystérésis : la cible courante est gardée tant qu'elle qualifie et que son
score ≤ 1,25 × meilleur + 20 px. Aide à la visée débrayable (`aim=0`) : les
compétences partent alors au point de visée du stick droit (ou devant le perso
dans la dernière direction de marche, distance `range_min`).

### 5.3 Boucle fermée « le jeu survole-t-il bien ma cible ? »

Le clic droit part **sans attendre** au point `(sx, sy − h)` (h = hauteur de
survol apprise par classe, défaut 28 px). Au tick suivant, on lit l'unité
survolée par le jeu (`[0x3a6a94]/[0x3a6a78]/[0x3a6a8c]`) :

- égale à la cible ⇒ **vérifié**, `h` mémorisé pour cette classe (table
  d'apprentissage en RAM, remise à zéro au boot) ;
- sinon on essaie, un par tick, h ∈ {14, 44, 64, 90} en gardant le bouton
  maintenu ; après 4 essais on reste au meilleur h connu.

Même mécanisme pour L (objets : point `(sx, sy − 6)`, objets interactifs 20 px).
Aucune latence ajoutée au premier lancer ; la précision s'améliore à l'usage.

### 5.4 Panneaux (`inGame && panelOpen`)

| Entrée | Action |
|---|---|
| Stick gauche / droit | Curseur libre (relatif, `sens`). |
| L, Croix | Clic gauche au curseur (maintenu = glisser). |
| Triangle | Clic droit au curseur (utiliser, équiper). |
| Carré maintenu | Shift maintenu (déplacement rapide avec le clic). |
| Rond | Échap (fermer). |
| **Arbre de compétences ouvert** | Faces = F1-F4, R + faces = F5-F8 **sur l'icône survolée** (assigner une compétence à un bouton) ; dépenser un point = L ou tap. |
| D-pad, R + D-pad, Start, Select, R + Start/Select | Comme en monde. |
| Tactile | Inchangé. |

Changement de contexte avec un bouton maintenu (panneau ouvert pendant un
lancer, sortie de partie) ⇒ tout ce qui est maintenu est relâché proprement
(rup/lup/keyup) avant de changer de mode.

### 5.5 Réticule

L'overlay hôte (`d2vita_overlay`, 960×544, même étirement que la présentation :
`X·SCR_W/g_game_w`, `Y·SCR_H/g_game_h`) dessine un losange sur la cible hostile
courante (doré si vérifié, blanc sinon) et un point au point de visée quand le
stick droit est poussé. Rien n'est dessiné dans l'image du jeu (le FBDUMP reste
pur). Le curseur du jeu lui-même se pose sur la cible pendant un lancer.

## 6. Architecture (quatre unités, frontières nettes)

| Unité | Fichiers | Rôle | Dépend de |
|---|---|---|---|
| `padst` — état par image | `src/runtime/pad_state.h/.cpp` (nouveau) | Struct hôte remplie par les crochets : caméra, joueur, viewport, niveau, unité survolée, `UiVar[38]`, liste des unités dessinées (≤ 128). Knob `padst::on()`. | rien (C++ pur) |
| crochets | `src/runtime/phase_hooks.cpp` (modifié) | Les crochets unité/caméra existants alimentent `padst` quand `padst::on()`, et sont armés si replay60 **ou** pad est actif. La sortie UNITEND (ring) n'est redirigée que pour replay60. | engine (Bridge/Cpu) |
| `pad` — cœur pur | `src/platform/pad_core.h/.cpp` (nouveau) | Projection, choix de cible, point d'orbite, point de visée, table d'apprentissage du survol, **machine à états du schéma** (entrées → liste d'actions à injecter). Aucun include Vita ni engine ⇒ **testable sur PC**. | rien |
| glue Vita | `src/platform/vita_present.cpp` (modifié) | Lit sceCtrl/tactile, construit les entrées, appelle `pad::Scheme::tick`, émet via `d2vita_inject`, dessine le réticule. Branche `scheme=mouse` = code actuel intact. | padst, pad, VitaSDK |
| manette virtuelle | `src/runtime/scripted_input.cpp` (modifié) | Commandes `padb:<masque>:0` et `pada:<lx\|ly<<8>:<rx\|ry<<8>` sur le canal `D2CMDFILE`, ORées avec la manette physique : pilotage à distance (console via `tools/vita_drive.sh`, Vita3K) sans toucher la console. | rien |
| tests PC | `tests/pad/pad_core_test.cpp` + cible CMake `pad_core_test` | Projection, sélection, orbite, séquences d'actions du schéma. | pad |

Même fil d'exécution partout : les crochets tournent sur le fil de rendu du
jeu, et le tick d'entrée est appelé depuis la pompe de messages du même fil ⇒
`padst` est lu et écrit sans course. Le tick lit la liste « en cours de
collecte » de l'image courante (réinitialisée à la sortie du crochet caméra).

Sortie de chaque tick : une liste d'actions `{MOVE, LDOWN, LUP, RDOWN, RUP,
CLICK, KEYDOWN, KEYUP, KEY}` avec coordonnées jeu, injectées dans l'ordre via la
grammaire existante de `scripted_input.cpp`.

## 7. Configuration (`ux0:data/d2vita/controls.txt`, lu au premier tick)

```
scheme=aim        # aim (défaut) | mouse (schéma legacy intégral)
aim=1             # 0 = pas de choix automatique de cible
orbit_min=40      # px (à 600 de haut), rayon d'orbite stick à peine poussé
orbit_max=110     # px, stick à fond
range_min=6       # sous-tuiles, sort au sol stick à peine poussé
range_max=20
cone=35           # demi-angle du cône de visée, degrés
hover_h=28        # hauteur de survol par défaut, px
hud_h=60          # bande basse interdite au curseur, px
deadzone=0.25
sens=10           # curseur libre (panneaux)
```

`D2_PAD=0|1` dans `env.txt` a priorité sur `scheme=` (A/B sans toucher au
fichier). Les remaps `cross=…` existants ne s'appliquent qu'au schéma `mouse`.

## 8. Diagnostic

`D2_PADLOG=1` (env.txt) journalise dans `boot_progress.txt`, plafonné (une ligne
par tick maximum, 2 000 lignes par session) : changement de cible (id, classe,
distance, h vérifié ou non), changement de `UiVar` (index, valeur), position
écran projetée du joueur pendant les 60 premières images en jeu, changement de
niveau, champs owner/monFlags de chaque nouvelle classe rencontrée. Le mode
`padb/pada` du `D2CMDFILE` permet de rejouer un scénario sans manette.

## 9. Validation (échelle du projet)

1. **PC** : `pad_core_test` vert (projection, cône, hystérésis, évitement des
   sprites, arrêt au relâchement, séquences d'actions du schéma, boucle de
   survol). Chaque tâche du plan passe par un test avant le code.
2. **qemu-arm** : `tools/rt_boot_arm_check.sh` PASS avec `D2_PAD=1` (les
   crochets armés ne cassent pas le boot déterministe ; hors jeu ils sont
   inertes).
3. **Vita3K** : boot du VPK, arrivée en jeu par l'autopilote, puis `padb/pada`
   via `D2CMDFILE` : le journal montre `inGame`, la projection du joueur stable,
   une cible choisie, un survol vérifié.
4. **Console** (seule source de vérité, `hardware-ab-bench`) :
   - camp des Rogues : marcher dans 8 directions sans parler à un PNJ ni
     ramasser ; relâcher arrête en moins d'un pas ;
   - Blood Moor : Croix attaque le monstre le plus proche sans toucher le stick
     droit ; le stick droit vers un autre monstre déplace le réticule et la
     cible ; Croix maintenue répète ;
   - assigner une compétence : radial → arbre, tap sur l'icône, Rond ⇒ Rond
     lance cette compétence en monde ;
   - L : ramasse un objet au sol, ouvre un coffre, parle à un PNJ en ville ;
   - inventaire ouvert : tap/glisser, Rond ferme, potions D-pad actives ;
   - `scheme=mouse` : comportement strictement identique à la 0.1.6 ;
   - images/s (overlay `D2VITA_FPS`) : pas d'écart visible entre `scheme=aim`
     et `scheme=mouse` sur le même eboot (le banc en jeu disperse de 8 %, on ne
     cherche qu'une régression grossière).

Une snapshot est « bonne » quand les 6 points console passent ; l'online n'est
pas requis pour la snapshot (aucun chemin réseau touché ; le F-key à chaque
lancer sera observé une fois en ligne en phase 2).

## 10. Risques et replis

| Risque | Repli |
|---|---|
| Projection écran fausse (viewport pas au sens attendu) | journal des 60 premières images ; mode `proj=delta` (position joueur = ancre écran mesurée + delta prouvé par replay60) |
| Champs owner/monFlags différents en 1.14d | journal par classe ; repli = exclusion par classe (mercs) + règle « jamais en ville » |
| Le survol du jeu n'est pas recalculé à chaque image | la vérification arrive un tick plus tard ; le lancer est déjà parti au point par défaut, on ne perd rien |
| Coût des crochets (≈ 100 pièges/image) | mesuré à 1,29 µs par appel ⇒ < 0,5 % d'une image de 40 ms ; `scheme=mouse` désarme tout |
| Le F-key à chaque lancer spamme le serveur en ligne | hors périmètre snapshot ; phase 2 : n'envoyer que si le slot change (lecture `pInfo+0xC` du joueur) |
| Panneau non détecté (chat, dialogue PNJ sans UiVar) | journal des UiVar ; le tactile reste toujours fonctionnel |

## 11. Résultat snapshot 1 (console, 2026-09-19)

**Bilan : projection et déplacement PROUVÉS sur console réelle après un bug bloquant corrigé en cours de route ; ciblage hostile encore bloqué par un second bug, non corrigé, documenté ci-dessous.**

### Bug bloquant trouvé et corrigé (commit `d190222`)

Le premier passage sur Vita3K (agent dédié) a montré `fine=(-1,1)` (quasi zéro) au lieu
d'une position plausible, et un déplacement au stick gauche sans aucun effet visible sur
2,5 s. Root cause (méthode `systematic-debugging`, reproduite en qemu déterministe via
`tools/rt_gameplay_arm_check.sh`, personnage VITA au camp) : le crochet de SORTIE caméra
lisait `pPath+8/+0xC` (convention « GetUnitX/Y » de la table §3, correcte pour les unités
lues au dessin) au lieu de `pPath+0/+4` — le pointeur que l'opcode `D2GR_OP_CAMERA`
existant (replay60, éprouvé sur console depuis des mois) capture déjà pour le même usage.
`+8/+0xC` n'est vraisemblablement rafraîchi que par la passe de dessin par-unité, qui
tourne APRÈS le crochet caméra (« juste avant le monde ») : le lire à cet instant renvoie
zéro ou la valeur de l'image précédente. Confirmé en qemu : `p04=(370507776,361988096)`
(~4874,~4228 sous-tuiles, plausible) contre `p8c=(2080,89416)` (~0,~1, incohérent), au
même tick. Correctif : réutiliser directement les mots déjà lus pour la caméra au lieu de
relire `pPath+8/+0xC`. Aucune régression (`rt_boot_arm_check.sh` et
`rt_gameplay_arm_check.sh` repassent verts).

### Vita3K (agent dédié, préfixe isolé `/tmp/vita3k_manette`, TITLE=DTWO00077)

Avant le correctif : projection fausse, aucun mouvement observable (captures avant/après
identiques au pixel après 2,5 s de stick plein droit).

Après le correctif (re-validation, même VPK reconstruit) :
- `input: schema v2 (visee assistee) actif` : présent.
- `pad: joueur ecran=(400,284) view=(-18240,77652) fine=(4313,5428)` : stable sur 60/60
  lignes, valeur plausible (centre de l'image 800×600).
- Déplacement confirmé deux fois par captures avant/après : le stick plein droit a
  rapproché le personnage assez pour déclencher le message de proximité de Warriv ; le
  stick plein bas a fait défiler entièrement le décor du camp (tentes/caisses hors
  cadre, nouveau terrain visible).
- `pad: niveau=0 ville=1` : présent (voir bug ouvert ci-dessous).
- Cible hostile (`pad: cible`) : non testée, le personnage n'a pas quitté la ville lors
  des essais (recherche de la sortie du camp infructueuse en un nombre raisonnable
  d'essais).

### Console réelle (10.113.1.159, DTWO00001, build `d190222`)

Procédure : `env.txt` absent avant test (confirmé, rien à restaurer sur ce fichier) ;
créé avec `D2_PADLOG=1` + `D2CMDFILE=ux0:data/d2vita/d2cmd.txt`, puis supprimé après le
test (état d'origine restauré). Sauvegarde `VITA.d2s`/`VITA.key` (identiques à celles du
banc Vita3K partagé, même format de sauvegarde) copiées sur la console pour éviter de
rejouer la création de personnage sur un flux de test — dossier `save/` vide au départ,
rien écrasé.

**Constat méthodologique important pour de futurs tests à distance** : le pipeline
`D2CMDFILE` (upload FTP → lecture par le jeu) a montré une latence systématique
d'environ 60 s entre l'écriture du fichier et son effet observable en jeu — largement
au-dessus de la période de scrutation nominale (`D2_CMDMS`, 500 ms par défaut). Cause non
investiguée (possiblement la couche FTP de VitaShell/le rouvre-fichier de `cmd_poll`
après une série de sondages sans nouveauté). Ceci a initialement fait paraître un test
d'interaction (L) en échec — une capture prise trop tôt ne montrait pas encore le menu
PNJ — alors que le journal, relu plus tard, montre `pad: uivar[8]=1` (menu PNJ de Warriv
ouvert) survenant bien après l'action, à l'heure attendue compte tenu du délai. Ne pas
conclure d'un test négatif sur console via ce canal sans attendre largement plus que le
délai naïvement attendu.

Résultats obtenus, avec preuve :
1. **Hooks et schéma actifs** : `ringtag: … pad=oui` et `input: schema v2 (visee
   assistee) actif` au boot. PASS.
2. **Projection joueur** : `pad: joueur ecran=(400,284) fine=(4313,5428)` — identique au
   run Vita3K post-correctif, à l'unité près. PASS.
3. **Déplacement (stick gauche)** : confirmé par capture d'écran réelle (`L+Start`) — le
   personnage est visible juste à côté de Warriv avec son infobulle « WARRIV / TALK /
   CANCEL » après un déplacement stick-droit-tenu de 3 s, exactement la même preuve
   comportementale que sur Vita3K. PASS sur la direction testée ; les 7 autres directions
   et le test précis « relâcher arrête en moins d'un pas » reposent sur la couverture PC
   (92 tests, dont `test_orbit`/`test_scheme_move`) plutôt que sur une capture console
   dédiée — non re-vérifiés individuellement sur console faute de temps.
4. **L (interagir)** : `pad: uivar[8]=1` confirme l'ouverture du menu PNJ de Warriv après
   un appui L maintenu. PASS (preuve journal ; la capture d'écran associée a été prise
   avant que l'effet ne soit visible, à cause du délai `D2CMDFILE` ci-dessus).
5. **Croix → attaque au corps-à-corps le plus proche (Blood Moor)** : NON TESTÉ — la
   sortie du camp n'a pas été atteinte pendant les essais (Vita3K et console). De toute
   façon **bloqué par construction tant que le bug niveau ci-dessous n'est pas corrigé** :
   `pad_is_town()` traite tout comme ville quand `niveau=0`, donc `hostile` n'est jamais
   vrai nulle part.
6. **Radial → arbre de compétences, inventaire, `scheme=mouse`, comparaison images/s** :
   non re-testés indépendamment sur console cette session (contrainte de temps face à la
   latence `D2CMDFILE` ~60 s/action). Couverts par les tests PC (`test_scheme_panel_and_leave`
   pour le radial/panneaux/arbre) ; `scheme=mouse` emprunte un chemin de code legacy
   **non modifié** par ce plan (seule une porte de sortie anticipée a été ajoutée avant
   lui), donc à risque structurellement bas mais pas vérifié empiriquement ici.

### Bug niveau — CORRIGÉ et confirmé sur console (2026-09-19, après `bdcbbad`)

**Chaîne de niveau** (`pPath+0x1C → Room1 → +0x10 Room2 → +0x58 Level → dwLevelNo`) :
`lvl=0` au lieu de `1` au camp des Rogues malgré des pointeurs intermédiaires non nuls.
Root-causé au désassemblage (capstone sur `Game.exe` 1.14d, pas un balayage mémoire) :

- **`Level+0x1F8` (table §3, sourcée « struct D2BS », jamais confirmée par
  désassemblage pour 1.14d) n'existe pas dans ce binaire.** Un passage exhaustif de
  toutes les instructions `.text` (mov/cmp/lea/movzx/test, adressage direct ET indexé)
  référençant ce déplacement donne **zéro résultat**.
- **`r1`/`r2` (Room1/Room2) sont prouvés dynamiquement** : 3 unités différentes (donc
  `r1` et `r2` différents — tuiles de salle distinctes) convergent toutes sur le même
  pointeur `r2+0x58`, exactement la convergence attendue de « plusieurs salles, un seul
  niveau ». La chaîne jusque-là est correcte.
- **Trois candidats plausibles trouvés dans la même structure** (`+0x1C0`, `+0x1D0`,
  `+0x1DC`), tous lisant `1` de façon stable au camp des Rogues sur plusieurs runs qemu
  indépendants — indiscernables par une seule mesure en ville.
- **Départagés le 19/09 par un vrai test terrain** : diagnostic `D2_PADLOG=1` déployé
  sur la console réelle, l'utilisateur a joué manuellement — sortie du camp des Rogues
  vers les Prairies de Sang, puis retour en ville. Résultat sans ambiguïté :

  | | camp (ville) | Prairies de Sang | retour au camp |
  |---|---|---|---|
  | `+0x1C0` | 1 | **2** | 1 |
  | `+0x1D0` | 1 | **2** | 1 |
  | `+0x1DC` | 1 | 1 | 1 |

  `+0x1C0` et `+0x1D0` suivent tous deux la vraie numérotation D2 (1 = camp des Rogues,
  2 = Prairies de Sang) ; `+0x1DC` ne bouge jamais — faux candidat. **`+0x1C0` est le
  correctif livré** (déjà en place avant ce test ; `+0x1D0` aurait été équivalent).
  `ville` bascule correctement à `0` hors ville. Le diagnostic des 3 candidats a été
  retiré du code une fois la question tranchée (plus nécessaire).

**Conséquence** : `pad_is_town()` fonctionne partout, le ciblage hostile automatique
(§5.2) s'engage hors ville comme prévu. Aucune régression (`rt_boot_arm_check.sh`
repasse vert après le nettoyage du diagnostic).
