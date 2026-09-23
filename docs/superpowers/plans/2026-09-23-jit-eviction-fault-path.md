# JIT eviction on the ERR_UNIMPL fault path — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make crash signature `SMRD2J34ZXU2A55I` (JIT-pool-exhaustion kills the
guest thread, 143 reports/42 consoles) observable at the moment it happens
instead of a laggy heartbeat, reproduce it deterministically without a
physical console, and ship both a wider eviction trigger and a
measurement-informed eviction-budget fix.

**Architecture:** All changes live in the `winx86` submodule
(`third_party/winx86`, itself vendoring `third_party/box86-dynarec` as plain
files, not a nested submodule) — `dynarec.c` (the DynaRun loop and its
escape valve / death branch) and `dynablock.c` (block lookup, `FillBlock`
retry, the eviction machinery). Nothing in the D2Vita-specific layer changes.

**Tech Stack:** C (box86-dynarec core), cross-compiled ARMv7
(`arm-linux-gnueabihf-gcc` for the qemu-arm target, `arm-vita-eabi-gcc` for
the real Vita target), bash test/repro scripts, an existing host-side C
selftest harness (`tools/selftest.sh` + `tools/jitpool_selftest.c`).

Read first: `docs/superpowers/specs/2026-09-23-jit-eviction-fault-path-design.md`
(same worktree) — this plan implements it and assumes its context.

---

## File Structure

| File | Role |
|---|---|
| `third_party/winx86/third_party/box86-dynarec/dynarec/dynarec.c` | DynaRun loop: escape valve (~L380), fatal-death branch (~L400-415), `DYN86_OOM_EXITS` (L40) |
| `third_party/winx86/third_party/box86-dynarec/dynarec/dynablock.c` | `internalDBGetBlock`/`FillBlock` retry (~L822), `dyn86_ev_dump` (~L691), `DYN86_EV_ROUNDS` (L332) |
| `third_party/winx86/third_party/box86-dynarec/custommem.c` | `AllocDynarecMap`, where `dyn86_jit_allocfail` actually increments (L622, L694/706/715); already carries `D2_JITFAILAFTER`/`D2_JITCHUNK_KB` fault injection |
| `third_party/winx86/src/dynarec86/shim/vita/mman_vita.c` | Real Vita pool ceiling (`dyn86_jitpool_used`/`size`), `__vita__`-only |
| `third_party/winx86/tools/jitpool_selftest.c` + `tools/selftest.sh` | Host-side proof for the pool layer; extend with the new death-visibility scenario |
| `tools/qemu_jitfail_repro.sh` (new, D2Vita root) | qemu-arm reproduction using `D2_JITFAILAFTER` |

No new files beyond the one repro script — this is a targeted fix to
existing, already-well-structured code, not new subsystems.

---

## Task 1: Enrich `dyn86_ev_dump` with the counters it's missing

