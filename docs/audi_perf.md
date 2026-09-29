# Audit de performance — acte V (neige), 25–26/09/2026

Audit des chutes d'images à l'acte V. Il a été mené sur **console réelle** (PS Vita, 10.113.1.159), en jeu libre piloté à la main. Le joueur posait des marques **L + haut** à chaque ralentissement ressenti.

Tous les chiffres ci-dessous sont des mesures console, sauf ceux explicitement marqués **qemu**. Ce ne sont pas des A/B entrelacés au sens de `hardware-ab-bench` : chaque passe est une partie jouée à la main, donc une scène différente. Les écarts inférieurs à ±3 % n'y veulent rien dire. Les conclusions fortes reposent sur des comparaisons à **une seule variable** avec un effet massif (×1,5 en cadence, 109 → 0 téléversements).

Les journaux bruts de chaque passe sont archivés dans `build-vita/bancs/acte5/` (hors git).

---

## 1. Résultat en une page

| Question | Réponse mesurée |
|---|---|
| Pourquoi 15 img/s en régime à l'acte V ? | La mémoire texture annoncée au jeu (4 Mio) plafonnait son cache de sprites à 2 Mio : le jeu renvoyait ~109 textures par image. |
| Correctif | Annoncer ≥ 6 Mio (défaut désormais **16 Mio**, `D2_GLTEXMEM`). Régime **15,5 → 23–24 img/s**, 0 téléversement en régime. |
| Pourquoi des chutes restent ponctuellement ? | Rafales de textures à l'arrivée de nouveaux monstres ou zones, jusqu'à 511 textures par image. Le coût est côté **fil de jeu** (préparation invitée), pas E/S, RAM, GPU ni JIT. |
| Poste n°1 du fil de jeu | L'option vidéo **Perspective** : 21 000 contre 9 000 sommets par image. Le fil de jeu est à 88–95 % Perspective ON, 60–68 % Perspective OFF. |
| Portages natifs réalisés | 5 fonctions feuilles sans trap : 0 divergence sur plusieurs millions d'appels (console et qemu). **Gain non mesurable** : ces fonctions pèsent peu en temps réel. |
| Crash rencontré | Course sur les tables de descripteurs de newlib entre le fil de jeu et le journal des fils de service. **Corrigé** : journal écrit en `sceIo`. |
| Ce qui manque | Un instrument de **temps par fonction** fiable. Les profileurs existants mesurent des fréquences d'entrée de bloc, pas du temps (§4). |

---

## 2. Inventaire de l'instrumentation

### 2.1 Instruments existants, activables par `env.txt`

| Question | Instrument | Ligne produite |
|---|---|---|
| Coût par image, pires images | `WX86_FRAMEPROF`, `D2_LAGWATCH=<ms>` (+ `_NOSAMP`) | `frames:`, `lente#NN` |
| Le jeu attend-il ou calcule-t-il ? | `D2_LOOPWATCH`, `D2_WAITPROF` | `rendu:`, `jeu:`, `images/duree(10s)`, `attentes(10s)` |
| Temps des shims natifs | `D2_NATPROF` | `natif N: … us %` |
| Attente GIL par fil | `D2_FILSTAT` | `fils: … /t /c /w` |
| GPU, ring Glide | `gxm:` + `D2_GRPROF` | `gxm:`, `gxm-flush`, `gxm-ring`, `flushfil:` |
| Système de fichiers | `D2_IOSTAT`, `D2_IOHIST` | `io(10s)`, `io/fichiers(10s)` |
| RAM, caches du jeu | ligne `alive:` (toujours) | `heap= va= spr= cel= jitp=` |
| Code invité « chaud » | `D2_TIMESAMP`, `D2_TIMEPROF`, `D2_EIPPROF` | `temps/eip`, `[TIMEPROF/EXACT]` — voir **§4, biaisés** |
| Marque du joueur | **L + haut** en jeu | `>>> MARQUE #n` |

### 2.2 Instruments ajoutés pendant l'audit

