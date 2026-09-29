# Rapport de session — performances acte V, 25 → 27/09/2026

À lire en premier à la prochaine session. Il fait le point sur l'état du dépôt et de la console, sur ce qui est mesuré et ce qui ne l'est pas, et sur la suite.

Le détail chronologique des passes est dans `docs/audi_perf.md` (brouillon hors git, §1–§12). Ce rapport le complète pour la fin de session : atlas, F3, images lentes.

Sauf mention contraire, tous les chiffres sont des **mesures console** (PS Vita réelle) sur le banc de patrouille. Aucun n'est un chiffre qemu ou Vita3K présenté comme console.

---

## 0. En une minute

| Sujet | État |
|---|---|
| Régime lent de l'acte V (15 img/s) | **Résolu.** Mémoire texture annoncée, puis 2 TMU annoncées : ~20,0 → ~24,0 img/s sur le banc (plafond du jeu : 25). |
| Death Maulers qui clignotent | **Corrigé sous qemu** : 718 dessins avec une mauvaise texture → 0. Cause : atlas saturé à 32 Mio avec 2 TMU. **Confirmation console en attente** (test joueur). |
| Images lentes restantes (~1 %, ~70 ms) | **Cause trouvée** : 27 sur 31 sont le saut d'image volontaire de D2 quand il accumule 40 ms de retard. Les 4 autres sont des vidages GPU longs (50–120 ms). Aucune correction faite ; décision à prendre (§4). |
| F3 natif (`D2_F3NATIF=1`) | ~−2 ms de calcul par image, sans changement sur le fps ni sur les images lentes. **Désactivé par défaut**, en attente de décision ; mis de côté à la demande du joueur. |
| Intrinsèques moins chers (piste 2) | Pas de gain mesurable. |
| Commits | **Aucun.** Tout est en modifications locales, dans le dépôt parent et dans le sous-module `third_party/winx86`. |
| Console | Build instrumenté déployé (instruments inactifs sans leurs variables). `env.txt` pointe encore sur la **sauvegarde du banc** : à restaurer (§6). |

---

## 1. Le banc de mesure (à réutiliser tel quel)

`tools/bancs/patrouille_acte5.sh <LAB> [VAR=val …]`, avec le générateur `tools/bancs/gen_patrouille_acte5.py`. Mode d'emploi dans `tools/bancs/README.md`.

**Déroulé d'une passe :**
- restaure la copie de la sauvegarde du joueur (`build-vita/bancs/acte5/save_banc_ref/`, hors git, **ne jamais la commiter**) ;
- démarre en Normal ;
- vérifie l'arrivée à Harrogath sur la capture de l'image 2499 ;
- descend au waypoint, puis fait 10 tours waypoint ↔ huttes ;
- mesure les images 2800–5700 ;
- contrôle le parcours sur la capture de l'image 5760, comparée à `ref_fin_5760.png`.

**Règles apprises :**
- **Pas d'A/B dans une même partie.** Faire des passes séparées, entrelacées A B A B, deux par variante (demande explicite du joueur).
- **Pas de capture GXM dans la fenêtre** : chacune gèle le jeu ~2,8 s.
- **Environ une passe sur deux diverge** en fin de parcours (PNJ, clic dévié). Le script la rejette et relance une fois.
- **Bruit du banc** : ~1,6 % en fps, ~1 ms en `run`/img.
- **La console charge toujours `app0:glide3x.dll`.** `deploy_eboot.sh` ne remplace que `eboot.bin` : toute modification de `glide3x_ring.c` doit être redéployée à part. Envoyer la DLL sous un nom temporaire, puis la renommer par FTP (`DELE` / `RNFR` / `RNTO`).

**Défauts du banc encore ouverts :**
1. Le script écrase `ux0:data/d2vita/env.txt` et **ne le restaure pas**.
2. Les journaux des passes rejetées gardent un nom de passe valide (`bp_<LAB>.txt`). Il faudrait un suffixe `.part` ou `_rejetee`.
3. Pas de statistique de divergence ; la référence de fin n'est pas explicite en argument.
4. Leçon : des passes rejetées ont été citées comme valides, d'où un faux « +22 % entrelacé » dans la doc. C'est corrigé en « ~+20 %, non entrelacé » dans `docs-site/gains*.md`, mais `audi_perf.md` §12.10 dit encore +22 %.

---

## 2. Ce qui a été fait et mesuré

### 2.1 Instrumentation réparée