**Files:**
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynablock.c:691-703`

- [ ] **Step 1: Add `dyn86_jit_allocfail` and `dyn86_meta_allocfail` to the dump message**

Current code (lines 691-703):

```c
void dyn86_ev_dump(const char* what)
{
    char m[208];
    snprintf(m, sizeof m,
        "JIT eviction: %s — tours=%u retires=%u rendus=%u ajournes=%u octets=%lluKo sauvees=%u rendues-au-reessai=%u sorties=%u morts=%u integrite=%u emus=%d retient=%d(prof=%u gen=%u/%u)",
        what, dyn86_ev_rounds, dyn86_ev_retired, dyn86_ev_reclaimed,
        dyn86_ev_deferred, (unsigned long long)(dyn86_ev_bytes>>10),
        dyn86_ev_refills, dyn86_ev_handback, dyn86_ev_oomexit, dyn86_ev_fatal, dyn86_ev_corrupt,
        g_emureg_n, g_grace_blocker, g_grace_bd, g_grace_bg,
        (g_grace_blocker>=0 && g_grace_blocker<DYN86_MAXEMU) ? g_retire_gen[g_grace_blocker] : 0u);
    fprintf(stderr, "[jit] %s\n", m);
    wx86_vita_progress_c(m);
}
```

Replace with:

```c
void dyn86_ev_dump(const char* what)
{
    char m[256];
    snprintf(m, sizeof m,
        "JIT eviction: %s — tours=%u retires=%u rendus=%u ajournes=%u octets=%lluKo sauvees=%u rendues-au-reessai=%u sorties=%u morts=%u integrite=%u emus=%d retient=%d(prof=%u gen=%u/%u) arenefail=%u metafail=%u",
        what, dyn86_ev_rounds, dyn86_ev_retired, dyn86_ev_reclaimed,
        dyn86_ev_deferred, (unsigned long long)(dyn86_ev_bytes>>10),
        dyn86_ev_refills, dyn86_ev_handback, dyn86_ev_oomexit, dyn86_ev_fatal, dyn86_ev_corrupt,
        g_emureg_n, g_grace_blocker, g_grace_bd, g_grace_bg,
        (g_grace_blocker>=0 && g_grace_blocker<DYN86_MAXEMU) ? g_retire_gen[g_grace_blocker] : 0u,
        dyn86_jit_allocfail, dyn86_meta_allocfail);
    fprintf(stderr, "[jit] %s\n", m);
    wx86_vita_progress_c(m);
}
```

(`dyn86_jit_allocfail` is declared `extern` in `custommem.h`, already included
by this file's neighbor `dynarec.c` and available here via the same include
chain — verify with the build in Step 3; `dyn86_meta_allocfail` is this same
file's own global at line 373, no extern needed. Buffer grew 208→256 to fit
the two new fields — `snprintf` truncates safely either way, but there's no
reason to court it.)

- [ ] **Step 2: Commit**

```bash
cd ~/repos/wt-jit-fault-path
git add third_party/winx86
git -C third_party/winx86 commit -m "$(cat <<'EOF'
diag: name the allocator counters in the eviction dump

dyn86_ev_dump already prints everything about eviction rounds and waves,
but not the two counters that actually distinguish "the allocator refused"
from "it never got the chance" - the exact question the D2Vita fault-path
investigation needs answered from the next real report.
EOF
)"
```

(This repo commits at the `winx86` submodule level first, then the outer
`d2-vita` worktree records the new submodule pointer — same two-step pattern
used by every prior `winx86` change in this project, e.g. commit `414a815`
"engine: pick up the JIT pool lots".)

---

## Task 2: Call the dump unconditionally at the death site

**Files:**
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynarec.c:400-415`

- [ ] **Step 1: Replace the gated call with an unconditional one, labeled by case**

Current code:

```c
                /* LA mort. Run() est un talon sans interpreteur
                 * (shim/shim_impl.c), donc franchir cette ligne TUE le fil
                 * invite. Le compteur n'est incremente que si l'allocateur JIT
                 * a deja refuse quelque chose : sinon la cause est un opcode
                 * non implemente, pas la saturation, et les confondre ferait
                 * accuser l'eviction d'un defaut qui n'est pas le sien. */
                if(dyn86_jit_allocfail) {
                    ++dyn86_ev_fatal;
                    /* La PREMIERE mort reelle emporte le bilan complet : sans
                     * ca, les compteurs d'eviction ne sont visibles qu'aux
                     * paliers d'affichage, et un journal peut montrer une mort
                     * sans dire si l'eviction avait tire, ajourne, ou jamais
                     * ete appelee. */
                    if(dyn86_ev_fatal==1) dyn86_ev_dump("MORT d'un fil invite");
                }
                skip = 0;
```

Replace with:

