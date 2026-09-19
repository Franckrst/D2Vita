# Manette v2 — navigation du butin au sol (Phase 2) — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pendant qu'Alt est maintenu (R puis L, comme aujourd'hui), le D-pad déplace un
curseur de sélection parmi les objets au sol affichés à l'écran (navigation par focus
directionnel relative au curseur, pas au personnage) et Croix valide le ramassage de
l'objet actuellement sélectionné, en réutilisant tel quel le mécanisme de ramassage déjà
existant pour L.

**Architecture:** Deux fonctions pures ajoutées à `pad::` (`nav_direction`,
`nearest_item`), testables sur PC comme le reste du cœur. Le nouvel état vit dans
`pad::Scheme` (Phase 1) : un nouvel état analogue à `interact_`, pas un composant
séparé. Aucun nouveau crochet côté `phase_hooks.cpp` — la liste des objets au sol est
déjà construite par la Phase 1.

**Tech Stack:** C++17, même toolchain que la Phase 1 (VitaSDK, CMake pour les tests PC,
`tools/build_rt_boot_vpk.sh`, `tools/rt_boot_arm_check.sh`).

**Spec :** `docs/superpowers/specs/2026-09-19-manette-butin-design.md` — à lire en
entier avant de commencer. S'appuie sur
`docs/superpowers/specs/2026-09-19-manette-ciblage-design.md` (Phase 1, déjà livrée et
validée sur console).

