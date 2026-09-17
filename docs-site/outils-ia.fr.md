# Outils IA du projet

Le moteur et le jeu sont deux dépôts, et l'outillage suit cette frontière :
**la méthode appartient au moteur, la recette appartient au portage.**

Tout ce qui concerne l'exécution de code x86 sur ARM — le dynarec, les
ordonnanceurs, les crochets invité, l'échelle de validation, la preuve d'un
instrument avant de s'y fier, le protocole A/B, la chaîne VitaSDK — vit
désormais côté winx86 et ne nomme aucun jeu :
[la page équivalente côté winx86](https://winx86-136891.gitlab.io/outils-ia/).

Ce qui reste ici est ce que seul ce portage peut répondre : quelle adresse,
quel réglage, quel marqueur, quel fichier.

Source : `.claude/skills/README.md`.

## Skills de ce dépôt

| Skill | Rôle |
|---|---|
| `runtime-debugging` | Les recettes de ce portage : la commande de reproduction sous qemu-arm, `D2_REALCLOCK`/`D2SCRIPT`, résoudre un EIP invité par rapport à la base d'origine de `Game.exe` (chargé sans relocation), le journal du chien de garde, `env.txt`, et quel marqueur signifie « en partie » sur chaque plateforme. |
| `hardware-ab-bench` | Piloter la vraie console : les verbes VitaCompanion, l'échange d'eboot par saveur sans réinstaller, le parcours `D2VITA_PATROL` et sa fenêtre de mesure 3600–8400, le fichier parasite qui bloque la sélection de personnage, les pauses qui invalident un relevé. |
| `vitasdk-build` | Construire et lancer localement le VPK de ce portage : le build en deux temps contre la bibliothèque du moteur, ses réglages, les identifiants de titre et noms de sortie, où doivent se trouver les données du jeu. |
| `mpq-reading` | Lire les archives MPQ Blizzard avec StormLib (outils côté hôte) — extraire, lister, diagnostiquer. |

Les trois premiers s'ouvrent sur un renvoi vers leur homologue du moteur : on
lit le skill du moteur pour le *comment*, celui-ci pour *ce qu'il faut taper*.

## Agents

Un skill dit *comment* faire ; un agent est dépêché pour *le faire*.

| Agent | Rôle |
|---|---|
| `doc-updater` | Remettre `docs-site/`, `docs/` et `ROADMAP.md` en phase avec le code après un changement — avec la règle qu'un message de commit n'est pas une source : chaque affirmation se vérifie dans le code. |
| `online-validation` | Prouver qu'un changement sur un chemin réseau **actif** est sans danger : serveur BNCS local, A/B octet pour octet sur du trafic réel, et le piège du run trop court qui a fait mentir deux témoins. |
| `local-validation` | Prouver une affirmation de correction ou de performance pour tout le reste : l'échelle qemu-arm/Vita3K/console, quel script couvre quel niveau, et pourquoi un chiffre qemu n'est jamais un résultat console. |

Le moteur a ses propres agents, dont la méthodologie de découpe
générique/spécifique employée pour déplacer du code à travers la frontière.

!!! note "Skills historiques retirés"
    Les skills liés aux stratégies abandonnées (réimplémentation native
    D2MOO, levée statique AOT) ont été retirés en même temps que ces
    stratégies, au profit de l'exécution directe des binaires d'origine.
