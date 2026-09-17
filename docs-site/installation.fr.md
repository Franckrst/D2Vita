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
├── Fog.dll
├── Storm.dll
├── D2Win.dll
├── D2Client.dll
├── D2Common.dll
├── D2gfx.dll
├── d2data.mpq      (requis)
├── d2exp.mpq       (requis, extension LoD)
├── patch_d2.mpq    (recommandé — patch 1.14d)
├── d2char.mpq
├── d2sfx.mpq
├── d2music.mpq
├── d2xmusic.mpq
├── d2xtalk.mpq
└── d2xvideo.mpq
```

L'écran de diagnostic au démarrage indique précisément quel fichier manque
s'il en manque un.

Rien d'autre à fournir : `ddraw.dll`, `glide3x.dll`, `checkrevision.dll` et
`d2vhost.dll` sont tous fabriqués ou simulés par d2vita lui-même, jamais lus
depuis le disque. Pareil pour `ux0:data/d2vita/shaders/` — les shaders GPU
précompilés voyagent déjà dans le VPK (`build_rt_boot_vpk.sh`), ce dossier
n'est qu'une surcharge optionnelle sur console, pas quelque chose qu'une
installation normale doit remplir.

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
telle quelle pour un usage normal (pas un banc de mesure).

L'environnement de **jeu** (perf/config) et l'environnement de
**diagnostic** (`D2_INPUTLOG`, sondes de profilage, etc.) sont séparés — ne
pas activer les seconds pour un usage normal, ils ont un coût.

## Vérifier l'installation

Le premier boot affiche un écran de diagnostic listant les MPQ trouvés/
manquants avant de lancer le jeu. Un boot qui s'arrête silencieusement avant
cet écran, sans rien écrire dans le journal, signifie généralement que la
console est bloquée dans un état incohérent — redémarrer avant de chercher
plus loin (piège documenté du projet : un journal vide ne veut pas dire que
le binaire précédent démarrait correctement).