- **`run=` exact** : temps invité par fil, mesuré aux frontières de trap (`runacc`), armé par `D2_LAGWATCH`. Il valait 0 sous l'ordonnanceur natif.
- **Journal `boot_progress` asynchrone** : tampon de 64 Kio, fil d'écriture, lignes de 2 Kio au plus, vidage à la faute, au crash et à la sortie, attente bornée. Sur console, `journal=0`. Avant, les publications périodiques bloquaient le fil de jeu 30 à 107 ms. `WX86_JOURNAL_SYNC=1` revient au mode synchrone.
- **`D2_PPTRACE=1`** (avec `D2_PHASEPROF=1`) : chronologie de la boucle de D2, vidée pour chaque image lente (ligne `chrono fN`). Événements :

  | Code | Signification |
  |---|---|
  | `T` | entrée dans le tour |
  | `S` / `s` | pas de simulation |
  | `D` | dessin |
  | `F` / `f` | vidage |
  | `w` | attente sur une file de messages vide |
  | `X` | saut ONEDRAW |
  | `t[K,écoulé,a8,mode]` | état du saut d'image à la sortie du tour |

### 2.2 Cache de textures : 2 TMU annoncées (`D2_GLNUMTMU`, défaut 2)

Avec 1 TMU, le cache de sprites de D2 était plafonné à 3 Mio et débordait en permanence : ~50 textures déjà connues renvoyées par image. Avec 2 TMU :
- **1 texture renvoyée par image** au lieu de ~50 ;
- **~20,0 → ~24,0 img/s** ;
- −8,5 ms par image sur le fil de jeu, −16 ms sur le fil de vidage.

Validation : console, passes valides, **non entrelacées**. La TMU1 porte les tuiles, et le `grTexCombine` de la TMU0 alterne entre LOCAL et SCALE_OTHER. L'hôte suit maintenant une source par TMU.

### 2.3 Death Maulers qui clignotent → atlas saturé (corrigé, à confirmer)

La bêta 7 ne clignotait pas : c'était une régression. Le suivi des sources par TMU était juste mais n'était pas la cause. La cause : avec 2 × 16 Mio annoncés, l'atlas de 32 Mio saturait dans les zones sauvages, et un téléversement raté laissait l'ancien sprite lié. Corrections :
- atlas porté à **48 Mio** ;
- un téléversement raté laisse l'adresse **non liée** (dessin sans texture plutôt qu'avec un sprite périmé) ;
- invalidation quand deux plages TMU se chevauchent ;
- récupération de pages **entre classes de taille** : une classe pleine prend les pages LRU inutilisées des autres classes, au lieu de vider des textures encore utiles.

Oracle qemu `D2_GXTEXCHECK=1` : **718 → 0** dessins avec une mauvaise texture.

**Console : pas encore confirmé.** Le joueur doit retourner voir des Death Maulers et poser **L + haut** à chaque clignotement.

### 2.4 Pistes négatives (désactivées par défaut)

