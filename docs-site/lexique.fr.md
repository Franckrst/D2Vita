# Lexique

Termes spécifiques à d2vita / Diablo II. Pour les termes du moteur générique
(PE32, RVA, dynarec, shim, GIL, trap, `set_alternate`...), voir le
[lexique de winx86](https://winx86-136891.gitlab.io/lexique/).

**MPQ** — format d'archive propriétaire de Blizzard contenant les données du
jeu (graphismes, sons, cartes). Lu avec StormLib. D2vita n'en distribue
aucun — voir [Installation](installation.md).

**DCC** — *Direct Cell Compression*, format propriétaire de sprite animé de
Blizzard (personnages, objets). Décodé par `src/runtime/dcc_native.cpp`.

**DC6** — format de sprite Blizzard plus ancien (interface, certains
décors), plus simple que le DCC.

**D2NET** — la pile réseau propriétaire de Diablo II (par opposition à
Battle.net lui-même).

**BNCS** — *Battle.net Chat Server*, le protocole de connexion/chat de
Battle.net classique (pas le protocole du jeu en partie lui-même). Le
handshake BNCS est ce que `src/platform/vita_net.cpp` doit reproduire pour
se connecter — c'est la raison pour laquelle ce fichier reste spécifique à
d2vita plutôt que de rejoindre winx86.

**Warden** — système anti-triche de Battle.net qui inspecte le client
connecté. Un audit du projet a établi que
`CheckRevision` (la fonction concernée) n'inspecte ni la table d'imports, ni
les modules chargés, ni la mémoire du processus dans le contexte de ce
portage — le risque pratique est donc jugé théorique, pas nul.

**CheckRevision** — fonction de vérification d'intégrité appelée au login
Battle.net.

**`g_114`** — variable interne signalant que le binaire chargé est
spécifiquement le patch 1.14d (par opposition à d'autres versions
historiquement visées par le projet) ; conditionne certains shims/hooks qui
ne sont valides que pour cette version précise.

**Oracle (qemu-arm)** — scénario de boot déterministe rejoué pour vérifier
qu'un changement n'a rien cassé ; voir
[Validation à trois niveaux](validation.md).

**Banc (bench)** — scénario de mesure de performance scripté, par
opposition à une mesure « en jeu réel ». Le banc scripté lit historiquement
~30 % au-dessus du jeu réel — un chiffre de banc n'est jamais un chiffre de
jeu sans le dire explicitement.

**GIL** — voir le lexique winx86. Dans le contexte des mesures de
performance de ce projet, « prise de GIL » et « µs par prise » reviennent
constamment comme unité de coût du passage entre code natif et code
traduit.

**Autopilote** — mode intégré au VPK qui pilote automatiquement le jeu des
menus jusqu'en partie, désactivé dès la première action physique du joueur ;
reste actif pour les tests headless. Voir
[Manette et clavier](controles.md).
