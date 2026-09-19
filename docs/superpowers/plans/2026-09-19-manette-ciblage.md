# Manette v2 (ciblage assisté, phase 1) — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Livrer une snapshot VPK de test où le stick gauche déplace seul, le stick droit vise, les faces lancent 8 compétences sur une cible choisie automatiquement, L interagit — en ne synthétisant que des messages Win32, l'état du jeu étant lu par les crochets existants.

**Architecture:** Un cœur pur `pad::Scheme` (`src/platform/pad_core.*`, testé sur PC) transforme manette + instantané par image (`padst`, rempli par les crochets unité/caméra de `phase_hooks.cpp`) en une liste d'actions injectées via `d2vita_inject`. La glue Vita (`vita_present.cpp`) reste mince ; le schéma legacy est conservé intact (`scheme=mouse`, et hors partie).

**Tech Stack:** C++17, VitaSDK (`/usr/local/vitasdk`), CMake pour le test PC, scripts `tools/build_rt_boot_vpk.sh` / `tools/rt_boot_arm_check.sh`, Vita3K puis console via VitaCompanion.

**Spec :** `docs/superpowers/specs/2026-09-19-manette-ciblage-design.md` — à lire en entier avant de commencer. Les adresses (RVA) qu'elle donne sont vérifiées au désassemblage ; ne pas en inventer d'autres.

**Worktree :** `/home/doudou/repos/wt-manette`, branche `wt/manette`. Build de référence déjà vert (VPK `build-vita/d2vita.vpk`, cache d'objets chaud). Après chaque commit : `git push origin wt/manette` (règle du dépôt : toujours pousser).

**Règles du dépôt à respecter (CLAUDE.md) :** jamais d'écriture mémoire invité ni d'appel de fonction du jeu ; jamais prétendre qu'un test a tourné s'il n'a pas tourné ; un résultat qemu/Vita3K n'est jamais présenté comme un résultat console ; les deux chemins de build restent verts.

---

## Carte des fichiers

| Fichier | Rôle |
|---|---|
| `src/platform/pad_core.h` / `.cpp` (nouveaux) | Cœur pur : projection, choix de cible, orbite, visée au sol, table de survol, machine à états `Scheme`. Aucun include Vita/engine. |
| `tests/pad/pad_core_test.cpp` (nouveau) + `CMakeLists.txt` | Tests PC du cœur (`pad_core_test`). |
| `src/runtime/pad_state.h` / `.cpp` (nouveaux) | Instantané par image écrit par les crochets, lu par le tick d'entrée. |
| `src/runtime/phase_hooks.cpp` (modifié) | Les crochets unité/caméra alimentent `padst` ; armés si replay60 **ou** pad. |
| `tools/rt_boot_srcs.sh` (modifié) | Ajout de `pad_state.cpp` et `pad_core.cpp` à la liste (tous les builds). |
| `src/runtime/scripted_input.cpp` (modifié) | Manette virtuelle `padb`/`pada` sur `D2CMDFILE`. |
| `src/platform/vita_present.cpp` (modifié) | `controls.txt` (nouvelles clés), `aim_tick`, tactile factorisé, réticule, journal `D2_PADLOG`. |
| `docs-site/controles.fr.md`, `docs-site/controles.md` (modifiés) | Doc joueur du nouveau schéma. |

---

## Task 0 : Prendre le worktree en main

**Files:** aucun.

- [ ] **Step 1 : Vérifier l'état**

```bash
cd /home/doudou/repos/wt-manette && git status --short && git log --oneline -3 && ls -la build-vita/d2vita.vpk
```
Attendu : arbre propre (hors `docs/superpowers` déjà commités), HEAD sur `wt/manette`, VPK présent (~8,8 Mo).

- [ ] **Step 2 : Mémoriser les commandes de build (ne pas les retaper de tête)**

```bash
export VITASDK=/usr/local/vitasdk; export PATH="$VITASDK/bin:$PATH"
cd /home/doudou/repos/wt-manette
TARGET=vita bash third_party/winx86/build.sh      # engine (déjà à jour : cache)
bash tools/build_rt_boot_vpk.sh                   # -> build-vita/d2vita.vpk (~13 s incrémental)
```
Si le build échoue sur `build-glide/glide3x.dll introuvable` : `bash tools/build_glide_ring.sh` puis relancer.

- [ ] **Step 3 : Configurer CMake une fois (tests PC)**

```bash
cd /home/doudou/repos/wt-manette && cmake -B build >/dev/null && echo CONFIGURED
```
Attendu : `CONFIGURED`.

---

## Task 1 : pad_core — projection, boîtes, clamp

**Files:**
- Create: `src/platform/pad_core.h`
- Create: `src/platform/pad_core.cpp`
- Create: `tests/pad/pad_core_test.cpp`
- Modify: `CMakeLists.txt` (fin de fichier)

- [ ] **Step 1 : Écrire l'en-tête (partie 1)**

`src/platform/pad_core.h` :

```cpp
// src/platform/pad_core.h — controller scheme v2 ("aim"): pure host logic.
// No VitaSDK, no engine include: everything here is testable on the desktop
// (tests/pad/pad_core_test.cpp). vita_present.cpp feeds it the controller
// state and the per-frame guest snapshot (runtime/pad_state.h) and emits the
// returned actions through d2vita_inject. All coordinates are GAME pixels
// (800x600 or 640x480), +y down. Design: docs/superpowers/specs/
// 2026-09-19-manette-ciblage-design.md
#pragma once
#include <cstdint>

namespace pad {

// Vita button bits (SceCtrlData.buttons), duplicated so the core stays free
// of VitaSDK headers. Values match vita_present.cpp's B_* constants.
enum : uint32_t {
    B_SELECT = 0x000001, B_START = 0x000008, B_UP = 0x000010, B_RIGHT = 0x000020,
    B_DOWN = 0x000040, B_LEFT = 0x000080, B_L = 0x000100, B_R = 0x000200,
    B_TRI = 0x001000, B_CIR = 0x002000, B_CROSS = 0x004000, B_SQR = 0x008000
};

struct Config {
    float deadzone = 0.25f;
    int   orbitMin = 40, orbitMax = 110;   // px at 600 lines (scaled by h/600)
    int   rangeMin = 6,  rangeMax = 20;    // subtiles: ground-cast distance vs stick tilt
    float coneDeg  = 35.f;                 // half-angle of the right-stick aim cone
    int   hoverH   = 28;                   // default hover height above a unit's feet, px
    int   hudH     = 60;                   // bottom band the cursor never enters, px
    bool  aim      = true;                 // automatic hostile target selection
    float sens     = 10.f;                 // free-cursor speed in panels
};

struct Ctl { float lx = 0, ly = 0, rx = 0, ry = 0; uint32_t buttons = 0; };   // sticks in [-1,1]

struct View { int w = 800, h = 600; int32_t playerFx = 0, playerFy = 0, viewX = 0, viewY = 0; };

// A drawn unit as the core sees it: already projected to game pixels.
struct Unit {
    uint32_t id = 0, type = 0, cls = 0, mode = 0;
    int sx = 0, sy = 0;             // feet anchor on screen
    bool hostile = false;           // can be a skill target
    bool interact = false;          // can be an L target (item, object, NPC)
};

struct Ctx {
    bool inGame = false, panelOpen = false, skillTree = false;
    uint32_t selValid = 0, selId = 0, selType = 0;   // unit the GAME hovers (previous frame)
    uint32_t playerId = 0;
};

enum ActKind { A_MOVE, A_LDOWN, A_LUP, A_RDOWN, A_RUP, A_CLICK, A_KEYDOWN, A_KEYUP, A_KEY };
struct Action { ActKind k; int a, b; };
struct Actions {
    enum { MAX = 24 };
    Action v[MAX]; int n = 0;
    void push(ActKind k, int a = 0, int b = 0) { if (n < MAX) v[n++] = Action{k, a, b}; }
};

struct Target { bool has = false, verified = false; uint32_t id = 0; int sx = 0, sy = 0; };

// ---- pure helpers -----------------------------------------------------------
// sx = (fx - fy)*16/65536 - viewX ; sy = (fx + fy)*8/65536 - viewY  (fx/fy 16.16 fine)
void world_to_screen(const View& v, int32_t fx, int32_t fy, int* sx, int* sy);
// Inverse of the delta part (direction only, not normalized).
void screen_dir_to_world(float dx, float dy, float* wx, float* wy);
void clamp_point(const View& v, const Config& cfg, int* px, int* py);
bool in_unit_box(const Unit& u, int x, int y);

} // namespace pad
```

- [ ] **Step 2 : Écrire le test (partie 1)**

`tests/pad/pad_core_test.cpp` :

```cpp
// tests/pad/pad_core_test.cpp — desktop tests for src/platform/pad_core.*
// Plain asserts, no framework: `./build/pad_core_test` exits 0 when green.
#include "platform/pad_core.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// A view where the player projects exactly to (400,300): viewX/Y absorb the
// player's absolute position.
static pad::View mkView(int32_t pfx = 100 << 16, int32_t pfy = 50 << 16) {
    pad::View v; v.w = 800; v.h = 600; v.playerFx = pfx; v.playerFy = pfy;
    v.viewX = (int32_t)(((int64_t)(pfx - pfy) * 16) / 65536) - 400;
    v.viewY = (int32_t)(((int64_t)(pfx + pfy) * 8) / 65536) - 300;
    return v;
}

static void test_projection() {
    pad::View v = mkView();
    int sx, sy;
    pad::world_to_screen(v, v.playerFx, v.playerFy, &sx, &sy);
    CHECK(sx == 400 && sy == 300);
    // +1 subtile in x -> +16 px right, +8 px down ; +1 in y -> -16, +8
    pad::world_to_screen(v, v.playerFx + (1 << 16), v.playerFy, &sx, &sy);
    CHECK(sx == 416 && sy == 308);
    pad::world_to_screen(v, v.playerFx, v.playerFy + (1 << 16), &sx, &sy);
    CHECK(sx == 384 && sy == 308);
    // inverse: a screen direction maps back to the same screen direction
    float wx, wy; pad::screen_dir_to_world(1.f, 0.f, &wx, &wy);
    int sx2, sy2;
    pad::world_to_screen(v, v.playerFx + (int32_t)(wx * 65536.f * 10.f), v.playerFy + (int32_t)(wy * 65536.f * 10.f), &sx2, &sy2);
    CHECK(sx2 > 400 && sy2 == 300);
}

static void test_clamp_and_box() {
    pad::View v = mkView(); pad::Config cfg;
    int x = -20, y = 590;
    pad::clamp_point(v, cfg, &x, &y);
    CHECK(x == 4 && y == 600 - cfg.hudH - 1);
    pad::Unit m; m.type = 1; m.sx = 500; m.sy = 300;
    CHECK(pad::in_unit_box(m, 500, 280));
    CHECK(pad::in_unit_box(m, 484, 252));
    CHECK(!pad::in_unit_box(m, 500, 240));
    CHECK(!pad::in_unit_box(m, 530, 290));
    pad::Unit it; it.type = 4; it.sx = 100; it.sy = 100;
    CHECK(pad::in_unit_box(it, 110, 105));
    CHECK(!pad::in_unit_box(it, 130, 100));
    pad::Unit ms; ms.type = 3; ms.sx = 100; ms.sy = 100;
    CHECK(!pad::in_unit_box(ms, 100, 100));
}

int main() {
    test_projection();
    test_clamp_and_box();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 3 : Cible CMake**

Ajouter à la fin de `CMakeLists.txt` :

```cmake
# -- Controller scheme v2 core: pure host logic, desktop-testable -------------
enable_testing()
add_executable(pad_core_test tests/pad/pad_core_test.cpp src/platform/pad_core.cpp)
target_include_directories(pad_core_test PRIVATE src)
add_test(NAME pad_core COMMAND pad_core_test)
```

- [ ] **Step 4 : Lancer le test, vérifier qu'il échoue (pas d'implémentation)**

```bash
cd /home/doudou/repos/wt-manette && : > src/platform/pad_core.cpp && cmake -B build >/dev/null && cmake --build build --target pad_core_test -j4 2>&1 | tail -3
```
Attendu : erreur de lien (`undefined reference to pad::world_to_screen`) : le fichier d'implémentation est vide pour l'instant.

- [ ] **Step 5 : Implémenter (partie 1)**

`src/platform/pad_core.cpp` :

```cpp
// src/platform/pad_core.cpp — see pad_core.h.
#include "platform/pad_core.h"
#include <cmath>
#include <cstring>

namespace pad {

static const float kPi = 3.14159265358979f;

void world_to_screen(const View& v, int32_t fx, int32_t fy, int* sx, int* sy) {
    const int64_t dx = (int64_t)fx - (int64_t)fy;
    const int64_t sm = (int64_t)fx + (int64_t)fy;
    *sx = (int)((dx * 16) / 65536) - v.viewX;
    *sy = (int)((sm * 8) / 65536) - v.viewY;
}

void screen_dir_to_world(float dx, float dy, float* wx, float* wy) {
    *wx = dx / 32.f + dy / 16.f;
    *wy = dy / 16.f - dx / 32.f;
}

void clamp_point(const View& v, const Config& cfg, int* px, int* py) {
    if (*px < 4) *px = 4;
    if (*px > v.w - 5) *px = v.w - 5;
    const int ymax = v.h - cfg.hudH - 1;
    if (*py < 4) *py = 4;
    if (*py > ymax) *py = ymax;
}

bool in_unit_box(const Unit& u, int x, int y) {
    if (u.type == 0 || u.type == 1)
        return x >= u.sx - 16 && x <= u.sx + 16 && y >= u.sy - 48 && y <= u.sy + 4;
    if (u.type == 2 || u.type == 4)
        return x >= u.sx - 20 && x <= u.sx + 20 && y >= u.sy - 16 && y <= u.sy + 8;
    return false;
}

} // namespace pad
```

- [ ] **Step 6 : Test vert**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error|warning' ; ./build/pad_core_test
```
Attendu : `13 passed, 0 failed`.

- [ ] **Step 7 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/platform/pad_core.h src/platform/pad_core.cpp tests/pad/pad_core_test.cpp CMakeLists.txt && git commit -m "pad: core v2 — projection monde/écran, boîtes de sprites, clamp (testé PC)" && git push origin wt/manette
```

---

## Task 2 : pad_core — choix de cible et table de survol

**Files:**
- Modify: `src/platform/pad_core.h` (avant la fermeture du namespace)
- Modify: `src/platform/pad_core.cpp`
- Modify: `tests/pad/pad_core_test.cpp`

- [ ] **Step 1 : Déclarations**

Ajouter dans `pad_core.h`, après `in_unit_box` :

```cpp
// Hostile target: right stick pushed -> nearest-ish inside a +-coneDeg cone
// (score = distance + 300*(1-cos)), <= 500 px; idle -> nearest <= 420 px.
// `current` = index of the current target in `u` (or -1); kept while it
// qualifies and its score <= 1.25*best + 20 (hysteresis). Returns -1 if none.
int  pick_hostile(const Unit* u, int n, const View& v, const Ctl& c, const Config& cfg, int current);
// L target: nearest item (type 4) <= 220 px, else nearest other interactable
// (object / town NPC) <= 160 px. Returns -1 if none.
int  pick_interact(const Unit* u, int n, const View& v, const Config& cfg);

// Learned hover height per (type, class): where the cursor must sit for the
// game to hover a unit of that class. Reset at boot (RAM only).
struct HoverTable {
    enum { N = 64 };
    uint32_t key[N]; int h[N]; int n = 0;
    int  get(uint32_t type, uint32_t cls, int def) const;
    void learn(uint32_t type, uint32_t cls, int hv);
    static int try_seq(int attempt, int def);   // 0 -> def, then 14, 44, 64, 90
};
```

- [ ] **Step 2 : Tests**

Ajouter dans `pad_core_test.cpp` avant `main` :

```cpp
static pad::Unit mkMon(uint32_t id, int sx, int sy, bool hostile = true) {
    pad::Unit u; u.id = id; u.type = 1; u.cls = 10; u.mode = 1; u.sx = sx; u.sy = sy; u.hostile = hostile; return u;
}

static void test_pick_hostile() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    pad::Unit u[4] = { mkMon(1, 600, 300), mkMon(2, 450, 300), mkMon(3, 400, 100, false), mkMon(4, 400, 320) };
    u[3].mode = 12;  u[3].hostile = false;   // corpse flagged by the glue
    // idle stick: nearest hostile
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == 1);
    // aimed to the right: the far right one wins over the near one? no: both in cone, near wins
    c.rx = 1.f; c.ry = 0.f;
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == 1);
    // aimed up: nothing hostile in the cone (unit 3 is not hostile)
    c.rx = 0.f; c.ry = -1.f;
    CHECK(pad::pick_hostile(u, 4, v, c, cfg, -1) == -1);
    // hysteresis: current target kept while close to best
    c.rx = 0.f; c.ry = 0.f;
    pad::Unit w[2] = { mkMon(1, 460, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(w, 2, v, c, cfg, 0) == 0);
    pad::Unit z[2] = { mkMon(1, 700, 300), mkMon(2, 450, 300) };
    CHECK(pad::pick_hostile(z, 2, v, c, cfg, 0) == 1);
    // range and cone limits
    pad::Unit far1[1] = { mkMon(9, 790, 300) };               // 390 px <= 420: taken when idle
    c.rx = 0.f; c.ry = 0.f;
    CHECK(pad::pick_hostile(far1, 1, v, c, cfg, -1) == 0);
    pad::Unit far2[1] = { mkMon(9, 400, 100) };               // 200 px straight up
    c.rx = 1.f; c.ry = 0.f;                                   // aimed right: out of the cone
    CHECK(pad::pick_hostile(far2, 1, v, c, cfg, -1) == -1);
}