| Piste | Mesure | Pourquoi |
|---|---|---|
| `D2_GLNATDRAW=1` (dessins sérialisés côté hôte) | ~+5,5 ms/img, −3,8 % | ~1 700 sorties du code traduit par image coûtent plus que la copie évitée. |
| `D2_INTRIN` (5 portages de l'audit) | ~+4,3 ms/img, ~−3,4 % | Petites fonctions entières, 20 000 à 35 000 appels par image. Natives, elles ne vont pas plus vite que traduites, et chaque appel paie la plomberie. |
| `rep movs` contre boucle pour la copie des sommets | aucune différence | La copie n'est pas un poste significatif. |
| Intrinsèques moins chers (piste 2) | pas de gain | Même leçon. |

**Leçon :** un portage natif ne rapporte que si le corps épargné est gros par appel. C'est le cas de F3, pas des feuilles.

**Régression trouvée et corrigée en route :** l'index direct B5 des intrinsèques se désarmait dès qu'un créneau était alloué loin des autres (`d2vGlideDraw`). Tous les intrinsèques repassaient alors par le Bridge (21 → 14,5 img/s). La fenêtre couvre maintenant le plus grand groupe de créneaux proches ; les autres créneaux sont servis par la table.

### 2.5 F3 natif (`D2_F3NATIF`, `src/runtime/native_f3_114.cpp`)

F3 = `Game+0x10cf70`, les dalles de sol en Perspective, 74 appels par image. On porte l'**appelant**, ce qui absorbe ~30 petits appels par cellule. Les dessins passent par `gr_draw_native`, et le portage vérifie que la table Glide `[0x87eb18]` pointe bien sur `fn_drawva`.

| Mode | Effet |
|---|---|
| `D2_F3NATIF=1` | sert F3 en natif |
| `D2_F3NATIF=2` | oracle : calcule en natif sans écrire, et compare |
| `D2_F3PROF` | profil de F3 |

Résultats :
- **Oracle qemu :** 0 divergence sur plus de 82 000 cellules.
- **Console :** calcul traduit 28,0 → ~20,6 ms, shims natifs ~5,3 ms, **net ~−2 ms/img**. Le fps ne change pas (déjà au plafond en ville), ni le nombre d'images lentes.
- **Décision en suspens** (le joueur a dit de laisser F3 de côté).

---

## 3. Images lentes : diagnostic complet

Sur la passe `bp_pptr3` (valide, 23,93 img/s, `run` médian 28,4 ms), on compte **31 images de plus de 60 ms** dans la fenêtre. La passe `bp_pptr2` donne 25 images lentes, pour la même répartition.

### 3.1 27 sur 31 : le saut d'image volontaire de D2

La boucle de tour de D2 est `0x44efa0`. Son mode `[0x7a0610]` vaut 0 en solo, ce qui est vérifié sur la trace. Les variables en jeu :

| Nom | Adresse | Rôle |
|---|---|---|
| période P | `[0x70ef1c]` | 40 ms |
| base | `[0x7a0490]` | départ de l'horloge d'affichage |
| now | `[0x7a048c]` | via `[0x6cc260]` |
| K | `[0x7a0704]` | le dessin n'a lieu que si K == 0 (`0x44f278`) ; K++ à chaque tour (`0x44f2ba`) |
| écoulé `esi` | — | `now − base`, calculé en `0x44f0b3` |

Déroulé d'un tour :
- Si `esi ≥ P`, alors `base = now`.
- La simulation (`0x52fc20`) avance sur **sa propre horloge**, à 25 Hz ; `edi` indique qu'elle a fait un pas.
- **Chemin mode 0 (`0x44f1f2`)** : sans pas de simulation, K n'est pas touché, donc pas de dessin. Avec un pas de simulation :
  - `esi ≤ P` : K = 0, on dessine ;
  - `P < esi < 2P` : `base += P − esi` (le retard est reporté), K = 0, on dessine ;
  - `esi ≥ 2P` : `base = now − (P − 1)`, K = (K ≤ 1), soit 1 : **pas de dessin**.

**Ce qui se passe chez nous.** Le jeu dessine uniquement sur un pas de simulation, et chaque image qui coûte plus de 40 ms ajoute son dépassement au retard reporté. Quand le cumul atteint 40 ms (soit `esi ≥ 80` au tour suivant, car le tour de dessin dure lui-même ~40 ms), D2 saute le dessin de ce pas. Il attend ensuite le pas suivant, ~38 ms plus tard, en tournant à vide : une quinzaine de tours `w`, soit une attente sur file vide suivie d'un `Sleep` de 1 ms.

C'est la « charge normale (~32 ms) + ~38 ms d'attente » qu'on n'expliquait pas. Signature dans la trace : le dessin précédent sort avec `t[1,39,…]`, puis `S` suivi de `t[2,39|40,…]`, puis `t[3,0]`, `t[4,2]`, … jusqu'au pas suivant.

Autres points :
- `[0x7a04a8]` vaut 24 en régime (8 tours à 23). Il ne sert que dans le chemin temporel (modes ≠ 0/1), donc sans effet en solo.
- **Fréquence :** 27 sauts en 110 s, soit un à-coup toutes les ~4 s environ.
- **Fidélité :** un PC qui descend sous 25 img/s fait exactement la même chose. Ce n'est pas un bug du runtime.

### 3.2 4 sur 31 : un dessin unique très long

Exemple : `T S s D+5 F+60…126`. Le tour dessine normalement, mais le vidage (`F`) dure 50 à 120 ms. Ce n'est pas encore analysé. Hypothèses à vérifier : rafale de téléversements de textures (nouveaux monstres à l'écran), attente d'une barrière GXM, récupération de pages de l'atlas.

### 3.3 Pourquoi un plafond à 20 img/s ne marche pas

Le joueur a posé la question. D2 ne dessine que sur un pas de simulation, espacés de 40 ms. Les cadences possibles sont donc 40, 80, 120 ms… Un plafond à 20 img/s (50 ms) arrondirait chaque image à 80 ms, soit ~12,5 img/s : toutes les images deviendraient des à-coups.

---

## 4. Décisions à prendre et options

1. **Supprimer les sauts d'image en solo (option 2), encore à tester.** Dans la branche `esi ≥ 2P`, dessiner quand même : neutraliser 9 octets à `0x44f22d` (`cmp [0x7a0704],edi` + `setle dl`) pour que `edx` reste 0, donc K = 0.
   - La simulation n'est pas touchée (horloge propre en `0x52fc20`).
   - Effet attendu, **non mesuré** : les pics de ~70 ms deviennent des images de ~45 ms, et le fps gagne ~0,7 % (le temps mort de ~30 ms par saut est récupéré).
   - **Contraintes :**
     - c'est une modification du comportement du jeu, à rendre désactivable (knob) ;
     - la limiter au solo : mode 0, le seul où ce chemin s'exécute ;
     - **jamais en ligne** : Warden lit la mémoire du jeu, et la règle interdit de masquer une retouche ;
     - mesurer sur le banc en passes séparées entrelacées, et vérifier que la simulation garde son rythme (compteur de pas `S` par seconde identique).
