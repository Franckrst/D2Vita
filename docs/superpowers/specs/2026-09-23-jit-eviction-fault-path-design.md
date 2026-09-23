# D2Vita — JIT eviction on the ERR_UNIMPL fault path

2026-09-23. Branch `fix/jit-eviction-fault-path` (d2-vita and winx86, same name).
Worktree `~/repos/wt-jit-fault-path`.

## Problem

Crash signature `SMRD2J34ZXU2A55I` (`abnormal_exit`, exit 0xC0000005, canon
`exit|main_thread_fault|3221225477|-`) is the #1 crash by volume: 143 reports /
42 consoles as of 2026-09-22, still open, still growing (was 20/8 three days
earlier). Mechanism, already established by prior sessions and confirmed today:

- box86's translated-code (JIT) pool never evicts a block. It saturates roughly
  20 minutes into a session (confirmed on the one stored sample: `piscine JIT
  16384/16384 Ko` for 10+ continuous minutes before death).
- Once saturated, the next block that has never been translated fails, and
  because this port has no interpreter fallback (`Run()` is an aborting stub),
  the guest thread dies outright — reported as the generic dynarec fault.
- Verified today by static disassembly of the real `Game.exe` 1.14d
  (`/home/doudou/d2-vita-refs/1.14d/Game.exe`, sha256 `631066c1…`, matches):
  the faulting bytes at the one stored sample (`Game+0x9c490`, frame 53213) are
  byte-for-byte a completely ordinary `call 0x408f20`. Not a corrupted
  pointer, not a genuinely-unimplemented opcode.

Three PRs already merged into winx86 (`fix/jit-pool-exhaustion`,
`fix/jit-pool-eager`, `fix/jit-eviction`, all in 0.1.10) built real
eviction-and-retry machinery: `dynablock.c` evicts and retries a failed
`FillBlock`, and `dynarec.c` has a graceful "exit DynaRun and let the guest
thread come back around" escape valve so eviction never has to happen under a
lock a stuck thread is holding. **Both of these are gated on
`dyn86_jit_allocfail`** (the low-level JIT-arena allocator's own refusal
counter) — the code's own comment states the assumption explicitly:
"the counter only increments if the JIT allocator already refused something:
otherwise the cause is an unimplemented opcode, not saturation."