static void test_pick_interact() {
    pad::View v = mkView(); pad::Config cfg;
    pad::Unit u[3];
    u[0].id = 1; u[0].type = 2; u[0].sx = 420; u[0].sy = 300; u[0].interact = true;    // chest, 20 px
    u[1].id = 2; u[1].type = 4; u[1].sx = 560; u[1].sy = 300; u[1].interact = true;    // item, 160 px
    u[2].id = 3; u[2].type = 4; u[2].sx = 700; u[2].sy = 300; u[2].interact = true;    // item, 300 px (too far)
    CHECK(pad::pick_interact(u, 3, v, cfg) == 1);        // items first, within 220
    u[1].sx = 700;                                         // both items too far -> the chest
    CHECK(pad::pick_interact(u, 3, v, cfg) == 0);
    u[0].sx = 600;                                         // chest at 200 px: beyond 160 -> nothing
    CHECK(pad::pick_interact(u, 3, v, cfg) == -1);
}

static void test_hover_table() {
    pad::HoverTable t;
    CHECK(t.get(1, 42, 28) == 28);
    t.learn(1, 42, 44);
    CHECK(t.get(1, 42, 28) == 44);
    CHECK(t.get(2, 42, 20) == 20);
    t.learn(1, 42, 64);
    CHECK(t.get(1, 42, 28) == 64 && t.n == 1);
    CHECK(pad::HoverTable::try_seq(0, 28) == 28);
    CHECK(pad::HoverTable::try_seq(1, 28) == 14);
    CHECK(pad::HoverTable::try_seq(4, 28) == 90);
    CHECK(pad::HoverTable::try_seq(5, 28) == 28);
}
```

Dans `main`, ajouter les appels `test_pick_hostile(); test_pick_interact(); test_hover_table();` après `test_clamp_and_box();`.

- [ ] **Step 3 : Test rouge**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error' | head -3
```
Attendu : erreurs `undefined reference` (pick_hostile, pick_interact, HoverTable).

- [ ] **Step 4 : Implémenter**

Ajouter dans `pad_core.cpp` (dans le namespace, après `in_unit_box`) :

```cpp
int pick_hostile(const Unit* u, int n, const View& v, const Ctl& c, const Config& cfg, int current) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    float ax = c.rx, ay = c.ry;
    const float am = std::sqrt(ax * ax + ay * ay);
    const bool aimed = am > cfg.deadzone;
    if (aimed) { ax /= am; ay /= am; }
    const float cosMin = std::cos(cfg.coneDeg * kPi / 180.f);
    const float maxD = aimed ? 500.f : 420.f;
    auto score = [&](int i, float* s) -> bool {
        const Unit& t = u[i];
        if (!t.hostile) return false;
        const float dx = (float)(t.sx - psx), dy = (float)(t.sy - psy);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > maxD) return false;
        if (aimed) {
            const float cs = d > 1.f ? (dx * ax + dy * ay) / d : 1.f;
            if (cs < cosMin) return false;
            *s = d + 300.f * (1.f - cs);
        } else *s = d;
        return true;
    };
    int best = -1; float bs = 0.f;
    for (int i = 0; i < n; ++i) { float s; if (score(i, &s) && (best < 0 || s < bs)) { best = i; bs = s; } }
    if (current >= 0 && current < n) {
        float s;
        if (score(current, &s) && (best < 0 || s <= bs * 1.25f + 20.f)) return current;
    }
    return best;
}

int pick_interact(const Unit* u, int n, const View& v, const Config& /*cfg*/) {
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    int best = -1; float bd = 0.f;
    for (int pass = 0; pass < 2 && best < 0; ++pass) {          // pass 0: items ; pass 1: the rest
        const float maxD = pass == 0 ? 220.f : 160.f;
        for (int i = 0; i < n; ++i) {
            const Unit& t = u[i];
            if (!t.interact) continue;
            if ((pass == 0) != (t.type == 4)) continue;
            const float dx = (float)(t.sx - psx), dy = (float)(t.sy - psy);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > maxD) continue;
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
    }
    return best;
}

int HoverTable::get(uint32_t type, uint32_t cls, int def) const {
    const uint32_t k = (type << 16) | (cls & 0xffffu);
    for (int i = 0; i < n; ++i) if (key[i] == k) return h[i];
    return def;
}

void HoverTable::learn(uint32_t type, uint32_t cls, int hv) {
    const uint32_t k = (type << 16) | (cls & 0xffffu);
    for (int i = 0; i < n; ++i) if (key[i] == k) { h[i] = hv; return; }
    if (n < N) { key[n] = k; h[n] = hv; ++n; }
}

int HoverTable::try_seq(int attempt, int def) {
    static const int seq[4] = { 14, 44, 64, 90 };
    if (attempt <= 0 || attempt > 4) return def;
    return seq[attempt - 1];
}
```

- [ ] **Step 5 : Test vert**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error|warning'; ./build/pad_core_test
```
Attendu : `… passed, 0 failed`.

- [ ] **Step 6 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add -A src/platform/pad_core.h src/platform/pad_core.cpp tests/pad/pad_core_test.cpp && git commit -m "pad: choix de cible (cône stick droit, hystérésis), cible d'interaction, table de survol apprise" && git push origin wt/manette
```

---

## Task 3 : pad_core — point d'orbite et point de visée au sol

**Files:**
- Modify: `src/platform/pad_core.h`, `src/platform/pad_core.cpp`, `tests/pad/pad_core_test.cpp`

- [ ] **Step 1 : Déclarations**

Ajouter dans `pad_core.h` après `HoverTable` :

