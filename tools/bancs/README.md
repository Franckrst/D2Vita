# tools/bancs — les bancs de mesure D2Vita (console, qemu, oracle)

Tout ce qui sert à **mesurer** vit ici. La synthèse publique des chiffres qui en sont
sortis est dans [`docs-site/gains.md`](../../docs-site/gains.md).

## 0. Variables communes (aucun chemin en dur)

| variable | défaut | rôle |
|---|---|---|
| `VITA_IP` | `10.113.1.159` | adresse de la console |
| `VITA_FTP_PORT` / `VITA_VC_PORT` | `1337` / `1338` | VitaShell FTP / VitaCompanion (`launch`, `destroy`, `nosleep on`) |
| `VITA_TITLE` | `DTWO00001` | titre lancé/déployé |
| `D2VITA_WORK` | `build-vita/bancs` (console) ou `build-arm/bancs` (qemu) | dossier de travail : envs déposés, journaux `bp_<LAB>.txt`, suites `.log`, captures `shots/` |
| `REFS` | `$HOME/d2-vita-refs/1.14d` | dossier du jeu pour qemu (**avec** `glide3x.dll` pour les bancs ring) |
| `BIN` | `build-arm/oracle_arm` ou argument | binaire ARM pour qemu (`tools/build_oracle_arm.sh`, `tools/rt_boot_arm_check.sh` produit `build-arm/rt_boot_arm`) |

Sur la console, tout passe par FTP avec le préfixe **`ux0:`** (`ftp://IP:1337/ux0:/...`, et
répété dans les `-Q "-DELE ux0:/..."`) : sans lui FTPVita répond `550` et le script croit
avoir déposé (cf. `tools/push_console.sh`). Rien n'est jamais déposé sans relecture : la chaîne
compare le md5 de l'eboot relu.

Jamais de binaire Blizzard (`*.exe`, `*.dll`, `*.mpq`) ni d'eboot dans git — `.gitignore`
bloque `*.bin`, `*.dll` ; les md5 utiles vont dans les docs.

## 1. Une jambe console — `passe_console.sh`

```
NOCAP=1 GLIDE=1 bash tools/bancs/passe_console.sh <LAB> [VAR=val ...]
```

> **Ordonnanceur.** Le portage ne cible plus que le **natif**, qui est
> désormais le **défaut** : `D2SCHED` absent ⇒ natif. Les `D2SCHED=native` explicites
> ci-dessous restent corrects, simplement redondants. `D2SCHED=coop` existe toujours
> mais comme **instrument de banc** — c'est l'ordonnanceur dont dépendent les 25
> oracles à horloge virtuelle, que le natif refuse par construction. Un run coop est
> estampillé comme tel dans `crash.log` : il ne doit jamais se relire comme nominal.
>
> Conséquence : les valeurs absolues d'avant cette date venaient de passes coop et
> **ne se comparent pas** aux chiffres d'une passe natif.

Une passe = un `env.txt` écrit à partir du **socle natif** des campagnes de banc
(`D2SCHED=native`, `NATIVECELLLOOP=1`, `D2_CALLRET=1`, `D2_NOPUMPWAIT=1`,
`WX86_FRAMEPROF=1`, `MAXFRAMES=6200`, patrouille `d2script_camp_patrouille.txt`) + les options :

- `NOCAP=1` (défaut) : `D2_NOCAP=1`, banc **déplafonné** (sinon le jeu se cale à 25 img/s — 25
  est le maximum visible, tout gain au-delà est invisible) ;
- `GLIDE=1` (défaut) : `D2ARGS=game.exe -3dfx` (chemin ring → sceGxm ; la DLL ring doit être dans
  `ux0:data/d2vita/1.14d/glide3x.dll`) ; `GLIDE=0` = chemin GDI ;
- `MAXFRAMES=`, `SCRIPT=` (autre script de patrouille).