2. **Accélérer l'image sous 40 ms.** C'est la seule voie 100 % fidèle. F3 natif (−2 ms) va dans ce sens mais ne suffit pas seul. Pour choisir le prochain poste, relancer le profil par fonction (`D2_PHASEPROF_X`, une sonde par passe).
3. **Enquêter sur les 4 vidages longs** : ajouter à la chronologie le nombre de téléversements et d'octets de ce vidage, l'attente de barrière, et les récupérations de l'atlas.
4. **F3 natif : l'activer ou non par défaut ?** Plus sûr après l'option 1 ou 2, pour voir s'il réduit la fréquence des sauts.

---

## 5. État du code (non commité)

**Sous-module `third_party/winx86`** (à commiter en premier). 4 fichiers, +276/−23 :
- `vita_host.cpp/.h` : journal asynchrone ;
- `cpu_box86.cpp` : `runacc`, index B5 par groupe de créneaux ;
- `x86emu_private.h`.

**Dépôt parent.** 24 fichiers modifiés, +1 238/−108, plus des fichiers non suivis :

| Fichier(s) | Contenu |
|---|---|
| `src/glide_ring/*` | 2 TMU, source par TMU, atlas 48 Mio, récupération entre classes, échec de téléversement → non lié, chevauchement TMU, oracle `D2_GXTEXCHECK` |
| `src/runtime/phase_hooks.cpp`, `frame_profile.cpp` | PPTRACE, `run` exact, `journal=`, ligne `lente#` |
| `src/runtime/native_f3_114.cpp` (nouveau), `d2_intrin_114.cpp` | F3 natif, intrinsèques |
| `src/platform/d2_boot_config.cpp` | rotation du journal, démarrage asynchrone |
| `tools/bancs/patrouille_acte5.sh`, `gen_patrouille_acte5.py` (nouveaux), `README.md` | le banc |
| `tools/oracle_natdraw.sh` (nouveau), `tools/oracle_intrin.sh` | oracles. `oracle_intrin.sh` : le verdict et l'en-tête restent à corriger. |
| `ROADMAP.md`, `docs-site/gains*.md` | à jour pour 2 TMU, atlas et F3. Les images lentes (§3) ne sont **pas encore documentées**. |
| `docs/audi_perf.md`, ce rapport | brouillons, **hors git** |

**Constructions :**
- chaîne Vita (`third_party/winx86/build.sh`, puis `tools/build_rt_boot_vpk.sh`) : verte au dernier build du 26/09 à 23:45 ;
- oracle qemu ARM : vert ;
- outils hôte CMake : **pas relancés depuis les dernières modifications**, à vérifier avant le commit.

**Avant de commiter :** relire le diff (aucun fichier Blizzard, aucune sauvegarde, aucune clé). Ne pas inclure `docs/audi_perf.md` ni ce rapport.

---

## 6. Console : ce qui y est en ce moment

- **`eboot.bin`** : build du 26/09 23:45, avec F3 (désactivé), PPTRACE (inactif sans `D2_PPTRACE`) et le journal asynchrone.
- **`app0:glide3x.dll`** : la DLL ring actuelle (2 TMU, atlas). L'ancienne est gardée en `glide3x.dll.0923`.
- **`ux0:data/d2vita/env.txt`** : **l'environnement du banc**, avec `D2WRITE=…/save_banc`, le script de patrouille `D2SCRIPT` et les diagnostics. **Le joueur qui lance le jeu tel quel rejouera la patrouille sur la sauvegarde du banc.** À remettre à son `env.txt` normal ; en demander le contenu au joueur si aucune copie locale n'existe.
- La sauvegarde du joueur elle-même n'est pas touchée : le banc écrit dans `save_banc`.

---

## 7. Ordre proposé pour la prochaine session

1. Restaurer l'`env.txt` du joueur, ou corriger le banc pour qu'il le fasse (§1, défaut 1).
2. Obtenir la confirmation console des Death Maulers (§2.3).
3. Tester l'option 1 du §4 sur le banc, si le joueur la valide, en comparant le nombre d'images de plus de 60 ms et le fps.
4. Instrumenter les 4 vidages longs (§4.3).
5. Mettre la doc à jour (images lentes, correction du +22 % dans `audi_perf.md`), vérifier le build CMake, puis commiter sur demande : sous-module d'abord, puis le parent.

Journaux de référence (hors git) : `build-vita/bancs/acte5/bp_pptr2.txt` et `bp_pptr3.txt` pour les images lentes, `bp_f3_*.txt` pour F3, `bp_maulers1.txt` pour l'atlas.