| Instrument | Où | Ce qu'il dit |
|---|---|---|
| `cpu(10s): occupes c0..c3` | `vita_present.cpp` (`sceKernelGetSystemInfo`) | Occupation réelle de chaque cœur. C'est lui qui tranche « calcul ou attente ». Le delta brut `idle` est publié pour vérifier l'unité. |
| `hot: … dcc=servis/replis ko=` | `rt_boot.cpp` (cases 100–103) | Décodages DCC par fenêtre : un churn de sprites devient visible en cours de partie. |
| `lente#` : `run=` **exact**, soustrait de `INEXPLIQUE` | `frame_profile.cpp`, moteur `cpu_box86.cpp` (`wx86_runacc`, armé par `D2_LAGWATCH`) | `run=` valait 0 sous l'ordonnanceur natif : il n'était crédité qu'au retour de `cpu->run()`, que le fil de jeu ne quitte presque jamais. Il compte désormais le temps du fil de rendu hors traps (intrinsèques, JIT et attente du GIL inclus). Le reste de `INEXPLIQUE` est le temps passé dans les shims autres que E/S, vidage GPU et attentes du jeu. Coût : deux lectures d'horloge par trap réel (~850/img), aucune par intrinsèque. Vérifié qemu : à l'écran titre, attente 36 + run 1 + reste 3 = 40 ms. |
| `atlas(fen): deja-la= neufs= copies= evictions= pages=` | `gx_host.cpp` | Séparent une texture renvoyée mais déjà connue (cache du jeu en échec) d'un contenu réellement neuf. |
| `tmu0/tmu1: up= deja-la= liaisons=` | `gx_host.cpp` | Téléversements et liaisons par TMU. |
| `flushfil: banc hachage 1 Mio` | `gx_host.cpp` (au démarrage) | Vitesse intrinsèque du hachage, scalaire contre NEON, avec **auto-vérification d'identité** sur 8 tailles. |
| `fichiers-ouverts=` sur `io(10s)`, `file C <nom>` | `io_stat.cpp`, `kernel32_files.cpp` | Détection de fuite de handles, symétrie ouverture/fermeture. |
| Compteurs d'entrée `D2_QUADCOUNT=1` | `d2_intrin_114.cpp` | Appels par image de F1/F2/F3 et du chemin texture des tuiles (§8). |
| Oracle croisé `D2_INTRINVERIFY=1` | `d2_intrin_114.cpp`, `native_hooks_codec.cpp` | Compare mémoire et registres au retour de chaque appel porté (§7). |
| `tools/lag_analyse.py` réparé | — | Sa regex ne reconnaissait plus aucune ligne `lente#` depuis l'ajout de `gx=`, `attente-invitee=` et `INEXPLIQUE=`. |

### 2.3 Knobs ajoutés

| Knob | Défaut | Rôle |
|---|---|---|
| `D2_GLTEXMEM=<Mio>` | **16** (était 4) | Mémoire texture annoncée au jeu (§5). |
| `D2_GLNUMTMU=<n>` | 1 | `GR_NUM_TMU` répondu par la DLL ring. 2 est testé et **non retenu** (§6.3). |
| `D2_TEXHASH_NEON=0` | NEON | Force le hachage scalaire (A/B). |
| `D2_INTRINVERIFY=1` | off | Oracle croisé des intrinsèques lut/lgrid/lfill/lut2. |
| `D2_QUADCOUNT=1` | off | Compteurs d'entrée, sonde de mesure. |

---

## 3. Chronologie des passes console

