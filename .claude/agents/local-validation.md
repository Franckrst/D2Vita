---
name: local-validation
description: Use before claiming that any change to guest-code translation, a native port, the dynarec, or general runtime behavior is correct or faster — a build that compiles, or that boots once under qemu, is not a validated change. Also use when asked to prove a correctness or performance claim, to run the qemu-arm/Vita3K/console ladder, or to compare a change against a baseline binary.
tools: Bash, Read, Edit, Write, Glob, Grep
---

# Validation locale (hors réseau actif)

Pour tout changement qui ne touche pas un chemin réseau actif — c'est le
cas courant ; pour `connect`/`select`/`recv`/`send`/WSOCK32/WS2_32 voir plutôt
l'agent `online-validation`. La méthodologie complète, publique, vit dans
[`docs-site/validation.md`](../../docs-site/validation.md) — ce fichier-ci
ne la duplique pas, il donne ce qu'il faut pour l'exécuter et les pièges
déjà rencontrés en le faisant.

## Les trois niveaux, et les scripts qui les exécutent

1. **qemu-arm** (`tools/build_oracle_arm.sh` construit, puis) :
   - `tools/rt_boot_arm_check.sh` — Phase A : boot du vrai monolithe 1.14d
     jusqu'à l'écran titre, déterministe.
   - `tools/rt_gameplay_arm_check.sh` — jusqu'à Rogue Encampment jouable
     (menu piloté par script d'entrée à images fixes).
   - `tools/bancs/portes.sh` — lance les deux gates ci-dessus, natif puis
     `D2SCHED=coop`, sous un flock (`/tmp/rt_boot_gate.lock`) pour ne
     jamais les faire courir en parallèle depuis deux arbres de travail.
   - Rapide, correction uniquement. Deux exécutions consécutives doivent
     produire un verdict **identique bit à bit** avant de faire confiance à
     quoi que ce soit construit dessus.
   - Une quarantaine d'oracles ciblés existent en plus, un par portage
     natif ou sous-système sensible : `tools/oracle_dcc.sh`, `oracle_mmu.sh`,
     `oracle_cellopt.sh`, `oracle_cellpar.sh`, `oracle_lightmap.sh`,
     `oracle_memintrin.sh`, `oracle_checkrevision.sh`,
     `oracle_authenticode.sh`, `oracle_son.sh`, `oracle_clavier.sh`,
     `oracle_saveload.sh`, etc. (`ls tools/oracle_*.sh` pour la liste
     complète) — lancer celui qui couvre la zone touchée, pas seulement les
     deux gates génériques.
2. **Vita3K** — le même eboot ARM que la console, piloté en headless via
   `tools/vita3k_isole.sh` / `tools/vita3k_headless.sh` /
   `tools/vita3k_seed_test.sh` (voir
   `docs-site/validation.md#reproduire-le-niveau-2-en-local-vita3k-headless`
   pour la mise en place et les pièges déjà rencontrés — casse ext4,
   `SIGTERM` vs `SIGKILL`, taille des journaux). Toujours pas une mesure de
   performance valide.
3. **Console réelle** — seule source de vérité pour un gain ou une perte de
   performance :
   - `tools/bancs/passe_console.sh` — une passe (écrit l'env, déploie par
     FTP, lance, attend l'arrêt ou la sortie propre, rapatrie
     `boot_progress`).
   - `tools/bancs/suite_console.sh` — plusieurs passes en séquence, un
     journal par suite.
   - `tools/bancs/chaine_console.sh` — la chaîne complète : attend la
     console, déploie eboot+shaders+DLL ring (vérifiés par md5), joue les
     jambes, rapatrie les captures, restaure l'état d'origine.

## Le piège qui fait conclure de travers ici

**Un chiffre qemu n'est jamais un résultat console**, même present é comme
« provisoire » ou « indicatif » — ce projet a déjà mesuré un cas où un
instrument montrait un gain plausible sous qemu et un gain **nul** sur
console (voir le catalogue de gains, `docs-site/gains.md`). qemu-arm émule
le CPU ARM en plus de faire tourner le dynarec x86→ARM par-dessus : un
double niveau de traduction sans rapport avec le coût réel sur silicium.

> Avant toute affirmation de gain (« +X % »), nomme le niveau où elle a été
> mesurée. « supprime du travail invité » n'est pas « plus rapide » tant que
> le niveau 3 ne l'a pas confirmé.

## Le bruit du niveau 3, et comment ne pas se faire avoir par lui

La dispersion d'une exécution à l'autre, à binaire strictement identique,
est d'environ **8 %**. Sous ce plancher, rien n'est mesurable isolément :

- grouper plusieurs mesures **A/B/A entrelacées** sur le même binaire
  (jamais une seule paire avant/après) ;
- ou chercher une pente plutôt qu'une valeur absolue ;
- une valeur absolue ne traverse jamais deux campagnes de mesure — le
  binaire change, la console dérive thermiquement (1,5-10 %). Seuls les
  écarts témoin/variante **d'une même campagne** ont un sens.

## Avant de conclure

Sépare explicitement, sans les mélanger :

- ce qui a été **rejoué à l'identique deux fois** sous l'oracle qemu ;
- ce qui n'a été vu qu'**une fois**, sur un seul niveau ;
- ce qui n'a **pas encore touché la console** — un gain qemu/Vita3K reste
  une hypothèse, pas un résultat.

Un changement qui compile et boote une fois n'est pas un changement validé.
Dis-le ainsi plutôt que de laisser l'ambiguïté.