```c
                /* LA mort. Run() est un talon sans interpreteur
                 * (shim/shim_impl.c), donc franchir cette ligne TUE le fil
                 * invite. Le compteur `dyn86_ev_fatal` reste reserve au cas
                 * allocateur (ci-dessous) pour ne pas fausser sa propre
                 * semantique historique, mais le DUMP, lui, est maintenant
                 * inconditionnel : distinguer "l'allocateur avait refuse" de
                 * "il n'a jamais refuse" EST la question, et elle ne se lit
                 * qu'ici, jamais sur le battement 10s qui arrive trop tard
                 * (voir vita_present.cpp, commentaire jitp=). */
                if(dyn86_jit_allocfail) {
                    ++dyn86_ev_fatal;
                    if(dyn86_ev_fatal==1) dyn86_ev_dump("MORT d'un fil invite — allocateur avait refuse");
                } else {
                    static int dyn86_ev_fatal_noalloc = 0;
                    if(!dyn86_ev_fatal_noalloc++) dyn86_ev_dump("MORT d'un fil invite — allocateur JAMAIS refuse");
                }
                skip = 0;
```

(`dyn86_ev_fatal_noalloc` is a new, function-local `static int` — it needs no
declaration elsewhere, mirrors how `dyn86_ev_rounds == 1` already gates the
"premier tour" line just above in `dynablock.c`, and keeps this dump to
"first occurrence" too instead of spamming on every subsequent death of the
same kind.)

- [ ] **Step 2: Commit**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
git commit -am "$(cat <<'EOF'
diag: dump the eviction state on EVERY guest-thread death, not just one case

The existing dump only fired if dyn86_jit_allocfail was already true,
which happens to be exactly the branch that needs the least help
distinguishing itself - the other branch (allocator never refused) is the
one nobody could previously tell apart from "allocator refused but
dyn86_ev_fatal already counted once". Both cases now say which they are,
unconditionally and on first occurrence only.
EOF
)"
```

---

## Task 3: Build both targets, confirm no regressions in the diagnostic-only change

**Files:** none new — verification only.

- [ ] **Step 1: Build qemu-arm target**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86 && ./build.sh
```

Expected: exits 0, produces `build-arm/libwinx86.a` with no new warnings
(the existing `-Wall -Wextra` flags in `build.sh`'s `CFLAGS` will catch a
typo in the new `snprintf` format string immediately — treat any new warning
as a stop-and-fix, not something to wave through).

- [ ] **Step 2: Build the real Vita target**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86 && TARGET=vita ./build.sh
```

Expected: exits 0, produces `build-vita/libwinx86_vita.a`.

- [ ] **Step 3: Run the existing host selftest**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86 && bash tools/selftest.sh
```

Expected: last line `SELFTEST: PASS`. Task 1/2's changes don't touch anything
this selftest exercises directly, so this is a pure regression check.

- [ ] **Step 4: Build the qemu-arm game-running binary that Task 4 will run**

```bash
cd ~/repos/wt-jit-fault-path && bash tools/rt_boot_arm_check.sh
```

This produces `build-arm/rt_boot_arm` — confirmed the right binary because
`tools/qemu_halt1420_repro.sh` (an existing, working repro script for a
different bug) already runs exactly this path against real game data.
(`tools/build_oracle_arm.sh` builds a *different* binary, `oracle_arm`, for
the pixel-level oracles — not what Task 4 needs.)

---

## Task 4: Reproduce the fault under qemu-arm with the existing injection knobs, read the now-visible counters

**Files:**
- Create: `tools/qemu_jitfail_repro.sh` (D2Vita root, modeled on the existing
  `tools/qemu_halt1420_repro.sh`)

- [ ] **Step 1: Write the repro script**