| Passe | Config (variable testée) | Régime img/s | Rafales img/s | Constat principal |
|---|---|---|---|---|
| référence | env joueur, 4 Mio | 18,3 (acte I) | — | E/S, JIT, sync, fail à 0 : les compteurs par défaut n'expliquent rien. |
| 1 | instruments, 4 Mio | **15,5** | 13–15 | Fil de vidage à 60 ms/img, 109 textures/img, c0 91 %, c1 95 %. |
| 2 | **16 Mio** | **23–24** | 18–20 | 0 téléversement en régime. Rafales de 16 à 54 textures/img à l'entrée des zones. |
| 3 | 32 Mio + audio sur cœur 2 | 23–24 | 15–20 | Même rafales, hachage inchangé (330 µs/texture) : l'audio n'était pas la cause. |
| 4 | + `atlas(fen)`, banc hachage | 22–24 | 16–20 | 77–96 % des textures des rafales sont **déjà dans l'atlas**. Hachage 7–11 ns/o intrinsèque. |
| 5 | + hachage **NEON** | 20–24 | 18–22 | NEON 2,6–2,9 ns/o (scalaire 5,7–6,2), identique bit à bit ; en jeu seulement −25 % (330 → 260 µs). Le fil de vidage n'est plus sur le chemin critique. |
| 6 | `GR_NUM_TMU=2` | **17,3** | 17–22 | Régression : 105 textures/img **permanentes**, les deux TMU annonçaient la même plage d'adresses. |
| 7 | 2 TMU, plages distinctes | 21,7–23,4 | 16–22 | Thrash disparu, mais `tmu1 up=0 liaisons=0` : le jeu n'utilise jamais la TMU1. Rafales inchangées → 2 TMU non retenu. |
| 8 | profil par bloc (`D2_TIMEPROF`) | 20–24 | 17–22 | Désigne la projection Perspective (18,5 %), la LUT couleur et la grille de lumières — **biaisé**, voir §4. |
| 8b | Perspective OFF en cours de partie | 22–25 | — | Fil de jeu **91–95 % → 60–67 %**, sommets 21 000 → 9 000/img. |
| 9 | Perspective OFF dès le boot | 22–24 | 19–23 | Confirme 8b : c0 65–68 %. |
| 10 | preuve projection native | (lent, oracle) | — | 11,2 M comparaisons, **0 divergence**. |
| 11 | projection native, Perspective ON | 19–23 | — | 100 % servis, c0 85–92 % : pas de gain mesurable. |
| 12 | `D2_TIMEPROF=100` | — | — | **Mesure invalide** (§4). |
| test preuve | 5 portages + oracles | (lent) | — | 5 cibles, 0 divergence, aucun crash, `fichiers-ouverts` stable (§7). |
| test gain | 5 portages, Perspective ON | 19–23 | 10,6–13,8 | 100 % servis, aucun crash. Pire fenêtre : **511 textures/img** (arrivée de contenu). |
| test compteurs | + compteurs corrigés | 19–23 | 18–22 | Répartition réelle des appels (§8). |

Hypothèses **écartées par mesure** sur toutes les passes :
- **E/S :** 0,5–2 s par 10 s, uniquement aux chargements.
- **JIT :** 0 bloc en régime, `fail=0`.
- **RAM :** heap 8/56 Mo ; sprites 7–16/64 Mo ; cel 30–68/500 Ko ; DCC 0–3 par fenêtre en régime.
- **GPU :** `attente-gpu-us` ≈ 35.
- **GIL :** `/w0` sur tous les fils.

---

## 4. Pièges d'instrument découverts (à ne pas refaire)

1. **`[TIMEPROF/EXACT]` n'est pas une part de temps.** L'échantillon est pris à l'expiration d'un budget de blocs (`cpu_box86.cpp`, `timeprof_sample`). L'adresse retenue est le bloc qui *allait s'exécuter* : l'échantillonnage suit la **fréquence d'entrée**. Le memcpy du CRT y ressortait à 10,8 % pour quelques centaines de Ko copiés par seconde. L'étiquette du rapport est corrigée.
2. **`D2_TIMESAMP` (échantillonneur horaire)** lit `emu->ip`, que le dynarec n'écrit qu'aux sorties vers le dispatcher, en pratique au retour de trap. Il nomme « la région après le dernier trap ». Les adresses `b90xxxx` sont la **fenêtre des traps** (`0x0F200000`), pas du code du jeu.
3. **Les combiner ne corrige rien** (passe 12). Le budget fin rafraîchit l'EIP, mais toujours sur le bloc choisi à l'expiration : on retrouve la fréquence d'entrée. La projection y ressortait à 15 % du « temps » alors qu'elle était servie à 100 % en natif. Ce résultat est impossible, donc l'instrument est réfuté.
4. **`D2_GRPROF`** coûte ~4 ms/img sur le fil de vidage : 5 878 lectures d'horloge × 697 ns.
5. **Oracle pixel qemu Glide non déterministe** : deux jambes témoins identiques donnent deux empreintes différentes, **y compris sur un binaire `HEAD` propre**. Le défaut est préexistant, non introduit par l'audit. Tant qu'il n'est pas corrigé, la preuve des portages repose sur les oracles croisés de retour.
6. **`[CELDATA]` (`D2_CELWATCH`)** ne publie rien sous Glide.

**Conséquence :** le dépôt n'a aujourd'hui **aucun instrument de temps par fonction** fiable. Les seules preuves solides de coût sont les A/B à une variable (texture 4 → 16 Mio, Perspective ON/OFF).

---

## 5. Cause racine du régime lent : mémoire texture annoncée

Désassemblage du gestionnaire de textures du jeu (`Game+0x1096e0..0x109860`) :

