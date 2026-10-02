# Changelog

Player-facing changes only — internal refactors, test-only commits and
doc-only commits are skipped unless a release shipped nothing else. Full
commit history: [GitHub compare view](https://github.com/Franckrst/D2Vita/commits/main).

## Unreleased

- **The aim button walks into level exits** (cave mouths, stairs): the
  nearest exit is the last-resort target when no monster, object or item is
  in reach. Confirmed on console at the Den of Evil entrance.
- **The aim button no longer chases invisible markers** in dungeons (the
  game's `dummy` units, which the Den of Evil is full of): a monster is only
  a target if the game itself flags it targetable. Confirmed on console.
- **No more endless auto-walk**: when the target dies or vanishes while the
  aim button is held, the character stops instead of walking on.

## v0.1.19-beta — 2026-10-01

- **Stash, waypoint, chests, town portal and doors are targeted by the aim
  button again.** An object the game flags as targetable is no longer
  written off for good after a slow hover (the game only answers after a
  rendered frame), and an object the game already hovers wins over our own
  hit box. Not confirmed on console yet.
- **Packs of untargetable critters** (Act III geckos) no longer hide the
  real monster behind them: the "game never hovers it" memory holds 32
  units instead of 8.
- **Hold Square, press L to pick up** the highlighted ground item (Cross
  still works). L keeps its aim role when there is nothing to pick up.
- **`diamond=off`** in `controls.txt` hides the aim diamond.
- **`controls.txt` reading is stricter and safer**: out-of-range numbers
  (`orbit=1000` from older builds, `deadzone=15`) are refused and reported in
  `boot_progress.txt` instead of making the character uncontrollable; a
  typo in `l=` (`l=lclik`) no longer leaves L doing nothing; `l=aim_assist`
  is recognised; a UTF-8 BOM at the start of the file is ignored.

## v0.1.18-beta — 2026-09-29

- **New default controller scheme — please test it and give feedback on
  Discord.** **L** is now the aim assist (hold): in game it acts on the
  best target (your corpse, what the cursor points at, an enemy in a cone,
  the nearest chest/door/NPC); in menus, open panels and out of a game it
  is a plain left click. The left stick only walks, the right stick is a
  free cursor. **Square** is the item assist (ground labels, D-pad to
  browse, Cross to pick up). **L + Cross** = inventory, **L + Circle** =
  automap, **L + Square** = mercenary inventory. Stick speed 18, dead zone
  0.15. Cross alone no longer clicks in menus — L does. To get the previous
  scheme back, put `l=lclick` in `controls.txt` (with no other `aim`
  binding). The aim hooks are now installed by default and their frame-time
  cost is not measured yet.
- **Larzuk's add-socket window** now accepts the aim click (it used to
  target the map behind it). First console test only.
- **Title screen: the "Controls" help button** opens only from its own
  small tab, no longer from the whole left edge.
- **Touch offset fixed** in game.

Beta: checked by hand on a console for menus, NPC windows and touch; the
new defaults are not tested by the community yet. Includes the 0.1.17-beta
options.

## v0.1.17-beta — 2026-09-29

- **Two single-player options, both off by default** (add them to
  `env.txt`):
  - `D2_RUNEWORDS_LADDER=1`: the 23 ladder-only runewords (Spirit, Insight,
    Infinity…) can be made in single player.
  - `D2_RESPEC_UNLIMITED=1`: Akara's "Reset Stat/Skill Points" stays
    available after use, once you have earned it (Den of Evil).
  Neither acts in TCP/IP, Open Battle.net or realm games, and no game file
  is changed. They depart from the unmodified game on purpose, which is
  why they are opt-in.

Beta: both were checked on a real console. Includes the 0.1.16-beta fixes.

## v0.1.16-beta — 2026-09-28

- **Multi-row belts no longer flicker.** Hovering or dragging potions in
  an expanded 2–4 row belt made it flicker: the game got two different
  screen positions for the same belt slot. This is very likely also why
  potions sometimes refused to go into the upper belt rows — please
  confirm on Discord.
- **`controls.txt`: lines uncommented from the reference file now work.**
  The reference file writes `#l=lclick   # comment`; uncommenting a line
  kept the trailing comment, so the line was rejected (e.g. `l=alt` left
  L as a left click). Comments after `#` and extra spaces around `=` are
  now ignored.

Beta: both fixes were checked on a real console. Tell us on Discord if
anything regressed compared to 0.1.15.

## v0.1.15-beta — 2026-09-28

- **Inventory, stash and character panels are centered by default now.**
  They used to be anchored to the screen edges, with a wide black gap
  between two open panels; they now sit together as one 800-wide block
  (the SGD2FreeRes model), which makes dragging items between an open
  inventory and stash easier. `D2_RES_PANNEAUX=bords` (or `edges`) in
  `env.txt` restores the previous edge anchoring.

Beta: the anchoring math itself hasn't changed and was already
console-measured before becoming the default, but nobody's looked at two
open panels side by side on this exact build yet. Tell us on Discord if
anything looks or clicks wrong.

## v0.1.14-beta — 2026-09-28

- **Title-screen "Controls" tab.** A small icon in the left letterbox band
  of the title screen opens a full-screen panel listing every binding
  currently in effect — defaults plus anything `controls.txt` changed —
  scrollable by D-pad or drag, closed with Circle or Start.
- **L, R and Select are now remappable**, without touching their role as
  combo-layer prefixes: `l=`/`r=`/`select=` in `controls.txt` change what
  pressing them *alone* does (default: left click, right click, the radial
  menu). A new `l+` layer mirrors the existing `r+` one and starts
  entirely free, for players to fill in themselves.
- **A bad `controls.txt` line now says so.** An unknown button, an unknown
  action, or a combo Select doesn't have used to be silently ignored;
  every rejected line is now named in `boot_progress.txt`.
- **A fully-commented reference `controls.txt`** ships inside the VPK and
  is copied to `ux0:data/d2vita/controls.txt` the very first time the game
  boots — it never overwrites a file a player already has.

Beta: boots cleanly on real hardware, but nobody has hand-tested every new
remap yet (`l=`, `r=`, `select=`, the `l+` layer). Feedback wanted on
Discord before this becomes the default release.

## v0.1.13 — 2026-09-28

- **Act V stutter gone.** On the scripted Harrogath patrol (real console,
  110 s), frames over 60 ms drop from ~25 to 3 and the frame rate reaches
  the game's own cap: 24.0 → 25.0 fps. The cause was ours: an engine-side
  25 Hz frame cap written for online play also ran in solo (it armed
  whenever `D2NET` was set, which the default config always sets). Solo
  Diablo II already draws exactly once per 40 ms simulation step; the cap
  waited a little longer than 40 ms every frame, drifted against the game's
  own clock, and the game then skipped a draw every few seconds. The cap is
  now off by default. **Online:** there is no engine cap either anymore —
  not re-measured online yet; if the game ever runs too fast in a
  Battle.net game, `D2_ONLINE_CAP=25` in `env.txt` brings the old cap back.
- **Less CPU per frame (~1.6 ms of game-thread work, ~5% of a frame).**
  The most frequent calls from the game into the runtime (clock reads,
  critical sections — ~650 per frame) are now served from inside the
  translated code instead of leaving it and coming back; the Glide ring
  DLL copies Perspective vertices in line; `wsprintfA` no longer allocates.
  Main core load on the patrol: ~77% → ~74%. Proven identical on qemu
  (pixel-identical image over 4000 frames, critical-section contention
  test) and measured on console with interleaved passes.
- **Fewer memory-card reads when new content streams in:** a seek now
  reads 32 KiB ahead instead of 8 (~60% fewer card reads on the patrol),
  no extra RAM.
- **Native Perspective floor (`D2_F3NATIF=1`) faster**, still opt-in: it is
  now served in line and writes its draws directly, another ~1.25 ms per
  frame on the patrol (0 divergence against the game's own code on 168,503
  compared cells).

## v0.1.12 — 2026-09-27

- **Perspective floor no longer flickers.** In heavy Perspective scenes
  (Harrogath's gate: ~1.3 MB of Glide records per frame) the render ring
  could not hold the frame being sent to the GPU on the second core *and*
  the next one, and the Glide DLL silently dropped the end of each frame —
  a patch of floor, different every frame. The flush thread now gives the
  ring space back as it walks the frame instead of at its end. Console, at
  the gate, Perspective ON: 0 dropped records, 24 fps (a first fix that sent
  those frames synchronously cured the flicker but fell to 9 fps there).
- **The in-game Resolution option zooms again.** *800×600* draws at the
  Vita's native 960×544 as before; *640×480* now draws at 848×480 — the
  original 640×480's height at the screen's shape — scaled ×1.13 to full
  screen, so characters look as big as native 640×480. Before, choosing
  640×480 only swapped the HUD art. Switching mid-game works in both
  directions (the world used to stop two thirds across the screen after a
  switch to 640). `D2_RES640=WxH` forces another size, `D2_RES640=0` gives
  the original bordered 640×480. Confirmed on console.
- **HUD gaps filled at 640×480 too.** The 640 bar and the column between
  two open panels are filled with stone taken from the game's own inventory
  panel, read from the game's already-loaded art at the first UI draw — no
  panel needs to be opened first, and no Blizzard art is added to the
  package. Not available for classic (non-Lord of Destruction) characters.
- **HUD gaps no longer come back with Perspective OFF**, and the black
  column between two open panels (character sheet + inventory) is filled
  again at 960×544 — its expected position had not followed the vertical
  centring of the panels. Confirmed on console, Perspective ON and OFF.
- **Act V runs faster:** the Glide driver now announces two texture units,
  which lifts D2's sprite cache from 3 MiB — it was re-sending ~50 known
  textures per frame. Console patrol bench: ~20 → ~24 fps (the game's own
  cap is 25).
- **Blinking monsters (Death Maulers) fixed in the texture atlas**: with two
  texture units the atlas could fill up in the wilds and a failed upload
  left the previous sprite bound. The atlas grows to 48 MiB, a failed
  upload now leaves the texture unbound instead of stale, and full size
  classes borrow unused pages from the others. Verified with the qemu
  texture oracle (718 wrong-texture draws → 0); console confirmation is
  still pending.
- The boot log (`boot_progress.txt`) is now written asynchronously: its
  periodic reports no longer stall the game thread (30–107 ms each before).

## v0.1.11-beta7 — 2026-09-24

- **Explored map kept across waypoint trips (and across games).** D2 keeps a
  level's automap in `<character>.ma0/.ma1/.ma2` (one per difficulty): it
  writes the file when leaving a level and reads it back on entry. The
  runtime answered "no such file" to every open of a missing automap — an
  old workaround for a fresh character's act load — which also refused the
  save, so the file never existed and every level entry started from a blank
  map. All automap opens are now honoured as on Windows (the game copes with
  the empty file itself, verified on console: no stall), and every automap
  open is logged in `boot_progress.txt`.

## v0.1.11-beta6 — 2026-09-24

- **Main menu no longer off-centre after Save & Exit.** Returning from a
  game, D2 calls neither `SetResolution` nor `GetResolutionSize` and keeps
  its Glide window as is, so the 960x544 in-game switch stayed armed and the
  800x600 menu art was drawn at the left of a 960-wide window with a black
  band on the right (reported by two players, reproduced on the maintainer's
  console). The runtime now notices Fog's menu loop running with the switch
  armed, restores the native 800x600 and the window size; the next game entry
  re-arms the switch.

Memory:

- Guest heap ceiling raised from ~32.9 to ~56.9 MiB: a 75-minute session
  filled the old ceiling and died on a Blizzard "Unrecoverable internal
  error" (Halt 904, crash report `SZDVJ7PNYRTOS54K`). The room comes from the
  VirtualAlloc window (216 → 160 MiB; two long field sessions peaked at
  97–98 MiB), not from the game's sprite cache, which stays at 64 MiB — no
  fps trade.
- Host-side heap 38 → 46 MiB (was at 31.9 MiB with 624 KiB free at the end
  of that same session).
- Boot log: `alive:` lines now carry `heap=<used>/<ceiling>MB` next to `va=`,
  so a session that gets close to the wall shows it before it dies.

With the [kubridge](https://github.com/bythos14/kubridge) kernel plugin
(v0.3 or later, optional but recommended — the one the big Vita ports already
require; a notice at boot says when it is missing, X or 10 s to continue):

- **JIT pool 16 → 32 MiB.** The kernel caps `sceKernelAllocMemBlockForVM`
  at 16 MiB per process (a second block is refused even with 220 MiB free),
  so every player session so far ran on a single 16 MiB segment and
  re-translated code late in a session. The pool now opens a second 16 MiB
  RWX segment outside that quota. Without the plugin it stays at 16 MiB, and
  `boot_progress.txt` says which case applies (`JIT: kubridge
  present/absent`).
- **Crash reports carry the exact x86 state at the fault:** a user-mode
  abort handler records, before the kernel's dump, the faulting host address,
  whether it was a read or a write, the dynablock and x86 instruction, and
  the eight live x86 registers — `CRASH abort …` in `boot_progress.txt` and
  `crash.log`.
- **Win32 exception fidelity:** a guest access violation is delivered to the
  game's own structured exception handling — the frame handlers at `fs:[0]`
  first, then the top-level filter — with an `EXCEPTION_RECORD` and a
  `CONTEXT` built from the live x86 registers. Fog's filter writes its real
  `Crash.txt` (`ACCESS_VIOLATION`, call chain from the faulting instruction)
  and terminates the process as it does on Windows; the runtime exits cleanly
  and the report is picked up at the next boot — the reporter now reads Fog's
  exception summaries too. Until now such a fault was a bare kernel dump with
  no game-side report.
- **Guard pages under the guest heap:** the 124 KiB null slack is now
  `PROT_NONE`, so a null-plus-offset dereference by the game faults at the
  source — and gets recorded — instead of silently corrupting a neighbour.
- **Self-modifying-code write barrier:** box86's `protectDB` now reaches a
  real `mprotect`, and a guest store into a translated page is served by the
  handler (blocks marked dirty, page reopened, store resumed) instead of
  going undetected. On by default — soaked on console with zero faults and
  unchanged fps; `D2_PROTECTDB=0` in `env.txt` turns it off.

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
