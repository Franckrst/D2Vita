# Portages natifs et gains mesurés

Catalogue des optimisations qui remplacent un bout de code x86 traduit par
une implémentation native ARM équivalente (voir le mécanisme dans la
documentation de winx86, page Point d'extension). Chaque ligne précise le
**niveau de validation** du chiffre cité (voir
[Validation à trois niveaux](validation.md)) — un chiffre qemu n'est jamais
présenté comme un résultat console. Chiffres tirés des campagnes de mesure
internes du projet (rapports datés, non publiés dans ce dépôt) ; ce qui suit
en est la synthèse.

!!! note "Aucune valeur absolue ne traverse une campagne"
    Le binaire change, la console dérive thermiquement (1,5-10 %). Les
    pourcentages ci-dessous viennent de mesures A/B/A **entrelacées sur un
    binaire identique**, jamais de deux dates différentes comparées entre
    elles.

## Portages ayant produit un gain confirmé sur console

| Portage | Gain mesuré | Niveau | Détail |
|---|---|---|---|
| Cache de textures : 2 TMU annoncées au jeu (`D2_GLNUMTMU=2`, défaut) | **~+20 %** (1 TMU : 20,06 et 19,95 img/s ; 2 TMU : 23,98, 23,95, 23,95 ; plafond 25) ; textures renvoyées ~50 → 1 par image | Console (banc de patrouille acte V, passes valides seulement, **non entrelacées**, prises à ~1 h d'écart) | Avec 1 TMU, son cache de sprites de 3 Mio débordait en régime permanent : ~50 textures déjà connues renvoyées par image. Exige un atlas assez grand : à 32 Mio il saturait dans les zones sauvages (sprites faux, Death Maulers qui clignotent) ; corrigé par l'atlas de 48 Mio et la gestion des échecs — **confirmation console en attente**. |
| Limiteur de cadence en ligne désactivé par défaut (`D2_ONLINE_CAP`, 28/09) | **24,0 → 25,0 img/s ; images > 60 ms : 25 → 3** par 110 s de patrouille | Console (banc de patrouille acte V, route A, 1 passe avant, 3 passes après) | Le limiteur 25 Hz du moteur s'armait dès que `D2NET` était présent, donc aussi en solo. Il arrondissait l'attente à la milliseconde supérieure (~41,6 ms/image) et dérivait contre l'horloge de D2, qui sautait alors des dessins. En solo, D2 se limite déjà lui-même à un dessin par pas de 40 ms. |
| Intrinsèques servis **en ligne** depuis le code traduit (`D2_INTRINLINE`, défaut depuis le 28/09) | **−0,9 à −1,25 ms de calcul par image** (route A 26,8/26,6 → 25,7/25,9 ms ; route B 27,1/27,0 → 25,8/25,8 ms) ; c0 76–77 % → 74–75 % | Console (banc de patrouille acte V, 8 passes entrelacées, comparaison à route égale) ; qemu : oracle pixel `FBHASH` identique sur 4000 images (2 témoins + 1 jambe), banc de contention `D2_CSTEST` PASS | ~1000 allers-retours de trap par image coûtaient ~4 µs chacun (sortie du JIT, verrou, ré-entrée, recherche de bloc : ~10 % du temps d'image, profil par bloc). Le stub du créneau devient un appel natif box86 ; retour direct à l'appelant par la pile CALLRET. GetTickCount et sections critiques (~650 appels/image). |
| Copie des sommets **en ligne** dans la DLL ring (défaut depuis le 28/09 ; `D2_GLRUNS=0` pour l'ancien chemin) | **−0,9 ms de calcul par image** (route A : 27,6/27,6/27,2 → 26,3/26,8 ms) | Console (5 passes) ; qemu en partie 4000 images Perspective : 1,07 M dessins relus octet par octet, 0 écart, sabotage détecté | La copie de chaque sommet (28 octets, bandes en zigzag) était un appel + `rep movs` : 4,4 % du temps dans la fonction de copie. |
| Pré-lecture fichiers 32 Kio sur saut (`D2_READAHEAD_JUMP`, défaut 32 depuis le 28/09) | Lectures carte dans la fenêtre 65–71 → 25–27 ; temps carte 208–223 → 130–197 ms | Console (4 passes, à route égale) | Pas de RAM en plus (fenêtre de 32 Kio inchangée). Effet sur les à-coups trop petit pour être mesuré (2–4 images de chargement par passe). |
| F3 natif **optimisé et servi en ligne** (`D2_F3NATIF=1` + `D2_INTRINLINE`, 28/09) | **−1,25 ms de plus** (route B 25,6 → 24,4/24,3 ms ; c0 74 → 72 %) | Console (4 passes entrelacées) ; qemu : oracle `D2_F3NATIF=2` 168 503 cellules, 0 divergence ; `D2_F3CHECK` 436 538 bandes, 0 écart | Bandes écrites directement (sans `hostptr` par sommet), coins des cellules rejetées différés, visibilité en entiers quand c'est exact. **Toujours désactivé par défaut** : décision du joueur. |
| F3 natif : dalles de sol en Perspective, `Game+0x10cf70` (`D2_F3NATIF=1`) | **~−2 ms de calcul par image** (calcul traduit 28,0 → ~20,6 ms, shims natifs ~5,3 ms ; c0 −2 points ; fps inchangés, déjà au plafond de 25 en ville) | Console (banc de patrouille acte V, 2 passes valides par variante) ; oracle qemu `D2_F3NATIF=2` : 0 divergence sur 82 000+ cellules dessinées | Portage de l'**appelant** (~30 petits appels par cellule absorbés). Le reste du coût est mémoire (tables de projection et LUT, ring partagé avec le fil de vidage). Désactivé par défaut en attendant la décision. |
| Boucle de cellules native (`NATIVECELLLOOP`) | **+16,1 %** | Console (A/B/A/B, binaire identique) | Traversées ÷2, blit ÷7. |
| `D2_CELLOPT` (masque d'optimisations dans la boucle native) | **+9,3 %** | Console | Moitié moins de travail par cellule. |
| `D2_CELLPAR=1` (parallélisme fork-join, 1 fil ouvrier) | **+12 %** | Console | `D2_CELLPAR=2` = **0 %** (`USER_2` partagé, pas de second cœur disponible). |
| `D2_CALLRET` | **+6,8 %** console / **0 %** qemu | Console (4 passes entrelacées, binaire unique) | Cas d'école : un gain réel que qemu ne voit pas du tout. |
| `fastmmu` + `D2_BUDGETTAIL` | **+2,8 %** | Console | Sortir l'ADD de base du chemin critique du dynarec. |
| `D2_CSINTRIN` + `WX86_B5` (intrinsèque d'horloge) | **+1,4 %** | Console | Intrinsèque d'horloge réécrit pour sortir du chemin critique. |
| Sonde `fog_raise_snap` désarmée | **+4 à 7 %** | Console | La sonde elle-même coûtait ce qu'elle mesurait. |
| Ring GPU — soumission asynchrone (`D2_GXMASYNC=2`) | **+52 %** (28,8 → 43,7 img/s banc) | Console | Le GPU met 11 ms/soumission ; le mode async recouvre ce temps. |
| Ring GPU — effacement plat (`D2_GXMCLEAR=plat`) | **+10,5 %** (−2,5 ms) | Console | Effacement du framebuffer simplifié. |
| Ring côté CPU — vue hôte + sommets rapides | **+11,5 %** (43,75 → 48,78 img/s banc) | Console | Parcours du ring ÷2,3. |
| Hissage de la résolution TLS hors de `CpuBox86::run()` | **+3,03 %** au banc menu, **+8,6 %** en jeu en ligne réel | Console (A/B/A/B entrelacé + garde-fou : travail invité identique à 0,1 % près) | Le gain suit la densité de prises de GIL (190/image au menu contre 2880/image en jeu) — un pourcentage mesuré au menu ne doit jamais être publié comme un gain de jeu. |
| Regroupement entrée/sortie de trap (`trap_retaddr`+`trap_epilogue`, commit `e13edbd`) | **+13,2 %** annoncé au commit | Chiffre de commit, cité tel quel dans une correction ultérieure — pas de document de campagne dédié retrouvé pour ce chiffre précis, à prendre avec cette réserve. | Déjà en place aujourd'hui (pas un knob optionnel). |
| `NATIVELIGHTMAP` (pose d'une lumière dynamique) | **−21 %** sur la passe lumière (1,20 → 0,95 ms/image), soit **−2,6 %** du dessin total | qemu (oracle croisé `D2_LIGHTMAPVERIFY`, 51 200 poses, 0 repli, 0 divergence) — **verdict console non pris** | Jamais mesuré sur console. |
| Cumul de la nuit du 04-05/09, banc déplafonné | Témoin 22,5-23,1 → **28,1-28,6 img/s** (+22 %) | Console, campagne entrelacée de 20 passes | Cumul de plusieurs des portages ci-dessus. |
| Cumul GDI → Glide asynchrone | 22,5 → **55,7 img/s** (+96 %) banc ; **25 pas/s + 60 img/s lissés** en jeu réel | Console | Cumul du passage au rendu GPU asynchrone (ring + soumission async). |
| F3 natif par défaut (`D2_F3NATIF`, dalles de sol Perspective, 06/10) | **22,27 → 23,68 img/s (+6,3 %)** | Console (banc Orbe de givre, 2 passes de référence, 4 passes F3, entrelacées) | Le portage existait depuis le 28/09 mais restait optionnel. |
| Branches de lumière des missiles (`NATIVELIGHTDISC`, `NATIVELIGHTDYN`, Game+0x4748d0 / 0x474d70, 06/10) | **23,76 → 24,66 img/s (+3,8 %)**, `run` 36,3 → 33,5 ms | Console (banc Orbe de givre, 2+2 passes entrelacées) ; oracles croisés console 133 000 + 14 000 appels, 0 divergence ; qemu pixel identique | La passe de lumière par lumière (0x4755a0) a trois branches ; seule celle du type 2 était portée. Les orbes et leurs éclats passent par les deux autres. |
| Cache de salles par appel dans la reconstruction de la grille de lumière (`D2_GRIDCACHE`, 06/10) | Part du temps **3,74 % → 1,68 %** (~−0,9 ms/image) | Console (profil `BLKSAMP`, banc Orbe de givre) ; qemu : mêmes décisions de salle case par case, pixel identique, oracle `D2_GRIDVERIFY` 0 divergence | Chaque case relisait la mémoire invitée par un accès contrôlé ; les ~5 % de cases qui changent de salle refaisaient toute la recherche. |
| Liaison de texture de F3 servie en natif (`D2_F3TEXNAT`, Game+0x50fbd0 branche « en cache », 06/10) | Marge libre par image **0,3-0,7 → 3,2-3,3 ms**, c0 96 → 90 % (au plafond de 25 img/s) | Console (banc Orbe de givre, 2+2 passes entrelacées) ; qemu : empreinte du ring identique (3,27 M enregistrements, états compris), sabotage détecté | ~400 allers-retours par image vers le code du jeu. L'hôte écrit `grTexSource`/`grTexCombine` à travers le cache de déduplication de la DLL, publié dans l'en-tête du ring (nouvelle `glide3x.dll` requise). |

## Portages réfutés ou sans effet mesurable — gardés comme mise en garde

| Portage | Résultat | Niveau | Pourquoi c'est instructif |
|---|---|---|---|
| `D2_MEMINTRIN` (memcpy/memset natifs) | Mécanisme prouvé correct (4,4 M d'appels, 0 divergence) mais **0 %** sur console | Console | Le CRT invité vectorise déjà ses gros transferts (NEON) ; le chemin Glide fait 90 appels/image contre ~1 100 sous GDI — le levier avait disparu avec le changement de renderer. |
| `D2_FORWARD` | **Inerte** (+0,6 % = bruit) | Console | Hypothèse « blocs plus courts » jamais testée. |
| `D2_NOPEND` (levier L4) | **Réfuté**, plafond 0,002 % | Console | Le document original visait le mauvais site de code (0 appel réel au lieu du site supposé). |
| Report du RLE sur un fil dédié | **FAIL** sur trois modes ; le mode qui passait trois jours plus tôt **ne passe plus** | qemu (oracle) | « Prouver l'oracle avant de conclure » — le PASS fondateur ne s'était jamais rejoué depuis. |
| Quatre hypothèses de cache d'image | **Toutes réfutées** (0 image identique sur 4 000) | qemu (`D2_CACHEPROBE`) | A trouvé autre chose à la place : l'option vidéo Perspective absente du registre coûtait +14,2 % de blocs invités. |
| PMU matériel (compteurs de cycles Cortex-A9) | **Abandonné** | Console | Le noyau Vita remet le registre d'accès à zéro à chaque changement de contexte ; demanderait de patcher l'ordonnanceur système. |
| `REP MOVSD` par paquets NEON de 16 octets (`D2_REPMOVS=1`, moteur) | **Neutre** (27,4 contre 27,4 ms) | Console (4 passes) ; exact (oracle, sabotage détecté) | La copie des sommets était dominée par l'appel, pas par les octets. Désactivé. |
| Regroupement des sommets contigus dans la DLL (`D2_GLRUNS=1`) | **Neutre** | Console (4 passes) | Les bandes Perspective référencent leurs sommets en zigzag : jamais contigus. |
| 5 intrinsèques de l'acte V (`D2_INTRIN` : projection, LUT couleur ×2, grille de lumière, remplissage) | **Négatif** : ~+4,3 ms de calcul par image, ~−3,2 % d'img/s (5 passes de patrouille, oracles à 0 divergence) | Console | Des fonctions minuscules (20 000 à 30 000+ appels par image) faites d'arithmétique entière, que le dynarec traduit déjà presque 1 pour 1 : le corps natif n'est pas plus rapide, et chaque appel paie la plomberie. Désactivés par défaut. |
| Sérialisation native des dessins (`D2_GLNATDRAW=1`, intrinsèque au point de trap) | **Négatif** : ~+5,5 ms par image, −3,8 % d'img/s (4 passes entrelacées, banc v1 sans contrôle de parcours, fenêtre 2600-5600) | Console | ~1 700 sorties du code traduit par image coûtent plus que la copie évitée. Même leçon : un intrinsèque ne paie que si le corps épargné est gros par appel. |
| Quinze leviers dynarec, bilan de campagne | **Un seul gain dans toute l'histoire du dynarec** (`D2_CALLRET`, +6,8 %) | Console | « le dynarec n'est pas le levier », le gain est dans le portage natif de code invité, pas dans la traduction elle-même. |
| Mémoriser les coins des cellules de F3 dans un appel (06/10) | **Réfuté** : 0 répétition sur 2,16 M cellules, même avec la clé réduite (`sub`, `dy`) | qemu (Glide déterministe, 5 000 images) | Les cellules d'un même appel ne partagent jamais leurs coins ; mesurer le taux de succès avant d'écrire le cache. |

## Méthode de sélection d'un portage qui rapporte

Ce que la campagne a appris se formalise ainsi :
`gain ≈ N × (T_invité − T_natif − 69 ns)` où **69 ns est le coût
mesuré du passage x86→natif lui-même** (le prix d'un aller-retour, même pour
un intrinsèque reconnu à la traduction plutôt qu'un vrai trap). La bonne
cible n'est **pas** la fonction la plus chaude au profil — c'est un
**appelant** avec un grand nombre d'appels (`N`) qui absorbe tout un
sous-arbre en une seule traversée. C'est exactement le patron de la boucle
de cellules : un seul site d'appel produit la totalité des ~1600 appels au
blit de cellule par image.

## Config de jeu retenue

`tools/bancs/env_jeu_glide.txt` (déposée sur console, choisie par
l'utilisateur) — c'est la combinaison de tous les leviers confirmés
ci-dessus :

```
D2SCHED=native
D2WRITE=ux0:data/d2vita/save2
NATIVECELLLOOP=1
D2_CALLRET=1
D2_LAZYSEEK=1
WX86_FRAMEPROF=1
D2_FRAMEUS=1
D2_CELLOPT=63
D2_MMUSTACK=3
D2_MMUFOLD=1
D2_BUDGETTAIL=1
D2_CSINTRIN=1
WX86_B5=1
D2ARGS=game.exe -3dfx
D2_GLIDERING=1
D2_GLIDEGXM=1
D2_GXMASYNC=1
D2_GXMCLEAR=plat
D2_GXMRING=4
WX86_YIELD=2
D2_ONEDRAW=1
D2_IOSTAT=1
D2_READAHEAD=128
D2_READAHEAD_MIN=32
D2_READAHEAD_JUMP=32
D2_READAHEAD_WIN=8
D2_READAHEAD_WINKB=32
D2_SON=1
```

(tenu à jour avec `tools/bancs/env_jeu_glide.txt`, la source de vérité — ce
bloc n'en est qu'une copie. `D2_NOCAP` n'y figure pas volontairement — c'est
un outil de banc qui débraye les limiteurs d'images internes du jeu, pas un
réglage à garder en jeu réel.)

`D2WRITE` désigne le dossier dans lequel le jeu écrit — sauvegardes,
`crash.log`, journal quotidien de D2. Jusqu'à la 0.1.6, un dossier nommé ici
n'était **pas** créé sur console : toutes les écritures échouaient, et la
bibliothèque C du jeu tuait le processus au bout d'une dizaine de secondes,
sans un mot sur le dossier en cause. Depuis la 0.1.7, le démarrage crée la
racine nommée (et son sous-dossier `Save`), vérifie qu'elle est inscriptible,
et bascule sur `ux0:data/d2vita/save` avec une ligne dans
`boot_progress.txt` quand ce n'est pas le cas.