- `grGet(GR_NUM_TMU)` → `[0x87efb4]`. Notre DLL répondait 1 à toute question `grGet`.
- Avec 1 TMU : cache sprites = `min(TMU/2, 3 Mio)`, cache tuiles = `min(TMU/4, 1 Mio)`.
- 4 Mio annoncés → 2 + 1 Mio. Le jeu de tuiles et sprites de l'acte V ne tient pas dans 2 Mio : éviction puis renvoi à chaque image, **109 textures et 3 Mo par image**, 60 ms sur le fil de vidage.
- **Toute valeur ≥ 6 Mio donne le même résultat** (3 + 1 Mio). Le nombre est seulement *annoncé* : aucune mémoire hôte ne le suit, les textures vivent dans l'atlas GXM (32 Mio de CDRAM, budget séparé et inchangé). Coût invité : les tables de créneaux du jeu, quelques centaines de Ko au plus. `heap=` lit la même valeur à 4, 16 et 32 Mio.
- Défaut retenu : **16 Mio** (`tools/rt_boot.cpp`). Le plan d'attribution de la RAM (tas 56,9 / VA 160 / arène 259 / atlas 32 CDRAM / JIT 32) n'est pas touché.

---

## 6. Chemin texture et rafales

### 6.1 Rafales

Les chutes ponctuelles coïncident avec des rafales de téléversements, de 16 à 511 textures par image, à l'arrivée de nouveaux monstres ou d'une nouvelle zone. Le chargement de contenu (RLE, explode, DCC) monte en même temps.

À 16–32 Mio, 77–96 % de ces textures sont **déjà dans l'atlas hôte** (renvoyées par le jeu). Le coût se partage entre :
- **le fil de vidage** : ~330 µs par texture, dont 98 % de hachage ;
- **le fil de jeu** : ~260 µs par texture, dont la préparation invitée et la copie dans le tampon.

### 6.2 Hachage NEON

- `tex_hash` mode 1 est réécrit en NEON, **bit-identique**. La vérification se fait à chaque démarrage sur 8 tailles, octets pseudo-aléatoires.
- Banc console : scalaire 5,7–6,2 ns/o, NEON 2,6–2,9 ns/o, memcpy 2,2 ns/o.
- En jeu, le gain n'est que de −25 % (330 → 260 µs par texture) : le transfert de cache entre cœurs domine.
- Depuis, le fil de vidage n'est plus sur le chemin critique : le jeu l'attend 0,1 ms/img.

### 6.3 Essai 2 TMU (non retenu)

- **Plages identiques** (passe 6) : le jeu place ses caches de tuiles « sur la TMU1 » dans la même plage que les sprites. Écrasement mutuel, 105 renvois par image en permanence.
- **Plages distinctes** (passe 7) : thrash supprimé, mais `tmu1: up=0 liaisons=0`. Le jeu ne raisonne qu'en adresses, et un cache sprites de 24 Mio ne change rien aux rafales.
- Conservé derrière `D2_GLNUMTMU`, défaut 1. La DLL ring annonce désormais une plage distincte par TMU (`grTexMin/MaxAddress(tmu)`).

---

## 7. Perspective et portages natifs

### 7.1 Coût de l'option Perspective (A/B à une variable, même partie)

|  | Perspective ON | Perspective OFF |
|---|---|---|
| Fil de jeu (c0) | 91–95 % | 60–67 % |
| Sommets par image | ~21 000 | ~9 000 |
| Fil de vidage | 23 ms/img | 13–15 ms/img |

L'option est conservée (choix du joueur). La valeur absente du registre retombait sur 1 sans que personne ne la choisisse (champ non initialisé côté jeu). Elle reste pilotable par `D2_PERSPECTIVE` et par les options vidéo du jeu.

### 7.2 Portages réalisés (intrinsèques sans trap, `D2_INTRIN=1`)

| Cible | Rôle | Oracle console (test preuve) | Oracle qemu | Servis en jeu |
|---|---|---|---|---|
| `0x50dd60` proj | projection monde → écran | 9,15 M / **0** | 8,50 M / 0 | 100 % |
| `0x50dc30` lut | LUT couleur par sommet | 3,29 M / **0** | 4,41 M / 0 | 100 % |
| `0x475aa0` lgrid | cellule de grille de lumière | 2,32 M / **0** | 1,40 M / 0 | 100 % |
| `0x4744b0` lfill | remplissage de rectangle de la grille | 2 409 / **0** | 8 756 / 0 | 100 % |
| `0x50dbe0` lut2 | LUT couleur des tuiles subdivisées | 288 930 / **0** | jamais appelée au camp | 100 % |

