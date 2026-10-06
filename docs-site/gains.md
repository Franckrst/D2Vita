# Native ports and measured gains

Catalog of optimizations that replace a chunk of translated x86 code with
an equivalent native ARM implementation (see the mechanism in winx86's
documentation, Extension point page). Each row states the **validation
level** of the cited number (see [Three-level validation](validation.md))
— a qemu number is never presented as a console result. Numbers drawn from
the project's internal measurement campaigns (dated reports, not published
in this repository); what follows is their synthesis.

!!! note "No absolute value carries across a campaign"
    The binary changes, the console drifts thermally (1.5-10%). The
    percentages below come from A/B/A measurements **interleaved on an
    identical binary**, never from two different dates compared to each
    other.

## Ports that produced a confirmed gain on console

| Port | Measured gain | Level | Detail |
|---|---|---|---|
| Texture cache: 2 TMUs announced to the game (`D2_GLNUMTMU=2`, default) | **~+20%** (1 TMU: 20.06 and 19.95 fps; 2 TMUs: 23.98, 23.95, 23.95; cap 25); re-sent textures ~50 → 1 per frame | Console (Act V patrol bench, valid passes only, **not interleaved**, ~1 h apart) | With 1 TMU its 3 MiB sprite cache overflowed in steady state: ~50 already-known textures re-sent per frame. Needs a large enough atlas: at 32 MiB it filled up in the wilderness (wrong sprites, blinking Death Maulers); fixed by the 48 MiB atlas and failed-upload handling — **console confirmation pending**. |
| Online frame cap off by default (`D2_ONLINE_CAP`, 2026-09-28) | **24.0 → 25.0 fps; frames over 60 ms: 25 → 3** per 110 s of patrol | Console (Act V patrol bench, route A, 1 pass before, 3 after) | The engine's 25 Hz cap armed whenever `D2NET` was set, i.e. in solo too. It rounded its wait up to the next ms (~41.6 ms/frame) and drifted against D2's own clock, which then skipped draws. In solo, D2 already limits itself to one draw per 40 ms step. |
| Intrinsics served **in line** from translated code (`D2_INTRINLINE`, default since 2026-09-28) | **−0.9 to −1.25 ms of guest work per frame** (route A 26.8/26.6 → 25.7/25.9 ms; route B 27.1/27.0 → 25.8/25.8 ms); c0 76–77% → 74–75% | Console (Act V patrol bench, 8 interleaved passes, same-route comparisons); qemu: identical `FBHASH` pixel oracle over 4000 frames (2 controls + 1 leg), `D2_CSTEST` contention bench PASS | ~1000 trap round trips per frame cost ~4 µs each (JIT exit, lock, re-entry, block lookup: ~10% of frame time, block profile). The slot's stub becomes a box86 native call; direct return to the caller through the CALLRET stack. GetTickCount and critical sections (~650 calls/frame). |
| Vertex copy **in line** in the ring DLL (default since 2026-09-28; `D2_GLRUNS=0` for the old path) | **−0.9 ms of guest work per frame** (route A: 27.6/27.6/27.2 → 26.3/26.8 ms) | Console (5 passes); qemu in game, 4000 Perspective frames: 1.07 M draws re-read byte for byte, 0 difference, sabotage detected | Each vertex copy (28 bytes, zigzag strips) was a call + `rep movs`: 4.4% of the time in the copy function. |
| File read-ahead 32 KiB on a seek (`D2_READAHEAD_JUMP`, default 32 since 2026-09-28) | Card reads in the window 65–71 → 25–27; card time 208–223 → 130–197 ms | Console (4 passes, same route) | No extra RAM (window still 32 KiB). Effect on stutter too small to measure (2–4 loading frames per pass). |
| Native F3 **optimized and served in line** (`D2_F3NATIF=1` + `D2_INTRINLINE`, 2026-09-28) | **A further −1.25 ms** (route B 25.6 → 24.4/24.3 ms; c0 74 → 72%) | Console (4 interleaved passes); qemu: `D2_F3NATIF=2` oracle 168,503 cells, 0 divergence; `D2_F3CHECK` 436,538 strips, 0 difference | Strips written directly (no per-vertex `hostptr`), culled cells' corners deferred, visibility in integers where exact. **Still off by default**: the player's call. |
| Native F3: Perspective floor tiles, `Game+0x10cf70` (`D2_F3NATIF=1`) | **~−2 ms of guest work per frame** (translated 28.0 → ~20.6 ms, native shims ~5.3 ms; c0 −2 points; fps unchanged, already at the 25 cap in town) | Console (Act V patrol bench, 2 valid passes per flavour); qemu oracle `D2_F3NATIF=2`: 0 divergence over 82,000+ drawn cells | Ports the **caller** (~30 small calls per cell absorbed). The remaining cost is memory (projection and LUT tables, ring shared with the flush thread). Off by default pending a decision. |
| Native cell loop (`NATIVECELLLOOP`) | **+16.1%** | Console (A/B/A/B, identical binary) | Traversals ÷2, blit ÷7. |
| `D2_CELLOPT` (optimization mask inside the native loop) | **+9.3%** | Console | Half the work per cell. |
| `D2_CELLPAR=1` (fork-join parallelism, 1 worker thread) | **+12%** | Console | `D2_CELLPAR=2` = **0%** (`USER_2` shared, no second core available). |
| `D2_CALLRET` | **+6.8%** console / **0%** qemu | Console (4 interleaved passes, single binary) | Textbook case: a real gain qemu doesn't see at all. |
| `fastmmu` + `D2_BUDGETTAIL` | **+2.8%** | Console | Taking the base ADD out of the dynarec's critical path. |
| `D2_CSINTRIN` + `WX86_B5` (clock intrinsic) | **+1.4%** | Console | Clock intrinsic rewritten to leave the critical path. |
| `fog_raise_snap` probe disarmed | **+4 to 7%** | Console | The probe itself cost what it measured. |
| GPU ring — asynchronous submission (`D2_GXMASYNC=2`) | **+52%** (28.8 → 43.7 fps bench) | Console | The GPU takes 11ms/submission; async mode overlaps that time. |
| GPU ring — flat clear (`D2_GXMCLEAR=plat`) | **+10.5%** (−2.5ms) | Console | Simplified framebuffer clear. |
| CPU-side ring — host view + fast vertices | **+11.5%** (43.75 → 48.78 fps bench) | Console | Ring traversal ÷2.3. |
| Hoisting TLS resolution out of `CpuBox86::run()` | **+3.03%** at the menu bench, **+8.6%** in real online play | Console (interleaved A/B/A/B + guard rail: guest work identical within 0.1%) | The gain tracks GIL-take density (190/frame at the menu vs. 2880/frame in play) — a percentage measured at the menu must never be published as an in-game gain. |
| Trap entry/exit batching (`trap_retaddr`+`trap_epilogue`, commit `e13edbd`) | **+13.2%** as announced in the commit | Commit-time figure, cited as-is in a later fix — no dedicated campaign document found for this exact number, take it with that caveat. | Already in place today (not an optional knob). |
| `NATIVELIGHTMAP` (dynamic light placement) | **−21%** on the light pass (1.20 → 0.95ms/frame), i.e. **−2.6%** of total drawing | qemu (cross-checked oracle `D2_LIGHTMAPVERIFY`, 51,200 placements, 0 fallback, 0 divergence) | This isolated qemu figure predates console validation. Since 2026-09-22 it's shipped as part of the compiled-default group below — see that row for the console number; no isolated console figure exists for this lever alone. |
| `D2_MEMINTRIN=3` (short copies emitted inline by the dynarec, no helper call) | **+4.6%** (21.93 vs 20.97 fps) | Console (`passe_barb.sh`, real player save) | Different mechanism from the large-transfer/helper path below (refuted): largest single-lever gain of the whole campaign. Arming proven on hardware (helper calls 24,250,123 → 4,248). Oracle `oracle_memintrin.sh` PASS, 4,816,757 calls cross-checked, 0 divergence. Compiled default since 2026-09-22. |
| `D2_FLUSHFIL` (ring flush moved to a dedicated core) + `D2_TEXHASH` + `NATIVEDCC` + `NATIVELIGHTMAP` — 4 settings that had sat in `env_jeu.txt` since 06–13/09 but were missed when commit `fff1904` froze the other 22 as compiled defaults | **+11.7%** at the real 25Hz cap (20.97 → 23.43 fps) | Console (`passe_barb.sh`, 4 witness passes, 0.3% spread over 8h) | The game thread no longer touches the flush at all (`gxm-flush: total-us=3` vs 9,600). Moving ~12–13ms of *memory*-bound work off-core only recovers ~4.7ms of frame time (L2/bandwidth shared across cores) — don't size further work on the un-offloaded figure. |
| All five settings above + `NATIVELIGHTOCC` (light-occlusion field that feeds `NATIVELIGHTMAP`, `Game+0x750f0`) — all six compiled defaults together | **+13.0%** at the real 25Hz cap (20.97 → 23.69 fps) | Console (same campaign) | 50–100ms frame bucket: 1,474 → 51 (÷29). 100–250ms bucket (the real 16-17fps drops): 41 → 28 (−32%) — moves *only* in this full combination; neither `D2_MEMINTRIN=3` nor the flush group alone touches it, because `NATIVELIGHTOCC` produces the field `NATIVELIGHTMAP` consumes. |
| Cumulative, night of 04-05/09, uncapped bench | Control 22.5-23.1 → **28.1-28.6 fps** (+22%) | Console, interleaved campaign of 20 passes | Cumulative effect of several of the ports above. |
| Cumulative GDI → asynchronous Glide | 22.5 → **55.7 fps** (+96%) bench; **25 steps/s + 60 fps** smoothed in real play | Console | Cumulative effect of moving to the asynchronous GPU rendering path (ring + async submission). |
| Native F3 by default (`D2_F3NATIF`, Perspective floor tiles, 10/06) | **22.27 → 23.68 fps (+6.3 %)** | Console (Frozen Orb bench, 2 baseline passes, 4 F3 passes, interleaved) | The port existed since 09/28 but was opt-in. |
| Missile light branches (`NATIVELIGHTDISC`, `NATIVELIGHTDYN`, Game+0x4748d0 / 0x474d70, 10/06) | **23.76 → 24.66 fps (+3.8 %)**, `run` 36.3 → 33.5 ms | Console (Frozen Orb bench, 2+2 interleaved passes); console cross-oracles 133,000 + 14,000 calls, 0 divergence; qemu pixel-identical | The per-light pass (0x4755a0) has three branches; only the kind-2 one was ported. Orbs and their shards take the other two. |
| Per-call room cache in the light-grid rebuild (`D2_GRIDCACHE`, 10/06) | Time share **3.74 % → 1.68 %** (~−0.9 ms/frame) | Console (`BLKSAMP` profile, Frozen Orb bench); qemu: same room decisions cell by cell, pixel-identical, `D2_GRIDVERIFY` 0 divergence | Every cell re-read guest memory through a checked access; the ~5 % of cells that change room redid the whole search. |
| F3's texture bind served natively (`D2_F3TEXNAT`, Game+0x50fbd0 resident branch, 10/06) | Free time per frame **0.3-0.7 → 3.2-3.3 ms**, c0 96 → 90 % (at the 25 fps cap) | Console (Frozen Orb bench, 2+2 interleaved passes); qemu: identical ring fingerprint (3.27 M records, states included), sabotage detected | ~400 round trips per frame into game code. The host writes `grTexSource`/`grTexCombine` through the DLL's own dedup cache, published in the ring header (new `glide3x.dll` required). |

