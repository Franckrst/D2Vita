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
      implemented, **disabled by default** (`D2_SON` to enable it; the
      default stays `DSERR_NODRIVER`, faithful to a machine with no sound
      card)
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
      backlog in [Fidelity Warden / anti-cheat](https://d2-vita-bb378b.gitlab.io/fidelite-warden/). None of it
      blocks play; it only matters if Warden ever activates.
- [ ] **RAM budget**: the multi-segment JIT pool (path 5.2) is partially
      refuted — a `ForVM` block larger than 16 MiB is refused by the kernel
      (a kernel ceiling, not a project choice); the risk of `std::bad_alloc`
      in real play is still open, with no known zero-cost fix

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