Le format est comparaisons / divergences. Chaque cible est protégée par une empreinte d'octets à l'entrée, refusée individuellement si elle ne correspond pas. Sa capture est vérifiée par `tools/verif_intrin_capture.py`.

Le bilan de gain est **non mesurable** : fil de jeu 85–93 % et 19–23 img/s avant comme après. Le classement qui les désignait mesurait des fréquences d'entrée (§4). `D2_INTRIN` reste **désactivé par défaut** dans le build.

### 7.3 Les fonctions « quad Perspective »

La rétro-ingénierie (spéc. complète hors dépôt, `quad_spec.md` / `quad_mech.md`) donne trois fonctions, pas quatre :

- **F1 `0x50acc0`** : quad simple, `ret 0x14`.
- **F2 `0x50ca00`** : tuile subdivisée, 15 sommets, `ret 0xc`.
- **F3 `0x50cf70`** : variante, `ret 0x10`.

**Aucune n'est une feuille** : chacune contient le chemin texture et des appels d'état Glide. Le seul découpage possible est un hook avec `redirect_next`, soit un trap de ~1–3 µs par quad, pour un reste à gagner du même ordre (les feuilles sont déjà natives).

Côté x87 (dynarec compilé `x87double=0`, `fastround=1`) : toutes les opérations rencontrées se reproduisent en `float` IEEE bit à bit.

Décision : **non porté**, faute de gain démontrable (§8).

---

## 8. Répartition réelle des appels (passe compteurs, acte V, Perspective ON)

| Fonction | Appels par image |
|---|---|
| projection (native) | 16 000 – 20 600 |
| LUT couleur (native) | 5 100 – 8 000 |
| LUT bis (native) | 250 – 1 230 |
| **F1** quad simple | **0** |
| F2 tuile subdivisée | 4 – 23 |
| F3 variante | 67 – 89 |
| texture de tuile `0x50a450` | ~80 |
| texture par cellule `0x50fbd0` | 365 – 584 |
| **échec du cache de tuiles** `0x50f820` | **4 – 21** |
| liaison de texture `0x50fad0` | 104 – 128 |

- F1 ne tourne jamais : le portage des « quad » aurait visé du code mort.
- Le cache de tuiles réussit presque toujours. La rafale à 511 textures est un chargement ponctuel de contenu neuf, pas un coût permanent.
- La saturation du fil de jeu est **répartie** sur des dizaines de milliers de petits appels par image. Un compte ne dit pas lequel coûte du temps.

---

## 9. Crash du 25/09 (884 s de jeu) et correctif