**Worktree :** `/home/doudou/repos/wt-manette`, branche `wt/manette` (même worktree que
la Phase 1 — déjà construit, cache d'objets chaud). Après chaque commit :
`git push origin wt/manette`.

**Règles du dépôt (CLAUDE.md, inchangées) :** jamais d'écriture mémoire invité ni
d'appel de fonction du jeu ; jamais prétendre qu'un test a tourné s'il n'a pas tourné ;
un résultat qemu n'est jamais présenté comme un résultat console ; les deux chemins de
build restent verts.

---

## Carte des fichiers

| Fichier | Rôle |
|---|---|
| `src/platform/pad_core.h` (modifié) | Déclarations : `Dir`, `nav_direction`, `nearest_item`, nouveaux membres/accesseurs de `Scheme`. |
| `src/platform/pad_core.cpp` (modifié) | Implémentation des deux fonctions pures + intégration dans `Scheme::commonButtons`/`worldTick`/`releaseAll`. |
| `tests/pad/pad_core_test.cpp` (modifié) | Tests PC des deux fonctions + de l'intégration dans `Scheme`. |
| `src/platform/vita_present.cpp` (modifié) | Overlay : repère visuel du curseur de butin (`aim_tick`, `draw_reticle`). |
| `docs-site/controles.fr.md`, `docs-site/controles.md` (modifiés) | Doc joueur de la navigation au D-pad pendant Alt. |

---

## Task 0 : Prendre le worktree en main

**Files:** aucun.

- [ ] **Step 1 : Vérifier l'état**

```bash
cd /home/doudou/repos/wt-manette && git status --short && git log --oneline -3 && ./build/pad_core_test
```
Attendu : arbre propre, HEAD sur `wt/manette`, `92 passed, 0 failed` (build déjà
configuré par la Phase 1 — si `./build/pad_core_test` n'existe pas, lancer
`cmake -B build && cmake --build build --target pad_core_test -j4` d'abord).

---

## Task 1 : `nav_direction` et `nearest_item` — fonctions pures testées sur PC

**Files:**
- Modify: `src/platform/pad_core.h` (après `ground_point`, avant la classe `Scheme`)
- Modify: `src/platform/pad_core.cpp`
- Modify: `tests/pad/pad_core_test.cpp`

- [ ] **Step 1 : Déclarations**

Dans `src/platform/pad_core.h`, ajouter après la déclaration de `ground_point` (juste
avant `// The scheme itself: ...` et la classe `Scheme`) :

```cpp
// Directional focus navigation among on-screen ground items (type==4 only,
// matching Alt's own item-only scope — objects/NPCs stay L-only). Returns
// the index in `u` of the nearest item to u[fromIdx] in screen direction
// `d`, or -1 if none qualifies. Non-item units in `u` are never candidates.
enum Dir { D_UP, D_DOWN, D_LEFT, D_RIGHT };
int nav_direction(const Unit* u, int n, int fromIdx, Dir d);
// Nearest on-screen item (type==4) to the player, no distance cap (unlike
// pick_interact's 220px pass — Alt already shows everything on screen).
// Returns -1 if none.
int nearest_item(const Unit* u, int n, const View& v);
```

- [ ] **Step 2 : Tests**

Dans `tests/pad/pad_core_test.cpp`, ajouter avant `test_scheme_cast` (ou n'importe où
avant `main`, tant que c'est avant les appels dans `main`) :

```cpp
static pad::Unit mkItem(uint32_t id, int sx, int sy) {
    pad::Unit u; u.id = id; u.type = 4; u.sx = sx; u.sy = sy; u.interact = true; return u;
}

static void test_nav_direction() {
    pad::View v = mkView();
    // "plus" layout centered on Z (400,300): one item per cardinal direction,
    // all at distance 100 so no direction's pick is ambiguous with another.
    pad::Unit u[6] = {
        mkItem(20, 400, 300),   // Z: the cursor start
        mkItem(21, 500, 300),   // right of Z, dist 100
        mkItem(23, 300, 300),   // left of Z, dist 100
        mkItem(24, 400, 200),   // above Z, dist 100
        mkItem(25, 400, 400),   // below Z, dist 100
    };
    CHECK(pad::nav_direction(u, 5, 0, pad::D_RIGHT) == 1);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_LEFT)  == 2);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_UP)    == 3);
    CHECK(pad::nav_direction(u, 5, 0, pad::D_DOWN)  == 4);

    // no candidate in a direction: cursor does not move (returns -1)
    pad::Unit only[2] = { mkItem(30, 400, 300), mkItem(31, 500, 300) };
    CHECK(pad::nav_direction(only, 2, 0, pad::D_LEFT) == -1);
    CHECK(pad::nav_direction(only, 2, 0, pad::D_UP)   == -1);
    CHECK(pad::nav_direction(only, 1, 0, pad::D_RIGHT) == -1);   // single item: no candidates at all

    // non-item units are never candidates, even when nearer than the real item
    pad::Unit withMon[3] = { mkItem(40, 400, 300), mkItem(41, 500, 300) };
    withMon[2] = mkItem(42, 450, 300); withMon[2].type = 1; withMon[2].mode = 1; withMon[2].hostile = true;
    CHECK(pad::nav_direction(withMon, 3, 0, pad::D_RIGHT) == 1);   // finds item 41, not the closer monster

    // a target at exactly 45 degrees qualifies for BOTH adjacent directions
    // (both tests use an inclusive >=), with nothing else around to compete
    pad::Unit diag[2] = { mkItem(50, 400, 300), mkItem(51, 500, 400) };
    CHECK(pad::nav_direction(diag, 2, 0, pad::D_RIGHT) == 1);
    CHECK(pad::nav_direction(diag, 2, 0, pad::D_DOWN)  == 1);

    // tie in distance: lower id wins, regardless of array order
    pad::Unit tie[3] = { mkItem(60, 400, 300), mkItem(41, 500, 250), mkItem(40, 500, 350) };
    CHECK(pad::nav_direction(tie, 3, 0, pad::D_RIGHT) == 2);   // index 2 = id 40, the lower id
}

static void test_nearest_item() {
    pad::View v = mkView();   // player projects to (400,300)
    pad::Unit u[3] = { mkItem(70, 450, 350), mkItem(71, 400, 250) };
    // dist(70) = sqrt(50^2+50^2) ~= 70.7 ; dist(71) = 50 -> 71 is nearest
    CHECK(pad::nearest_item(u, 2, v) == 1);

    // no cap: an item far outside L's 220px catchment is still found when it's
    // the only one present
    pad::Unit far[1] = { mkItem(72, 400, 900) };
    CHECK(pad::nearest_item(far, 1, v) == 0);

    // non-item units are ignored
    pad::Unit withMon[2]; withMon[0] = mkItem(73, 500, 500);
    withMon[1].id = 74; withMon[1].type = 1; withMon[1].sx = 401; withMon[1].sy = 301; withMon[1].mode = 1;
    CHECK(pad::nearest_item(withMon, 2, v) == 0);

    // no items at all
    pad::Unit mon[1]; mon[0].id = 75; mon[0].type = 1; mon[0].sx = 400; mon[0].sy = 300; mon[0].mode = 1;
    CHECK(pad::nearest_item(mon, 1, v) == -1);
    CHECK(pad::nearest_item(nullptr, 0, v) == -1);
}
```

Ajouter `test_nav_direction(); test_nearest_item();` dans `main`, après les appels
existants (`test_ground_point();` puis avant `test_scheme_cast();`).

- [ ] **Step 3 : Test rouge**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error' | head -5
```
Attendu : `undefined reference to pad::nav_direction` et `pad::nearest_item`.

- [ ] **Step 4 : Implémenter**

Dans `src/platform/pad_core.cpp`, ajouter après `ground_point` (avant le bloc
`// ---------------------------------------------------------------------------`
`// Scheme` déjà présent) :

```cpp
int nav_direction(const Unit* u, int n, int fromIdx, Dir d) {
    if (!u || fromIdx < 0 || fromIdx >= n) return -1;
    const Unit& from = u[fromIdx];
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (i == fromIdx || u[i].type != 4) continue;
        const float dx = (float)(u[i].sx - from.sx), dy = (float)(u[i].sy - from.sy);
        bool ok = false;
        switch (d) {
            case D_RIGHT: ok = dx > 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case D_LEFT:  ok = dx < 0.f && std::fabs(dx) >= std::fabs(dy); break;
            case D_DOWN:  ok = dy > 0.f && std::fabs(dy) >= std::fabs(dx); break;
            case D_UP:    ok = dy < 0.f && std::fabs(dy) >= std::fabs(dx); break;
        }
        if (!ok) continue;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd || (dist == bd && u[i].id < u[best].id)) { best = i; bd = dist; }
    }
    return best;
}

int nearest_item(const Unit* u, int n, const View& v) {
    if (!u) return -1;
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    int best = -1; float bd = 0.f;
    for (int i = 0; i < n; ++i) {
        if (u[i].type != 4) continue;
        const float dx = (float)(u[i].sx - psx), dy = (float)(u[i].sy - psy);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bd) { best = i; bd = dist; }
    }
    return best;
}
```

- [ ] **Step 5 : Test vert**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error|warning'; ./build/pad_core_test
```
Attendu : `0 failed`.

- [ ] **Step 6 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/platform/pad_core.h src/platform/pad_core.cpp tests/pad/pad_core_test.cpp && git commit -m "pad: navigation par focus directionnel parmi les objets au sol (testé PC)" && git push origin wt/manette
```

---

## Task 2 : `Scheme` — navigation au D-pad et validation à la Croix pendant Alt

**Files:**
- Modify: `src/platform/pad_core.h`
- Modify: `src/platform/pad_core.cpp`
- Modify: `tests/pad/pad_core_test.cpp`

- [ ] **Step 1 : Nouveaux membres et accesseur de `Scheme`**

Dans `src/platform/pad_core.h`, dans la classe `Scheme`, ajouter dans la section
publique, après `Target target() const { return tgt_; }` :

```cpp
    Target lootCursor() const { return lootTgt_; }
```

Et dans la section privée, après la ligne `uint32_t tgtId_ = 0; Target tgt_;` :

```cpp
    // ground-item browsing (Alt held: D-pad moves the cursor, Croix confirms)
    uint32_t lootCursorId_ = 0; bool lootConfirm_ = false; Target lootTgt_;
```

- [ ] **Step 2 : Tests**

Dans `tests/pad/pad_core_test.cpp`, ajouter avant `test_scheme_panel_and_leave` (ou
n'importe où avant `main`) :

```cpp
static void test_scheme_loot_browse() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.playerId = 99;
    pad::Unit u[3];
    u[0] = mkItem(50, 480, 300);                                 // dist 80 from player (400,300): nearest
    u[1] = mkItem(51, 300, 300);                                 // dist 100: farther, to the LEFT of u[0]
    u[2].id = 52; u[2].type = 1; u[2].sx = 470; u[2].sy = 300; u[2].mode = 1; u[2].hostile = true;
    // ^ monster: closer to u[1] (dist 170) than u[0] is (dist 180) -- must never be selected
    pad::Ctl c; pad::Actions a;

    // R then L: Alt activates; cursor defaults to the item nearest the player
    c.buttons = pad::B_R; s.tick(c, x, v, u, 3, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x12));
    CHECK(s.lootCursor().has && s.lootCursor().id == 50);

    // D-pad left: moves to item 51; does NOT also drink a belt potion
    c.buttons = pad::B_R | pad::B_L | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 51);
    CHECK(!hasAct(a, pad::A_KEYDOWN, 0x32));
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);

    // D-pad right: back to item 50 -- the closer monster must never win
    c.buttons = pad::B_R | pad::B_L | pad::B_RIGHT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 50);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);

    // left stick pushed during Alt: no movement click, character stays put (spec 3.6)
    c.buttons = pad::B_R | pad::B_L; c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(!hasAct(a, pad::A_LDOWN) && !s.cursorOwned());
    c.lx = 0.f;

    // right stick pushed during Alt: no aim reticle (spec 3.6)
    c.buttons = pad::B_R | pad::B_L; c.rx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(!s.aimActive());
    c.rx = 0.f;

    // Croix: picks up item 50 via the same hover mechanism L already uses (h=6 for items)
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_MOVE, 480, 294) && hasAct(a, pad::A_LDOWN, 480, 294));
    CHECK(!hasAct(a, pad::A_KEY, 0x70));                          // did NOT also cast skill 1

    // D-pad is locked out while the pickup is held
    c.buttons = pad::B_R | pad::B_L | pad::B_CROSS | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(s.lootCursor().id == 50);

    // release Croix: LUP, browsing resumes
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_LUP));

    // release L (R still held): Alt ends, selection forgotten
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, u, 3, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x12));
    CHECK(!s.lootCursor().has);
}
```

Ajouter `test_scheme_loot_browse();` dans `main`, après `test_scheme_interact_and_keys();`.

- [ ] **Step 3 : Test rouge**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error' | head -10
```
Attendu : échecs (pas encore de méthode `lootCursor()` fonctionnelle — compile mais les
`CHECK` échouent, ou erreurs si la Step 1 n'a pas été appliquée avant).

- [ ] **Step 4 : Intégrer dans `commonButtons`**

Dans `src/platform/pad_core.cpp`, dans `Scheme::commonButtons`, remplacer :

```cpp
    for (int i = 0; i < 4; ++i) {
        if (down & kDpadBits[i]) {
```

par :

```cpp
    for (int i = 0; i < 4; ++i) {
        if (!alt_ && (down & kDpadBits[i])) {   // D-pad navigates the loot cursor while Alt is held, not potions
```

(la ligne `if ((up & kDpadBits[i]) && dpad_[i].vk) {` juste après reste inchangée — le
relâchement d'une potion déjà en cours doit toujours être traité, même si Alt vient de
s'activer entre-temps.)

- [ ] **Step 5 : Rendre le stick droit inerte pendant Alt (spec §3.6)**

Toujours dans `worldTick`, remplacer :

```cpp
        const float am = std::sqrt(c.rx * c.rx + c.ry * c.ry);
        aimActive_ = am > cfg_.deadzone;
```

par :

```cpp
        const float am = std::sqrt(c.rx * c.rx + c.ry * c.ry);
        aimActive_ = !alt_ && am > cfg_.deadzone;
```

Sans ce correctif, pousser le stick droit pendant Alt afficherait quand même le point
de visée blanc — sans effet fonctionnel (le lancer est déjà bloqué par la Step 6
ci-dessous), mais visuellement en contradiction avec la spec et source de confusion à
côté du nouveau repère cyan.

- [ ] **Step 6 : Empêcher le lancer de compétence 1 (Croix) pendant Alt**

Toujours dans `worldTick`, remplacer :

```cpp
    // ---- cast: faces = slots 1-4, R + faces = 5-8 ----
    if (castSlot_ < 0 && !interact_) {
```

par :

```cpp
    // ---- cast: faces = slots 1-4, R + faces = 5-8 ----
    if (castSlot_ < 0 && !interact_ && !alt_) {
```

- [ ] **Step 7 : Ne pas afficher le losange de cible hostile pendant Alt**

Toujours dans `worldTick`, dans le bloc `// overlay target`, remplacer :

```cpp
    } else if (ti >= 0) { tgt_ = Target{}; tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy; }
```

par :

```cpp
    } else if (ti >= 0 && !alt_) { tgt_ = Target{}; tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy; }
```

(`ti` lui-même reste calculé inconditionnellement plus haut — seul l'affichage du
losange est concerné — `ti` sert aussi de repli pour l'interaction L, qui reste
inchangée hors Alt.)

- [ ] **Step 8 : Séparer la continuation de l'interaction L de celle du ramassage-Croix**

Toujours dans `worldTick`, remplacer :

```cpp
    } else if (interact_) {
        if (!(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); interact_ = false; interId_ = 0; }
```

par :

```cpp
    } else if (interact_ && !lootConfirm_) {
        if (!(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); interact_ = false; interId_ = 0; }
```

(le reste de ce bloc — la vérification/apprentissage de hauteur de survol — reste
inchangé.)

- [ ] **Step 9 : Ajouter le bloc de navigation et de validation**

Toujours dans `worldTick`, juste après la ligne
`if ((up & B_L) && interNoop_) interNoop_ = false;` et avant le commentaire
`// ---- left stick: move-only orbit, hard stop on release ----`, insérer :

```cpp

    // ---- Alt held: browse ground items (D-pad = cursor, Croix = pick up) ----
    if (alt_) {
        int fromIdx = findId(u, n, lootCursorId_);
        if (fromIdx < 0 && !interact_) { fromIdx = nearest_item(u, n, v); lootCursorId_ = fromIdx >= 0 ? u[fromIdx].id : 0; }
        if (fromIdx >= 0 && !interact_) {
            static const Dir kDirs[4] = { D_UP, D_LEFT, D_DOWN, D_RIGHT };   // matches kDpadBits order
            for (int i = 0; i < 4; ++i) if (down & kDpadBits[i]) {
                const int next = nav_direction(u, n, fromIdx, kDirs[i]);
                if (next >= 0) { lootCursorId_ = u[next].id; fromIdx = next; }
                break;
            }
        }
        if ((down & B_CROSS) && !interact_ && fromIdx >= 0) {
            interact_ = true; lootConfirm_ = true;
            interId_ = u[fromIdx].id; interType_ = u[fromIdx].type; interCls_ = u[fromIdx].cls;
            interH_ = 6;                                              // items: fixed hover height, same as L
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int px, py; hoverPoint(u[fromIdx], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);
            out.push(A_LDOWN, px, py);
        } else if (interact_ && lootConfirm_) {
            if (!(c.buttons & B_CROSS)) { out.push(A_LUP, cx_, cy_); interact_ = false; lootConfirm_ = false; interId_ = 0; }
            else {
                const int ii = findId(u, n, interId_);
                if (ii >= 0) { int px, py; hoverPoint(u[ii], interH_, v, &px, &py); moveTo(px, py, out); }
            }
        }
        lootTgt_ = Target{};
        if (fromIdx >= 0) { lootTgt_.has = true; lootTgt_.id = u[fromIdx].id; lootTgt_.sx = u[fromIdx].sx; lootTgt_.sy = u[fromIdx].sy; }
    } else {
        lootTgt_ = Target{};
        lootCursorId_ = 0;   // fresh entry next time always re-derives nearest-to-player (spec §3.2/§3.7)
        if (lootConfirm_ && interact_) { out.push(A_LUP, cx_, cy_); interact_ = false; lootConfirm_ = false; interId_ = 0; }
    }
```

> **Mise à jour post-relecture (commit `dec0090`, après une revue qualité qui a trouvé
> deux vrais problèmes dans le Step 9 ci-dessus tel qu'écrit initialement — ce texte
> reflète maintenant le code réellement livré, pas la version d'origine) :**
> 1. Le repli de réacquisition (`if (fromIdx < 0) {...}`) n'était PAS protégé par
>    `!interact_`, contrairement à la navigation D-pad juste en dessous. Quand l'objet
>    en cours de ramassage disparaît (cas normal peu après un clic réussi, pendant que
>    Croix est encore physiquement maintenu), le curseur affiché dérivait silencieusement
>    vers un autre objet alors que le clic réel restait correctement ancré sur `interId_`.
>    Corrigé ci-dessus par `&& !interact_`.
> 2. `lootCursorId_` n'était jamais remis à zéro à la fin normale d'Alt (seulement via
>    `releaseAll` lors d'un changement de mode), donc rentrer à nouveau dans Alt avec le
>    même objet encore visible reprenait silencieusement l'ancienne sélection au lieu de
>    redériver « le plus proche du personnage » (spec §3.2). Corrigé ci-dessus par
>    `lootCursorId_ = 0;` dans la branche `else`.
>
> Le scénario du point 1 (objet ramassé qui disparaît pendant que Croix est encore
> maintenu) n'a toujours **aucun test de non-régression** — la relecture qualité l'a
> signalé comme point important à traiter avant ou pendant la Task 3, puisque c'est
> exactement le moment où une régression silencieuse deviendrait visible à l'écran
> (le repère cyan du curseur de butin).

- [ ] **Step 10 : Rendre le stick gauche inerte pendant Alt (spec §3.6)**

Toujours dans `worldTick`, dans le bloc `// ---- left stick: move-only orbit, hard stop
on release ----` juste en dessous de ce que la Step 9 vient d'insérer, remplacer :

```cpp
    if (castSlot_ < 0 && !interact_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
```

par :

```cpp
    if (castSlot_ < 0 && !interact_ && !alt_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
```

Sans ce correctif, pousser le stick gauche pendant Alt ferait quand même marcher le
personnage — la branche `else if (lmb_) { out.push(A_LUP, ...); lmb_ = false; }` déjà
présente juste après s'occupe correctement de relâcher un déplacement en cours dès que
la condition devient fausse, exactement comme pour un lancer ou une interaction L.

- [ ] **Step 11 : Nettoyer l'état de butin dans `releaseAll`**

Dans `Scheme::releaseAll`, remplacer :

```cpp
    interact_ = false; interNoop_ = false; interId_ = 0; interVerified_ = false;
```

par :

```cpp
    interact_ = false; interNoop_ = false; interId_ = 0; interVerified_ = false;
    lootConfirm_ = false; lootCursorId_ = 0; lootTgt_ = Target{};
```

- [ ] **Step 12 : Test vert**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error|warning'; ./build/pad_core_test
```
Attendu : `0 failed`. Si un `CHECK` échoue, corriger l'implémentation, pas le test,
sauf si le test contredit la spec (le dire dans le commit).

- [ ] **Step 13 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add -A src/platform tests/pad && git commit -m "pad: navigation du butin au D-pad pendant Alt, validation à la Croix (réutilise le ramassage L)" && git push origin wt/manette
```

---

## Task 3 : Overlay — repère visuel du curseur de butin

**Files:**
- Modify: `src/platform/vita_present.cpp`

- [ ] **Step 1 : État partagé**

Trouver la ligne (ajoutée en Phase 1, juste après `radial_menu::State g_rm{};`) :

```cpp
volatile int g_ret_has = 0, g_ret_ver = 0, g_ret_x = 0, g_ret_y = 0, g_aim_on = 0, g_aim_x = 0, g_aim_y = 0;
```

Ajouter juste après :

```cpp
volatile int g_loot_has = 0, g_loot_x = 0, g_loot_y = 0;   // Phase 2: ground-item browse cursor
```

- [ ] **Step 2 : Alimenter depuis `aim_tick`**

Trouver, dans `aim_tick` :

```cpp
    const pad::Target t = g_scheme->target();
    g_ret_has = t.has; g_ret_ver = t.verified; g_ret_x = t.sx; g_ret_y = t.sy;
    g_aim_on = g_scheme->aimActive(); g_aim_x = g_scheme->aimX(); g_aim_y = g_scheme->aimY();
```

Ajouter juste après :

```cpp
    const pad::Target lt = g_scheme->lootCursor();
    g_loot_has = lt.has; g_loot_x = lt.sx; g_loot_y = lt.sy;
```

- [ ] **Step 3 : Dessiner le repère**

Dans `draw_reticle`, remplacer la ligne de garde :

```cpp
    if (!g_ret_has && !g_aim_on) return;
```

par :

```cpp
    if (!g_ret_has && !g_aim_on && !g_loot_has) return;
```

Et, juste avant l'accolade fermante finale de `draw_reticle` (après le bloc
`if (g_aim_on) { ... }`), ajouter :

```cpp
    if (g_loot_has) {
        // Cyan corner brackets: deliberately distinct from the gold/white
        // hostile-target diamond, so the two are never confused at a glance.
        const int cx = g_loot_x * SCR_W / gw, cy = g_loot_y * SCR_H / gh - 6;
        const uint8_t r = 80, g = 220, b = 255;
        for (int d = 0; d <= 6; ++d) {
            blend_px(fb, SCR_W, SCR_H, cx - 12, cy - 12 + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - 12 + d, cy - 12, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 12, cy - 12 + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 12 - d, cy - 12, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - 12, cy + 12 - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - 12 + d, cy + 12, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 12, cy + 12 - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 12 - d, cy + 12, r, g, b, 220);
        }
    }
```

- [ ] **Step 4 : Build VPK**

```bash
export VITASDK=/usr/local/vitasdk; export PATH="$VITASDK/bin:$PATH"; cd /home/doudou/repos/wt-manette && bash tools/build_rt_boot_vpk.sh 2>&1 | tail -3
```
Attendu : `== built …/build-vita/d2vita.vpk ==`, aucune erreur.

- [ ] **Step 5 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/platform/vita_present.cpp && git commit -m "input: repère visuel du curseur de butin dans l'overlay (crochets cyan)" && git push origin wt/manette
```

---

## Task 4 : Documentation joueur

**Files:**
- Modify: `docs-site/controles.fr.md`
- Modify: `docs-site/controles.md`

- [ ] **Step 1 : Doc française**

Dans `docs-site/controles.fr.md`, remplacer la ligne :

```markdown
| R puis L (maintenus) | Alt — étiquettes des objets au sol |
```

par :

```markdown
| R puis L (maintenus) | Alt — étiquettes des objets au sol ; D-pad = déplacer le curseur d'objet en objet (vers le plus proche du curseur dans cette direction), Croix = ramasser l'objet sélectionné (repère cyan) |
```

- [ ] **Step 2 : Doc anglaise**

Dans `docs-site/controles.md`, trouver la ligne équivalente (« Alt — ground item
labels ») et appliquer la même extension en anglais :

```markdown
| R then L (held) | Alt — ground item labels ; D-pad = move the selection cursor from item to item (nearest to the cursor in that direction), Cross = pick up the selected item (cyan marker) |
```

- [ ] **Step 3 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add docs-site/controles.fr.md docs-site/controles.md && git commit -m "docs: navigation du butin au D-pad pendant Alt" && git push origin wt/manette
```

---

## Task 5 : Validation — build, non-régression, test console guidé

**Files:** aucun changement de code attendu ; corrections éventuelles commitées
séparément.

- [ ] **Step 1 : Non-régression qemu**

```bash
cd /home/doudou/repos/wt-manette && bash tools/rt_boot_arm_check.sh 2>&1 | tail -6
```
Attendu : `PASS(natif)`, comme à la fin de chaque tâche de la Phase 1. Cette tâche ne
touche pas `phase_hooks.cpp` — c'est une vérification de non-régression, pas une
nouvelle preuve : la liste d'objets utilisée existe déjà depuis la Phase 1.

- [ ] **Step 2 : Rebuild VPK final**

```bash
export VITASDK=/usr/local/vitasdk; export PATH="$VITASDK/bin:$PATH"; cd /home/doudou/repos/wt-manette && bash tools/build_rt_boot_vpk.sh 2>&1 | tail -3
```

- [ ] **Step 3 : Déployer sur la console et demander à l'utilisateur de jouer**

Suivre exactement la procédure déjà éprouvée pour la Phase 1 (§Task 9 du plan
`2026-09-19-manette-ciblage.md`) : sauvegarder l'`env.txt` existant de la console
(`10.113.1.159`, FTP 1337, commandes 1338) avant modification, le restaurer après.
Déployer le nouvel eboot (`ux0:/app/DTWO00001/eboot.bin`, `cmd destroy` avant,
`nosleep on` + `launch DTWO00001` après).

**Ne PAS scripter la navigation en aveugle par D2CMDFILE** — la Phase 1 a montré
qu'un test joué directement par l'utilisateur (avec retour visuel immédiat) est
strictement plus rapide et plus fiable qu'une séquence de clics à l'aveugle sur
console. Décrire à l'utilisateur, en une seule fois, la checklist :

1. Faire tomber ou repérer 2-3 objets au sol distincts sur la même zone d'écran.
2. Maintenir R puis L : les étiquettes apparaissent, un repère cyan marque l'objet le
   plus proche du personnage.
3. Appuyer sur chaque direction du D-pad l'une après l'autre : le repère cyan doit
   sauter vers un autre objet dans la direction pressée (ou rester immobile s'il n'y a
   rien dans cette direction).
4. Appuyer sur Croix (en gardant R+L) : le personnage doit se diriger vers l'objet
   actuellement marqué et le ramasser — pas un autre.
5. Relâcher L (en gardant R) : les étiquettes et le repère disparaissent, D-pad et
   Croix reprennent leur rôle normal (potions, compétence 1).

- [ ] **Step 4 : Consigner le résultat**

Ajouter le résultat (5 lignes PASS/FAIL) en fin de
`docs/superpowers/specs/2026-09-19-manette-butin-design.md`, sous un titre
`## 8. Résultat console (date)`. Un FAIL = ouvrir une tâche de correction ciblée
(nouveau commit), pas un contournement dans la doc.

```bash
cd /home/doudou/repos/wt-manette && git add docs/superpowers/specs/2026-09-19-manette-butin-design.md && git commit -m "pad: résultat console de la navigation du butin" && git push origin wt/manette
```

---

## Auto-revue du plan (faite à la rédaction)

- **Couverture de la spec** : §3.1 (déclenchement/périmètre) → Task 2 (réutilise `alt_`
  existant, filtre `type==4` dans `nav_direction`/`nearest_item`) ; §3.2 (curseur
  initial) → Task 2 Step 9 (`nearest_item` en fallback) ; §3.3 (navigation) → Task 1 ;
  §3.4 (affichage) → Task 3 ; §3.5 (validation Croix) → Task 2 Step 9 (réutilise
  `interId_`/`interType_`/`interH_`/`hoverPoint`) ; §3.6 (sticks inertes) → **pas vrai
  par construction, corrigé pendant la relecture du plan** — le code Phase 1 calcule
  `aimActive_` et le mouvement du stick gauche sans jamais regarder `alt_` ; Task 2
  Steps 5 (stick droit) et 10 (stick gauche) corrigent ça, couvert par les deux
  nouveaux `CHECK` de la Step 2 ; §3.7 (relâchement propre) → Task 2 Steps 9, 11 ;
  §4 (architecture) → Tasks 1-2 (fonctions pures + état `Scheme`, aucun nouveau
  crochet) ; §6 (validation) → Task 5.
- **Types identiques d'une tâche à l'autre** : `pad::Dir{D_UP,D_DOWN,D_LEFT,D_RIGHT}`,
  `nav_direction(const Unit*, int, int, Dir) -> int`,
  `nearest_item(const Unit*, int, const View&) -> int`, `Scheme::lootCursor() -> Target`
  (réutilise le type `Target` déjà défini en Phase 1, aucun nouveau type).
- **Hors périmètre volontaire (YAGNI)** : pas de nouvelle clé `controls.txt` (aucun
  paramètre ergonomique identifié) ; pas de navigation parmi les objets/coffres/PNJ
  (hors périmètre, cf. spec §3.1) ; pas de nouveau crochet `phase_hooks.cpp` (la liste
  d'unités existe déjà).
