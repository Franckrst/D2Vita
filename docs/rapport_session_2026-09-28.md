# Rapport de session — à-coups de l'acte V, nuit du 27 au 28/09/2026

Brouillon hors git, comme `docs/rapport_session_2026-09-27.md`, qu'il prolonge. Mandat du joueur : pistes 1, 4, 2, 3 de la proposition du 27/09 (vidages longs, profil de temps par fonction, rafales de textures, marge sous 40 ms), en autonomie, console pilotée par VitaCompanion.

Sauf mention contraire, **tous les chiffres sont des mesures console** (banc de patrouille de l'acte V, fenêtre des images 2800–5700). Les validations qemu sont signalées comme telles.

---

## 0. En une minute

| Sujet | Résultat |
|---|---|
| « Vidages longs » (piste 1) | **Mal nommés.** Ce ne sont pas des vidages GPU : le dessin attend (`WaitForSingleObject`) le fil de chargement de D2, qui lit d2exp.mpq sur la carte (2,6–7 ms par lecture). 2 à 3 images par passe. |
| Profil de temps par fonction (piste 4) | **Fait.** Saveur de mesure `D2VPK_BLKSAMP=1` : chaque bloc traduit publie son adresse à l'entrée, un fil l'échantillonne à intervalle fixe. Premier profil de temps fiable du jeu. |
| Ce que le profil a montré | ~10 % du temps d'image part en **plomberie de trap** (~1000 allers-retours par image, ~4 µs chacun), 4,4 % dans la copie des sommets de notre DLL ring, F3 natif ne rapportait presque rien parce que chaque cellule visible fait un aller-retour de trap. |
| Gains validés console | Copie des sommets en ligne (DLL) : **−0,9 ms/img**. Intrinsèques servis en ligne (`D2_INTRINLINE`) : **−0,9 à −1,25 ms/img**, c0 −2 points. `wsprintfA` rapide : oracle 155 520 appels, 0 écart. |
| Rafales de textures (piste 2) | Plus visibles sur le banc depuis les 2 TMU (1 texture renvoyée par image). Les images lentes « autres » restantes sont le chargement de fichiers, pas des textures. |
| Images lentes | ~20–27 par passe, dont **90 % de sauts d'image de D2** : inchangé à ce stade — les gains de 1 à 2 ms ne suffisent pas à passer sous le seuil de 40 ms de façon stable. |

_(sections détaillées ci-dessous, complétées au fil de la session)_

---

## 1. Instruments ajoutés

| Instrument | Où | Ce qu'il dit |
|---|---|---|
| `natif-image fN` (`D2_NATPROF` + `D2_LAGWATCH`) | moteur `bridge.cpp` (`wx86_natprof_frame_cut`), `frame_profile.cpp` | Pour chaque image lente, les shims où **le fil de jeu** a passé son temps. Attribution par fil (drapeau thread-local lu seulement sous `D2_NATPROF`). |
| `D2_IOTRACE=<image>` | `io_stat.cpp` | Une ligne `iotr` par lecture : fichier, offset, taille, RAM ou carte, µs. |
| Saveur `D2VPK_BLKSAMP=1` + `D2_TIMESAMP=<us>` | moteur (`dynarec_arm_pass.c`, `bridge.cpp`, `cpu_box86.cpp`, `dynarec.c`), `vita_present.cpp`, `tools/bancs/blksamp_fonctions.py` | Temps par bloc / fonction / shim / **étape moteur d'un trap** / phase d'un portage natif, sans lecture d'horloge. Jamais livrée. |
| `D2_TRAPTOP=1` | `vita_present.cpp` | Top 12 des créneaux de trap par image (shims ET intrinsèques). |
| `dbgetblock:` (saveur blksamp) | moteur `dynablock.c` | Appels de `DBGetBlock` par image, passages par le re-hachage. |
| `tools/bancs/a_coups.py` | — | Par passe : fps, `run`, images > 60 ms, sauts d'image de D2 contre autres, pire image, route. |
| `taille-sommet=` | `gx_host.cpp` (lignes `ring:` et `[ring-final]`) | Taille de sommet courante de la DLL ring. |
| `jpline` 220 → 1024 caractères | `jit_profile.cpp` | Les lignes `chrono` étaient tronquées. |

**Banc :**
- restaure l'`env.txt` du joueur à la sortie (défaut 1 du rapport du 27/09) ;
- journaux des passes rejetées renommés `_rejetee` (défaut 2) ;
- **deux routes** : l'écart de fin de parcours est bimodal (~1 ou ~16,7 sur 30 passes). La patrouille bifurque sur un seul clic et chaque branche est reproductible (écart ≤ 1,6 entre passes B). Une passe A ou B est mesurée et étiquetée ; les comparaisons se font à route égale. Le rendement du banc passe de ~50 % à ~95 %.

## 2. Piste 1 — les « vidages longs »

Chronologie + `natif-image` sur les passes `diag1`, `diag2` : dans les images à `D+5 F+60…126`, c'est le **dessin** qui dure, pas le vidage (`F→f` = 0). Le fil de jeu y passe 38–88 ms dans `WaitForSingleObject` : il attend le fil de chargement de D2, qui fait les `ReadFile`.

Trace `D2_IOTRACE` : ~25 lectures dans d2exp.mpq par événement, fichiers différents (contenu neuf), 2,6 à 7 ms par lecture carte ; les secteurs d'un même fichier sont lus en 2–3 fois (la pré-lecture sur saut est de 8 Kio).

**Non corrigé** : piste restante `D2_READAHEAD_JUMP=32` (une seule lecture carte par fichier), à mesurer.

## 3. Piste 4 — le profil de temps

Profil `blksamp1` (avant les changements, console) : F3 9,8 %, projection `0x10dd60` 9,2 %, **DLL ring 9,2 %** (dont la copie `rep movsd` 4,4 %), fenêtre de trap 6,3 %, `wsprintfA` 1,45 %, attentes (Sleep/Peek/WFSO) ~14 %.

Profil avec F3 natif et marqueurs de phase/étape (`blksamp4/5/6`) :
- F3 natif coûte autant que F3 traduit (~17 % du temps) : culling de ~2 000 cellules/image, dessins, et **~380 allers-retours de trap par image** (un par cellule visible).
- Allers-retours de trap, ~1000/image, ~4 µs chacun, ~10 % du temps : recherche de bloc `DBGetBlock` 2,5 % (succès de table, pas de re-hachage — défauts de cache probables), verrou + intrinsèque 3,6 %, libération 1,1 %, entrée/queue du Bridge 1,8 %.
- Top des traps : GetTickCount ~322/img, Enter/LeaveCriticalSection ~150 chacun, f3_cont 230–410, f3 73, wsprintfA 15–40.

## 4. Changements et mesures

### 4.1 DLL ring : copie des sommets en ligne (`D2_GLRUNS`, défaut en ligne)

- Regroupement des sommets contigus : **neutre** (les bandes Perspective sont en zigzag, jamais contiguës).
- `REP MOVSD` par paquets NEON (`D2_REPMOVS=1`, moteur) : exact (oracle, sabotage détecté), **neutre** sur console. Laissé désactivé.
- **Copie en ligne déroulée par taille de sommet** : route A, 27,5 → 26,6 ms/img (3 + 2 passes). Oracle qemu en partie : 1,07 M dessins, 0 écart ; sabotage détecté (639 868 écarts).
- Découvert au passage : l'oracle qemu « 1200 images » ne quitte pas les menus. Les oracles de dessin doivent tourner ≥ 4000 images avec `D2_PERSPECTIVE=1`.

### 4.2 `wsprintfA` sans allocation

Formateur réécrit (mêmes conversions, `snprintf` gardé pour les formes à drapeaux). Oracle console `D2_WSPRINTF_VERIFY=1` : **155 520 appels comparés, 0 divergence** (qemu n'appelle jamais `wsprintfA` sur son parcours).

### 4.3 Intrinsèques en ligne (`D2_INTRINLINE`, moteur)

Le stub de trap d'un créneau intrinsèque devient un stub d'appel natif box86 (`CC 'S' 'C' wrapper créneau`) : l'intrinsèque est servi depuis le bloc traduit, et le retour à l'appelant passe par la pile CALLRET (ou la table de sauts), sans sortir de `DynaRun` ni refaire la recherche de bloc. Un refus (section critique contendue) retombe sur le Bridge sans rappeler l'intrinsèque.

- qemu : portes PASS ; coop 1200 images, géométrie identique au témoin ; en partie 4000 images, 0 écart de dessin. Défaut trouvé et corrigé en route : la lecture du stub par le dynarec ignorait l'arène (`DYN86_G2H`).
- Console, 8 passes entrelacées : route A 26,8/26,6 → 25,7/25,9 ms ; route B 27,1/27,0 → 25,8/25,8 ms ; c0 76–77 % → 74–75 %.

### 4.4 F3 natif servi en ligne et allégé (`D2_F3NATIF=1`, toujours opt-in)

Profil par phase (marqueurs `PH()` sous la saveur blksamp) : culling ~1,9 ms/img (~2 000 cellules), dessins ~1,3 ms, grille ~0,7 ms, et le retour de trap par cellule visible.
- entrée et continuation servies en ligne (`set_inline_shim`, liste blanche : shims natifs qui ne bloquent jamais) ;
- bandes écrites directement en mémoire hôte (`gr_draw_native_hv`) ;
- coins des cellules rejetées différés (seuls ceux de la cellule visible et de la dernière cellule sont observables) ;
- visibilité en entiers quand toutes les valeurs tiennent sur 24 bits (conversion en float alors exacte).

qemu en partie : oracle `D2_F3NATIF=2` 38 342 appels / 168 503 cellules / 98 Mo, 0 divergence ; `D2_F3CHECK=1` 436 538 bandes, 0 écart.
Console, route B, entrelacé : 25,6 ms (F3 traduit) → 24,4 / 24,3 ms ; c0 74 → 72 %. ~10 M cellules par passe, 0 repli.

⚠️ Avec F3 natif en ligne, le temps de F3 compte désormais dans `run=` (il ne passe plus par la fenêtre de trap) : comparer c0 ou le fps, pas `run` seul, contre une passe F3 natif hors ligne.

### 4.5 Pré-lecture 32 Kio sur saut (défaut)

À route égale : lectures carte 65–71 → 25–27, temps carte 208–223 → 130–197 ms. Aucune RAM en plus.

### 4.6 Passe de contrôle avec les seuls défauts

`bp_final_a` (route A, aucun knob) : 25,9 ms/img, c0 74 %, 25 images > 60 ms dont 23 sauts. Début de nuit, même route et même instrumentation : 27,4–27,6 ms, c0 77–78 %.

## 5. Ce qui n'a pas bougé : les sauts d'image de D2

Toutes configurations confondues : 20 à 27 images > 60 ms par passe, dont ~90 % de sauts d'image (retard cumulé ≥ 40 ms). Les ~3 ms gagnés ne suffisent pas : la charge d'une image de dessin reste proche de 40 ms (calcul invité ~24 ms + shims + vidage), et un pic ponctuel suffit à déclencher le saut.

Options, à décider par le joueur :
1. **Activer F3 natif par défaut** (−1,25 ms de plus, exact, prouvé).
2. **Option 1 du rapport du 27/09** (dessiner quand même en solo) : seule voie qui supprime les sauts à coup sûr ; modifie le comportement du jeu (knob, solo, jamais en ligne).
3. Continuer à creuser la marge : la projection `0x10dd60` (2,5 % hors F3), la grille de lumière `0x075aa0` + `0x0de260` (~4,7 %), le dispatch `0x0f6760`, et les ~300 traps restants par image (autres shims).

## 6. État

- **Branche** `perf/acte5-a-coups` (dépôt parent et sous-module), **rien de commité**.
- **Console** : eboot final (défauts : intrinsèques en ligne, copie en ligne, pré-lecture 32 Kio ; F3 natif désactivé), DLL ring en ligne (`app0:glide3x.dll`, anciennes gardées en `.0923` et `.0926`), **`env.txt` du joueur restauré** (D2NET + D2_ALLOW_OFFICIAL).
- **Constructions** : chaîne Vita verte, chaîne hôte CMake verte, porte qemu `rt_boot_arm_check` PASS (défaut et `D2_INTRINLINE=0`).
- Journaux (hors git) : `build-vita/bancs/acte5/bp_{diag1,diag2_rejetee,blksamp1..6,ix*,il*,cp*,rm*,f3o*,ra*,final_a,wsv1}.txt`, profils `blksamp1_fonctions.txt`, `blksamp2_fonctions.txt`.
- Avant de commiter : relire le diff (aucun fichier Blizzard, aucune sauvegarde), ne pas inclure `docs/audi_perf.md` ni les rapports de session.

## 7. Pièges de la nuit

- L'oracle qemu à 1200 images **ne quitte pas les menus** : un oracle de dessin doit aller à 4000 images avec `D2_PERSPECTIVE=1`, et être prouvé par un sabotage.
- `qemu` n'appelle jamais `wsprintfA` sur son parcours : l'oracle de ce shim est console.
- Le dynarec lisait les octets d'un stub d'appel natif sans passer par l'arène (`DYN86_G2H`) : invisible dans la porte sans arène, SIGSEGV en coop avec arène.
- `pkill -f <motif>` et `pgrep -f <motif>` se reconnaissent eux-mêmes dans la ligne de commande du shell qui les lance.
- `rt_now_us` coûte ~1,3 µs sur Vita : `D2_F3PROF` gonflait ce qu'il mesurait ; les marqueurs d'échantillonneur ne coûtent qu'une écriture.