- **Dump** (`psp2core-1790373637…`) : faute dans le fil de jeu, dans newlib (`__vita_fd_grab`, lecture à l'adresse 4), `x86-insn=0`. Ce n'est ni le code du jeu ni le dynarec.
- **Séquence** : sauvegarde puis chargement d'automap (`jujd.map`/`.ma0`) au changement de niveau. Entre l'ouverture de `jujd.map` et sa lecture, le fil anti-famine écrit dans le journal. La `FILE*` du jeu ressort en erreur (`SHORT read … err=1`), puis le processus avorte.
- **Cause retenue** : course sur les tables `FILE*` et descripteurs de newlib entre le fil de jeu (shims fichiers) et les fils de service qui journalisaient par `fopen`/`fclose`. C'est rare, et favorisé par le volume de journal des instruments.
- **Correctif** : `wx86_vita_progress` écrit en `sceIoOpen`/`sceIoWrite`/`sceIoClose` (SceUID, hors newlib). Ajouts : `fichiers-ouverts=` et la trace `file C`.
- **Vérifié sur console** : 3 parties avec changements de niveau, 0 crash, `fichiers-ouverts=10` stable.

---

## 10. État du dépôt et de la console

- **Rien n'est commité.** Les modifications (instruments, correctifs, portages, défaut 16 Mio) sont dans l'arbre de travail : `src/glide_ring/*`, `src/runtime/*`, `src/platform/vita_present.cpp`, `tools/*`, et le sous-module `third_party/winx86` (journal en `sceIo`).
- **Console** : eboot instrumenté `7ece83d9…` (build `-dirty`), DLL ring modifiée (l'ancienne est en `1.14d/glide3x.dll.avant`), `env.txt` de mesure. L'`env.txt` du joueur est sauvegardé en `env_joueur.txt`.
- **Validation** : qemu `rt_boot_arm_check` PASS(natif) sur l'état final ; oracles croisés verts (qemu et console) ; oracle pixel qemu non concluant (§4.5).

---

## 11. Prochaines étapes proposées

1. **Instrument de temps inclusif par fonction** : une sonde d'entrée qui arme un trap de retour et chronomètre, activable par fonction. C'est la pièce manquante pour choisir un portage sans tâtonner. Cibles de la première passe : F3, `0x50fbd0`, `0x50fad0`, dispatch de dessin `0x4f6760`, boucle `0x4df1c0`.
2. **Consolidation** :
   - commit des instruments et du défaut 16 Mio ;
   - documentation des knobs et des pièges du §4 dans `tools/bancs/README.md` et `docs-site/gains*.md` ;
   - `env.txt` de jeu restauré sur la console.
3. **Corriger le non-déterminisme de l'oracle pixel Glide qemu** : sans lui, aucun portage touchant au dessin n'a de preuve pixel.
4. **Rafales de contenu neuf** : le seul coût non résolu, côté fil de jeu. Mesurer son temps avec l'instrument 1 avant tout portage.

---

## 12. Suite du 26/09 : `run=` exact, le journal, les dessins natifs

### 12.1 `run=` réparé : les à-coups ne sont pas du calcul invité

Passe console acte V, Perspective ON (`bancs/acte5/bp_runacc1*.txt`) avec `run=` exact (§2.2). Sur les images lentes en jeu (100–480 ms), `run=` vaut 30–55 ms, soit le calcul d'une image normale. Le surplus est dans les shims. Ce ne sont ni les E/S (0 ms), ni le vidage GPU (1–2 ms), ni `d2vGlideTexUpload`, ni le filet anti-famine.

Recalées dans le temps, **presque toutes les images lentes tombent dans une seconde où 12 à 37 lignes de journal sont écrites** (publications périodiques toutes les 10 s). Les marques du joueur suivent ces rafales. Mécanisme probable : depuis le correctif du §9, chaque ligne faisait `sceIoOpen` + `sceIoWrite` + `sceIoClose` sur la carte, souvent depuis le fil de jeu, ou le bloquait par le mutex du journal. Une partie des « rafales » des §3 et §6.1 a donc pu être gonflée par l'instrumentation elle-même.

### 12.2 Correctif du journal (à confirmer sur console)

`wx86_vita_progress` garde le descripteur ouvert et le rouvre au plus une fois par seconde. Le fichier lu par FTP n'a donc jamais plus d'~1 s de retard. `wx86_vita_progress_close()` est appelé avant la rotation `_prev` au boot. La durabilité est inchangée (les octets écrits appartiennent au noyau). Une nouvelle colonne `journal=` sur les lignes `lente#` mesure le temps passé par le fil de rendu à journaliser, attente du verrou comprise.

### 12.3 Dessins sérialisés en natif (`D2_GLNATDRAW`, défaut 1)

La DLL ring copiait les sommets de chaque dessin dans le ring en x86 traduit, mot par mot : ~570 Ko/image à l'acte V, sur le fil de jeu. `d2vGlideDraw` écrit le même enregistrement côté hôte, servi en intrinsèque (sans passage par le Bridge). La DLL garde l'ancien chemin si l'hôte ne le demande pas ; `D2_GLNATDRAW=0` y revient.

Oracle qemu `tools/oracle_natdraw.sh` : **PASS**.
- Oracle croisé (`D2_GLNATDRAW=2`) : la DLL traduite relit chaque enregistrement écrit par l'hôte et le compare à ses octets source. 140 296 enregistrements sur 1 200 images, **0 divergence** ; 642 477 sur 3 200 images, en partie, **0 divergence**.
- Géométrie (dessins, sommets, lots, recompositions) identique aux deux témoins à 1 200 images. Au-delà, même deux témoins divergent, donc la géométrie n'y conclut plus.

Gain console : **non mesuré à ce stade**. La passe console du 26/09 (`bp_journal_natdraw1.txt`) affichait `natdraw=0/img` : sur Vita, le chargeur prend **toujours** `app0:glide3x.dll` (`kernel32_modules.cpp`), et `deploy_eboot.sh` ne remplace que `eboot.bin`. La DLL de l'application datait du 23/09 : ni cette copie native, ni les modifications de DLL de l'audit (`GR_NUM_TMU`, plages par TMU) n'avaient tourné sur console. **Les passes 6 et 7 (§6.3) sont donc à reprendre.** Une DLL déposée dans `1.14d/` est ignorée. La nouvelle DLL est maintenant en place dans `ux0:app/DTWO00001/` ; l'ancienne est gardée en `glide3x.dll.0923`.

### 12.4 Journal : premier correctif insuffisant, passage en asynchrone

Avec le descripteur gardé ouvert (§12.2), `INEXPLIQUE` tombe près de 0 : le bilan de chaque image se referme. Mais `journal=` vaut encore **30 à 107 ms** sur les images des publications périodiques. Le journal passe donc en **asynchrone**. Les lignes vont dans un tampon de 256 Kio, vidé toutes les 50 ms par un fil d'écriture. L'écriture reste synchrone avant la lecture d'`env.txt`, sur `d2_crashlog`, dans le gestionnaire de faute natif, à la sortie, et quand le tampon est plein (jamais de ligne perdue). `WX86_JOURNAL_SYNC=1` revient au mode synchrone. En cas de mort brutale, les lignes non vidées restent dans le tampon, donc dans le psp2core.

### 12.5 Régression trouvée et corrigée : l'index des intrinsèques se désarmait

Passe console `bp_async1.txt` : la copie native tourne (`natdraw` ≈ 1 700/img), mais le régime tombe de ~21 à **14,5 img/s** et les traps montent de 857 à 2 578 par image. `d2vGlideDraw`, `GetTickCount` et `EnterCriticalSection` apparaissent dans `natif`, donc passent par le Bridge.

Cause : l'index direct B5 (`cpu_box86.cpp`, `rebuild_direct`) couvrait l'intervalle de tous les créneaux intrinsèques. Or `d2vGlideDraw` est alloué loin des créneaux KERNEL32 : la fenêtre dépassait 256 créneaux, l'index se **désarmait**, et la branche B5 ne retombait jamais sur la table. **Tous** les intrinsèques repassaient par le Bridge. La passe `bp_journal_natdraw1.txt` était touchée aussi (`csintrin` à 0).

Correctif : la fenêtre couvre le plus grand groupe de créneaux proches, et les créneaux hors fenêtre sont servis par la table. Vérifié sous qemu avec B5 actif et inactif : 100 % des dessins servis par l'intrinsèque (`intrinseque=` sur `[ring-final]`), `csintrin` à nouveau servi. Oracle natdraw PASS.

### 12.6 Banc reproductible : la patrouille de l'acte V

Les passes jouées à la main ne permettaient pas de mesurer un effet de quelques ms : de 16 000 à 27 000 sommets par image selon la scène. Le banc `tools/bancs/patrouille_acte5.sh` (README §1bis) fonctionne ainsi :
- il démarre la partie **sur la sauvegarde du joueur** (copie `save_banc` restaurée avant chaque passe) : `jujd`, niveau 86, Normal, acte V ;
- il vérifie l'arrivée à Harrogath par capture ;
- il fait 5 tours de patrouille (apparition → waypoint → huttes) et mesure sur les images 2600–5600.

Reproductibilité vérifiée : mêmes vues aux mêmes images d'une passe à l'autre.

### 12.7 Copie native des sommets : réfutée, désactivée par défaut

A/B entrelacé sur le banc, 2 passes par variante, console :

| `D2_GLNATDRAW` | fps (A / B) | `run`/img médian (A / B) |
|---|---|---|
| 1 (intrinsèque hôte) | 19,51 / 20,16 | 38,6 / 38,9 ms |
| 0 (copie traduite) | 20,73 / 20,49 | 33,5 / 33,2 ms |

La copie native coûte **~5,5 ms de fil de jeu par image, soit ~−3,7 % d'img/s**. Sortir du code traduit à chaque dessin (~1 700 par image) coûte plus cher que la copie qu'on évite. Défaut désormais 0 ; le knob reste pour mémoire. Candidat suivant, sans aucune sortie : la même copie en `rep movsd`, que le dynarec traduit en boucle ARM native. Oracle croisé `D2_GLNATDRAW=3` : 623 993 enregistrements, 0 divergence.

Mesure `rep movs` contre boucle (`D2_GLCOPY=boucle`) : **pas de différence mesurable**. À trajectoire égale, les écarts vont dans les deux sens (39,0 contre 41,9 ms/img, puis 41,1 contre 39,1), et les fps sont identiques. La copie des sommets n'est pas un poste significatif. `rep movs` reste le défaut : exact et sans perte.

### 12.8 Les 5 portages natifs de l'audit (`D2_INTRIN`) : négatifs

Banc de patrouille, trajectoire de référence, console :

| `D2_INTRIN` | passes | `run`/img | fps |
|---|---|---|---|
| 1 | 3 | 41,0 / 41,6 / 41,0 ms | 19,26–19,53 |
| 0 | 2 | 37,9 / 36,0 ms | 20,06 / 19,95 |

Les portages **coûtent ~4,3 ms de calcul par image (~−3,4 % d'img/s)**. Ce sont des fonctions minuscules appelées ~35 000 fois par image : chaque appel servi termine le bloc traduit et repasse par le dispatcher pour l'adresse de retour, ce qui coûte plus que le corps épargné. Ils restent **désactivés par défaut**. Le banc ne les active plus ; toutes les mesures de banc antérieures au 26/09 15 h les avaient actifs, ce qui ne change pas les comparaisons faites à configuration égale.

### 12.9 Profil par fonction (`D2_PHASEPROF_X`, une sonde par passe)

L'instrument de temps inclusif par fonction existait déjà (`D2_PHASEPROF_X=<rva,…>`, 8 fonctions au plus). Le §4 le disait manquant. Chaque sonde coûte ~10 µs par appel : ne sonder qu'une fonction peu appelée à la fois, et déduire le surcoût.

| Fonction | appels/img | inclusif/img |
|---|---|---|
| boucle de dessin `0x4df1c0` | 1 | 14,0 ms |
| F3 `0x50cf70` | 74 | 11,4 ms (~150 µs/appel) |
| texture/cellule `0x50fbd0` | 281 | ~1,6 ms (4,4 − ~2,8 de sonde) |
| dispatch `0x4f6760` | 3 758 | trop appelée pour cette sonde |

F3 est le seul candidat de portage qui paraît rentable ; voir §11 et `quad_mech.md`.

### 12.10 Cache de textures : 2 TMU annoncées (`D2_GLNUMTMU`, défaut 2)

Sur le parcours de patrouille, le jeu renvoyait **44 à 106 textures par image (1,3 à 3,3 Mo)**, dont 97 à 99,9 % étaient déjà dans l'atlas. Son cache de sprites, plafonné à 3 Mio avec 1 TMU (§5), débordait en régime permanent. Les essais 2 TMU des passes 6–7 (§6.3) n'avaient jamais tourné : la DLL de la console datait du 23/09 (§12.3). Ils sont refaits avec la bonne DLL :

| TMU annoncées | textures renvoyées/img | fil de vidage | fps | `run`/img |
|---|---|---|---|---|
| 2 (2 passes) | **1** | 8,7 / 9,1 ms | **24,00 / 23,98** | 27,9 / 28,6 ms |
| 1 (2 passes) | 46 / 53 | 23,8 / 26,1 ms | 19,68 / 19,62 | 36,2 / 37,0 ms |

**+22 % d'img/s** (plafond du jeu : 25), −8,5 ms de calcul par image sur le fil de jeu, −16 ms sur le fil de vidage. Même image à la même position (capture de fin de parcours) ; 0 liaison manquée, 0 texture périmée, tas inchangé (8/56 Mo). La TMU1 porte les tuiles. Le chiffre n'est qu'annoncé : les textures vivent toujours dans l'unique atlas GXM, indexées par (TMU, adresse). **Défaut désormais 2** ; `D2_GLNUMTMU=1` pour revenir en arrière.

La conclusion du §6.3 (« 2 TMU non retenu ») est donc **caduque** : elle reposait sur une DLL qui ne répondait jamais 2.

**Correctif associé : une texture par TMU.** Avec 2 TMU, le jeu lie les tuiles sur la TMU1 et bascule le `grTexCombine` de la TMU0 entre `LOCAL` (sa propre texture) et `SCALE_OTHER/ONE` (la texture de la TMU1), ~16 000 fois chacun sur 3 200 images. L'hôte ne gardait qu'une texture courante, celle du dernier `grTexSource` toutes TMU confondues, et ignorait `grTexCombine`. Signalement console : des Death Maulers qui clignotent. `gx_host.cpp` suit désormais une source par TMU, et c'est la fonction de combinaison de la TMU0 qui choisit laquelle est échantillonnée, comme sur le matériel. En mode 1 TMU, le comportement est inchangé (vérifié qemu : `choix-tmu1=0`).

Deux compteurs sur la ligne `gxm-flush` : `choix-tmu1` et `ancienne-regle-fausse`. Le second compte les résolutions où l'ancienne règle aurait choisi une autre texture. Sous qemu (camp de l'acte I), il reste à 0 : la scène n'exerce pas le cas. **À confirmer sur console, près des Death Maulers.**
