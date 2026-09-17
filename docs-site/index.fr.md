# d2vita

**Portage de Diablo II: Lord of Destruction sur PS Vita — code source et VPK publics.**

!!! danger "Statut légal — à lire avant tout"
    Chaque version publique (code source comme VPK) respecte un socle fixe
    avant publication : pas de fichier ni de donnée Blizzard dans le VPK,
    licences des composants tiers recensées, hygiène des secrets. Les
    joueurs fournissent leur propre copie de D2/LoD légitimement possédée ;
    ce dépôt n'en distribue aucune.

    Ce projet exécute le binaire Blizzard d'origine (voir
    [Architecture](architecture.md)) plutôt que de le décompiler ou de le
    réimplémenter — les stratégies antérieures qui en décompilaient des
    parties ont été abandonnées avant de rien livrer. Aucun fichier ni asset
    Blizzard n'est distribué par ce dépôt ou par ce site.

## Ce que c'est

d2vita fait tourner les **vrais binaires Windows** de Diablo II: Lord of
Destruction (patch 1.14d) sur PS Vita, via un moteur d'exécution x86→ARMv7 :
pas une réimplémentation du jeu, une exécution directe de son code d'origine
sous traduction dynamique, avec seulement les dépendances Windows
(KERNEL32, GDI32, USER32, DirectDraw/DirectSound...) remplacées par des
implémentations natives ARM.

Stratégie actuelle : **faire tourner Diablo, pas le réécrire.** Deux
stratégies antérieures
(réimplémentation native via D2MOO, puis levée statique AOT du code x86 vers
C) ont été abandonnées — la seconde n'avait atteint que ~3,4 % du code de
D2Common après un travail conséquent.

## Le moteur : winx86

Le moteur générique (chargeur PE32, dynarec dérivé de Box86, ordonnanceurs,
pont d'imports Win32) vit dans un dépôt séparé,
[winx86](https://winx86-136891.gitlab.io/), consommé ici comme
sous-module git (`third_party/winx86`). Ce qui est spécifique à Diablo II
reste dans ce dépôt : les hooks de performance posés sur des adresses
précises du binaire du jeu, le rendu Glide, et les shims Win32 dont le corps
dépend réellement du jeu.

Les shims génériques, eux, ont migré vers le moteur au fil d'un tri fonction
par fonction — environ un tiers des clés aujourd'hui. Voir
[Architecture](architecture.md) pour le principe de cette frontière, et
[Liste des shims Win32](shims.md) pour le décompte exact, généré depuis les
sources.

## Où aller ensuite

- [Architecture](architecture.md) — comment ce dépôt s'articule avec winx86.
- [Validation à trois niveaux](validation.md) — la méthode qui fait qu'un
  résultat de performance est crédible ou non.
- [Portages natifs et gains mesurés](gains.md) — le catalogue, avec les
  vrais chiffres, mesurés où et comment.
- [Installation](installation.md) — installer le VPK et les fichiers du jeu.
- [Jeu en ligne](en-ligne.md) — ce qui est prouvé contre Battle.net, et ce
  qui ne l'est pas.
- [Fidélité Warden / anti-triche](fidelite-warden.md) — le backlog technique
  détaillé des écarts encore ouverts par rapport à un vrai processus
  Windows.
- [Manette et clavier](controles.md) — le mapping complet et sa
  configuration.
- [Privacy — crash reports](privacy.md) — ce qui est collecté, retiré,
  chiffré, et comment tout désactiver.