```bash
#!/usr/bin/env bash
# tools/qemu_jitfail_repro.sh — reproduce the JIT-pool-exhaustion guest-thread
# death under qemu-arm, using AllocDynarecMap's own fault-injection knobs
# (D2_JITFAILAFTER/D2_JITCHUNK_KB — see custommem.c's header comment) instead
# of the real Vita pool ceiling (mman_vita.c, TARGET=vita-only, not linked
# into the qemu-arm build at all).
# Usage: [FAILAFTER=1] [CHUNKKB=64] [FRAMES=6000] [BIN=build-arm/rt_boot_arm] bash $0
# Output: $LOG (default /tmp/qemu_jitfail.log). Look for "[jit] ... MORT d'un
# fil invite" and read the arenefail=/metafail= fields Task 1 added.
set -u; cd "$(dirname "$0")/.."
TAG="${TAG:-jitfail}"; FRAMES="${FRAMES:-6000}"
LOG="${LOG:-/tmp/qemu_$TAG.log}"; W=/tmp/d2vita_write_$TAG; rm -rf "$W"; mkdir -p "$W"
env GAMEEXE=1 D2ARGS="game.exe -w" D2LAYOUT=compact D2ARENA=12900000 WLOG=1 \
    MAXSW=60000000 MAXFRAMES="$FRAMES" \
    D2_JITFAILAFTER="${FAILAFTER:-1}" D2_JITCHUNK_KB="${CHUNKKB:-64}" \
    D2WRITE="$W" timeout 600 qemu-arm -B 0x10000 "${BIN:-build-arm/rt_boot_arm}" "$HOME/d2-vita-refs/1.14d" > "$LOG" 2>&1
echo "QEMU EXIT=$?" >> "$LOG"
echo "--- injection lines ---"; grep -E "INJECTION|JIT eviction|MORT d'un fil" "$LOG"
echo "--- log at $LOG ---"
```

- [ ] **Step 2: Make it executable and run it with an aggressive, fast-failing injection**

```bash
cd ~/repos/wt-jit-fault-path
chmod +x tools/qemu_jitfail_repro.sh
FAILAFTER=1 CHUNKKB=64 FRAMES=6000 bash tools/qemu_jitfail_repro.sh
```

`D2_JITFAILAFTER=1` with `D2_JITCHUNK_KB=64` means `AllocDynarecMap` refuses
every new chunk past the very first 64 KiB one — the pool should exhaust in
well under a second of guest time, no gameplay script needed to reach it.

- [ ] **Step 3: Read the result and record it**

Look in `$LOG` (`/tmp/qemu_jitfail.log`) for:
- `[jit] INJECTION: arene bornee a 1 chunks de 64 Ko (= 64 Ko)` — confirms
  injection armed.
- A `[jit] JIT eviction: MORT d'un fil invite — ...` line — confirms a death
  happened, and which label fired.
- Its `arenefail=` field: **nonzero confirms (A)** (the allocator's own
  counter did fire — the escape valve had its chance and either used it or
  the round/exit budget still wasn't enough); **zero confirms (B)** (this
  exact death path never touched `AllocDynarecMap`'s failure at all — some
  other route reaches the death branch).
- `tours=`/`rendus=`/`ajournes=`/`sorties=` — if `arenefail=` is nonzero,
  these say whether eviction ever actually reclaimed anything (`rendus=0`
  with `tours=` at the `DYN86_EV_ROUNDS` ceiling points at Task 6's first
  lever) or kept deferring (`ajournes=` climbing points at a grace/starvation
  issue, a different and NOT yet scoped problem — stop and report rather
  than guessing a fix for it in this chantier).

If the run exits without ever printing an injection or death line (e.g. the
game never got far enough to translate 64 KiB before hitting some unrelated
qemu-arm limitation), retry with `FAILAFTER=8 CHUNKKB=256` (matches the real
2 MiB chunk default more closely) before concluding it doesn't reproduce.

- [ ] **Step 4: Commit the repro script regardless of outcome**

```bash
cd ~/repos/wt-jit-fault-path
git add tools/qemu_jitfail_repro.sh
git commit -m "$(cat <<'EOF'
test: qemu-arm repro for the JIT-pool-exhaustion guest-thread death

Uses AllocDynarecMap's existing D2_JITFAILAFTER/D2_JITCHUNK_KB injection
(custommem.c) since the real Vita pool ceiling (mman_vita.c) doesn't link
into the qemu-arm target at all. Settles whether dyn86_jit_allocfail fires
for this failure mode without needing a console or a 20-minute session.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016YV2ySxuScxYmstEqDV9Bx
EOF
)"
```