Our forensic opcode (an ordinary `call`) contradicts "unimplemented opcode."
But it does **not** yet tell us whether `dyn86_jit_allocfail` was 0 (escape
valve never tried) or 1 (it tried, and still lost) at the moment of death —
and `tools/selftest.sh`'s own comment on the `jitpool` test says the pool's
**second 16 MiB segment was requested exactly once across 8 consoles on
0.1.6, at 2690s — and refused** — a timestamp essentially identical to our
sample's fault at 2691s. So a real allocator refusal may well have fired
right at/near death; the field heartbeat (`fail=` on the `alive:` line) is
proven too laggy to say (see `vita_present.cpp` comment: "on every 0.1.6
report it moved inside the watchdog's own 10s blind window — too late to be
evidence of anything").

**This is the crux the design has to respect: two different root causes are
still both live, and each needs its own fix.**

- **(A) True exhaustion.** The escape valve already fired, tried its bounded
  budget (8 eviction rounds, 64 no-progress exits), and genuinely could not
  free enough — because the live working set of translated blocks needs more
  room than even a maximally-evicted pool has. Widening the trigger condition
  alone would not help this case — but *how well the existing eviction budget
  is used* is still a fair target (that is not the same as the separately-
  tracked "give the pool a 2nd 16 MiB segment" RAM-ceiling wall, which stays
  out of scope — see Non-goals).
- **(B) Missed trigger.** `dyn86_jit_allocfail` stayed 0 through the failure
  that killed the thread — some other failure path inside `FillBlock` (or a
  refusal not attributed to that counter) killed the thread without the
  escape valve ever getting a chance.

Per the user: handle both, rather than picking one based on which Phase 1
confirms. Phase 1 still matters — it tells us which is *actually* happening in
the field, and lets Phase 2b's tuning be measured instead of guessed — but
Phase 2 now ships a fix for each, unconditionally.

## Non-goals

- Not conjuring more total memory: the "pool never gets a 2nd segment"
  RAM-budget ceiling stays a separate, already-tracked, already-hard problem
  (ROADMAP: "No affordable fix known"). This design does not touch how much
  memory the pool can ever have — only how well eviction uses what it already
  has. That distinction is what keeps Phase 2b in scope while the RAM
  ceiling itself stays out of it.
- Not a general audit of `winx86`'s locking. The existing eviction machinery
  is deliberately, carefully built (extensive comments on lock ordering,
  starvation avoidance, bounded retries) — this design extends its inputs,
  it does not redesign it.
- Not shipping anything that needs the physical console to validate. Console
  validation (does this crash actually stop happening after 20+ minutes of
  real play) stays a task for the user, same as every prior chantier.
- Not specifically validating carn-vita's behavior. `winx86` is shared with
  carn-vita ([[feedback_winx86_ne_pas_supprimer_shims_inutilises]]) so this
  *will* change runtime behavior there too — the design keeps changes
  conservative and bounded for that reason, but functional carn-vita
  validation is out of scope unless asked for.

## Phase 1 — make the invisible visible, and try to reproduce without console

### 1a. Unconditional death-site dump

`dyn86_ev_dump()` (`dynablock.c`) already prints everything relevant —
eviction rounds, retired/reclaimed/deferred blocks, refills, handbacks,
OOM-exits — and is already called at the fatal-death site in `dynarec.c`
(~line 407), but **only `if(dyn86_jit_allocfail)`**. Change: call it
unconditionally, with a label that distinguishes the two cases (e.g. "MORT —
allocateur avait refuse" vs "MORT — allocateur jamais refuse"), and add
`dyn86_meta_allocfail` and the exact `dyn86_jitpool_used`/`dyn86_jitpool_size`
at that same instant — not the lagging 10 s heartbeat, which the code itself
already documents as too late to be useful.

Zero behavior change. Every field report of this signature from here on
settles (A) vs (B) directly.

### 1b. Reproduce under qemu-arm using the fault-injection knobs already built for this

**Corrected twice while planning, this is the settled answer.** `mman_vita.c`
(the real 16 MiB pool ceiling) is `TARGET=vita`-only — confirmed in
`build.sh` and the file's own header — so qemu-arm cannot see *that*
mechanism. But `custommem.c`'s `AllocDynarecMap` — which *is* built for both
targets, and is the exact function `dynarec_arm.c` calls to get executable
space for a freshly-translated block — already has a fault-injection knob
built for precisely this gap, documented in its own header comment:

> `D2_JITFAILAFTER=<n>`: from the n-th chunk onward, this allocator stops
> opening new ones. This is EXACTLY the Vita pool's behavior once full, and
> it's the ONLY way to reproduce the defect under qemu-arm — where
> mman_vita.c isn't compiled at all. `D2_JITCHUNK_KB=<n>` sets the chunk size
> so the ceiling can be hit in seconds instead of 20 minutes.

This is a direct, designed-for-this-purpose substitute: set both env vars,
boot, play a few seconds, and `AllocDynarecMap` starts refusing new chunks
immediately — the exact same failure `dynarec_arm.c` hits on real hardware at
2690s, propagating through the exact same `FillBlock` → `internalDBGetBlock`
(`dynablock.c`) → `DynaRun` (`dynarec.c`) chain, incrementing the exact same
`dyn86_jit_allocfail`. No Vita3K, no console, no 20-minute wait.

With Phase 1a's dump active, this settles (A) vs (B) directly: if
`dyn86_jit_allocfail` reads nonzero at the death dump under injection, the
escape valve is wired correctly and (A) is what field sessions hit; if it
still reads 0, (B) is confirmed — some path reaches the death branch in
`dynarec.c` without ever going through `AllocDynarecMap`'s counted failure.

**Time-box this**: this is a diagnostic spike, not the deliverable. If
injection somehow doesn't reproduce a clean death (e.g. the game boots too
little code before the injected ceiling to hit a real translation), fall back
to Vita3K ([[feedback_d2vita_vita3k_banc]]) before waiting on a field report —
do not guess.

## Phase 2 — both fixes, unconditionally

### 2a. Fixes (B): widen the trigger

Widen the two gate conditions — `dynarec.c`'s escape valve (~line 380) and
`dynablock.c`'s FillBlock-retry (~line 822) — to also fire when
`dyn86_jitpool_used` is within a threshold of `dyn86_jitpool_size` (proposed
≥95%), even if `dyn86_jit_allocfail` never moved. Same bounds as today
(`DYN86_EV_ROUNDS`=8, `DYN86_OOM_EXITS`=64) — no new deadlock risk, no new
unbounded retry. A near-full-triggered retry is **not** counted as
`dyn86_jit_allocfail` in the stats, so existing observability doesn't get
muddied by a different trigger. This helps regardless of whether (A) or (B)
turns out to be what actually happened in our one sample: it is strictly more
chances to evict-and-retry before falling to the death branch, at no cost
when the pool isn't near-full.

### 2b. Fixes (A): make the existing eviction budget work harder

This does **not** mean asking for more memory (that's the out-of-scope RAM
ceiling). It means checking whether the *existing* evictable pool is being
used as well as it can be before declaring defeat:

- Check whether `DYN86_EV_ROUNDS` (8) / `DYN86_OOM_EXITS` (64) are actually
  the binding constraint, or just an arbitrary historical choice — cheap and
  safe to raise (they only bound retries of work that's already safe to
  retry) if the qemu reproduction (1b) shows a session surviving longer with
  higher bounds. Ship the higher bounds even if 1b can't fully confirm it —
  low-risk, and the next field report becomes the real judge either way.
- Read `dyn86_evict_round`'s actual eviction policy (age/LRU/all-threads?)
  and check for any block that's artificially protected from eviction longer
  than it needs to be (e.g. a grace period sized for correctness that's
  wider than correctness actually requires) — free real, safe headroom
  without touching the total pool size.
- Report honestly if neither of these moves the needle and the wall really is
  "the working set does not fit no matter how it's evicted" — that specific
  finding is the one case where no further fix belongs in this chantier
  (it would be the RAM-ceiling problem in different clothes).

### If Phase 1b doesn't reproduce and no fresh field data arrives

Ship 2a and 2b anyway on the merits above (both are bounded, low-risk, and
independently justified), but say plainly in the report that they are
untested-in-the-field until the next real report confirms — do not claim
the crash is fixed without evidence
([[feedback_oracle_prouver_avant_conclure]]).

## Testing

- `bash tools/selftest.sh` (winx86) must stay green throughout — it already
  covers the `mman_vita.c` pool layer via `jitpool_selftest.c`'s 7 scenarios;
  a new scenario for the "2nd segment refused mid-session" shape belongs here.
- Both targets must build clean: `./build.sh` (qemu-arm/Linux) and
  `TARGET=vita ./build.sh` (real Vita target) — the jitpool-gauge part of 2a
  (`dyn86_jitpool_used`/`size`) only exists under `__vita__`, guard it
  accordingly; the `dyn86_jit_allocfail`-based part of 2a/2b needs no such
  guard, since that counter is common to both targets.
- qemu-arm boot + a normal play session (existing tooling) confirms no
  regression when nothing is injected.
- qemu-arm + `D2_JITFAILAFTER`/`D2_JITCHUNK_KB` (1b) is the fast, designed-
  for-this reproduction of the actual failure this whole chantier is about;
  keep its output regardless of whether it reproduces cleanly, and fall back
  to Vita3K only if it doesn't.
- No physical console step in this design. Final console validation ("does
  the crash actually stop") is explicitly left to the user.

## Open questions for plan-writing

- Exact threshold for "pool near-full" (95% proposed, not load-bearing on the
  design — tune during implementation).
- Whether `dyn86_jitpool_used`/`size` are visible to `dynarec.c` today or need
  a new `extern` (implementation detail, not a design decision).