```cpp
// Left-stick "move only" click point: on a ring around the player (radius
// orbitMin..orbitMax by tilt, scaled by h/600), rotated/shrunk until it sits
// on no unit box. Returns false when the stick is inside the dead zone.
bool orbit_point(const View& v, const Ctl& c, const Config& cfg, const Unit* u, int n, int* px, int* py);
// Ground cast point: along the right stick (or the fallback direction
// fbx/fby when idle), rangeMin..rangeMax subtiles by tilt, clamped on screen.
void ground_point(const View& v, const Ctl& c, const Config& cfg, float fbx, float fby, int* px, int* py);
```

- [ ] **Step 2 : Tests**

Ajouter avant `main` :

```cpp
static void test_orbit() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    int px = 0, py = 0;
    CHECK(!pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));           // idle
    c.lx = 1.f; c.ly = 0.f;                                               // full right
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(px == 400 + cfg.orbitMax && py == 300);
    c.lx = 0.5f;                                                          // half tilt
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(px > 400 + cfg.orbitMin && px < 400 + cfg.orbitMax && py == 300);
    // a monster standing exactly on the ring: the point moves off its box
    c.lx = 1.f;
    pad::Unit m = mkMon(7, 400 + cfg.orbitMax, 300 + 10);
    CHECK(pad::orbit_point(v, c, cfg, &m, 1, &px, &py));
    CHECK(!pad::in_unit_box(m, px, py));
    // never below the HUD band
    c.lx = 0.f; c.ly = 1.f; v.viewY -= 250;                               // player low on screen
    CHECK(pad::orbit_point(v, c, cfg, nullptr, 0, &px, &py));
    CHECK(py <= 600 - cfg.hudH - 1);
}

static void test_ground_point() {
    pad::View v = mkView(); pad::Config cfg; pad::Ctl c;
    int px, py;
    c.rx = 1.f; c.ry = 0.f;                                               // full right -> rangeMax subtiles
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    CHECK(py == 300 && px > 400);
    const int farX = px;
    c.rx = 0.5f;                                                          // half tilt -> closer
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    CHECK(px > 400 && px < farX);
    c.rx = 0.f; c.ry = 0.f;                                               // idle -> fallback direction (down), rangeMin
    pad::ground_point(v, c, cfg, 0.f, 1.f, &px, &py);
    // straight down on screen = world diagonal: ~11.3 px per subtile
    CHECK(px == 400 && py > 300 && py < 300 + 12 * cfg.rangeMin + 2);
}
```
Ajouter `test_orbit(); test_ground_point();` dans `main`.

- [ ] **Step 3 : Test rouge** — même commande qu'en Task 2 Step 3 ; attendu : `undefined reference` (orbit_point, ground_point).

- [ ] **Step 4 : Implémenter**

Ajouter dans `pad_core.cpp` :

```cpp
bool orbit_point(const View& v, const Ctl& c, const Config& cfg, const Unit* u, int n, int* px, int* py) {
    float lx = c.lx, ly = c.ly;
    const float m = std::sqrt(lx * lx + ly * ly);
    if (m <= cfg.deadzone) return false;
    lx /= m; ly /= m;
    float t = (m - cfg.deadzone) / (1.f - cfg.deadzone);
    if (t > 1.f) t = 1.f;
    const float sc = (float)v.h / 600.f;
    const float r = (cfg.orbitMin + t * (cfg.orbitMax - cfg.orbitMin)) * sc;
    int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
    static const float dAng[7] = { 0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f };
    for (int shrink = 0; shrink < 3; ++shrink) {
        const float rr = r - shrink * 20.f * sc;
        if (rr < 16.f * sc) break;
        for (int k = 0; k < 7; ++k) {
            const float a = dAng[k] * kPi / 180.f, ca = std::cos(a), sa = std::sin(a);
            const float dx = lx * ca - ly * sa, dy = lx * sa + ly * ca;
            int x = psx + (int)std::lround(dx * rr), y = psy + (int)std::lround(dy * rr);
            clamp_point(v, cfg, &x, &y);
            bool hit = false;
            for (int i = 0; i < n; ++i) if (in_unit_box(u[i], x, y)) { hit = true; break; }
            if (!hit) { *px = x; *py = y; return true; }
        }
    }
    *px = psx + (int)std::lround(lx * r); *py = psy + (int)std::lround(ly * r);
    clamp_point(v, cfg, px, py);
    return true;
}

void ground_point(const View& v, const Ctl& c, const Config& cfg, float fbx, float fby, int* px, int* py) {
    float dx = c.rx, dy = c.ry;
    const float m = std::sqrt(dx * dx + dy * dy);
    float t;
    if (m > cfg.deadzone) {
        dx /= m; dy /= m;
        t = (m - cfg.deadzone) / (1.f - cfg.deadzone);
        if (t > 1.f) t = 1.f;
    } else {
        const float fm = std::sqrt(fbx * fbx + fby * fby);
        if (fm < 1e-3f) { dx = 0.f; dy = 1.f; } else { dx = fbx / fm; dy = fby / fm; }
        t = 0.f;
    }
    float wx, wy; screen_dir_to_world(dx, dy, &wx, &wy);
    const float wm = std::sqrt(wx * wx + wy * wy);
    wx /= wm; wy /= wm;
    const float dist = cfg.rangeMin + t * (cfg.rangeMax - cfg.rangeMin);      // subtiles
    const int32_t fx = v.playerFx + (int32_t)(wx * dist * 65536.f);
    const int32_t fy = v.playerFy + (int32_t)(wy * dist * 65536.f);
    world_to_screen(v, fx, fy, px, py);
    clamp_point(v, cfg, px, py);
}
```

- [ ] **Step 5 : Test vert** — `./build/pad_core_test` → `0 failed`.

- [ ] **Step 6 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add -A src/platform tests/pad && git commit -m "pad: point d'orbite évitant les sprites, point de visée au sol proportionnel à l'inclinaison" && git push origin wt/manette
```

---

## Task 4 : pad_core — la machine à états `Scheme`

**Files:**
- Modify: `src/platform/pad_core.h`, `src/platform/pad_core.cpp`, `tests/pad/pad_core_test.cpp`

- [ ] **Step 1 : Déclaration de la classe**

Ajouter dans `pad_core.h` après `ground_point` :

```cpp
// The scheme itself: one tick per controller sample (30 Hz). Emits the Win32
// actions the glue injects, in order. Modes: WORLD (in game, no panel), PANEL
// (in game, a panel open). Out of game the glue does not call tick(); it
// calls leave() once so everything held is released.
class Scheme {
public:
    explicit Scheme(const Config& c) : cfg_(c) {}
    void tick(const Ctl& c, const Ctx& x, const View& v, const Unit* units, int n, Actions& out);
    void leave(Actions& out);                      // release everything, forget the mode
    Target target() const { return tgt_; }
    bool cursorOwned() const { return lmb_ || rmb_ || interact_; }
    int  cx() const { return cx_; }
    int  cy() const { return cy_; }
    void setCursor(int x, int y) { cx_ = x; cy_ = y; }   // touch moved the cursor
    bool aimActive() const { return aimActive_; }
    int  aimX() const { return aimX_; }
    int  aimY() const { return aimY_; }
    bool casting() const { return castSlot_ >= 0; }

private:
    enum Mode { M_NONE, M_WORLD, M_PANEL };
    struct Held { int vk = 0; bool shift = false; };
    void moveTo(int x, int y, Actions& out);
    void worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, uint32_t down, uint32_t up, Actions& out);
    void panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out);
    void commonButtons(const Ctl& c, uint32_t down, uint32_t up, Actions& out);
    void releaseAll(Actions& out);
    int  findId(const Unit* u, int n, uint32_t id) const;
    void hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const;

    Config   cfg_;
    Mode     mode_ = M_NONE;
    uint32_t prev_ = 0;
    int      cx_ = 400, cy_ = 300;
    bool     lsOn_ = false, lmb_ = false, rmb_ = false;
    float    lastDx_ = 0.f, lastDy_ = 1.f;
    // cast in progress
    int      castSlot_ = -1; uint32_t castBit_ = 0;
    uint32_t castId_ = 0, castType_ = 0, castCls_ = 0;
    int      castAttempt_ = 0, castH_ = 0; bool castVerified_ = false;
    // interaction in progress
    bool     interact_ = false, interNoop_ = false;
    uint32_t interId_ = 0, interType_ = 0, interCls_ = 0;
    int      interAttempt_ = 0, interH_ = 0; bool interVerified_ = false;
    // modifiers / keys held
    bool     alt_ = false, shiftSq_ = false, esc_ = false, wkey_ = false;
    Held     dpad_[4];
    // overlay
    uint32_t tgtId_ = 0; Target tgt_;
    bool     aimActive_ = false; int aimX_ = 0, aimY_ = 0;
    HoverTable hover_;
};
```

- [ ] **Step 2 : Tests de séquences**

Ajouter avant `main` :

```cpp
static bool hasAct(const pad::Actions& a, pad::ActKind k, int x = -1, int y = -1) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k && (x < 0 || a.v[i].a == x) && (y < 0 || a.v[i].b == y)) return true;
    return false;
}
static int actIndex(const pad::Actions& a, pad::ActKind k) {
    for (int i = 0; i < a.n; ++i) if (a.v[i].k == k) return i;
    return -1;
}

static void test_scheme_cast() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.playerId = 99;
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    pad::Ctl c; pad::Actions a;
    s.tick(c, x, v, u, 1, a);                                   // settle, nothing pressed
    CHECK(a.n == 0);
    CHECK(s.target().has && s.target().id == 5 && !s.target().verified);
    // press Cross: F1, cursor on the target (28 px above its feet), right button down
    c.buttons = pad::B_CROSS; a = pad::Actions{};
    s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x70));
    CHECK(hasAct(a, pad::A_MOVE, 500, 272));
    CHECK(hasAct(a, pad::A_RDOWN, 500, 272));
    CHECK(actIndex(a, pad::A_KEY) < actIndex(a, pad::A_RDOWN));
    CHECK(s.casting() && s.cursorOwned());
    // held, game does not hover it yet: second attempt height (14)
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 286));
    // now the game hovers it: verified, learned
    x.selValid = 1; x.selId = 5; x.selType = 1;
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(s.target().verified);
    // release: right button up, no more cast
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP) && !s.casting());
    // next cast of the same class starts at the learned height (14)
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE, 500, 286));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // R + Triangle = slot 8 (F8)
    c.buttons = pad::B_R | pad::B_TRI; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x77));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP));
    // no hostile: ground cast along the fallback direction
    c.buttons = pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEY, 0x71) && hasAct(a, pad::A_RDOWN));
    CHECK(!s.target().has);
}

static void test_scheme_move() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.playerId = 99;
    pad::Ctl c; pad::Actions a;
    c.lx = 1.f; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_MOVE, 400 + cfg.orbitMax, 300));
    CHECK(hasAct(a, pad::A_LDOWN, 400 + cfg.orbitMax, 300));
    a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);        // held: nothing new (same point)
    CHECK(a.n == 0);
    c.lx = 0.f; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);   // release: LUP then stop click at the feet
    CHECK(hasAct(a, pad::A_LUP));
    CHECK(hasAct(a, pad::A_CLICK, 400, 306));
    CHECK(actIndex(a, pad::A_LUP) < actIndex(a, pad::A_CLICK));
    // moving then casting: the left button is released for the cast, re-pressed after
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(actIndex(a, pad::A_LUP) >= 0 && actIndex(a, pad::A_LUP) < actIndex(a, pad::A_RDOWN));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP));
    a = pad::Actions{}; s.tick(c, x, v, u, 1, a);              // stick still pushed: movement resumes
    CHECK(hasAct(a, pad::A_LDOWN));
}

