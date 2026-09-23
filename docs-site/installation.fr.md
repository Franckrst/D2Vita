# Installation

!!! danger "Usage personnel uniquement"
    d2vita ne distribue ni VPK ni fichiers de jeu. Cette page suppose une
    copie de Diablo II: Lord of Destruction **légitimement possédée**, sur
    du matériel Vita personnel. Voir la notice légale sur la page d'accueil.

## Fichiers du jeu

d2vita ne contient **aucun fichier Blizzard**. La 1.13c est retirée — seuls
les exécutables et MPQ 1.14d authentiques sont supportés. Les deux vont
ensemble, exactement comme une vraie installation PC, dans un sous-dossier
`1.14d` (le chargeur lit là spécifiquement, pas directement dans
`ux0:data/d2vita/`) :

```
ux0:data/d2vita/1.14d/
├── Game.exe
├── d2data.mpq      (requis)
├── d2exp.mpq       (requis, extension LoD)
├── patch_d2.mpq    (requis — patch 1.14d)
├── d2char.mpq
├── d2sfx.mpq
├── d2music.mpq
├── d2speech.mpq    (voix — le contenu change de langue, le nom jamais)
├── d2video.mpq
├── d2xmusic.mpq
├── d2xtalk.mpq
└── d2xvideo.mpq
```

C'est la liste complète. La 1.14d est un `Game.exe` monolithique unique,
tout est lié statiquement dedans — d2vita ne lit jamais `Fog.dll`,
`Storm.dll`, `D2Win.dll`, `D2Client.dll`, `D2Common.dll` ni `D2gfx.dll`
depuis le disque, même si une installation PC les fournit à côté.

Ta copie du dossier peut encore contenir ces DLLs séparées et quelques
lanceurs en trop (`Diablo II.exe`, `BNUpdate.exe`, `SystemSurvey.exe`,
`BlizzardError.exe`) — une installation PC/Mac complète en a presque toujours.
**Pas de souci : d2vita les ignore.** Il ne charge jamais une vieille DLL
1.13c à côté du monolithe 1.14d (charger ce code ancien et incompatible, c'est
ce qui cassait le rendu ou plantait le boot — d2vita refuse désormais de le
faire). Tu peux les supprimer pour faire le ménage si tu veux, mais ce n'est
pas obligatoire ; seuls `Game.exe`, les MPQ et tes fichiers `.key` servent
réellement.

