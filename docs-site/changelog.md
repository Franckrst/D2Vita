# Changelog

Player-facing changes only — internal refactors, test-only commits and
doc-only commits are skipped unless a release shipped nothing else. Full
commit history: [GitHub compare view](https://github.com/Franckrst/D2Vita/commits/main).

## v0.1.11-beta5 — 2026-09-24

- Boot-time warning when a required game file's size doesn't match the
  official 1.14d install (e.g. a `patch_d2.mpq` from a different patch
  level) — shown on screen, requires pressing X to force startup anyway
  instead of silently loading a mismatched version.

## v0.1.11-beta4 — 2026-09-23

- LiveArea background updated (community contribution, issue #19): adds
  the Blizzard Entertainment mark and moves the full "Diablo II: Lord of
  Destruction" logo to the top, same base art and border otherwise.

## v0.1.11-beta3 — 2026-09-23

- On-screen build tag (top-right corner: version + commit hash) so a bug
  report screenshot says exactly which build it's from. `D2VITA_BUILDTAG=0`
  in `env.txt` hides it.
- 960x544 panels centered vertically, matching the horizontal centering
  from v0.1.11-beta.

## v0.1.11-beta2 — 2026-09-23

- **JIT pool exhaustion crash fixed (signature `SMRD2J34ZXU2A55I`, #1 crash
  by volume — 143 reports / 42 consoles as of 22/09).** The pool's
  eviction-and-retry machinery (already shipped, v0.1.10) only triggered
  when the low-level allocator explicitly refused a request. It now also
  triggers directly on pool occupancy (≥95% full), closing a second path
  to the same crash the allocator counter alone couldn't see. Retry budget
  raised (8→24 eviction rounds, 64→192 graceful re-entries) as a low-risk
  measure — not yet proven by a field report, but the underlying retries
  were already bounded and lock-safe.
- Guest VA window trimmed 220→216 MiB to hand the JIT pool a few more MiB
  of headroom (the pool was short by roughly that much, by two independent
  estimates).
- Not console-validated yet — watch for a drop in this signature's report
  count over the next few days.

## v0.1.11-beta — 2026-09-23

- Fixed: at 960x544, on-screen clicks (panel close crosses, buttons,
  touch taps, L/R) landed on stale coordinates instead of what was drawn.
  The screen offset is now written where the game itself lays out the
  UI, so clicks and drawing always agree.
- Panels anchor to the screen edges by default; the old SGD2-style
  centered layout is now an opt-in (`controls.txt`).

## v0.1.10 — 2026-09-22

- Image is no longer stretched: Glide renders at 800x600 and the old
  projection mapped it affinely onto the whole screen (×1.2 in X, ×0.907
  in Y — 32% too wide). Now isotropic by default (725×544, letterboxed);
  `D2_ASPECT=etire` restores the old stretched look.
- Optional native 960x544 rendering (`D2_RES`, off by default): the game
  draws directly at the Vita's screen resolution instead of upscaling
  800x600 — one texel per pixel, no filtering.
- `d2vGlideFlush` moved off the game thread onto its own core, and six
  settings that had been sitting in the reference config since early
  September became compiled defaults: **+13% in real play, frame-time
  spikes cut ÷29** (measured on console, interleaved passes).
- The 960x544 layout's remaining gaps filled in: panels now follow their
  background art (tables, clicks, frame, belt), and the black column/row
  between adjacent open panels is gone.
- box86's translated-code (JIT) pool and RW metadata heap made visible on
  the console heartbeat line and in crash diagnostics, ahead of the pool
  eviction work that shipped in v0.1.11-beta2.
- Removed `grab_checkrevision.py` and the `D2_ALLOW_OFFICIAL` escape
  hatch it existed for.

## v0.1.9 — 2026-09-21

- The virtual keyboard now opens automatically when a text field gets
  focus (character name, Battle.net account/password, game name) — no
  Win32 focus event exists for this, so it's read from D2's own UI state
  directly. Manual open (R+Triangle) still works; `D2_KBAUTO=0` in
  `env.txt` disables the automatic behavior.
- Keyboard opacity is adjustable (`D2_KBALPHA=0-100`, default 80% —
  translucent, character/menu stays visible behind it) and composes on
  the GPU rather than reading the framebuffer back on the CPU.

## v0.1.8 — 2026-09-20

- Fixed the eight crash signatures still active on 0.1.6 (issue tracked
  in PR #15): unreported failed file reads/writes, an unvalidated write
  root, missing archive validation at boot, and a wrong crash-report
  directory.
- Guest heap ceiling raised to ~32.9 MiB (was hitting `ALLOC FAIL` and a
  Halt 904 on large allocations — Traditional Chinese installs' glyph
  atlas was the most common trigger).
- Radial menu (Select) rendering moved entirely to the GPU: it was
  costing ~81 ms per frame while open (24.5 → 8.2 fps on console), ~70%
  of that just re-reading CDRAM from the CPU to blend. Now composited as
  textured quads with no framebuffer readback.
- First round of JIT pool exhaustion mitigations picked up from winx86
  (segment requests step down instead of giving up outright; occupancy
  published on the heartbeat) — not yet the full fix, see v0.1.11-beta2.

## v0.1.7 — 2026-09-20

- Mostly documentation and art this release: `keys.txt`'s layout
  documented (EN/FR), LiveArea artwork from a community contribution
  (issue #7).

## v0.1.6 — 2026-09-19

- Guest heap ceiling raised to ~25 MiB (a 4.2 MB `HeapAlloc` was being
  refused at the old 20 MiB cap while over 200 MB of other memory sat
  free) — the same class of fix carried further in v0.1.8.
- Dynarec-fault reports now dump the faulting opcode bytes and region,
  making this whole family of crashes traceable instead of generic.

## v0.1.5 — 2026-09-18

- Release-process fix only (VPK packaging margin after the first
  segment) — no player-facing changes.

## v0.1.4 — 2026-09-20 *(tag corrected after the fact — see note)*

- The game now refuses to load anything but the official 1.14d `Game.exe`
  **before** running it, instead of failing later with Diablo's own
  cryptic "Unsupported graphics mode" error. (Root cause of a real report:
  a 1.14b executable that looked close enough to pass the version check.)
- Crash-report admin tool: last-seen version per signature, dropdown
  filters.

!!! note "Why v0.1.4 is dated after v0.1.5–v0.1.7"
    Its first release attempt failed CI (a VPK packaging issue); the tag
    was corrected and re-pushed after v0.1.5, v0.1.6 and v0.1.7 had
    already shipped, so its git history and publish date land later than
    its version number suggests. The content above is what v0.1.4 itself
    shipped, independent of when the tag was fixed.