---

## Task 5: Widen both eviction-retry gates to also trigger on near-full occupancy

**Files:**
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynarec.c:31,380`
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynablock.c:822`

This task ships regardless of Task 4's finding (per the spec: handle both
root causes, not one gated on the other) — it is a strict widening (more
chances to evict before the death branch), inert when the pool isn't near
full, and it does not touch `dyn86_ev_fatal`'s existing counting semantics.

- [ ] **Step 1: Add a `pool_near_full()` helper, guarded for both targets**

`dyn86_jitpool_used`/`dyn86_jitpool_size` only exist under `__vita__`
(defined in `mman_vita.c`, `TARGET=vita`-only) — referencing them
unconditionally would break the qemu-arm link. Add the helper in
`dynablock.c` (which already sits next to `dyn86_jit_allocfail`'s user, and
is where `dyn86_evict_armed()` — the pattern this mirrors — already lives),
right after `dyn86_evict_armed` (after line 638):

```c
/* Vraie quand la piscine JIT est pres de pleine, MEME SI l'allocateur n'a
 * encore rien refuse. Sous qemu-arm (pas de mman_vita.c, donc pas de
 * jauge), toujours faux : le seul signal disponible y reste
 * dyn86_jit_allocfail, exactement comme avant ce lot — voir
 * tools/qemu_jitfail_repro.sh pour la seule facon d'y observer une
 * saturation (l'injection D2_JITFAILAFTER, qui EST dyn86_jit_allocfail). */
#ifdef __vita__
extern unsigned int dyn86_jitpool_used, dyn86_jitpool_size;
int dyn86_pool_near_full(void)
{
    if(!dyn86_jitpool_size) return 0;
    return (uint64_t)dyn86_jitpool_used * 100ull >= (uint64_t)dyn86_jitpool_size * 95ull;
}
#else
int dyn86_pool_near_full(void) { return 0; }
#endif
```

- [ ] **Step 2: Widen the `FillBlock`-retry gate in `dynablock.c`**

Current code (line 822):

```c
    if(!ret && dyn86_jit_allocfail != dyn86_af0 && dyn86_evict_armed()) {
```

Replace with:

```c
    if(!ret && (dyn86_jit_allocfail != dyn86_af0 || dyn86_pool_near_full()) && dyn86_evict_armed()) {
```

- [ ] **Step 3: Widen the escape-valve gate in `dynarec.c`**

Line 35 already reads `extern int  dyn86_evict_armed(void);` — leave it as
is, and insert one new line directly after it:

```c
extern int  dyn86_pool_near_full(void);
```

Current code (line 380):

```c
                if(dyn86_jit_allocfail && dyn86_evict_armed()
                   && emu->dyn86_oomexit < DYN86_OOM_EXITS) {
```

Replace with:

```c
                if((dyn86_jit_allocfail || dyn86_pool_near_full()) && dyn86_evict_armed()
                   && emu->dyn86_oomexit < DYN86_OOM_EXITS) {
```

- [ ] **Step 4: Keep `dyn86_ev_fatal`'s stats honest**

The death branch (Task 2's output) already only increments `dyn86_ev_fatal`
`if(dyn86_jit_allocfail)` — leave that condition exactly as Task 2 left it.
A death reached only via `dyn86_pool_near_full()` (allocator never refused,
pool was just near-full) will correctly fall into the "allocateur JAMAIS
refuse" branch and increment `dyn86_ev_fatal_noalloc` instead, per the spec's
"don't muddy `dyn86_jit_allocfail`'s existing meaning" requirement.

- [ ] **Step 5: Build both targets and re-run the host selftest**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
./build.sh && TARGET=vita ./build.sh && bash tools/selftest.sh
```

Expected: all three green, no new warnings (the `#ifdef __vita__` split
must produce a real function body on both sides — a stray `-Wunused-function`
on the qemu-arm side would mean the guard is wrong).

