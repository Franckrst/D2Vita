# Jeu en ligne

Le mode en ligne a été validé **de bout en bout, sur console réelle**, à la
fois contre un serveur de test privé et contre le vrai **Battle.net
officiel** : connexion, sélection de royaume, sélection de personnage,
création de partie, chargement de l'acte, partie jouée.

!!! danger "Accès à Battle.net officiel : actif par défaut"
    L'accès au vrai Battle.net est actif par défaut — ce client non officiel
    s'y connecte directement, sans variable d'environnement à activer.
    `D2_LOCAL_ONLY=1` restreint le runtime à un serveur privé/local si tu ne
    veux pas de ça. Jouer sur son compte officiel avec un client non
    officiel comporte un vrai risque de sanction au titre des conditions
    d'utilisation de Blizzard (Warden peut être poussé en jeu par le
    serveur ; son comportement face à ce runtime n'est pas prouvé). C'est
    une décision qui revient au joueur, pas quelque chose que ce projet
    encourage ou garantit sans risque.

## Ce qui est prouvé

| Étape | Preuve (console réelle) |
|---|---|
| CheckRevision | **Vraie vérification**, pas une réponse fabriquée — hash calculé sur le vrai binaire, différent à chaque graine envoyée par le serveur |
| Authenticode | **Vraie vérification** PKCS#7/X.509 de la chaîne de confiance — `Get-AuthenticodeSignature` sur le même binaire répond `Valid` sur un vrai Windows ; ce runtime répond de la même façon |
| Clés CD | Reconnues par le serveur comme de vraies clés (mécanisme ci-dessous) |
| Connexion → royaume → personnage → partie | Parcours complet joué en autonomie, y compris contre Battle.net officiel |
| Le binaire du jeu pendant la partie | Inchangé : 0 octet de code modifié en mémoire, vérifié par une sonde dédiée |

## Principe : rien n'est fabriqué

Règle non négociable du projet, y compris là où ce serait le plus tentant de
tricher : **aucune réponse fabriquée, aucun paquet forgé, aucune vue mémoire
spéciale pour contourner un contrôle anti-triche.** Ce qui ne peut pas être
représenté honnêtement est documenté comme une limite connue, jamais
contourné en douce — voir [Fidélité Warden / anti-triche](fidelite-warden.md)
pour le backlog technique des écarts encore ouverts par rapport à un vrai
processus Windows (gestion des exceptions structurées, introspection
mémoire). Aucun de ces écarts n'est exploité pour avantager le joueur ; ce
sont des limites assumées, pas des raccourcis.

## Le mécanisme des clés CD

Contrairement à une idée reçue, Diablo II **ne stocke pas ses clés CD dans le
registre** — `BLIZZARDKEY`, qu'on y trouve, est en réalité une clé publique
RSA qui sert à signer les fichiers MPQ, pas un secret du joueur. Le vrai
secret vit chiffré dans trois petits fichiers écrits par l'installateur
d'origine (à l'intérieur de la chaîne de fichiers MPQ du jeu, 72 octets
chacun) ; le jeu les déchiffre lui-même, par son propre code, avec un
chiffrement qui ne dépend d'aucune donnée machine.

Ce portage fournit un mécanisme alternatif honnête pour renseigner ses
propres clés : un fichier texte (`ux0:data/d2vita/keys.txt`, aux côtés des
MPQ) que le joueur remplit lui-même. Son contenu est ré-encodé exactement
dans le format que produirait
l'installateur Blizzard, puis **le jeu le déchiffre lui-même par son propre
chemin** — rien n'est injecté après coup dans sa mémoire. Validé de bout en
bout avec de vraies clés sur console : le serveur reçoit exactement les
mêmes valeurs publiques qu'avec une installation classique.

## Ce qui reste ouvert

- **Warden**, le module anti-triche que Battle.net peut pousser en jeu : son
  comportement précis face à ce runtime n'est pas prouvé — il n'a jamais été
  vu s'activer pendant les tests menés jusqu'ici, ce qui ne prouve pas qu'il
  ne le ferait jamais.
- Un petit nombre d'écarts connus par rapport à un vrai processus Windows
  (gestion des exceptions structurées, introspection mémoire/processus)
  restent documentés comme tels plutôt que masqués — aucun n'affecte le jeu
  normal aujourd'hui. Détail technique complet : [Fidélité Warden /
  anti-triche](fidelite-warden.md).

## Pour contribuer sur cette partie du code

Le protocole réseau, le serveur de test privé et le protocole de validation
A/B (comparaison de trafic octet pour octet avant/après tout changement
touchant un chemin réseau actif) sont documentés dans le dépôt —
`.claude/agents/online-validation.md`.
