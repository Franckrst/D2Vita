# Validation à trois niveaux

Règle non négociable du projet : **aucune affirmation de correction ou de
performance n'a de valeur si elle n'a pas été vérifiée au niveau adapté.**
Trois niveaux, chacun voit quelque chose que les autres ne voient pas.

## Niveau 1 — qemu-arm (rapide, correction uniquement)

Le binaire ARMv7 réel tourne sous `qemu-arm` sur une machine de
développement Linux, sans matériel Vita. Rapide à itérer, utile pour vérifier
qu'un changement ne casse rien de déterministe (l'« oracle » : un scénario de
boot rejoué qui doit produire un verdict identique bit à bit d'une exécution
à l'autre — nombre de commutations d'ordonnanceur, nombre de fils vivants,
sortie propre, nombre d'images).

!!! danger "Les chiffres de performance sous qemu-arm ne sont JAMAIS comparables à la console"
    qemu-arm émule le CPU ARM lui-même en plus de faire tourner le
    dynarec x86→ARM par-dessus — un double niveau de traduction qui n'a
    aucun rapport avec le coût réel sur silicium. Un gain ou une régression
    mesurée sous qemu ne dit rien sur ce qui se passera sur la vraie
    console. Ce projet a documenté au moins un cas où un instrument
    mesurait un gain plausible sous qemu et un gain **nul** sur console
    (`D2_MEMINTRIN`, voir le catalogue de gains) — pas une exception,
    un rappel que qemu ne mesure que la correction.

Deux exécutions consécutives de l'oracle doivent produire un verdict
identique avant de faire confiance à quoi que ce soit d'autre — un PASS qui
ne se rejoue pas est du bruit, pas une preuve.

## Niveau 2 — Vita3K (le même eboot ARM, sur PC)