static void test_scheme_interact_and_keys() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.playerId = 99;
    pad::Unit u[2] = { mkMon(5, 500, 300) };
    u[1].id = 8; u[1].type = 4; u[1].sx = 430; u[1].sy = 310; u[1].interact = true;
    pad::Ctl c; pad::Actions a;
    c.buttons = pad::B_L; s.tick(c, x, v, u, 2, a);                  // L: the item wins over the monster
    CHECK(hasAct(a, pad::A_MOVE, 430, 304) && hasAct(a, pad::A_LDOWN, 430, 304));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 2, a);
    CHECK(hasAct(a, pad::A_LUP));
    // L with nothing around and no target: no click at all
    a = pad::Actions{}; c.buttons = pad::B_L; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(a.n == 0);
    // R then L = Alt held ; released with either
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    c.buttons = pad::B_R | pad::B_L; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x12) && !hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_L; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x12));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    // D-pad up = potion 1 ; R + D-pad left = Shift + potion 2 (merc)
    c.buttons = pad::B_UP; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x31));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x31));
    c.buttons = pad::B_R | pad::B_LEFT; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x10) && hasAct(a, pad::A_KEYDOWN, 0x32));
    c.buttons = pad::B_R; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x32) && hasAct(a, pad::A_KEYUP, 0x10));
    // R + Start = W ; Start = Escape
    c.buttons = pad::B_R | pad::B_START; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x57));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x57));
    c.buttons = pad::B_START; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x1B));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, nullptr, 0, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x1B));
}

static void test_scheme_panel_and_leave() {
    pad::Config cfg; pad::Scheme s(cfg); pad::View v = mkView();
    pad::Ctx x; x.inGame = true; x.playerId = 99;
    pad::Unit u[1] = { mkMon(5, 500, 300) };
    pad::Ctl c; pad::Actions a;
    // a cast is held when a panel opens: it is released on the mode change
    c.buttons = pad::B_CROSS; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RDOWN));
    x.panelOpen = true; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_RUP) && !s.casting());
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // panel: Circle = Escape ; Cross = click at the cursor ; Square = Shift held
    s.setCursor(200, 200);
    c.buttons = pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x1B));
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LDOWN, 200, 200));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_LUP, 200, 200));
    c.buttons = pad::B_SQR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEYDOWN, 0x10));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x10));
    // left stick moves the free cursor in a panel
    c.lx = 1.f; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_MOVE) && s.cx() > 200);
    c.lx = 0.f;
    // skill tree: faces are hotkeys, not clicks
    x.skillTree = true;
    c.buttons = pad::B_CROSS; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x70) && !hasAct(a, pad::A_LDOWN));
    c.buttons = pad::B_R | pad::B_CIR; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    CHECK(hasAct(a, pad::A_KEY, 0x75));
    c.buttons = 0; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    // leave() while a Shift is held releases it
    c.buttons = pad::B_SQR; x.skillTree = false; a = pad::Actions{}; s.tick(c, x, v, u, 1, a);
    a = pad::Actions{}; s.leave(a);
    CHECK(hasAct(a, pad::A_KEYUP, 0x10));
}
```
Ajouter dans `main` : `test_scheme_cast(); test_scheme_move(); test_scheme_interact_and_keys(); test_scheme_panel_and_leave();`.

- [ ] **Step 3 : Test rouge** — `undefined reference to pad::Scheme::tick` etc.

- [ ] **Step 4 : Implémenter la machine**

Ajouter dans `pad_core.cpp` :

```cpp
// ---------------------------------------------------------------------------
// Scheme
// ---------------------------------------------------------------------------
static const uint32_t kFaceBits[4] = { B_CROSS, B_CIR, B_SQR, B_TRI };
static const uint32_t kDpadBits[4] = { B_UP, B_LEFT, B_DOWN, B_RIGHT };   // belt 1..4, same order as the legacy table

void Scheme::moveTo(int x, int y, Actions& out) {
    if (x != cx_ || y != cy_) { cx_ = x; cy_ = y; out.push(A_MOVE, x, y); }
}

int Scheme::findId(const Unit* u, int n, uint32_t id) const {
    if (!id) return -1;
    for (int i = 0; i < n; ++i) if (u[i].id == id) return i;
    return -1;
}

void Scheme::hoverPoint(const Unit& t, int h, const View& v, int* px, int* py) const {
    *px = t.sx; *py = t.sy - h;
    clamp_point(v, cfg_, px, py);
}

void Scheme::releaseAll(Actions& out) {
    if (rmb_)    { out.push(A_RUP, cx_, cy_); rmb_ = false; }
    if (lmb_)    { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    if (alt_)    { out.push(A_KEYUP, 0x12); alt_ = false; }
    if (shiftSq_){ out.push(A_KEYUP, 0x10); shiftSq_ = false; }
    if (esc_)    { out.push(A_KEYUP, 0x1B); esc_ = false; }
    if (wkey_)   { out.push(A_KEYUP, 0x57); wkey_ = false; }
    for (Held& h : dpad_) if (h.vk) { out.push(A_KEYUP, h.vk); if (h.shift) out.push(A_KEYUP, 0x10); h = Held{}; }
    castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
    interact_ = false; interNoop_ = false; interId_ = 0; interVerified_ = false;
    lsOn_ = false; tgt_ = Target{}; tgtId_ = 0; aimActive_ = false;
}

void Scheme::leave(Actions& out) { releaseAll(out); mode_ = M_NONE; prev_ = 0; }

void Scheme::tick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, Actions& out) {
    const Mode m = !x.inGame ? M_NONE : (x.panelOpen ? M_PANEL : M_WORLD);
    if (m != mode_) { releaseAll(out); mode_ = m; }
    const uint32_t down = c.buttons & ~prev_, up = prev_ & ~c.buttons;
    if (mode_ == M_WORLD)      worldTick(c, x, v, u, n, down, up, out);
    else if (mode_ == M_PANEL) panelTick(c, x, v, down, up, out);
    prev_ = c.buttons;
}

// Start / D-pad / Alt — identical in both modes.
void Scheme::commonButtons(const Ctl& c, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    if (down & B_START) {
        if (layer) { out.push(A_KEYDOWN, 0x57); wkey_ = true; }       // W: weapon swap
        else       { out.push(A_KEYDOWN, 0x1B); esc_ = true; }        // Escape
    }
    if (up & B_START) {
        if (wkey_) { out.push(A_KEYUP, 0x57); wkey_ = false; }
        if (esc_)  { out.push(A_KEYUP, 0x1B); esc_ = false; }
    }
    for (int i = 0; i < 4; ++i) {
        if (down & kDpadBits[i]) {
            dpad_[i].vk = 0x31 + i; dpad_[i].shift = layer;
            if (layer) out.push(A_KEYDOWN, 0x10);                     // Shift + belt key = potion to the mercenary
            out.push(A_KEYDOWN, 0x31 + i);
        }
        if ((up & kDpadBits[i]) && dpad_[i].vk) {
            out.push(A_KEYUP, dpad_[i].vk);
            if (dpad_[i].shift) out.push(A_KEYUP, 0x10);
            dpad_[i] = Held{};
        }
    }
    // Alt (ground item labels): R held first, then L. Ends when either goes up.
    if ((down & B_L) && layer && !interact_ && castSlot_ < 0) { out.push(A_KEYDOWN, 0x12); alt_ = true; }
    if (alt_ && ((up & B_L) || (up & B_R))) { out.push(A_KEYUP, 0x12); alt_ = false; }
}

void Scheme::worldTick(const Ctl& c, const Ctx& x, const View& v, const Unit* u, int n, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    commonButtons(c, down, up, out);

    // ---- hostile target of the moment (overlay + next cast) ----
    int ti = -1;
    if (cfg_.aim) { ti = pick_hostile(u, n, v, c, cfg_, findId(u, n, tgtId_)); tgtId_ = ti >= 0 ? u[ti].id : 0; }
    {
        const float am = std::sqrt(c.rx * c.rx + c.ry * c.ry);
        aimActive_ = am > cfg_.deadzone;
        if (aimActive_) ground_point(v, c, cfg_, lastDx_, lastDy_, &aimX_, &aimY_);
    }

    // ---- cast: faces = slots 1-4, R + faces = 5-8 ----
    if (castSlot_ < 0 && !interact_) {
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) {
            castSlot_ = i + (layer ? 4 : 0); castBit_ = kFaceBits[i];
            castAttempt_ = 0; castVerified_ = false;
            out.push(A_KEY, 0x70 + castSlot_);                          // F1..F8 selects the right skill
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }      // movement suspended while casting
            int px, py;
            if (ti >= 0) {
                castId_ = u[ti].id; castType_ = u[ti].type; castCls_ = u[ti].cls;
                castH_ = hover_.get(castType_, castCls_, cfg_.hoverH);
                hoverPoint(u[ti], castH_, v, &px, &py);
            } else { castId_ = 0; ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py); }
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);              // forced move: a new cast always re-places the cursor
            out.push(A_RDOWN, px, py); rmb_ = true;
            break;
        }
    } else if (castSlot_ >= 0) {
        if (!(c.buttons & castBit_)) {
            out.push(A_RUP, cx_, cy_); rmb_ = false;
            castSlot_ = -1; castBit_ = 0; castId_ = 0; castVerified_ = false;
        } else {
            int px = cx_, py = cy_;
            int ci = findId(u, n, castId_);
            if (castId_ && (ci < 0 || !u[ci].hostile)) {               // target died or left: re-pick
                ci = cfg_.aim ? pick_hostile(u, n, v, c, cfg_, -1) : -1;
                if (ci >= 0) {
                    castId_ = u[ci].id; castType_ = u[ci].type; castCls_ = u[ci].cls;
                    castAttempt_ = 0; castVerified_ = false;
                    castH_ = hover_.get(castType_, castCls_, cfg_.hoverH);
                } else castId_ = 0;
            }
            if (ci >= 0 && castId_) {
                if (!castVerified_) {
                    if (x.selValid && x.selId == castId_ && x.selType == castType_) {
                        castVerified_ = true; hover_.learn(castType_, castCls_, castH_);
                    } else if (castAttempt_ < 4) {
                        ++castAttempt_;
                        castH_ = HoverTable::try_seq(castAttempt_, hover_.get(castType_, castCls_, cfg_.hoverH));
                    }
                }
                hoverPoint(u[ci], castH_, v, &px, &py);
            } else ground_point(v, c, cfg_, lastDx_, lastDy_, &px, &py);
            moveTo(px, py, out);
        }
    }
    // overlay target
    if (castSlot_ >= 0 && castId_) {
        const int ci = findId(u, n, castId_);
        tgt_ = Target{};
        if (ci >= 0) { tgt_.has = true; tgt_.id = castId_; tgt_.sx = u[ci].sx; tgt_.sy = u[ci].sy; tgt_.verified = castVerified_; }
    } else if (ti >= 0) { tgt_ = Target{}; tgt_.has = true; tgt_.id = u[ti].id; tgt_.sx = u[ti].sx; tgt_.sy = u[ti].sy; }
    else tgt_ = Target{};

    // ---- L: interact (items > objects / town NPCs > the hostile target) ----
    if ((down & B_L) && !layer && castSlot_ < 0 && !interact_ && !alt_) {
        int ii = pick_interact(u, n, v, cfg_);
        if (ii < 0 && ti >= 0) ii = ti;
        if (ii >= 0) {
            interact_ = true; interId_ = u[ii].id; interType_ = u[ii].type; interCls_ = u[ii].cls;
            interAttempt_ = 0; interVerified_ = false;
            interH_ = interType_ == 4 ? 6 : hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH);
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
            cx_ = px; cy_ = py; out.push(A_MOVE, px, py);              // forced move, same reason as the cast
            out.push(A_LDOWN, px, py);
        } else interNoop_ = true;
    } else if (interact_) {
        if (!(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); interact_ = false; interId_ = 0; }
        else {
            const int ii = findId(u, n, interId_);
            if (ii >= 0) {
                if (!interVerified_) {
                    if (x.selValid && x.selId == interId_ && x.selType == interType_) {
                        interVerified_ = true;
                        if (interType_ != 4) hover_.learn(interType_, interCls_, interH_);
                    } else if (interAttempt_ < 4 && interType_ != 4) {
                        ++interAttempt_;
                        interH_ = HoverTable::try_seq(interAttempt_, hover_.get(interType_, interCls_, interType_ == 2 ? 20 : cfg_.hoverH));
                    }
                }
                int px, py; hoverPoint(u[ii], interH_, v, &px, &py);
                moveTo(px, py, out);
            }
        }
    }
    if ((up & B_L) && interNoop_) interNoop_ = false;

    // ---- left stick: move-only orbit, hard stop on release ----
    const float lm = std::sqrt(c.lx * c.lx + c.ly * c.ly);
    const bool on = lsOn_ ? (lm > cfg_.deadzone) : (lm > cfg_.deadzone + 0.05f);
    if (on) { lastDx_ = c.lx / lm; lastDy_ = c.ly / lm; }
    if (castSlot_ < 0 && !interact_) {
        if (on) {
            int px, py; orbit_point(v, c, cfg_, u, n, &px, &py);
            moveTo(px, py, out);
            if (!lmb_) { out.push(A_LDOWN, px, py); lmb_ = true; }
        } else if (lsOn_) {
            if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
            int psx, psy; world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
            psy += 6; clamp_point(v, cfg_, &psx, &psy);
            cx_ = psx; cy_ = psy; out.push(A_CLICK, psx, psy);            // click at the feet = stop
        }
    } else if (lmb_) { out.push(A_LUP, cx_, cy_); lmb_ = false; }        // suspended while casting / interacting
    lsOn_ = on;
}

