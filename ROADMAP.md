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
- [x] Native screen resolution in play: the game itself draws 960×544 (one
      texel = one pixel, no filter, no pillarbox), driven by `alternate`
      hooks on D2's own resolution plumbing — `D2_RES=0` reverts to its
      800×600. Menus stay 800×600 pillarboxed: their art is fixed-size.
      D2's 800-wide HUD bar then opens two `(W-800)/2` px gaps (80 px each
      at 960), which the bar fills with its own stone re-sampled from the
      texture the game already loaded — no Blizzard art is added or
      shipped. `D2_HUDFILL=0` shows the gaps again. The gap fill is
      **confirmed on console** (2026-09-22); the qemu gate
      `tools/hudfill_arm_check.sh` only proves the hook and its numbers,
      since GPU submission there is a sink.
- [ ] **Interface at 960×544 beyond the HUD bar**: the inventory and other
      panels still misbehave after the resolution change. Reported from
      play on 2026-09-22, not yet diagnosed.
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
- [ ] **RAM budget, the JIT pool's second segment**: the entry above used to
      blame a kernel ceiling. That is not what binds. The budget is, and the
      console log prints the whole subtraction: 317 440 KiB free before the
      arena, the arena takes 291 MiB, **19 456 KiB are left** — enough for
      exactly one 16 MiB segment plus 3 MiB of change. Splitting the request
      into 2 × 16 MiB (path 5.2) removed a real obstacle, the per-block 16 MiB
      ceiling, but a second segment was never affordable; at 88 s the ladder
      is refused at 16, 8, 4, 2 and 1 MiB with `libre user` already negative.
      Freeing 16 MiB would mean shrinking the guest VA window (220 → 204 MiB)
      to ~4 MiB above its measured 200,2 MiB peak — and that peak is Act I
      only. No affordable fix known.
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
      reports: 52 of the 62 crash claims received from 0.1.6, across 27
      consoles. A segment request now steps down (16 → 8 → 4 → 2 → 1 MiB)
      instead of giving up, the pool's occupancy is on the `alive:` line and
      announced at 75/90/95 %, and the death is named rather than filed under
      a generic dynarec fault — but **the real fix is not in**: flushing the
      translated cache and retrying the translation needs all guest threads
      quiesced first (`mutex_dyndump` is non-recursive and `FreeRangeDynablock`
      takes it), so it is a design task with its own qemu + console
      validation, not a patch.
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