## Ports refuted or with no measurable effect — kept as a warning

| Port | Result | Level | Why it's instructive |
|---|---|---|---|
| `D2_MEMINTRIN=1` (native memcpy/memset served via a helper call, large transfers) | Mechanism proven correct (4.4M calls, 0 divergence) but **0%** on console | Console | The guest CRT already vectorizes its large transfers (NEON); the Glide path makes 90 calls/frame vs. ~1,100 under GDI — the lever had disappeared with the renderer change. Still true for this specific path; `D2_MEMINTRIN=3` (short copies, inline, no helper) is a different mechanism — see the confirmed-gains table, now the compiled default. |
| `D2_FORWARD` | **Inert** (+0.6% = noise) | Console | "Shorter blocks" hypothesis never actually tested. |
| `D2_NOPEND` (lever L4) | **Refuted**, ceiling 0.002% | Console | The original document targeted the wrong code site (0 real calls instead of the assumed site). |
| Offloading RLE to a dedicated thread | **FAIL** on three modes; the mode that passed three days earlier **no longer passes** | qemu (oracle) | "Prove the oracle before concluding" — the founding PASS had never been replayed since. |
| Four image-cache hypotheses | **All refuted** (0 identical frame out of 4,000) | qemu (`D2_CACHEPROBE`) | Found something else instead: the Perspective video option missing from the registry cost +14.2% guest blocks. |
| Hardware PMU (Cortex-A9 cycle counters) | **Abandoned** | Console | The Vita kernel resets the access register on every context switch; would require patching the system scheduler. |
| 5 Act V intrinsics (`D2_INTRIN`: projection, color LUT ×2, light grid, rect fill) | **Negative**: ~+4.3 ms of guest run per frame, ~−3.2% fps (5 patrol passes, oracles at 0 divergence) | Console | Tiny integer functions (20,000–30,000+ calls per frame) that the dynarec already translates almost 1:1: the native body is no faster, and every call pays the plumbing. Off by default. |
| Native draw serialization (`D2_GLNATDRAW=1`, trap-window intrinsic) | **Negative**: ~+5.5 ms per frame, −3.8% fps (4 interleaved passes, v1 bench without route check, window 2600-5600) | Console | ~1,700 exits from translated code per frame cost more than the copy they avoid. Same lesson: an intrinsic only pays when the body it saves is large per call. |
| Fifteen dynarec levers, campaign summary | **A single gain in the entire history of the dynarec** (`D2_CALLRET`, +6.8%) | Console | "The dynarec isn't the lever" — the gain is in native porting of guest code, not in translation itself. |
| Memoizing F3 cell corners within a call (10/06) | **Refuted**: 0 repeats over 2.16 M cells, even with the reduced key (`sub`, `dy`) | qemu (deterministic Glide, 5,000 frames) | Cells of one call never share corners; measure the hit rate before writing the cache. |