- [ ] **Step 6: Re-run Task 4's repro to see whether the widened gate changes anything**

```bash
cd ~/repos/wt-jit-fault-path
FAILAFTER=1 CHUNKKB=64 FRAMES=6000 TAG=jitfail_v2 bash tools/qemu_jitfail_repro.sh
```

Under qemu-arm this specific repro can't exercise `dyn86_pool_near_full()`
(it's compiled to always return 0 there) — this run is expected to look
IDENTICAL to Task 4's, and that is the correct regression confirmation for
this target, not a sign the change did nothing. The pool-aware branch only
matters on the real Vita target; no pre-console venue can exercise it
end-to-end (that's explicitly the user's final task, per the spec).

- [ ] **Step 7: Commit**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
git commit -am "$(cat <<'EOF'
fix: widen JIT eviction triggers to fire on near-full occupancy too

Both retry gates (dynablock.c FillBlock-retry, dynarec.c escape valve)
only fired on dyn86_jit_allocfail actually moving. Widened to also fire
when the pool is >=95% full even if the allocator itself never explicitly
refused - strictly more chances to evict before the death branch, inert
on qemu-arm (no jitpool gauge there) and inert whenever the pool has
headroom. Same bounds as before (DYN86_EV_ROUNDS, DYN86_OOM_EXITS) - no
new deadlock or starvation surface.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016YV2ySxuScxYmstEqDV9Bx
EOF
)"
```

---

## Task 6: Tune the eviction budget, decided by Task 4's actual numbers

**Files:**
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynablock.c:332`
  (`DYN86_EV_ROUNDS`)
- Modify: `third_party/winx86/third_party/box86-dynarec/dynarec/dynarec.c:40`
  (`DYN86_OOM_EXITS`)

This task's shape depends on what Task 4 (and Task 5's re-run) actually
showed. Follow the branch that matches:

- [ ] **Step 1: Check the decision criterion**

From Task 4/5's `dyn86_ev_dump` output: was `dyn86_ev_reclaimed` (rendus=)
still 0 after `dyn86_ev_rounds` (tours=) reached exactly 8 (`DYN86_EV_ROUNDS`)
every time eviction was attempted, with `dyn86_ev_deferred` (ajournes=) NOT
climbing without bound (i.e. waves were retiring, just never finding enough
freed before giving up — not stuck waiting on a grace period that never
passes)?

- [ ] **Step 2a: If yes — raise the round/exit bounds**

Current code (`dynablock.c:332`):

```c
#define DYN86_EV_ROUNDS  8      /* tours d'eviction avant d'abandonner */
```

Replace with:

```c
#define DYN86_EV_ROUNDS  24     /* tours d'eviction avant d'abandonner (releve
                                 * depuis 8 le 2026-09-23 : sous injection
                                 * D2_JITFAILAFTER, 8 tours epuisait le budget
                                 * sans jamais recuperer un octet — voir
                                 * tools/qemu_jitfail_repro.sh) */
```

Current code (`dynarec.c:40`):

```c
#define DYN86_OOM_EXITS 64
```

Replace with:

```c
#define DYN86_OOM_EXITS 192     /* releve depuis 64 en meme temps que
                                 * DYN86_EV_ROUNDS (dynablock.c) — meme
                                 * raison, meme date */
```

Both are pure retry-count ceilings on work that is already proven safe to
retry (bounded, lock-respecting) — raising them changes how long a thread
tries before giving up, not whether the retry itself is safe.

- [ ] **Step 2b: If no (eviction was deferring, not exhausting rounds) — do not touch these constants**

Raising retry counts would not help a grace period that never passes; it
would just retry a stuck situation more times. Report the actual
`dyn86_ev_deferred`/`g_grace_blocker` values from Task 4/5's log instead of
guessing a fix — this is a different, more structural problem than this
chantier's scope (see the spec's "true exhaustion" branch: report back
rather than force a fix that fights a wall this project already knows
about).

