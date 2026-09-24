# ROADMAP

Execute the original Blizzard binary (`Game.exe` 1.14d) under a Win32/x86
runtime + ARMv7 dynarec, rather than reimplementing it. The earlier
clean-room strategy (a D2MOO hybrid + Ghidra decompilation of D2 1.13c) was
abandoned before it shipped an engine.

This document lists finished and open verticals at a coarse level, and is
the source of truth for the public repository.

## Done

- [x] PE32 loader + import bridge (`Bridge`) + x86→ARMv7 dynarec derived
      from Box86, validated instruction-by-instruction against a desktop
      Unicorn oracle (generic engine, now lives in the
      `third_party/winx86` submodule)
- [x] Boot → menus → solo play, on Vita3K **and** real console
- [x] Rogue Encampment playable: full physical controls + virtual keyboard
      (upper/lowercase, digits, punctuation; opens by itself on text fields —
      validated on the console on 2026-09-21 for every menu field: character
      name, Battle.net account and password, game name; in-game chat is phase 2)
- [x] Native scheduler (real preemption): the default; the older
      cooperative scheduler remains as a benchmarking instrument
      (the virtual-clock oracles depend on it)
- [x] Memory budget calibrated on hardware (compact arena, newlib heap
      raised to 38 MiB)
- [x] GPU rendering (sceGxm) via a guest-side reconstructed Glide3x ring,
      asynchronous submission
- [x] Native 960×544 resolution (`D2_RES`, on by default; `D2_RES=0` or
      `D2_RES=WxH`, 640×480 to 1280×1024, to override): the game itself
      draws 960×544 (one texel = one pixel, no filter, no pillarbox),
      driven by `alternate` hooks on D2's own resolution plumbing, so the
      126 interface centering sites downstream follow without their own
      changes. Menus stay 800×600 pillarboxed: their art is fixed-size.
      4:3 aspect preserved by default for 800×600 (no stretch;
      `D2_ASPECT=etire` for the old stretched behavior)
- [x] HUD bar at 960×544: D2's 800-wide bar opens two `(W-800)/2` px gaps
      (80 px each at 960), which the bar fills with its own stone
      re-sampled from the texture the game already loaded — no Blizzard
      art is added or shipped. `D2_HUDFILL=0` shows the gaps again.
      **Confirmed on console** (2026-09-22); the qemu gate
      `tools/hudfill_arm_check.sh` only proves the hook and its numbers,
      since GPU submission there is a sink.