void Scheme::panelTick(const Ctl& c, const Ctx& x, const View& v, uint32_t down, uint32_t up, Actions& out) {
    const bool layer = (c.buttons & B_R) != 0;
    commonButtons(c, down, up, out);
    // free cursor: both sticks, relative, quadratic response
    {
        const float sc = cfg_.sens * ((float)v.w / 800.f);
        float dx = 0.f, dy = 0.f;
        if (std::fabs(c.lx) > cfg_.deadzone || std::fabs(c.ly) > cfg_.deadzone) { dx += c.lx * std::fabs(c.lx) * sc; dy += c.ly * std::fabs(c.ly) * sc; }
        if (std::fabs(c.rx) > cfg_.deadzone || std::fabs(c.ry) > cfg_.deadzone) { dx += c.rx * std::fabs(c.rx) * sc; dy += c.ry * std::fabs(c.ry) * sc; }
        if (dx != 0.f || dy != 0.f) {
            int px = cx_ + (int)dx, py = cy_ + (int)dy;
            if (px < 0) px = 0; if (px > v.w - 1) px = v.w - 1;
            if (py < 0) py = 0; if (py > v.h - 1) py = v.h - 1;
            moveTo(px, py, out);
        }
    }
    if (x.skillTree) {                                                    // faces = hotkeys on the hovered icon
        for (int i = 0; i < 4; ++i) if (down & kFaceBits[i]) out.push(A_KEY, 0x70 + i + (layer ? 4 : 0));
    } else {
        if (down & B_CROSS) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
        if ((up & B_CROSS) && lmb_ && !(c.buttons & B_L)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
        if (down & B_TRI) { out.push(A_RDOWN, cx_, cy_); rmb_ = true; }
        if ((up & B_TRI) && rmb_) { out.push(A_RUP, cx_, cy_); rmb_ = false; }
        if (down & B_SQR) { out.push(A_KEYDOWN, 0x10); shiftSq_ = true; }
        if ((up & B_SQR) && shiftSq_) { out.push(A_KEYUP, 0x10); shiftSq_ = false; }
        if (down & B_CIR) out.push(A_KEY, 0x1B);
    }
    if ((down & B_L) && !layer && !alt_) { out.push(A_LDOWN, cx_, cy_); lmb_ = true; }
    if ((up & B_L) && lmb_ && !(c.buttons & B_CROSS)) { out.push(A_LUP, cx_, cy_); lmb_ = false; }
    tgt_ = Target{}; aimActive_ = false;
}
```

- [ ] **Step 5 : Test vert**

```bash
cd /home/doudou/repos/wt-manette && cmake --build build --target pad_core_test -j4 2>&1 | grep -E 'error|warning'; ./build/pad_core_test
```
Attendu : `0 failed`. Si un CHECK échoue, corriger l'implémentation, pas le test, sauf si le test contredit la spec (le dire dans le commit).

- [ ] **Step 6 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add -A src/platform tests/pad && git commit -m "pad: machine à états du schéma (lancer, interaction, orbite, panneaux, relâchement propre)" && git push origin wt/manette
```

---

## Task 5 : `padst` — instantané par image alimenté par les crochets

**Files:**
- Create: `src/runtime/pad_state.h`, `src/runtime/pad_state.cpp`
- Modify: `tools/rt_boot_srcs.sh` (liste `RT_BOOT_SRCS_TAIL`, après `phase_hooks.cpp`)
- Modify: `src/runtime/phase_hooks.cpp` (fonction `ringtag_hooks_install`, lignes ~363-457)

- [ ] **Step 1 : En-tête**

`src/runtime/pad_state.h` :

```cpp
// src/runtime/pad_state.h — per-frame guest snapshot for the controller
// scheme v2 (docs/superpowers/specs/2026-09-19-manette-ciblage-design.md).
// WRITTEN by the unit/camera hooks in phase_hooks.cpp (game render thread),
// READ by the input tick (same thread: the tick runs from the message pump).
// Double-buffered: frame_begin() publishes the unit list collected during the
// previous frame together with the camera just read, so the reader never sees
// a half-collected list.
#pragma once
#include <cstdint>

namespace padst {

constexpr int MAX_UNITS = 128;

struct Unit {
    uint32_t id, type, cls, mode;
    int32_t  fx, fy;                 // 16.16 fine world position (what GetUnitX/Y return)
    uint32_t ownerType, ownerId;     // UnitAny+0x94 / +0x98 (type 1 only, else 0)
    uint32_t monFlags;               // MonsterData+0x16 byte (type 1 only, else 0)
};

struct Snapshot {
    uint32_t frame;                  // +1 per camera hook exit (one per in-game frame)
    bool     inGame;                 // camera exit saw a non-null player
    uint32_t playerId; int32_t playerFx, playerFy;
    int32_t  viewX, viewY;           // [Game+0x3a520c] / [Game+0x3a5208]
    uint32_t levelNo;                // Path->Room1->Room2->Level->dwLevelNo, 0 if unreadable
    uint32_t selValid, selId, selType;   // [0x3a6a94] / [0x3a6a78] / [0x3a6a8c]: unit the game hovers
    uint32_t uiVars[38];             // [0x3a27c0 + 4*i]
    int      nUnits; Unit units[MAX_UNITS];
};

bool on();                           // D2_PAD absent or != "0" -> true (hooks armed)
void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelNo,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* uiVars38);
void add_unit(const Unit& u);
bool read(Snapshot& out);            // false until the first frame_begin

} // namespace padst
```

- [ ] **Step 2 : Implémentation**

`src/runtime/pad_state.cpp` :

```cpp
// src/runtime/pad_state.cpp — see pad_state.h.
#include "runtime/pad_state.h"
#include <cstdlib>
#include <cstring>

namespace padst {

static Snapshot g_live, g_stable;
static bool g_written = false;

bool on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("D2_PAD"); v = (e && *e == '0') ? 0 : 1; }
    return v != 0;
}

void frame_begin(uint32_t playerId, int32_t pfx, int32_t pfy, int32_t vx, int32_t vy, uint32_t levelNo,
                 uint32_t selValid, uint32_t selId, uint32_t selType, const uint32_t* ui) {
    g_stable.nUnits = g_live.nUnits;
    if (g_live.nUnits > 0) memcpy(g_stable.units, g_live.units, sizeof(Unit) * (size_t)g_live.nUnits);
    g_live.frame += 1; g_stable.frame = g_live.frame; g_live.nUnits = 0;
    g_stable.inGame = playerId != 0;
    g_stable.playerId = playerId; g_stable.playerFx = pfx; g_stable.playerFy = pfy;
    g_stable.viewX = vx; g_stable.viewY = vy; g_stable.levelNo = levelNo;
    g_stable.selValid = selValid; g_stable.selId = selId; g_stable.selType = selType;
    memcpy(g_stable.uiVars, ui, sizeof g_stable.uiVars);
    g_written = true;
}

void add_unit(const Unit& u) {
    if (g_live.nUnits < MAX_UNITS) g_live.units[g_live.nUnits++] = u;
}

bool read(Snapshot& out) {
    if (!g_written) return false;
    out = g_stable;
    return true;
}

} // namespace padst
```

- [ ] **Step 3 : Ajouter les deux unités à la liste des sources**

Dans `tools/rt_boot_srcs.sh`, juste après la ligne `"$ROOT/src/runtime/phase_hooks.cpp"`, ajouter :

```bash
  "$ROOT/src/runtime/pad_state.cpp"
  "$ROOT/src/platform/pad_core.cpp"
```

- [ ] **Step 4 : Alimenter `padst` depuis les crochets**

Dans `src/runtime/phase_hooks.cpp` :

(a) ajouter l'include après `#include "runtime/rt_host.h"` :
```cpp
#include "runtime/pad_state.h"
```

(b) dans `ringtag_hooks_install`, remplacer
```cpp
    if(r60Tag && g_114 && g_d2base){
        g_r60TagOn=true;
```
par
```cpp
    const bool padOn = padst::on();
    if((r60Tag || padOn) && g_114 && g_d2base){
        g_r60TagOn = r60Tag;      // the ring consumer; padOn is the second consumer of the same hooks
```

(c) dans le shim d'ENTRÉE unité (`e.fn`), remplacer le corps complet par :
```cpp
            e.fn=[&br,padOn](Cpu&c)->uint32_t{
                const uint32_t u=c.reg(R_ECX);
                uint32_t w[7]={0,0,0,0,0,0,0};
                if(u){
                    const uint32_t type=c.read_u32(u), id=c.read_u32(u+0xc), mode=c.read_u32(u+0x10), path=c.read_u32(u+0x2c);
                    int32_t x=0,y=0; uint32_t r8=0,rc=0;
                    if(path){
                        // STATIC path (objects, items, tiles): GetUnitX/Y read [+4]/[+8]
                        // (0x620697); DYNAMIC: [+8]/[+0xC] (0x6489c0/0x6489d0). Both 16.16.
                        if(type==2||type==4||type==5){ x=(int32_t)c.read_u32(path+4); y=(int32_t)c.read_u32(path+8); }
                        else { x=(int32_t)c.read_u32(path); y=(int32_t)c.read_u32(path+4); r8=c.read_u32(path+8); rc=c.read_u32(path+0xc); } }
                    w[0]=id; w[1]=type; w[2]=(uint32_t)x; w[3]=(uint32_t)y; w[4]=mode; w[5]=r8; w[6]=rc;
                    if(padOn){
                        padst::Unit pu; std::memset(&pu,0,sizeof pu);
                        pu.id=id; pu.type=type; pu.mode=mode; pu.cls=c.read_u32(u+4);
                        if(type==2||type==4||type==5){ pu.fx=x; pu.fy=y; } else { pu.fx=(int32_t)r8; pu.fy=(int32_t)rc; }
                        if(type==1){
                            pu.ownerType=c.read_u32(u+0x94); pu.ownerId=c.read_u32(u+0x98);
                            const uint32_t md=c.read_u32(u+0x14);
                            if(md) pu.monFlags=(c.read_u32(md+0x14)>>16)&0xffu;   // byte at MonsterData+0x16
                        }
                        padst::add_unit(pu);
                    }
                }
                if(g_r60TagOn) gr_emit(c,D2GR_OP_UNITTAG,w,7);
                const uint32_t E=c.reg(R_ESP);
                if(g_r60TagOn){
                    if(!s_unit.busy){ s_unit.busy=true; s_unit.ret=c.read_u32(E); s_unit.id=w[0]; c.write_u32(E,s_unitExit); }
                    else ++g_r60Reent;
                }
                c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(s_unitEntry+1);   // faithful fallback: push ebp
                return c.reg(R_EAX); };
```

(d) dans le shim de SORTIE caméra (`cx.fn`), remplacer le corps par :
```cpp
            cx.fn=[&br,padOn](Cpu&c)->uint32_t{
                uint32_t w[5]={0,0,0,0,0};
                w[0]=c.read_u32(g_d2base+0x003a520cu); w[1]=c.read_u32(g_d2base+0x003a5208u);
                const uint32_t pl=c.read_u32(g_d2base+0x003a6a70u);
                uint32_t path=0;
                if(pl){ w[2]=c.read_u32(pl+0xc); path=c.read_u32(pl+0x2c);
                        if(path){ w[3]=c.read_u32(path); w[4]=c.read_u32(path+4); } }
                if(g_r60TagOn) gr_emit(c,D2GR_OP_CAMERA,w,5);
                if(padOn){
                    uint32_t ui[38]; for(int i=0;i<38;i++) ui[i]=c.read_u32(g_d2base+0x003a27c0u+4u*(uint32_t)i);
                    uint32_t lvl=0; int32_t pfx=0,pfy=0;
                    if(pl && path){
                        pfx=(int32_t)c.read_u32(path+8); pfy=(int32_t)c.read_u32(path+0xc);     // same source as the units
                        const uint32_t r1=c.read_u32(path+0x1c);
                        if(r1){ const uint32_t r2=c.read_u32(r1+0x10);
                            if(r2){ const uint32_t lv=c.read_u32(r2+0x58); if(lv) lvl=c.read_u32(lv+0x1f8); } } }
                    padst::frame_begin(w[2], pfx, pfy, (int32_t)w[0], (int32_t)w[1], lvl,
                                       c.read_u32(g_d2base+0x003a6a94u), c.read_u32(g_d2base+0x003a6a78u),
                                       c.read_u32(g_d2base+0x003a6a8cu), ui);
                }
                const uint32_t ret=s_cam.ret; s_cam.busy=false;
                c.set_reg(R_ESP,c.reg(R_ESP)-4); br.redirect_next(ret); return c.reg(R_EAX); };
```

(e) remplacer la ligne `jpline("ringtag: crochets poses — …` par :
```cpp
            jpline("ringtag: crochets poses — unite Game+0xdc7b0 (ECX=UnitAny*), camera Game+0x5b440, pas Game+0x12fd90 ; rejeu=%s ui=Game+0x%x ; pad=%s",
                   g_r60Active?"ACTIF":"non",(unsigned)g_r60UiRva, padOn?"oui":"non");
```

Vérifier que `std::memset` a `<cstring>` (déjà inclus). Le bloc `(d) phases` qui suit reste inchangé.

- [ ] **Step 5 : Build ARM qemu = la porte de non-régression du boot**

```bash
cd /home/doudou/repos/wt-manette && bash tools/rt_boot_arm_check.sh 2>&1 | tail -6
```
Attendu : verdict `PASS` (natif) sur les deux passes, et dans le journal la ligne `ringtag: crochets poses … pad=oui`. Le boot ne va pas jusqu'en jeu : les crochets tirent peu, mais le code est exercé (chargement, symboles, pas de faute).

- [ ] **Step 6 : Build VPK**

```bash
export VITASDK=/usr/local/vitasdk; export PATH="$VITASDK/bin:$PATH"; cd /home/doudou/repos/wt-manette && bash tools/build_rt_boot_vpk.sh 2>&1 | tail -3
```
Attendu : `== built …/build-vita/d2vita.vpk ==`.

- [ ] **Step 7 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/runtime/pad_state.h src/runtime/pad_state.cpp tools/rt_boot_srcs.sh src/runtime/phase_hooks.cpp && git commit -m "pad: instantané par image (unités dessinées, caméra, unité survolée, UiVar, niveau) alimenté par les crochets ringtag" && git push origin wt/manette
```

---

## Task 6 : Manette virtuelle sur `D2CMDFILE` (`padb` / `pada`)

**Files:**
- Modify: `src/runtime/scripted_input.cpp` (fonction `inj_queue`, avant `else if(act=="eipdump")`)
- Modify: `src/runtime/scripted_input.h` (déclaration)

- [ ] **Step 1 : État et commandes**

Dans `scripted_input.cpp`, après `uint8_t g_keyState[256]={0};` ajouter :

```cpp
// Virtual controller for remote tests (D2CMDFILE / D2SCRIPT): ORed with the
// physical pad by vita_present.cpp's input tick. Axes 0..255, 128 = centered.
//   padb:<buttons>:0            e.g. padb:16384:0 = Cross held, padb:0:0 = release
//   pada:<lx|ly<<8>:<rx|ry<<8>  e.g. pada:32896:32896 = both sticks centered (128|128<<8)
static uint32_t g_vpadButtons=0; static uint8_t g_vpadAxes[4]={128,128,128,128};
extern "C" void d2vita_vpad_get(uint32_t* buttons, uint8_t axes[4]){ *buttons=g_vpadButtons; std::memcpy(axes,g_vpadAxes,4); }
```

Dans `inj_queue`, avant `else if(act=="eipdump")`, ajouter :

```cpp
    else if(act=="padb"){ g_vpadButtons=(uint32_t)a; }
    else if(act=="pada"){ g_vpadAxes[0]=(uint8_t)(a&0xff); g_vpadAxes[1]=(uint8_t)((a>>8)&0xff);
                          g_vpadAxes[2]=(uint8_t)(b&0xff); g_vpadAxes[3]=(uint8_t)((b>>8)&0xff); }
```

Dans `scripted_input.h`, ajouter la déclaration :
```cpp
extern "C" void d2vita_vpad_get(uint32_t* buttons, uint8_t axes[4]);   // virtual pad (padb/pada commands)
```

- [ ] **Step 2 : Build qemu rapide (compile seulement)**

```bash
cd /home/doudou/repos/wt-manette && bash tools/rt_boot_arm_check.sh 2>&1 | tail -3
```
Attendu : `PASS`.

- [ ] **Step 3 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/runtime/scripted_input.cpp src/runtime/scripted_input.h && git commit -m "input: manette virtuelle padb/pada sur D2CMDFILE (tests sans toucher la console)" && git push origin wt/manette
```

---

## Task 7 : Glue Vita — `controls.txt`, `aim_tick`, tactile factorisé, journal

**Files:**
- Modify: `src/platform/vita_present.cpp` (bloc « physical controls », lignes ~1412-1769)

- [ ] **Step 1 : Includes et état**

Après `#include "platform/radial_menu.h"` (ligne ~16) ajouter :
```cpp
#include "platform/pad_core.h"      // scheme v2 ("aim"): pure core
#include "runtime/pad_state.h"      // per-frame guest snapshot (hooks)
#include "runtime/scripted_input.h" // d2vita_vpad_get
```

Dans le `namespace {` anonyme du bloc physical controls (après `int g_sel_mode = SEL_IDLE;`), ajouter :

```cpp
// ---- scheme v2 ("aim") — docs/superpowers/specs/2026-09-19-manette-ciblage-design.md
bool         g_scheme_aim = true;          // controls.txt scheme=aim|mouse ; env D2_PAD=0 forces mouse
pad::Config  g_padcfg;
pad::Scheme* g_scheme = nullptr;           // built at first use (after controls.txt)
bool         g_scheme_active = false;      // the scheme currently owns the controls (in game)
uint32_t     g_pad_lastFrame = 0; int g_pad_stale = 0;
int          g_padlog = -1;                // D2_PADLOG
// overlay (game coords), written by the tick, read by the presentation thread (display only)
volatile int g_ret_has = 0, g_ret_ver = 0, g_ret_x = 0, g_ret_y = 0, g_aim_on = 0, g_aim_x = 0, g_aim_y = 0;

void pad_emit(const pad::Actions& a){
    for (int i = 0; i < a.n; i++) {
        const pad::Action& x = a.v[i];
        switch (x.k) {
            case pad::A_MOVE:    d2vita_inject("move", x.a, x.b); break;
            case pad::A_LDOWN:   d2vita_inject("ldown", x.a, x.b); break;
            case pad::A_LUP:     d2vita_inject("lup", x.a, x.b); break;
            case pad::A_RDOWN:   d2vita_inject("rdown", x.a, x.b); break;
            case pad::A_RUP:     d2vita_inject("rup", x.a, x.b); break;
            case pad::A_CLICK:   d2vita_inject("click", x.a, x.b); break;
            case pad::A_KEYDOWN: d2vita_inject("keydown", x.a, 0); break;
            case pad::A_KEYUP:   d2vita_inject("keyup", x.a, 0); break;
            case pad::A_KEY:     d2vita_inject("key", x.a, 0); break;
        }
    }
}
// Release everything the scheme holds (radial menu / keyboard opening, leaving the game).
void pad_leave(){
    if (g_scheme && g_scheme_active) {
        pad::Actions a; g_scheme->leave(a); pad_emit(a); g_scheme_active = false;
        g_cx = (float)g_scheme->cx(); g_cy = (float)g_scheme->cy();
    }
    g_ret_has = 0; g_aim_on = 0;
}
bool pad_is_town(uint32_t lvl){ return lvl == 0 || lvl == 1 || lvl == 40 || lvl == 75 || lvl == 103 || lvl == 109; }
bool pad_is_merc(uint32_t cls){ return cls == 271 || cls == 338 || cls == 359 || cls == 560; }
```

- [ ] **Step 2 : `controls.txt` — nouvelles clés**

Dans `load_controls_txt`, dans la chaîne des `if/else if` de clés scalaires (après `anchor_y`), ajouter :

```cpp
        else if (!strcasecmp(line,"scheme"))   { g_scheme_aim = strcasecmp(v,"mouse")!=0; n++; continue; }
        else if (!strcasecmp(line,"aim"))      { g_padcfg.aim = atoi(v)!=0; n++; continue; }
        else if (!strcasecmp(line,"orbit_min")){ g_padcfg.orbitMin=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"orbit_max")){ g_padcfg.orbitMax=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"range_min")){ g_padcfg.rangeMin=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"range_max")){ g_padcfg.rangeMax=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"cone"))     { g_padcfg.coneDeg=(float)atof(v); n++; continue; }
        else if (!strcasecmp(line,"hover_h"))  { g_padcfg.hoverH=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"hud_h"))    { g_padcfg.hudH=atoi(v); n++; continue; }
```
Et à la fin de `load_controls_txt` (après `fclose`), avant le message, ajouter :
```cpp
    g_padcfg.deadzone = g_dz; g_padcfg.sens = g_sens;
```
Puis dans le bloc d'init du tick (`if (!g_ctl_init){ … load_controls_txt(); …`), juste après `load_controls_txt();` :
```cpp
        if (const char* e = getenv("D2_PAD")) g_scheme_aim = (*e && *e != '0');     // env wins over controls.txt
        if (!padst::on()) g_scheme_aim = false;                                         // hooks unarmed: nothing to read
        g_padcfg.deadzone = g_dz; g_padcfg.sens = g_sens;
        g_padlog = getenv("D2_PADLOG") ? 1 : 0;
        d2vita_progress(g_scheme_aim ? "input: schema v2 (visee assistee) actif" : "input: schema souris (legacy)");
```
`d2vita_progress("input: mapping par defaut")` existant reste tel quel.

- [ ] **Step 3 : Factoriser le tactile**

Extraire le bloc `// Front touch: absolute cursor; a brief still tap = full left click` (lignes ~1751-1763) dans une fonction du namespace anonyme, placée avant `d2vita_input_tick` :

```cpp
// Front touch: absolute cursor; a brief still tap = full left click.
// allowMove=false: the cursor position is tracked but no move is injected
// (the aim scheme owns the cursor while a button is held).
void touch_tick(bool allowMove, bool* moved){
    SceTouchData td; memset(&td,0,sizeof td);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT,&td,1)>=0){
        if (td.reportNum>0){
            int tx=td.report[0].x*g_game_w/1920, ty=td.report[0].y*g_game_h/1088;
            if (g_t_at<0){ g_t_at=g_itick; g_t_x0=tx; g_t_y0=ty; g_t_moved=false; }
            if (std::abs(tx-g_t_x0)>10||std::abs(ty-g_t_y0)>10) g_t_moved=true;
            g_t_x=tx; g_t_y=ty;
            if (allowMove){ g_cx=(float)tx; g_cy=(float)ty; *moved=true; }
        } else if (g_t_at>=0){
            if (g_itick-g_t_at<g_tap_ticks && !g_t_moved) d2vita_inject("click",g_t_x,g_t_y);
            g_t_at=-1;
        }
    }
}
```
et remplacer le bloc d'origine par `touch_tick(true, &moved);`.

- [ ] **Step 4 : `aim_tick`**

Ajouter dans le namespace anonyme, après `touch_tick` :

```cpp
// Scheme v2 tick. Returns true when the scheme handled the tick (in game);
// false = out of game, the legacy path runs (menus, character select).
bool aim_tick(const SceCtrlData& cd, uint32_t b){
    padst::Snapshot s;
    if (!padst::read(s)) return false;
    if (s.frame == g_pad_lastFrame) { if (g_pad_stale < 1000) ++g_pad_stale; }
    else { g_pad_stale = 0; g_pad_lastFrame = s.frame; }
    const bool inGame = s.inGame && g_pad_stale < 20;          // camera hook silent ~0.7 s = not in game
    if (!inGame) { pad_leave(); return false; }
    if (!g_scheme) g_scheme = new pad::Scheme(g_padcfg);
    g_scheme_active = true;

    pad::View v; v.w = g_game_w; v.h = g_game_h;
    v.playerFx = s.playerFx; v.playerFy = s.playerFy; v.viewX = s.viewX; v.viewY = s.viewY;
    pad::Ctx x; x.inGame = true; x.playerId = s.playerId;
    static const int panels[] = { 1, 2, 4, 8, 9, 0x0C, 0x14, 0x19, 0x1A, 0x21, 0x24 };
    for (int p : panels) if (s.uiVars[p]) x.panelOpen = true;
    x.skillTree = s.uiVars[4] != 0;
    x.selValid = s.selValid; x.selId = s.selId; x.selType = s.selType;
    const bool town = pad_is_town(s.levelNo);

    static pad::Unit units[padst::MAX_UNITS]; int n = 0;
    for (int i = 0; i < s.nUnits && n < padst::MAX_UNITS; i++) {
        const padst::Unit& q = s.units[i];
        if (q.type == 0 && q.id == s.playerId) continue;              // never box/target ourselves
        if (q.type == 3 || q.type == 5) continue;                     // missiles, tiles
        pad::Unit& o = units[n]; o = pad::Unit{};
        o.id = q.id; o.type = q.type; o.cls = q.cls; o.mode = q.mode;
        pad::world_to_screen(v, q.fx, q.fy, &o.sx, &o.sy);
        if (q.type == 1) {
            const bool alive = q.mode != 0 && q.mode != 12;
            const bool ours  = q.ownerId != 0 && q.ownerId == s.playerId;
            o.hostile  = alive && !town && !ours && !pad_is_merc(q.cls);
            o.interact = alive && town;                                // town NPCs
        } else if (q.type == 4 || q.type == 2) o.interact = true;     // items, objects
        ++n;
    }
    pad::Ctl c;
    c.lx = (cd.lx - 128) / 128.f; c.ly = (cd.ly - 128) / 128.f;
    c.rx = (cd.rx - 128) / 128.f; c.ry = (cd.ry - 128) / 128.f;
    c.buttons = b & ~(uint32_t)pad::B_SELECT;                          // Select = radial menu, handled before us

    pad::Actions a; g_scheme->tick(c, x, v, units, n, a); pad_emit(a);
    g_cx = (float)g_scheme->cx(); g_cy = (float)g_scheme->cy();
    const pad::Target t = g_scheme->target();
    g_ret_has = t.has; g_ret_ver = t.verified; g_ret_x = t.sx; g_ret_y = t.sy;
    g_aim_on = g_scheme->aimActive(); g_aim_x = g_scheme->aimX(); g_aim_y = g_scheme->aimY();

    bool moved = false;
    touch_tick(!g_scheme->cursorOwned(), &moved);
    if (moved) { d2vita_inject("move", (int)g_cx, (int)g_cy); g_scheme->setCursor((int)g_cx, (int)g_cy); }

    if (g_padlog) {                                                    // capped diagnostics (boot_progress.txt)
        static int lines = 0; static uint32_t lastTgt = 0, lastLvl = 0xffffffffu; static uint32_t lastUi[38] = {0};
        static int projN = 0;
        char m[160];
        if (lines < 2000) {
            if (projN < 60) { int psx, psy; pad::world_to_screen(v, v.playerFx, v.playerFy, &psx, &psy);
                snprintf(m, sizeof m, "pad: joueur ecran=(%d,%d) view=(%d,%d) fine=(%d,%d) unites=%d", psx, psy, v.viewX, v.viewY, v.playerFx >> 16, v.playerFy >> 16, n);
                d2vita_progress(m); ++projN; ++lines; }
            if (s.levelNo != lastLvl) { lastLvl = s.levelNo; snprintf(m, sizeof m, "pad: niveau=%u ville=%d", s.levelNo, (int)town); d2vita_progress(m); ++lines; }
            for (int i = 0; i < 38 && lines < 2000; i++) if (s.uiVars[i] != lastUi[i]) { lastUi[i] = s.uiVars[i];
                snprintf(m, sizeof m, "pad: uivar[%d]=%u", i, s.uiVars[i]); d2vita_progress(m); ++lines; }
            if (t.id != lastTgt) { lastTgt = t.id;
                int ti = -1; for (int i = 0; i < n; i++) if (units[i].id == t.id) ti = i;
                snprintf(m, sizeof m, "pad: cible id=%u type=%u cls=%u ecran=(%d,%d) verif=%d sel=(%u,%u,%u)", t.id,
                         ti >= 0 ? units[ti].type : 0u, ti >= 0 ? units[ti].cls : 0u, t.sx, t.sy, (int)t.verified, s.selValid, s.selId, s.selType);
                d2vita_progress(m); ++lines; }
        }
    }
    return true;
}
```

- [ ] **Step 5 : Brancher dans `d2vita_input_tick`**

(a) Après `if (sceCtrlPeekBufferPositive(0,&cd,1)<1) return;` ajouter la manette virtuelle :
```cpp
    { uint32_t vb = 0; uint8_t va[4]; d2vita_vpad_get(&vb, va);
      cd.buttons |= vb;
      if (va[0] != 128 || va[1] != 128) { cd.lx = va[0]; cd.ly = va[1]; }
      if (va[2] != 128 || va[3] != 128) { cd.rx = va[2]; cd.ry = va[3]; } }
```
(b) Dans le bloc d'ouverture du clavier (`if ((b&B_TRI)&&!(was&B_TRI)&&layer&&!g_kb.open){`), avant `d2kb::open_kb`, ajouter `pad_leave();`.
(c) Dans le bloc Select, sur la branche `else { g_sel_mode=SEL_RADIAL; radial_menu::begin(g_rm); }`, ajouter `pad_leave();` avant `radial_menu::begin`.
(d) Juste après `if (g_sel_mode==SEL_RADIAL) return;   // menu open: …` ajouter :
```cpp
    // Scheme v2 owns everything below while in game; out of game the legacy
    // scheme (menus, character select, autopilot hand-over) runs unchanged.
    if (g_scheme_aim && aim_tick(cd, b)) return;
```
Le reste (L/R legacy, table de boutons, sticks, tactile via `touch_tick(true,&moved)`, clamp, move, `lmb_update`) reste inchangé.

- [ ] **Step 6 : Build VPK et chercher les avertissements**

```bash
export VITASDK=/usr/local/vitasdk; export PATH="$VITASDK/bin:$PATH"; cd /home/doudou/repos/wt-manette && bash tools/build_rt_boot_vpk.sh 2>&1 | grep -E 'error|vita_present.cpp.*warning' | head; bash tools/build_rt_boot_vpk.sh 2>&1 | tail -2
```
Attendu : aucune erreur, `== built … ==`.

- [ ] **Step 7 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/platform/vita_present.cpp && git commit -m "input: schéma v2 branché (controls.txt scheme=aim, aim_tick, tactile factorisé, D2_PADLOG) ; legacy intact hors partie et via scheme=mouse" && git push origin wt/manette
```

---

## Task 8 : Réticule dans l'overlay

**Files:**
- Modify: `src/platform/vita_present.cpp` (`d2vita_overlay`, ligne ~392)

- [ ] **Step 1 : Dessin**

Avant `void d2vita_overlay(uint32_t* fb)`, ajouter :

```cpp
// Scheme v2 overlay: a diamond on the current hostile target (gold once the
// game is verified to hover it, white before), a dot at the ground aim point.
// Game -> screen uses the same stretch as the presentation (vita_gxm.cpp).
static void draw_reticle(uint32_t* fb) {
    if (!g_ret_has && !g_aim_on) return;
    using radial_menu::draw_detail::blend_px;
    const int gw = g_game_w > 0 ? g_game_w : 800, gh = g_game_h > 0 ? g_game_h : 600;
    if (g_ret_has) {
        const int cx = g_ret_x * SCR_W / gw, cy = g_ret_y * SCR_H / gh - 8;
        const uint8_t r = g_ret_ver ? 255 : 240, g = g_ret_ver ? 200 : 240, b = g_ret_ver ? 60 : 240;
        for (int d = 0; d <= 10; ++d) {
            blend_px(fb, SCR_W, SCR_H, cx - 10 + d, cy - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 10 - d, cy - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - 10 + d, cy + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 10 - d, cy + d, r, g, b, 220);
        }
    }
    if (g_aim_on) {
        const int ax = g_aim_x * SCR_W / gw, ay = g_aim_y * SCR_H / gh;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) blend_px(fb, SCR_W, SCR_H, ax + dx, ay + dy, 255, 255, 255, 200);
    }
}
```
Dans `d2vita_overlay`, après `if (fps_en) draw_fps(fb, fps10);` ajouter `draw_reticle(fb);`.

Les variables `g_ret_*`/`g_aim_*` sont dans le namespace anonyme défini plus bas dans le fichier : déplacer leur définition (le bloc `volatile int g_ret_has …`) AVANT `d2vita_overlay` (en tête du fichier, à côté de `g_rm`), sinon le compilateur ne les voit pas.

- [ ] **Step 2 : Build VPK** — même commande qu'en Task 7 Step 6 ; attendu : `== built … ==`.

- [ ] **Step 3 : Commit**

```bash
cd /home/doudou/repos/wt-manette && git add src/platform/vita_present.cpp && git commit -m "input: réticule de cible et point de visée dans l'overlay" && git push origin wt/manette
```

---

## Task 9 : Validation Vita3K puis console, avec journal

**Files:** aucun changement de code attendu ; corrections éventuelles commitées séparément.

- [ ] **Step 1 : Vita3K (skill `third_party/winx86/.claude/skills/vita3k-testing`, script `tools/vita3k_isole.sh`)**

```bash
cd /home/doudou/repos/wt-manette && sed -n 1,40p tools/vita3k_isole.sh     # lire l'usage exact avant de lancer
V3K2ROOT=/tmp/vita3k_manette TITLE=DTWO00077 bash tools/vita3k_isole.sh init
V3K2ROOT=/tmp/vita3k_manette TITLE=DTWO00077 VPK=build-vita/d2vita.vpk bash tools/vita3k_isole.sh install
```
Écrire dans le `env.txt` du préfixe (`/tmp/vita3k_manette/ux0/data/d2vita/env.txt`) : `D2_PADLOG=1` et `D2CMDFILE=ux0:data/d2vita/d2cmd.txt`. Lancer (`DUR=240 … launch`) : l'autopilote conduit en jeu. Puis, dans `d2cmd.txt` du préfixe, ajouter des lignes (une par action, ≥ 1 s entre deux) :
```
pada:32896:32896
padb:16384:0
padb:0:0
pada:33024:32896
pada:32896:32896
```
(`16384` = Croix ; `pada:33024:32896` = stick gauche à droite `lx=255`, `ly=128`.)
Vérifier dans `boot_progress.txt` du préfixe :
- `input: schema v2 (visee assistee) actif` ;
- `pad: joueur ecran=(X,Y) …` **60 lignes à X,Y quasi constants** (± 2 px) — sinon la projection est fausse : voir la spec §10 (mode `proj=delta`) et corriger avant de continuer ;
- `pad: niveau=1 ville=1` au camp ;
- après `padb:16384:0` : `pad: cible id=…` (ou aucune cible en ville, normal : le test de cible se fait hors ville, Step 3).

- [ ] **Step 2 : Déployer sur la console (skill `.claude/skills/hardware-ab-bench`)**

Console `10.113.1.159` (FTP 1337, commandes 1338). Sauvegarder puis compléter `env.txt` de la console (`ux0:data/d2vita/env.txt` — il contient des réglages utilisateur : **le récupérer avant, le restaurer après**) avec `D2_PADLOG=1` et `D2CMDFILE=ux0:data/d2vita/d2cmd.txt`.

```bash
IP=10.113.1.159
cmd(){ timeout 10 bash -c "exec 3<>/dev/tcp/$IP/1338; printf '$1\n' >&3; timeout 3 head -c 120 <&3" 2>/dev/null; }
cmd destroy; sleep 12
rm -rf /tmp/ex && mkdir /tmp/ex && unzip -q /home/doudou/repos/wt-manette/build-vita/d2vita.vpk -d /tmp/ex
curl -s -f -T /tmp/ex/eboot.bin "ftp://$IP:1337/ux0:/app/DTWO00001/eboot.bin" && echo UPLOADED
curl -s -I "ftp://$IP:1337/ux0:/app/DTWO00001/eboot.bin" | grep -i length ; ls -l /tmp/ex/eboot.bin   # MÊME taille, sinon retenter
cmd "nosleep on"; cmd "launch DTWO00001"
```

- [ ] **Step 3 : Checklist console (à la main, puis lecture du journal)**

Dans l'ordre, noter PASS/FAIL pour chacun :
1. Camp des Rogues : stick gauche dans 8 directions, aucun dialogue PNJ ni ramassage involontaire ; relâcher ⇒ arrêt en moins d'un pas.
2. L devant Akara ⇒ dialogue ; L sur un objet au sol ⇒ ramassage ; L devant le coffre/waypoint ⇒ ouverture.
3. Blood Moor : Croix ⇒ attaque le monstre le plus proche sans toucher le stick droit (réticule doré au bout de 1-4 ticks) ; stick droit vers un autre ⇒ le réticule change ; Croix maintenue ⇒ répétition ; le monstre meurt ⇒ passage au suivant.
4. Radial → arbre de compétences ; tap sur une icône ; Rond ⇒ en monde, Rond lance cette compétence.
5. Inventaire (radial) : tap/glisser fonctionnent, Rond ferme, D-pad boit une potion.
6. `controls.txt` avec `scheme=mouse` (relancer) ⇒ comportement 0.1.6 à l'identique.
7. Overlay `D2VITA_FPS=1` : images/s comparables entre `scheme=aim` et `scheme=mouse` dans la même zone (pas de chute grossière).

Rapatrier le journal : `VITA_IP=$IP bash tools/vita_drive.sh log` ; vérifier les lignes `pad: cible … verif=1` (survol confirmé), et noter pour chaque classe de monstre le `h` retenu (utile pour la phase 2). Si le journal montre `ownerId` toujours à 0 pour un mercenaire embauché, la règle « pas de mercenaire » repose sur la liste de classes — l'écrire dans le rapport.

- [ ] **Step 4 : Consigner**

Ajouter le résultat (7 lignes PASS/FAIL, valeurs `joueur ecran`, `h` appris, images/s) en fin de `docs/superpowers/specs/2026-09-19-manette-ciblage-design.md` sous un titre `## 11. Résultat snapshot 1 (console, date)`. Un FAIL = ouvrir une tâche de correction ciblée (nouveau commit), pas un contournement dans la doc.