Les knobs en argument (`D2_GXMASYNC=2 D2_GXMCLEAR=plat …`) sont ajoutés tels quels. Le script
dépose l'env, purge `boot_progress.txt`, `launch`, puis **attend que `frames=` cesse d'avancer**
(3 relevés à 15 s ; sous natif + `MAXFRAMES` le `CLEAN EXIT` n'arrive pas toujours) ou un
`CLEAN EXIT`, rapatrie le journal en `$D2VITA_WORK/bp_<LAB>.txt` et fait `destroy`.

Le socle de jeu (`D2_CELLOPT=63 D2_MMUSTACK=3 D2_MMUFOLD=1 D2_BUDGETTAIL=1 D2_CSINTRIN=1 WX86_B5=1`)
n'est **pas** dans le socle de banc : le passer explicitement (c'est `COMMON` dans les chaînes de
banc), pour qu'une jambe « sans » reste possible.

Pendant une campagne, garder la console éveillée : `nohup bash tools/bancs/nosleep_console.sh &`
(un sommeil au milieu d'une fenêtre détruit la passe).

## 2. Une suite — `suite_console.sh`

```
SUITE=s_clear bash tools/bancs/suite_console.sh "A1:D2_GXMASYNC=2" "B1:D2_GXMASYNC=2 D2_GXMCLEAR=plat" "A2:D2_GXMASYNC=2"
```

Passes en séquence, journal `$D2VITA_WORK/<SUITE>.log`. Toujours **entrelacer** (A/B/A ou
A/B/A/B) : la dispersion témoin-témoin du banc est ~1 % en patrouille, 8 % en jeu libre, et
la dérive thermique/session 1,5-10 %. Un chiffre absolu ne traverse pas une session.

## 3. Une chaîne — `chaine_console.sh` (déploiement + jambes + restauration)

```
EBOOT=build-vita/eboot.bin DLL=build-glide/glide3x.dll SUITE=c42 \
  bash tools/bancs/chaine_console.sh "R_A:$G $COMMON" "R_B:$G D2_REPLAY60=1 $COMMON"
```

1. attend la console (`alive` = listing FTP, jusqu'à `ATTENTE_CONSOLE` s), `destroy` ;
2. si `EBOOT=` : dépose `app/<TITLE>/eboot.bin`, **relit et compare le md5** (abandon sinon), puis
   les `shaders/*.gxp` (`SHADERS=0` pour sauter, `SHADERS_DIR=`) ;
3. si `DLL=` : dépose la DLL ring en `1.14d/glide3x.dll` ;
4. `fetch_shots PREALABLE` (vide les `shot_*.bmp` restants) ; puis pour chaque jambe :
   `passe_console.sh` + rapatriement des captures (`D2_GXMSHOT=<img>`) en PNG dans
   `$D2VITA_WORK/shots/<LAB>_shot_<n>.png` + contrôle `alive` — **arrêt si la console a disparu**
   (un gel GPU peut rendre la console injoignable) ;
5. `DLL_RETIRER=1` retire la DLL (env GDI) — par défaut elle reste (l'env de jeu est Glide) ;
6. restaure `ENV_RESTAURE` (défaut `tools/bancs/env_jeu_glide.txt`) dans `env.txt`.

La capture BMP est **la seule preuve de fidélité** du chemin GPU (« un compteur ne voit pas une
texture fausse ») : une jambe chiffrée sans capture vérifiée à l'œil ne vaut pas un verdict.

## 4. Les deux envs de jeu

- `env_jeu_glide.txt` — **env de jeu retenu** : socle natif + leviers de jeu +
  `-3dfx D2_GLIDERING=1 D2_GLIDEGXM=1 D2_GXMASYNC=2 D2_GXMCLEAR=plat WX86_YIELD=2 D2_ONEDRAW=2
  D2_REPLAY60=1` (rejeu 60 Hz interpolé, +40 ms de latence ; `D2_REPLAY60_MODE=extrap` en option).
  Suppose la DLL ring en `1.14d/glide3x.dll` et les shaders déposés.
- `env_jeu_gdi.txt` — secours GDI : même socle, `D2_CELLPAR=1`, pas de `-3dfx`. C'est
  `ux0:data/d2vita/env_gdi.txt` sur la console.

Les deux sont sans `D2SCRIPT` (jeu libre) ; `MAXFRAMES` absent.

**Route « panneaux »** (`d2script_camp_panneaux.txt`) : même entrée en partie que la
patrouille, puis `3600:chr:27` — les cinq `Enter` de l'entrée laissent la boîte de chat
ouverte et, tant qu'elle l'est, aucune touche-lettre injectée n'agit ; seul le WM_CHAR
Échap (`chr:27`) la ferme — puis `key:73`/`key:84`/`key:67` (I, T, C) et `snap`. Sous Glide
`snap` déclenche aussi la capture GPU (`shot_<image>.bmp`, 960×544, 1:1) ; sous GDI il
vide le DIB, mais le chemin GDI ne redimensionne pas son DIB : à 960 l'image est
cisaillée, la référence exacte GDI n'est valable qu'avec `D2_RES=0`.

## 5. Agrégation — `agg_gxm.py`, `agg_natif.py`

```
python3 tools/bancs/agg_gxm.py   $D2VITA_WORK/bp_A1.txt $D2VITA_WORK/bp_B1.txt
python3 tools/bancs/agg_natif.py $D2VITA_WORK/bp_X_NATPROF.txt
```

- `agg_gxm.py` : img/s = moyenne des fenêtres de 10 s dont **la pire image est ≥ f3600**
  (`SEUIL_IMAGE=`) — la patrouille seule, chargement et menus écartés — plus les moyennes
  `soumission-us / attente-gpu-us / presentation-us` des fenêtres qui dessinent. Reproduit au
  centième les tableaux de mesure GPU des campagnes internes.
- `agg_natif.py` : sur un journal `WX86_NATPROF=1`, fenêtres `alive:` en régime, img/s, top-9 des
  shims natifs en ms/image hors `Sleep`/`WaitForSingleObject`/`PeekMessageA` (attentes, pas du
  travail).

## 6. Banc qemu du ring — `banc_ring_qemu.sh`

```
REFS=/tmp/refs_ring OUT=/tmp bash tools/bancs/banc_ring_qemu.sh build-arm/rt_boot_arm B D2_RINGHOST=1
```

4 000 images de patrouille, Glide ring + puits GXM, **horloge virtuelle + mur gelé** : le banc est
déterministe, comparable par ses empreintes de fin de run `[ring-final] … h=` (ce que l'hôte a lu)
et `lh=` (ce qu'il en a fait : sommets, indices, lots) — **elles doivent rester identiques** entre
un témoin et une variante — et par `parcours-ring-us/img`. Les durées sont celles de qemu : seule
la pente parle. `$REFS` doit contenir `glide3x.dll` : fabriquer un dossier de liens vers
`~/d2-vita-refs/1.14d` et y copier `build-glide/glide3x.dll` (`tools/build_glide_ring.sh`) ; ne
jamais poser la DLL dans le dossier de référence partagé.

## 6bis. Bancs du DESSIN en Glide — `banc_dessin_qemu.sh`, `profil_dessin_qemu.sh`

```
DETERMINISTE=1 REFS=/tmp/refs_ring OUT=/tmp bash tools/bancs/banc_dessin_qemu.sh B D2_PHASEPROF_X=75800 NATIVELIGHTMAP=1
REFS=/tmp/refs_ring OUT=/tmp/profil_dessin bash tools/bancs/profil_dessin_qemu.sh
```

`banc_dessin_qemu.sh` est le jumeau **Glide** de `tools/banc_phases.sh` (qui mesure le chemin
GDI) : `-3dfx` + DLL ring + puits GXM, patrouille du camp, `D2_PHASEPROF=1`, une jambe par
étiquette (journal `$OUT/dessin_<LAB>.log`). Par défaut horloge réelle et plafond levé à
l'image 3200 ; **`DETERMINISTE=1`** bascule sur horloge virtuelle + mur gelé, ce qui rend la
scène **identique d'une jambe à l'autre** (même nombre de lumières, mêmes cellules) alors que
le chronomètre reste réel : c'est le seul banc où un écart de 2 % sur `dessin=` ou sur une
ligne `phase/x:` veut dire quelque chose (en horloge réelle la dispersion de scène entre
fenêtres est de 8-15 %). Contrôler que les jambes rendent la même empreinte `[ring]`.

`profil_dessin_qemu.sh` est le jumeau Glide de `profil_fil_io.sh` : binaire `-DPROF_COUNTERS`
(`LIBTAG=_prof EXTRA=-DPROF_COUNTERS EXTRA_DEFS=-DPROF_COUNTERS OUTBIN=build-arm/oracle_prof
tools/build_oracle_arm.sh`), vidages à l'image 3400 et en fin, dépouillement
`python3 tools/eip_fils.py $OUT/eip.txt --fil 1 --top 25 --delta`. Ce sont des **blocs**, pas
du temps : croiser avec `D2_PHASEPROF_X`.

### Sonde des caches — `D2_CACHEPROBE` (mesure seulement)

```
REFS=/tmp/refs_ring OUT=/tmp bash tools/bancs/banc_ring_qemu.sh build-arm/oracle_arm P \
    D2_CACHEPROBE=1 D2_RINGTAG=1 D2_RINGPHASE_X=df1c0,76bc0,56ee0
```

Deux lignes `cache[camera fixe]` / `cache[camera MOBILE]` par fenêtre (deltas) et
au cumul : quelle fraction des **dessins**, des **lots** et des **images** se
répète d'une image à l'autre — en place, à une translation près, ou en géométrie
seule (couleur constante exclue) — plus la part du **SOL** (filtrage bilinéaire,
contrôlé par la texture 256×128) et, avec `D2_RINGPHASE_X`, l'empreinte du span
de ring de chaque phase. `D2_CACHEDUMP=<image>` / `D2_CACHEDUMP2=<image>` vident
les dalles à la main. **Multiplie le parcours du ring par ~7 : jamais dans une
jambe chiffrée.** `tools/bancs/d2script_camp_marche.txt` est le script de
**marche continue** (un clic toutes les 40 images) qui fournit l'échantillon
« caméra mobile » : la patrouille laisse la caméra fixe 79 % du temps.

### Oracle de la pose native des lumières — `tools/oracle_lightmap.sh`

```
bash tools/oracle_lightmap.sh                              # A, A2, B, V (4 000 images, camp)
PATROUILLE=1 MAXFRAMES=8300 TAG=P bash tools/oracle_lightmap.sh
```

Même patron que `oracle_dcc.sh` : empreinte image identique **et** oracle croisé
(`D2_LIGHTMAPVERIFY=1` — le natif pose dans une copie des 18 432 octets de la grille,
l'invité pose ensuite, comparaison octet à octet à chaque appel).

`ring_lots_qemu.sh <bin> <tag> [VAR=val]` vide les lots d'une image (`LOTS=4000`) : compte
`gxlot:`, `HORS-CELLULE` (uv hors cellule d'atlas — doit rester 0) et les lots filtrés.

Autres bancs qemu, rangés dans `tools/` : `banc_replay60.sh` (rejeu 60 Hz, horloge réelle),
`banc_nocap.sh` (limiteur), `banc_phases.sh` (phases), `tools/vita3k_isole.sh` (Vita3K isolé —
ne prouve ni palette ni filtre GPU).

## 7. L'oracle image

```
bash tools/oracle_phaseprof.sh          # A == A2 == B1 == B2 == B3 = 0xd332da5981bade4a (4000 images, GDI)
```

Empreinte de référence **`0xd332da5981bade4a` sur 4 000 images en GDI** (`D2_VIRTCLOCK` +
`D2_FAKEWALL`, layout console). Deux passes témoins identiques d'abord (A == A2), sinon un FAIL
est du bruit. Un changement qui touche au dessin doit garder cette empreinte ET les `h=`/`lh=` du
banc ring. L'oracle ne couvre pas ce que la scène ne traverse pas (cellules échelle, sauts de
dessin sous `D2_NOCAP`) : voir `oracle_nocap.sh`.

### Oracle des intrinsèques mémoire — `tools/oracle_memintrin.sh`

```
bash tools/oracle_memintrin.sh                                   # A, A2, P, B, V en parallele (~1 min)
PATROUILLE=1 MAXFRAMES=8300 bash tools/oracle_memintrin.sh       # scenario console (sprites en marche)
bash tools/bancs/banc_memintrin_qemu.sh                          # A/P/B/M/C SEQUENTIELS (duree + [blocs] executes=)
```

Même empreinte image que ci-dessus. `D2_MEMINTRIN=2` = profil seul (le helper mesure et replie
toujours : il doit être pixel-neutre), `=1` = servi, `D2_MEMVERIFY=1` = oracle croisé appel par
appel (le natif écrit un tampon hôte, l'invité écrit le vrai, comparaison au retour détourné).
Le banc lit `[blocs] executes=` — le travail INVITÉ, **exactement reproductible** — parce que la
durée qemu ne résout pas mieux que ±2 % sur cette machine.

### Oracle des lectures de fichiers — `tools/oracle_readahead.sh`

```
STRACE=1 bash tools/oracle_readahead.sh                       # REF, A, A2, B16, B64, B64m16 en parallele (~1-2 min)
EXTRAENV="D2_LAZYSEEK=1" LEGS="64m16" PATROUILLE=1 bash tools/oracle_readahead.sh   # config console, patrouille
```

Même empreinte que ci-dessus, knobs `D2_IOSTAT=1` (ligne `io:` par fenêtre de 10 s) et
`D2_READAHEAD=<Kio>[m<Kio>]` (lecture anticipée par poignée, `m` = taille du saut adaptatif) ;
`STRACE=1` compte en plus ce que l'HÔTE a vraiment demandé au système de fichiers
(`tools/bancs/strace_io_tally.py` : `read()`/octets/`lseek()` par `.mpq`), la seule mesure de
l'amplification stdio que le talon `ReadFile` ne voit pas.

### Oracle du SON — `tools/oracle_son.sh`

```
bash tools/oracle_son.sh                    # A A2 B C D E F en parallele + jambe Z (~15-25 min)
MAXFRAMES=600 SONMAXS=120 bash tools/oracle_son.sh    # version courte de mise au point
```

Huit jambes, une par question, et **aucune assertion qui puisse rester verte à vide** :

| jambe | env | ce qu'elle prouve |
|---|---|---|
| A / A2 | *(rien)* | déterminisme (A == A2) **et** empreinte de référence `0xd332da5981bade4a` intacte |
| B | `D2_SON=null` | le socle COM vit (≥ 20 tampons secondaires, plus de « Couldn't initialize DirectSound ») |
| C | `D2_SON=wav` | WAV avec du signal ; **codecs natifs ont tiré** (`huff>0 adpcm>0 repli=0`) |
| D | `+ NATIVEHUFF=0 NATIVEADPCM=0` | md5(C) == md5(D) — octet-exact ; **et `huff=0 adpcm=0`**, donc le knob désarme vraiment |
| E | `D2_SON=wav`, **registre `registry_console_son.txt`** | le WAV porte un signal **structuré** (tonalité ≥ 40 % contre ~4 % pour du bruit blanc) ; seule jambe qui exerce ce fichier |
| F | `+ Master Volume = 0`, `D2_SONLOG=1` | **le journal du volume TIRE** (≥ 1 ligne `SetVolume`). MESURE, pas assertion : md5(F) == md5(E) — sur l'écran-titre le jeu n'émet que deux `SetVolume`, les mêmes à 0 et à 100 ; ce banc **ne peut pas** trancher la chaîne du volume, et ne le prétend plus |
| Z | `D2_SON=0 D2_SONDUMP=…` | **le bouton coupe vraiment** : aucun WAV, journal muet, codecs à zéro |

La jambe muette A assert `huff=0 adpcm=0 repli=0` : c'est la preuve que les trois
`set_alternate` de codec, posés inconditionnellement, ne tirent **pas une fois**
dans le binaire par défaut. `tools/registry_console_son.txt` a été **réécrit**
: il posait quatre noms de clés (`Sound Volume`, `3D Sound`,
`Environmental Sound`, `3D Bias`) **absents de Game.exe 1.14d** — vérifié par
`grep -aiob` sur le binaire — plus un `Music Volume`
= 128 que le jeu **rejette** (> 100). Il porte maintenant les six vraies clés de
`0x514b60`.

## 8. Portes et build

```
bash tools/bancs/portes.sh                                   # PASS(natif) puis PASS(coop), sous flock
SON=1 bash tools/bancs/portes.sh                             # + tools/oracle_son.sh (jeu requis, ~15 min)
bash tools/build_rt_boot_vpk.sh && md5sum build-vita/eboot.bin
```

`portes.sh` enchaîne, dans l'ordre : **branches orphelines**, oracle clavier,
porte natif, porte coop, **compilation du banc du son**, auto-tests hors ligne,
hygiène des secrets, serveur privé strict, oracle CheckRevision (et, sous `SON=1`,
l'oracle du son).

### `tools/bancs/porte_branches.sh` — le travail validé vit-il dans `main` ?

Une seconde, aucun prérequis. Elle existe parce que son absence a coûté cinq
jours : deux branches — verrou réseau, keystore, oracle
CheckRevision à trois jambes, diagnostic du fil audio — sont restées hors de
`main` sans que **rien** ne le signale, pendant que `main` publiait trois
affirmations que l'une d'elles corrigeait. La question « laquelle de mes treize
branches n'est pas dans `main` ? » ne se posait nulle part, alors qu'une commande
y répond.

Elle nomme, elle ne classe pas à la place de qui triera : pour chaque branche en
avance, le nombre de commits absents, son âge, et combien de ses fichiers
diffèrent encore de `main` (zéro = indice fort d'un pointeur périmé, jamais une
preuve). Informative en dessous de `SEUIL_JOURS` (3 jours — les deux branches qui ont motivé cette porte en avaient cinq), **fatale** au-delà. Une
branche délibérément gardée dehors s'inscrit dans `tools/bancs/branches.allow`
**avec sa raison** — ce fichier n'excuse rien d'autre et doit rester court.

### `tools/build_oracle_arm.sh` dans la porte

Compiler le banc du son est devenu une jambe à part entière :
`ds_emul.cpp` est parti au moteur, la ligne de compilation ne l'a pas suivi, et
l'oracle du son est resté **incompilable un jour entier** sans que rien ne le
dise — parce que sa jambe est facultative (`SON=1`). Une porte qu'on n'allume
qu'à la demande ne protège rien ; compiler coûte une minute.

**Sous-module obligatoire.** Tous les scripts qui compilent `rt_boot.cpp` dépendent de
`third_party/StormLib` (Huffman + ADPCM audio). `git worktree add` **ne le peuple pas** :
dans un worktree neuf, la première porte échoue sur
`fatal error: ../../third_party/StormLib/src/huffman/huff.h`. Faire
`git submodule update --init --recursive` avant la première compilation.

`tools/rt_boot_arm_check.sh` écrit dans `/tmp/rt_boot_arm.log` et `/tmp/d2vita_write_bootcheck`,
**partagés entre worktrees** : deux portes simultanées se lisent l'une l'autre (faux
`FAIL: backend=NATIVE stamp missing`). `portes.sh` sérialise avec `flock /tmp/rt_boot_gate.lock` ;
tout appel direct doit faire de même. VitaSDK est hors PATH (`/usr/local/vitasdk/bin`) : le build
l'exporte lui-même, `objdump`/`nm` à la main non. Après avoir édité une source pendant une
compilation : `touch` puis rebuild (objet périmé à mtime récent).

## `env_jeu_glide.txt` — D2_NOPUMPWAIT retiré

`D2_NOPUMPWAIT=1` figurait dans la config depuis longtemps alors que
`tools/rt_boot.cpp:7758` le commente lui-même « diag: isolate the peek-throttle » :
il annule le correctif qui transforme le pompage vide en attente chronométrée
de 1 ms au lieu d'un spin (« each spin lap costs a trap plus a scheduler
round, which is what starved rendering on console »). Un knob de diagnostic
s'était glissé dans la config de production.

Reproduit deux fois sur console (isolation de variable, config par ailleurs
identique) : avec, un fil garde le GIL en continu (jusqu'a des millions de
microsecondes sans le relacher), le filet anti-famine echoue en boucle, la
GPU cesse de recevoir des images (« chien de garde — 3 s sans image
soumise ») — pump proche de 2 millions de sondages pour une douzaine
d'images reellement rendues. Sans lui : stable, 45 s surveillees, ~20-25
img/s sans decrochage.

Ne JAMAIS le remettre dans une config de jeu — il existe pour isoler un
probleme de pompage en diagnostic, pas pour tourner en continu.

## env_jeu_glide.txt — révision console du 12/09 (mode asynchrone 1, carrousel 4)

Après le correctif D2_NOPUMPWAIT, une session de test en direct sur console a
révisé trois autres réglages :

- **`D2_GXMASYNC=2` → `1`, `D2_GXMVSYNC=2` retiré, `D2_GXMRING=4` ajouté.**
  Le mode 2 (avec `D2_GXMVSYNC=2`) fonctionnait mais coûtait une vraie attente
  de balayage mesurée (~8,5 ms/soumission). Le mode 1 ("ASYNCHRONE-FILE") est
  documenté comme gérant nativement le déchirement au vblank, sans attente
  explicite ; un carrousel à 4 tampons (au lieu de 3) est la
  seconde garantie contre la réécriture d'un tampon encore
  affiché. **Vérifié stable sur console 65 s, aucun blocage, aucun accroc
  au-dessus de 376 ms** (contre 2-7 s observés avant la lecture anticipée).
  ⚠️ Une PREMIÈRE tentative de ce même mode 1 (avec `D2_GXMRING=3`, sans
  retirer `D2_GXMVSYNC`) a figé la console entière (écran noir, FTP ne
  répondant plus), nécessitant une extinction complète — cause non identifiée,
  possible état bas niveau (contexte sceGxm) laissé à moitié initialisé. La
  combinaison ring=3 + vsync=2 explicite en mode 1 n'a plus été retestée ;
  rien ne prouve que ring=4 seul soit la variable qui a réglé ça plutôt que le
  redémarrage complet lui-même.

- **`D2_ONEDRAW=2` → `1`.** Le mode 2 ajoutait une vraie pause d'1 ms
  (`sieste=1000us`) à chaque dessin redondant court-circuité. Retirée sans
  perte apparente de l'optimisation elle-même (le court-circuit reste actif).

- **`D2_READAHEAD` (famille complète) ajoutée**, absente depuis toujours de ce
  fichier alors que le code la documente comme correctif direct des gros gels
  d'E/S. Réglages **volontairement plus prudents que la mesure qemu**
  (2 Mio de plafond, pas 8) : la console n'a que 6-7 Mio de RAM utilisateur
  libre en régime, et un plafond trop haut a déjà fait planter un chargement
  par le passé.

- **`D2_REPLAY60` retiré** (désarmé sur demande explicite pendant la session
  de test — pas une conclusion de perf, juste un choix de confort visuel :
  sans lui, l'affichage colle aux ~25 img/s réels de la simulation au lieu
  d'être lissé à ~50-60 par rejeu interpolé). À réarmer si le lissage est
  souhaité, rien ne l'interdit.

**Non résolu** : des gels de plusieurs secondes existent toujours en régime
lourd (changement de zone), non expliqués par le profileur d'image lui-même
(`INEXPLIQUE` domine le temps mesuré). La lecture anticipée réduit
l'amplification par octet lu mais ne rend pas le chargement asynchrone au
reste du jeu — chantier séparé, pas encore ouvert.