- [ ] **Step 3 (only if 2a was taken): rebuild and re-run the repro to confirm it actually helps**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86 && ./build.sh
cd ~/repos/wt-jit-fault-path
FAILAFTER=1 CHUNKKB=64 FRAMES=6000 TAG=jitfail_v3 bash tools/qemu_jitfail_repro.sh
```

Compare `rendus=`/`tours=` against Task 4/5's numbers. If still 0 reclaimed
after 24 rounds, this specific lever isn't it either — report rather than
raising the numbers again on a hunch.

- [ ] **Step 4 (only if 2a was taken and Step 3 confirms an improvement): commit**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
git commit -am "$(cat <<'EOF'
tune: raise the eviction retry budget (measured, not guessed)

DYN86_EV_ROUNDS 8->24, DYN86_OOM_EXITS 64->192. Confirmed under
D2_JITFAILAFTER injection (tools/qemu_jitfail_repro.sh) that the old
bounds were exhausted with zero blocks ever reclaimed, and the new
bounds let eviction actually free memory before giving up.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016YV2ySxuScxYmstEqDV9Bx
EOF
)"
```

---

## Task 7: Extend the host selftest with the death-visibility scenario

**Files:**
- Modify: `third_party/winx86/tools/jitpool_selftest.c`

`jitpool_selftest.c` proves `mman_vita.c`'s pool policy; it does not (and
cannot, without the full `x86emu_t` machinery) exercise `dynarec.c`'s escape
valve directly. What it CAN prove on desktop, cheaply: that `sc_0_1_7`'s
existing "pool frozen, translation refused" scenario is exactly the
condition Task 5's `dyn86_pool_near_full()` is meant to catch — i.e. that
`dyn86_jitpool_used`/`size` really do read as ~100% at the point that
scenario already asserts a refused translation.

- [ ] **Step 1: Add an assertion to `sc_0_1_7`**

Current code (line ~165-174):

```c
static void sc_0_1_7(void) {
    setenv("D2_JITPOOL_PCT", "0", 1);            /* anticipation disabled */
    g_free_user  = 23u << 20;                    /* 16 MiB seg1 + 7 MiB left, as at 13 s */
    g_vm_max     = 16u << 20;
    g_vm_close_at = 9u << 20;                    /* VM space closes early in the session */
    size_t got = translate(20u << 20);
    CHECK(dyn86_jitpool_size == (16u << 20), "0.1.7 : piscine figee a 16 Mo");
    CHECK(got < (20u << 20),                  "0.1.7 : une traduction est REFUSEE (fil invite mort)");
    CHECK(log_has("piscine figee"),           "0.1.7 : le refus est nomme dans le journal");
}
```

Replace with:

```c
static void sc_0_1_7(void) {
    setenv("D2_JITPOOL_PCT", "0", 1);            /* anticipation disabled */
    g_free_user  = 23u << 20;                    /* 16 MiB seg1 + 7 MiB left, as at 13 s */
    g_vm_max     = 16u << 20;
    g_vm_close_at = 9u << 20;                    /* VM space closes early in the session */
    size_t got = translate(20u << 20);
    CHECK(dyn86_jitpool_size == (16u << 20), "0.1.7 : piscine figee a 16 Mo");
    CHECK(got < (20u << 20),                  "0.1.7 : une traduction est REFUSEE (fil invite mort)");
    CHECK(log_has("piscine figee"),           "0.1.7 : le refus est nomme dans le journal");
    /* dyn86_pool_near_full() (dynablock.c) lit ce meme couple used/size a
     * >=95% pour declencher l'eviction meme sans refus de l'allocateur — la
     * scene qui a motive ce chantier est exactement celle-ci : la piscine EST
     * pleine ici, le seuil doit donc lire vrai. */
    CHECK(dyn86_jitpool_used * 100ull >= (uint64_t)dyn86_jitpool_size * 95ull,
          "0.1.7 : la piscine pleine franchit bien le seuil 95% de dyn86_pool_near_full");
}
```