```bash
cd /home/doudou/repos/wt-manette && git add docs/superpowers/specs/2026-09-19-manette-ciblage-design.md && git commit -m "pad: résultat de la snapshot 1 sur console" && git push origin wt/manette
```

---

## Task 10 : Documentation joueur et livraison de la snapshot

**Files:**
- Modify: `docs-site/controles.fr.md`, `docs-site/controles.md`

- [ ] **Step 1 : Doc française**

Remplacer les sections « En jeu » et « Couche R (R maintenu + …) » de `docs-site/controles.fr.md` par :

```markdown
## En jeu (schéma « visée assistée », défaut depuis la snapshot manette v2)

| Entrée Vita | Action Diablo II |
|---|---|
| Stick gauche | **Déplacement seul** : le personnage marche dans la direction du stick (rayon selon l'inclinaison), sans jamais attaquer, parler ni ramasser par accident ; relâcher = arrêt net |
| Stick droit | **Visée** : choisit l'ennemi dans un cône de ±35° ; pour un sort au sol (téléport, météore…), la distance suit l'inclinaison |
| Croix / Rond / Carré / Triangle | **Compétences 1 à 4**, lancer immédiat sur l'ennemi ciblé (le plus proche si le stick droit est au repos) ; maintenir = répéter |
| R maintenu + faces | Compétences 5 à 8 |
| L | **Interagir** : objet au sol le plus proche, sinon coffre / porte / PNJ, sinon attaque de base sur la cible |
| R puis L (maintenus) | Alt — étiquettes des objets au sol |
| D-pad ↑ / ← / ↓ / → | Potions ceinture 1 / 2 / 3 / 4 |
| R + D-pad | Potion au mercenaire |
| Start | Échap ; R + Start : échange d'armes |
| Select | Menu radial (voir plus bas) ; R + Select : Espace |
| Écran tactile | Curseur absolu ; tap bref = clic gauche |

Un losange marque l'ennemi ciblé (doré dès que le jeu confirme le survol) et un
point le lieu d'impact d'un sort au sol quand le stick droit est poussé.

**Assigner une compétence à un bouton** : ouvrir l'arbre de compétences
(menu radial), toucher l'icône, presser le bouton voulu (R + bouton pour les
emplacements 5 à 8). Dépenser un point : L ou tap.

**Panneaux ouverts** (inventaire, coffre, marchand…) : les sticks déplacent le
curseur, L ou Croix cliquent, Triangle = clic droit, Carré maintenu = Shift
(déplacement rapide), Rond ferme.

Hors partie (menus, sélection de personnage), l'ancien schéma souris reste
actif. `scheme=mouse` dans `controls.txt` le rétablit partout (mapping de la
0.1.6 : L/R = clics, Croix = marche/course, Rond = Shift, Carré = Alt,
Triangle = W, R + D-pad = F1-F4).
```

