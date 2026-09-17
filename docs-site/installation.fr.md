# Installation

!!! danger "Usage personnel uniquement"
    d2vita ne distribue ni VPK ni fichiers de jeu. Cette page suppose une
    copie de Diablo II: Lord of Destruction **légitimement possédée**, sur
    du matériel Vita personnel. Voir la notice légale sur la page d'accueil.

## Fichiers du jeu

d2vita ne contient **aucun fichier Blizzard**. Il faut fournir ses propres
MPQ depuis une installation légitime de D2 + LoD, déposés sur la carte
mémoire de la console :

```
ux0:data/d2vita/
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

## Construire le VPK

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

TARGET=vita bash third_party/winx86/build.sh   # construit le moteur (winx86 + dynarec) pour la cible Vita
bash tools/build_rt_boot_vpk.sh                # produit le VPK final (d2vita_boot.vpk)
```

Le `CMakeLists.txt` à la racine ne construit **pas** le binaire de
livraison — seulement des utilitaires de développement côté hôte
(`pe_analyze`, etc.). Le vrai pipeline de build passe par les scripts
`tools/build_*.sh` ci-dessus.

## Installer sur la console

Installer le VPK produit (`build-vita/d2vita_boot.vpk` ou équivalent selon
le script utilisé) avec VitaShell, comme n'importe quel homebrew.

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