- [ ] **Step 2: Run it**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86 && bash tools/selftest.sh
```

Expected: `SELFTEST: PASS`, including the new check line under `== jitpool ==`.

- [ ] **Step 3: Commit**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
git commit -am "$(cat <<'EOF'
test: prove the 95% threshold reads true in the exact scenario it targets

sc_0_1_7 already reproduces the field timeline (pool frozen, translation
refused); this just asserts the number dyn86_pool_near_full() will read
in that same state, without needing the full dynarec machinery this
desktop harness can't host.
EOF
)"
```

---

## Task 8: Full regression pass and documentation update

**Files:**
- Modify: `ROADMAP.md` (D2Vita root)

- [ ] **Step 1: Full test sweep**

```bash
cd ~/repos/wt-jit-fault-path/third_party/winx86
./build.sh && TARGET=vita ./build.sh && bash tools/selftest.sh
```

Expected: all green.

- [ ] **Step 2: qemu-arm normal play smoke test (no injection)**

```bash
cd ~/repos/wt-jit-fault-path
FRAMES=3000 FAILAFTER=0 CHUNKKB=2048 bash tools/qemu_jitfail_repro.sh
```

`FAILAFTER=0` disables injection entirely (matches `custommem.c`'s own
"0 = inerte" convention) — this confirms an ordinary short session shows no
new `[jit]` lines beyond what already exists today, i.e. Task 5's widened
gate stays silent when nothing is actually near-full.

- [ ] **Step 3: Update ROADMAP.md**

Read the current text of the `- [ ] **The JIT pool is never evicted, and
that is fatal**` entry (`ROADMAP.md`, "In progress" section, confirmed
present at the start of this chantier) and replace its checkbox and body to
reflect what shipped: `[x]` if Task 4's repro confirmed root cause (B) was
real and Task 5's fix directly addresses it, or update the body honestly to
say "(B) ruled out / confirmed, (A) mitigated by a measured budget increase,
still bounded by the same RAM ceiling this entry already named" — whichever
matches the actual Task 4/6 findings. Do not mark it fully resolved unless
the findings support that; a partial, honest update (still `[ ]` with a
narrower remaining gap described) is correct if that's what the evidence
says.

- [ ] **Step 4: Commit the ROADMAP update in the outer d2-vita repo**

```bash
cd ~/repos/wt-jit-fault-path
git add ROADMAP.md third_party/winx86
git commit -m "$(cat <<'EOF'
docs: update ROADMAP for the JIT eviction fault-path chantier

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016YV2ySxuScxYmstEqDV9Bx
EOF
)"
```

---

## Task 9: Prepare for merge (stop before pushing/merging — needs explicit go-ahead)

**Files:** none — review and reporting only.

- [ ] **Step 1: Summarize findings for the user**

State plainly, from the actual Task 4/5/6 measurements (not from this plan's
predictions): which of (A)/(B) turned out to be real, what changed, and what
(if anything) remains unresolved (e.g. the separately-tracked RAM-ceiling
item if Task 6 hit that wall). Also state explicitly that `winx86` is shared
with carn-vita and this chantier did not run or validate anything there
(matches the spec's non-goals) — the user maintains both and may want to
follow up separately.

- [ ] **Step 2: Do not push or merge to `main`/`github/main` without asking**

This chantier's established pattern ([[project_d2vita_clavier_auto_focus]])
squashes the feature branch into a single commit on `main` at the end, via
`commit-tree` from the main worktree (never `git branch -f` from another
worktree) — that step touches the repo other worktrees and eventual sessions
share, and both the `d2-vita` and `winx86` remotes need their own push
credentials sorted first (`winx86`'s `origin` is HTTPS, unlike `d2-vita`'s
SSH deploy-key convention — confirm which credential actually works before
attempting either push). Ask the user how they want to finish the branch;
consider invoking `superpowers:finishing-a-development-branch` at that point
rather than deciding unilaterally.