Et compléter la section « Remappage sans rebuild » avec les nouvelles clés (`scheme`, `aim`, `orbit_min`, `orbit_max`, `range_min`, `range_max`, `cone`, `hover_h`, `hud_h`) et leur valeur par défaut (voir spec §7), en précisant que les remaps `cross=…` ne concernent que `scheme=mouse`.

- [ ] **Step 2 : Doc anglaise**

Même structure dans `docs-site/controles.md` (traduction fidèle des mêmes tableaux : « Move only », « Aim », « Skills 1-4 / 5-8 », « Interact », « Alt (item labels) », « Belt potions », « Potion to the mercenary », « Escape / weapon swap », « Radial menu / Space », plus le paragraphe sur l'assignation, les panneaux et `scheme=mouse`).

- [ ] **Step 3 : Build de la doc (si mkdocs est installé) et commit**

```bash
cd /home/doudou/repos/wt-manette && (command -v mkdocs >/dev/null && mkdocs build -q -d /tmp/site_manette && echo DOC_OK || echo "mkdocs absent: verification visuelle du markdown seulement")
git add docs-site/controles.fr.md docs-site/controles.md && git commit -m "docs: contrôles du schéma v2 (visée assistée) et repli scheme=mouse" && git push origin wt/manette
```

- [ ] **Step 4 : Livrer la snapshot**

```bash
cd /home/doudou/repos/wt-manette && H=$(git rev-parse --short HEAD) && cp build-vita/d2vita.vpk /home/doudou/build/d2vita_manette_$H.vpk && ls -l /home/doudou/build/d2vita_manette_$H.vpk
```
Puis envoyer ce fichier à l'utilisateur (outil `SendUserFile` s'il est disponible dans la session, sinon indiquer le chemin), avec en trois lignes : le hash, ce qui est actif par défaut (`scheme=aim`), comment revenir en arrière (`scheme=mouse` dans `ux0:data/d2vita/controls.txt` ou `D2_PAD=0` dans `env.txt`), et le résultat de la checklist console (Task 9).