- [x] Side panels at 960×544 (inventory, skill tree, stash, trade, belt):
      D2's UI-draw routine rewrites its screen shift to the 800×600 values
      (+80/−60) at the start of every frame, so the whole 800 layout
      (panel art, buttons, tooltips) was drawn with one shift while clicks,
      evaluated outside the draw with the shift our hook had set, landed
      80 px away — the button under the cursor lit up, the click did
      nothing (measured on console, 2026-09-23). The shift is now written
      at the *entry* of that routine (its prologue replayed from the host,
      zero bytes of `.text` changed), so drawing and clicks share it. By
      default the panels stay at the screen edges (left panel 0..400,
      right panel `W−400..W`, the game's own +80/−60 shift); the
      `inventory.bin`/`belts.bin` tables and the replayed `800BorderFrame`
      follow that anchoring. `D2_RES_PANNEAUX=centre` centres the 800
      layout as one block instead (panels contiguous, SGD2FreeRes's
      model, an 80 px stone strip on each side); `D2_RES_PANNEAUX=0`
      keeps the game's own tables and frame, for A/B. With two panels
      open the game draws no world, so the column between them (or, when
      centred, the side strips) is filled in the Glide ring with the
      frame's own stone (`D2_HUDFILL=0` to disable). Verified on console:
      close buttons respond to a scripted click and to a separated
      press/release, captures of each panel and of two panels open.
      Known gap: the GDI path (`-w`) never resizes its DIB, so `D2_RES`
      only works under Glide (the default).
      **Vertical anchor (2026-09-23):** X and Y are anchored independently.
      Staying bottom-flush in Y — like the game's own +80/−60 — left only
      ~4 px above the panel on a 544-tall screen (600-tall design), enough
      to slice the ornate corner clean off while the bottom stayed intact
      (reported by a beta tester as "mercenary inventory still not right";
      confirmed on console, zoomed captures of the mercenary and inventory
      panels). Y is now always centred (X keeps following
      `D2_RES_PANNEAUX`), splitting the 56 px shortfall instead of taking
      it all from the top. `dy_tab`/`dy_cadre` (our tables and the replayed
      border) and the native `ScreenShiftY` DrawUI reads now derive from
      the *same* `dy_centre()` value, so they cannot drift apart the way
      draw and click could in X before this fix. Verified on console: full
      ornament visible top and bottom on both panels, inventory close-click
      still lands after the shift (594,432 vs the old 594,404)
- [x] DirectSound audio (host mixer, natively-ported Storm codecs) —
      implemented, **enabled by default**; `D2_SON=0` opts back out to
      `DSERR_NODRIVER`, faithful to a machine with no sound card
- [x] Networking on real hardware: primitive bench 10/10 on hardware
- [x] Online on console, a full game played end-to-end (connect → realm →
      character → game created, act loaded) against both the private
      `jaenster` test realm **and official Battle.net**, with real
      CheckRevision, real Authenticode, and real CD keys. Official access is
      available by default (unconditionally refused while any debugging
      tool is active; `D2_LOCAL_ONLY=1` restricts to a private/local server)
      — see [`docs-site/en-ligne.md`](docs-site/en-ligne.md)
- [x] On-screen build tag (top-right, always on): the same `<VERSION>+<12
      hex>[-dirty]` string as the crash report (`crashreport/build_id.h`),
      drawn with the virtual keyboard's own font. Added 2026-09-23 after a
      beta tester's bug report could not be tied to a specific build with
      confidence; `D2VITA_BUILDTAG=0` hides it

## In progress / open

- [ ] **Warden / anti-cheat fidelity**: no structured exception handling at
      all (a guest fault kills the thread), no PEB/LDR, no per-region
      `VirtualProtect` tracking, self `OpenProcess` still denied — detailed
      backlog in [Fidelity Warden / anti-cheat](https://franckrst.github.io/D2Vita/fidelite-warden/). None of it
      blocks play; it only matters if Warden ever activates.
- [x] **RAM budget, box86's RW metadata**: box86's per-block metadata (one
      `dynablock_t` per translated block, the red-black tree nodes) was the
      only memory this port still asked the kernel for *once play had
      started*, and `customMalloc` did not test `mmap`: a refusal wrote 64 KiB
      through `MAP_FAILED`. That is crash signature
      `hfault_sys|SceLibKernel|0x120` — 11 claims, five consoles, 0.1.7 and
      0.1.9. It now has a reserve of its own, 8 MiB in the **PHYCONT**
      partition (26 624 KiB no session has ever seen move), and a refusal is
      handled rather than fatal. Measured on console: metadata 100 % served by
      the pool (`custom=1152 Ko` = `piscine RW 1152/8192 Ko`), zero
      `mmap FAIL`, user partition 1 MiB less in the red, A/B cost bounded by
      0,5 % over 4+4 patrol passes (three armed passes bracket the control
      mean). Ceiling measured at 124–131 bytes per translated block on three
      sessions, console and qemu.
- [x] **RAM budget, the JIT pool's second segment** — settled 2026-09-24,
      and neither earlier theory was right. It is not the user budget: after
      the arena shrink (VA 216 → 160 MiB, arena 291 → 259 MiB) the console
      had 34 816 KiB free and the kernel still refused a second
      `sceKernelAllocMemBlockForVM` segment down to 1 MiB (`0x80024B0B`); a
      bare test app reproduced it with 220 MiB free. It is a **per-process
      VM quota of 16 MiB**, full stop. The way around it is the kubridge
      kernel plugin: `kuKernelMemReserve(USER_RX)` + `kuKernelMemCommit(RWX)`
      hands back memory written in place and executed (tested on console,
      `kutest`), so the pool's segments 2+ come from there
      (`mman_vita.c`, weak stub in `third_party/kubridge-stub`, runtime
      detection, 16 MiB fallback without the plugin). Console: `segment 2/2
      de 16 Mo reserve (anticipe, via kubridge RWX hors quota VM) — piscine
      32 Mo`, `libre user 19456 Ko` after. The same arena shrink took the
      guest heap from 32,9 to 56,9 MiB (a 75-minute field session had died on
      a full heap, Halt 904, report `SZDVJ7PNYRTOS54K`) and the `alive:` line
      gained `heap=<used>/<ceiling>MB`.
- [x] **Fault handler, guard pages, SMC barrier and SEH delivery (kubridge)**
      — 2026-09-24. With the kubridge plugin the engine registers a user-mode
      abort handler (`fault_vita.c`): a guest fault leaves a durable
      `CRASH abort …` record (host pc, fault address, read/write, dynablock
      and exact x86 instruction, the eight live x86 registers) before anything
      else; the null slack under the guest heap is `PROT_NONE` (a wild null
      dereference faults at the source); box86's `protectDB` reaches a real
      `mprotect` (arena-bounded, MISC/stacks/trap window excluded) and a guest
      store into a translated page is served in the handler — on by default
      with the plugin, soaked in-game (547 pages protected over 6 minutes
      with zone changes, 0 faults, fps and per-block sync unchanged;
      `D2_PROTECTDB=0` cuts it); and a guest access violation is dispatched
      like Windows does — `fs:[0]` frame handlers (`_except_handler4` answers
      ContinueSearch), then the top-level filter: Fog writes its real
      `Crash.txt` (`ACCESS_VIOLATION`, call chain from the faulting
      instruction) and calls `TerminateProcess`, the runtime exits cleanly.
      Proven on console with `D2_SEHTEST=600:32ddd4`. Not done: `RtlUnwind`
      is still a no-op (inner `__finally` blocks are skipped when an outer
      `__except` claims an exception) and `ExceptionContinueExecution` cannot
      resume an abandoned dynablock activation.
- [x] **`D2_JITFLOOR_KB` retired**: the floor reserved ~3 MiB of user RAM for
      box86's metadata, which now has its own phycont pool — it protected
      nobody. It also protected nobody *before*: guarded by `free_kb >= 0`
      while `size_user` reads NEGATIVE from 13 s onward (`libre user=-2048 Ko`
      in every field log), so the one situation where it would have had
      something to protect was exactly the one where it did not apply. Gone,
      along with its deferral path and log line. The anticipated grow now
      takes the largest step that fits: a selftest scenario that used to
      assert "1 MiB taken, not 2, floor honoured" now asserts 4 MiB. On a
      console whose gauge reads negative this changes NOTHING, and the A/B
      shows it: segment 2 is refused from 16 MiB down to 1 MiB at 88 s in all
      four passes, RW pool armed or not. The pool returns ~1 MiB of user RAM
      (free user goes -2048 -> -1024 KiB) and the floor returns nothing it
      never took; the ladder's smallest step is 1 MiB and a negative balance
      cannot pay it. The removal is worth having on a console that reads a
      positive gauge, not on this one.
- [ ] **The JIT pool is never evicted, and that is fatal**: nothing frees a
      translated block, so the pool saturates after roughly 20 minutes of
      play. Once it is full and the kernel refuses a new segment, the first
      block that has never been translated kills the guest thread outright —
      there is no interpreter to fall back to. This is what the field
      reports: signature `SMRD2J34ZXU2A55I`, 143 claims across 42 consoles as
      of 22/09, the #1 crash by volume, still open. 0.1.10 already shipped
      real eviction-and-retry machinery (PR #3, `fix/jit-eviction`) — but
      both its trigger points were gated on `dyn86_jit_allocfail` alone, the
      low-level arena allocator's own refusal counter, on the unstated
      assumption that nothing else could reach the death branch. A qemu-arm
      reproduction (`tools/qemu_jitfail_repro.sh`, driving the arena's own
      `D2_JITFAILAFTER` fault-injection knob — the real pool ceiling in
      `mman_vita.c` is Vita-only and unreachable under qemu) confirms that
      assumption for the case tested: the allocator does refuse before
      death, and eviction is genuinely working — tens of thousands of blocks
      reclaimed — before eventually losing one race under artificial
      pressure. Both trigger points are now ALSO armed directly by pool
      occupancy (`dyn86_pool_near_full()`, >=95 %), independent of whether
      the allocator's own counter ever moved, closing a second, silent path
      to the same death. The retry budget (`DYN86_EV_ROUNDS` 8→24,
      `DYN86_OOM_EXITS` 64→192) was raised as a low-risk bet — same locking,
      same bounded retries, just more of them — not a proven fix: repeated
      qemu runs at identical injection parameters showed >10x variance in
      how long eviction ran before a death (cumulative rounds 69 to 728
      across otherwise-identical runs), too noisy for one before/after run
      to settle. Every death now dumps the full eviction ledger
      unconditionally, labeled by which case fired, instead of the old
      allocator-only-gated dump that stayed silent on exactly the case most
      worth seeing — the next field report settles this directly. **Not
      console-validated**: whether this actually stops the crash across real
      20+ minute sessions is still open. If it does not, the remaining gap
      is very likely the RAM budget entry above (2nd segment never
      affordable) rather than eviction itself, since eviction is now
      demonstrably working — just still bounded by how much the pool can
      ever hold.
- [ ] **`Crash.txt` reports a stack address where the game writes
      `_ReturnAddress()`**: all six `halt` reports received show it, on every
      build. The line number pushed as an immediate is correct, so only EAX
      is wrong at `Game+0x8090` (`mov eax,[esp]`). Mechanism not understood;
      it degrades every `Crash.txt` we receive.
- [ ] **Signature `SNALU33A2TNV5UYB` (halt 3544) unresolved**: the game is
      refused `grey.dat` from `d2data.mpq` 8 s into boot, while the log proves
      the archive was mounted and that no sector was ever read from it — so
      the failure is in name resolution, in memory, before any I/O. The two
      candidates left are a hash table whose CONTENT is wrong without being
      truncated, and a signature that covers more than one cause. A refused
      archive open is now named in `boot_progress.txt`, which is the evidence
      that was missing.

## Not started

- Acts II through V, classes other than the Rogue played so far
- Ubers, Diablo Clone, Cow Level, modding, ladder, seasons
- Remastered rendering (the classic renderer remains the only path)
- Cinematics (Bink video codecs)

## No time estimates

The old M0-M9 milestones (from the abandoned clean-room strategy, with
estimates in weeks/months/years) have been removed from this document: they
described a different project (from-scratch reimplementation) and no longer
make sense for the current runtime. No time estimate is maintained in their
place — verdicts on this project have reversed often enough that a
projection would be misleading; the sections above are kept current
instead.