[Vita3K](https://vita3k.org/) exécute le **même eboot ARM** que celui
installé sur la vraie console — pas une recompilation, le binaire de
livraison — piloté par un `cmd.txt`. C'est le banc de reproduction local :
plus proche de la réalité que qemu (vraie ABI ARM, vrai comportement des
bibliothèques Vita émulées), mais toujours pas une mesure de performance
valide — Vita3K n'a pas le même profil de coût que le matériel réel non
plus.

## Niveau 3 — console réelle (seule source de vérité pour la performance)

Seule la vraie PS Vita fait foi pour toute affirmation de gain ou de perte
de performance. Avec une contrainte mesurée et documentée : **la dispersion
d'une exécution à l'autre, à binaire strictement identique, est d'environ
8 %** (bruit témoin-témoin ramené de 8,6 % à 1,6 % seulement après
correction d'un biais de banc — un panneau d'inventaire ouvert sur un
personnage mort, et une métrique en secondes de mur plutôt qu'en images).
Sous ce plancher de bruit, rien n'est mesurable isolément :
il faut soit grouper plusieurs mesures A/B/A entrelacées sur le même binaire,
soit chercher une pente plutôt qu'une valeur absolue.

Conséquences pratiques qui reviennent tout le long du catalogue de gains :

- Un chiffre publié précise toujours **à quel niveau** il a été mesuré —
  jamais présenté comme un résultat console s'il vient de qemu.
- On dit « supprime du travail invité » plutôt que « plus rapide » tant que
  le niveau 3 n'a pas confirmé le gain — un aller-retour de trap en moins
  n'est pas automatiquement une image par seconde en plus.
- Une valeur absolue ne traverse jamais une campagne de mesure : le binaire
  change, la console dérive thermiquement (1,5-10 %). Seuls les écarts
  témoin/variante **d'une même campagne entrelacée** ont un sens.
- Un compteur logiciel ne voit pas une texture rendue fausse : sur le
  chemin GPU, la fidélité se prouve par capture d'image, pas par des
  compteurs.

## Pourquoi les trois, pas juste le matériel

Le matériel est lent à itérer (accès physique à la console, pas de
capture d'écran/mémoire aussi commode qu'en émulation) et bruité (8 %). Sans
qemu-arm pour filtrer les régressions de correction en amont, chaque
itération coûterait un aller-retour matériel pour découvrir un bug trivial.
Sans Vita3K, la première fois qu'un changement rencontre le vrai eboot ARM
serait sur la console elle-même. Les trois niveaux existent parce qu'aucun
seul ne suffit.

## Reproduire le niveau 2 en local (Vita3K headless)

Vita3K exécute le VPK livré (`build-vita/d2vita.vpk`) via `-r TITLE_ID`, sans
interface graphique interactive — utile pour valider un build sans accès
physique à la console.

**Mise en place, une fois** : extraire l'AppImage Vita3K, lancer l'appli une
première fois pour installer le firmware Vita requis (« Download Pre-Install
Firmware » + « Download Firmware Font Package »), puis dans
`~/.config/Vita3K/config.yml` désactiver l'écran d'accueil
(`initial-setup: false`, `show-welcome: false`) et choisir le rendu OpenGL
(`backend-renderer: OpenGL` — llvmpipe expose de l'OpenGL 4.5, suffisant).

**Installer et lancer un VPK** (un VPK est un zip) :

```bash
V=~/.local/share/Vita3K/Vita3K/ux0/app/DVITA0001
mkdir -p "$V" && 7z x -y -o"$V" build-vita/d2vita.vpk

D=~/.local/share/Vita3K/Vita3K/ux0/data/d2vita
mkdir -p "$D"
for f in ~/d2-vita-refs/<version>/*.[mM][pP][qQ]; do ln -sf "$f" "$D/$(basename "$f")"; done

export APPDIR=~/tools/vita3k/squashfs-root APPIMAGE=fake DISPLAY=:0
export PATH="$APPDIR/usr/bin:$PATH" LD_LIBRARY_PATH="$APPDIR/usr/lib" QT_PLUGIN_PATH="$APPDIR/usr/plugins"
$APPDIR/usr/bin/Vita3K -l 0 -r DVITA0001
```

Journal : `~/.cache/Vita3K/vita3k.log`.

Pièges rencontrés, dans l'ordre où ils coûtent le plus de temps :

- **`AppRun.wrapped` ne fait rien sans `$APPIMAGE`** — exporter
  `APPIMAGE=fake`, ou appeler directement `usr/bin/Vita3K`.
- **Un seul processus Vita3K à la fois** : deux instances entrelacent leurs
  écritures dans `boot_progress.txt` (chronologie illisible). Tuer par nom
  exact (`pkill -x Vita3K`) — `pkill -f` matche aussi la ligne de commande du
  shell courant.
- **Le journal ne se remplit qu'à l'arrêt propre** : un `pkill -9` le
  tronque ; préférer `SIGTERM` (le défaut de `pkill`), avec un court délai.
  Vita3K ignore parfois SIGTERM quand il est bloqué en headless — encadrer
  avec `timeout -k 10 90 …` pour garantir un SIGKILL de secours.
- **Les journaux peuvent remplir le disque en quelques minutes** (10+ Go) :
  un fil invité bloqué dans une boucle de faute spamme des lignes `|E|`
  quel que soit le niveau configuré. Garder `log-level: 4` (erreurs
  seulement) dans `config.yml` — un run ponctuel en `-l 0` (TRACE) **persiste**
  ce réglage pour tous les runs suivants.
- **Sensibilité à la casse ext4** : les noms de fichiers exacts du jeu
  (`patch_d2.mpq` vs `Patch_D2.mpq` sur disque) passent à travers le système
  de fichiers virtuel de Vita3K sans le repli insensible à la casse de
  `rt_boot`. Créer des alias en minuscules pour chaque fichier de données.
  Le vrai exFAT d'une console n'a pas ce problème — c'est un artefact de
  l'émulateur, pas du portage.
- **Les écritures ne sont durables qu'au `fclose`** — pour tracer un gel pas
  à pas, écrire un fichier marqueur par étape et le fermer immédiatement ;
  un simple `fflush` peut ne jamais atteindre l'hôte.
