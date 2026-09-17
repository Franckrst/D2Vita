---
name: online-validation
description: Use before claiming that any change to an ACTIVE network path is safe — connect, select, recv, send, socket, gethostbyname, the gateway list, or anything in WSOCK32/WS2_32 on either side of the moteur/jeu boundary. A qemu-arm boot check does NOT cover network code. Also use when asked to prove online play still works, or to compare network traffic before and after a refactor.
tools: Bash, Read, Edit, Write, Glob, Grep
---

# Validation en ligne réelle

Une passe `rt_boot_arm_check.sh` sous qemu-arm **ne valide pas du code
réseau**. Elle amène le jeu jusqu'au menu, sans jamais ouvrir une socket vers
un serveur. Un changement sur `connect`/`select`/`recv`/`send` peut la passer
et casser toute connexion.

Le seul verdict qui compte ici : **faire tourner le vrai `Game.exe` contre le
serveur local, et comparer le trafic avant/après octet pour octet.**

## Le piège qui a fait mentir deux runs témoins

**Un run trop court ne prouve rien.** Le clic automatique sur BATTLE.NET
(`D2_AUTOBNET`) est conditionné à `g_frame >= 600` (`tools/rt_boot.cpp`, bloc
`[autobnet]`), suivi d'une séquence de clic de ~12 images, puis du `connect`.
Un budget `MAXFRAMES` inférieur à ~900 **n'atteint jamais la passerelle**.

Conséquence : dans un tel run, l'absence d'un évènement réseau dans le journal
ne prouve **pas** son absence — elle prouve seulement que le run s'est arrêté
avant. C'est déjà arrivé, avec la conclusion inverse (« ce code est mort, on
peut le supprimer ») sur le point d'être tirée d'un run de 260 images.

> Avant toute conclusion négative (« ça ne se déclenche jamais », « ce chemin
> n'est plus pris »), **prouve d'abord que ton run atteint réellement le
> chemin en question** — par un évènement positif dans le même journal (le
> `connect ENTER` vers la passerelle, par exemple). Sans cette preuve, ton
> observation ne vaut rien.

## L'infrastructure

- `tools/bncs_local.py` — serveur BNCS local. `tools/net_check.sh` le démarre
  et lance le banc des primitives. **Refuse de démarrer si le port 6112 est
  déjà pris** : deux serveurs simultanés donnent des résultats incohérents, ce
  garde-fou existe pour une raison.
- Pile complète côté serveur (realmd + d2ingress + GS 1.14d, jusqu'au
  `player_join` réel) : infrastructure privée du mainteneur (images docker
  non publiées), pas documentée dans ce dépôt — ce qu'un contributeur externe
  peut reproduire avec les outils publics d'ici s'arrête à
  `tools/bncs_local.py`/`tools/net_check.sh` ci-dessus. Le résumé côté joueur
  de ce qui est prouvé en ligne vit dans `docs-site/en-ligne.md`.
- Compte de test : `franckrstd2` (serveur local uniquement).

### Sécurité réseau — non négociable

**ZÉRO trafic vers Blizzard.** Le pare-feu doit rejeter les plages Blizzard
avant tout run ; seuls le loopback et le réseau docker sont autorisés. Jamais
de vraie clé CD sur le fil. Jamais de paquet client forgé : tout le trafic
client→serveur doit être produit par le vrai code de `Game.exe` — sinon tu ne
testes pas le jeu, tu testes ta propre fiction.

Ne code jamais un identifiant réel en dur, et **jamais rien de tout cela dans
winx86** : c'est un moteur générique, potentiellement public un jour.

### La façon fidèle de brancher le serveur local

Pas en réécrivant la destination dans `connect` — ce n'est pas comme ça qu'on
fait sur PC. La **liste de passerelles** du jeu est un `REG_MULTI_SZ` dans le
registre (`software\battle.net\configuration`, cf.
`tools/registry_console.txt`). Réduite à `127.0.0.1`, elle suffit pour toute
la voie BNCS/MCP/D2GS : une fois connecté, le serveur annonce lui-même ses
adresses.

## Le protocole A/B

1. **Construire la référence** : un binaire depuis la révision **avant** ton
   changement. Pas « de mémoire » — une vraie construction depuis un `git
   stash`/`git worktree` sur la révision de base.
2. **Lancer les deux** contre le même serveur local, même budget d'images
   (≥ 900), même environnement, serveur redémarré proprement entre les deux.
3. **Comparer le trafic octet pour octet**, vu du serveur.
4. **Masquer les champs aléatoires par conception** — il y en a exactement
   deux, et les confondre avec une régression fait perdre une heure :
   - la formule `CheckRevision` tirée par le serveur ;
   - le jeton client (`client_token`) tiré par D2.
   Tout autre écart est un vrai écart, à expliquer avant de conclure.
5. **Comparer aussi le décodage côté client** (`D2_NETWATCH`, observateur
   passif) : même nombre de paquets, mêmes identifiants.

## Le niveau de référence connu

L'échec de handshake « **sélecteur 0xff inconnu** » est **préexistant** et ne
se manifeste que sous **horloge virtuelle** (`D2_VIRTCLOCK`). En configuration
normale le handshake passe et va bien plus loin (`SID_AUTH_CHECK` puis BNFTP).

C'est donc le niveau de référence, **pas** une régression que tu aurais
introduite — et surtout pas quelque chose que tu peux prétendre avoir corrigé.
Si tu le vois des deux côtés d'un A/B, c'est normal ; il doit apparaître à
l'identique.

## Avant de conclure

Sépare explicitement, sans jamais les mélanger :

- ce qui a été **réellement exercé par du trafic D2 réel** ;
- ce qui n'a été testé **qu'en isolation** (un banc, un test unitaire) ;
- ce qui n'est **pas testé du tout** — typiquement `accept`/`bind`/`listen` :
  D2 est un client pur et ne les appelle jamais.

Un correctif sur un chemin que le run n'emprunte pas est un **défaut latent
réparé**, pas un correctif validé. Dis-le ainsi.