---

## Auto-revue du plan (faite à la rédaction)

- **Couverture de la spec** : §4 contexte → Task 5 (snapshot) + Task 7 (`aim_tick`) ; §5.1 mapping monde → Task 4 (`worldTick`, `commonButtons`) ; §5.2 cible → Task 2 ; §5.3 boucle de survol → Task 4 (cast/interact) + Task 5 (`sel*`) ; §5.4 panneaux → Task 4 (`panelTick`) ; §5.5 réticule → Task 8 ; §6 unités → Tasks 1-7 ; §7 config → Task 7 Step 2 ; §8 diagnostic → Task 7 Step 4 + Task 6 ; §9 validation → Tasks 1-4 (PC), 5 (qemu), 9 (Vita3K, console) ; §10 replis → Task 9 Step 1 (projection), Step 3 (owner/merc).
- **Types** : `pad::Unit{id,type,cls,mode,sx,sy,hostile,interact}`, `pad::Ctx`, `pad::View`, `pad::Actions::push(k,a,b)`, `padst::Unit{…fx,fy,ownerType,ownerId,monFlags}`, `padst::frame_begin(10 args)`, `d2vita_vpad_get(uint32_t*, uint8_t[4])` — identiques d'une tâche à l'autre.
- **Hors périmètre volontaire** : `proj=delta` n'est PAS codé d'avance (YAGNI) : il n'est écrit que si le journal de Task 9 Step 1 montre une projection instable.