## Method for picking a port that pays off

What the campaign learned formalizes as:
`gain ≈ N × (T_guest − T_native − 69ns)` where **69ns is the measured cost
of the x86→native crossing itself** (the price of a round trip, even for an
intrinsic recognized at translation time rather than a real trap). The
right target is **not** the hottest function in the profile — it's a
**caller** with a large number of calls (`N`) that absorbs an entire
subtree in a single traversal. That's exactly the cell loop's pattern: a
single call site produces the entirety of the ~1,600 cell-blit calls per
frame.

## Chosen game configuration

`tools/bancs/env_jeu_glide.txt` (deployed on console, chosen by the
maintainer) — this is the combination of every confirmed lever above:

```
D2SCHED=native
D2WRITE=ux0:data/d2vita/save2
NATIVECELLLOOP=1
D2_CALLRET=1
D2_LAZYSEEK=1
WX86_FRAMEPROF=1
D2_FRAMEUS=1
D2_CELLOPT=63
D2_MMUSTACK=3
D2_MMUFOLD=1
D2_BUDGETTAIL=1
D2_CSINTRIN=1
WX86_B5=1
D2ARGS=game.exe -3dfx
D2_GLIDERING=1
D2_GLIDEGXM=1
D2_GXMASYNC=1
D2_GXMCLEAR=plat
D2_GXMRING=4
WX86_YIELD=2
D2_ONEDRAW=1
D2_IOSTAT=1
D2_READAHEAD=128
D2_READAHEAD_MIN=32
D2_READAHEAD_JUMP=8
D2_READAHEAD_WIN=8
D2_READAHEAD_WINKB=32
D2_SON=1
```

(kept in sync with `tools/bancs/env_jeu_glide.txt`, the source of truth —
this block is a copy, not the other way around. `D2_NOCAP` is deliberately
not in it — it's a bench tool that disengages the game's internal frame
limiters, not a setting to keep for real play.)

`D2WRITE` names the directory the game writes into — saves, `crash.log`,
D2's own daily log. Up to 0.1.6 a directory named here was **not** created
on the console: every write then failed, and the game's C runtime killed
the process about 11 s into the session, with no message pointing at the
directory. Since 0.1.7 the boot creates the named root (and its `Save`
subfolder), probes that it is writable, and falls back to
`ux0:data/d2vita/save` with a line in `boot_progress.txt` when it is not.