À chaque démarrage, d2vita vérifie que `Game.exe` et chaque MPQ requis
ci-dessus existent bien, et note précisément lequel manque s'il en manque
un — voir [Vérifier l'installation](#verifier-linstallation) plus bas pour
savoir où cette vérification est consignée.

Il vérifie aussi que `Game.exe` est bien la **version officielle 1.14d**
(3 618 792 octets, liée le 2016-05-31 — les deux sont écrits dans le journal à
chaque démarrage). Toute autre version est refusée à l'écran avant même
d'être chargée : un monolithe 1.14a/b/c, un lanceur 1.13c, un fichier
recompressé ou relié. d2vita patche `Game.exe` à des offsets propres à la
1.14d, donc une autre version ne « marche pas à peu près » : elle échoue plus
tard, ailleurs, sans explication (un `Game.exe` 1.14b s'arrêtait sur la boîte
de dialogue de Diablo *« Error 1: Unsupported graphics mode »*, écran vide).
Si tu vois l'écran « fichiers du jeu invalides », remplace `Game.exe` par
celui de l'installeur ou du patch officiel 1.14d de Blizzard ; les MPQ
peuvent rester.

Rien d'autre à fournir : `ddraw.dll`, `checkrevision.dll` et `d2vhost.dll` sont
tous fabriqués ou simulés par d2vita lui-même, jamais lus depuis le disque.
`glide3x.dll` — le renderer Glide propre à d2vita, que le jeu charge vraiment
depuis le disque via `LoadLibrary` (D2 est lancé avec `-3dfx`) — voyage dans le
VPK (`app0:glide3x.dll`, `build_rt_boot_vpk.sh`), donc tu ne le fournis pas non
plus. Pareil pour `ux0:data/d2vita/shaders/` — les shaders GPU précompilés
voyagent déjà dans le VPK. Une copie de `glide3x.dll` ou d'un shader dans le
dossier du jeu n'est qu'une surcharge optionnelle sur console, pas quelque
chose qu'une installation normale doit remplir.

Les clés CD (optionnelles) vont un niveau au-dessus, à plat dans
`ux0:data/d2vita/keys.txt` — voir
[Jeu en ligne](en-ligne.fr.md#le-mecanisme-des-cles-cd).

## Obtenir le VPK

Télécharge `d2vita.vpk` depuis la [page des
Releases](https://github.com/Franckrst/D2Vita/releases) — chaque release
est construite et publiée automatiquement par la CI.

Construire depuis les sources (pour le développement, ou pour suivre `main`
entre deux releases) est documenté dans le [README du
dépôt](https://github.com/Franckrst/D2Vita#building) ; le résultat se trouve
alors dans `build-vita/d2vita.vpk`.

## Installer sur la console

Installer le VPK avec VitaShell, comme n'importe quel homebrew.

## Configuration (`env.txt`)

Le comportement du runtime se règle via un fichier `env.txt` déposé aux
côtés des MPQ (`ux0:data/d2vita/env.txt`), une variable par ligne. La
configuration de jeu retenue est documentée dans
[Portages natifs et gains mesurés](gains.md#config-de-jeu-retenue) —
`tools/bancs/env_jeu_glide.txt` en est la version de référence, à copier
telle quelle pour un usage normal (pas un banc de mesure). Elle inclut
`D2_SON=1`, qui active le son (DirectSound, codecs Storm portés nativement)
— sans ça, le runtime se présente comme une machine sans carte son et reste
muet.

L'environnement de **jeu** (perf/config) et l'environnement de
**diagnostic** (`D2_INPUTLOG`, sondes de profilage, etc.) sont séparés — ne
pas activer les seconds pour un usage normal, ils ont un coût.

Un réglage reste volontairement hors de ce bloc de référence :
`D2_LOCAL_ONLY=1` restreint le runtime à un serveur privé/local au lieu du
Battle.net officiel, qui est le comportement par défaut. C'est un choix de
politique réseau, pas un levier de perf, donc il ne fait pas partie de la
config ci-dessus — ajoute-le toi-même dans `env.txt` si tu le veux. Détails :
[Jouer en ligne](en-ligne.md).

### L'image

Rien à régler : une fois en partie, le jeu dessine lui-même au 960×544 de
l'écran, un texel par pixel. Les réglages ne servent qu'à défaire ça.
`D2_RES=0` remet le jeu à son 800×600 (donc les bandes latérales
reviennent) ; `D2_RES=1280x720` force une autre taille. `D2_ASPECT=etire`
étire les menus sur toute la largeur au lieu de les border — l'art des
menus est en taille fixe, il n'est jamais redessiné en 960×544.
`D2_HUDFILL=0` empêche le bandeau d'interface de combler les deux trous que
son art de 800 px laisse sur un écran de 960, et empêche aussi de combler
avec la pierre du cadre la colonne noire entre deux panneaux ouverts
(personnage + inventaire ou arbre). `D2_RES_PANNEAUX=centre` centre la
disposition 800 d'un bloc au lieu de coller les panneaux aux bords (ils se
touchent alors, avec une bande de pierre de 80 px de chaque côté) ;
`D2_RES_PANNEAUX=0` laisse au jeu ses tables d'inventaire et son cadre
(objets, clics et cadre alors décalés par rapport aux panneaux).

Le champ de vision élargi qu'offre une résolution native est un avantage en
jeu : à réserver au solo et aux serveurs privés.

## Vérifier l'installation

Chaque démarrage écrit un journal texte dans :

```
ux0:data/d2vita/boot_progress.txt
```

À lire avec le lecteur de texte intégré de VitaShell, ou à récupérer par
FTP — c'est un fichier, pas un message à l'écran. Si `Game.exe` ou un MPQ
requis (voir plus haut) manque, les toutes premières lignes nomment
précisément le fichier et le chemin complet attendu, avant que le jeu
n'abandonne. Si le jeu se ferme juste après le lancement et que la liste
des MPQ ci-dessus semble correcte, c'est la première chose à vérifier.

Un boot qui s'arrête silencieusement sans *rien* écrire dans ce fichier —
même pas ces toutes premières lignes — signifie généralement que la
console elle-même est bloquée dans un état incohérent plutôt qu'un
problème d'installation : redémarrer avant de chercher plus loin (piège
documenté du projet : un journal vide ne veut pas dire que le binaire
précédent démarrait correctement).
