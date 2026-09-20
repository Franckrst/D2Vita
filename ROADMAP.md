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
- [x] Rogue Encampment playable: full physical controls + system virtual
      keyboard (upper/lowercase, digits, punctuation)
- [x] Native scheduler (real preemption): the default; the older
      cooperative scheduler remains as a benchmarking instrument
      (the virtual-clock oracles depend on it)
- [x] Memory budget calibrated on hardware (compact arena, newlib heap
      raised to 38 MiB)
- [x] GPU rendering (sceGxm) via a guest-side reconstructed Glide3x ring,
      asynchronous submission
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
- [ ] **RAM budget**: the multi-segment JIT pool (path 5.2) is partially
      refuted — a `ForVM` block larger than 16 MiB is refused by the kernel
      (a kernel ceiling, not a project choice); the risk of `std::bad_alloc`
      in real play is still open, with no known zero-cost fix
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
