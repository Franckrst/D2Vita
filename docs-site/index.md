# d2vita

**Port of Diablo II: Lord of Destruction to the PS Vita — public source code and VPK.**

!!! danger "Legal status — read before anything else"
    Every public version (source code as much as the VPK) meets a fixed bar
    before publication: no Blizzard file or data in the VPK, third-party
    components licensed and recorded, secrets hygiene. Players supply their
    own legitimately-owned copy of D2/LoD; this repository distributes none.

    This project runs Blizzard's original binary (see
    [Architecture](architecture.md)) rather than decompiling or
    reimplementing it — earlier strategies that decompiled parts of it were
    abandoned before shipping anything. No Blizzard file or asset is
    distributed by this repository or this site.

## What this is

d2vita runs the **real Windows binaries** of Diablo II: Lord of Destruction
(patch 1.14d) on PS Vita, via an x86→ARMv7 execution engine: not a
reimplementation of the game, a direct execution of its original code under
dynamic translation, with only the Windows dependencies (KERNEL32, GDI32,
USER32, DirectDraw/DirectSound...) replaced by native ARM implementations.

Current strategy: **run Diablo, don't rewrite it.** Two earlier strategies
(a native reimplementation via D2MOO, then static AOT lifting of the x86
code to C) were abandoned — the second had only reached ~3.4% of D2Common's
code after substantial work.

## The engine: winx86

The generic engine (PE32 loader, Box86-derived dynarec, schedulers, Win32
import bridge) lives in a separate repository,
[winx86](https://winx86-136891.gitlab.io/), consumed here as a git
submodule (`third_party/winx86`). What's specific to Diablo II stays in
this repository: performance hooks placed at precise addresses of the
game's binary, Glide rendering, and the Win32 shims whose body genuinely
depends on the game.

The generic shims have migrated to the engine through a function-by-function
sort — about a third of the keys today. See [Architecture](architecture.md)
for the principle behind this boundary, and [Win32 shim
list](shims.md) for the exact count, generated from the sources.

## Where to go next

- [Architecture](architecture.md) — how this repository fits together with
  winx86.
- [Three-level validation](validation.md) — the method that makes a
  performance result credible or not.
- [Native ports and measured gains](gains.md) — the catalog, with real
  numbers, measured where and how.
- [Installation](installation.md) — installing the VPK and the game files.
- [Online play](en-ligne.md) — what's proven against Battle.net, and what
  isn't.
- [Fidelity Warden / anti-cheat](fidelite-warden.md) — the detailed
  technical backlog of gaps still open relative to a real Windows process.
- [Controller and keyboard](controles.md) — the full mapping and its
  configuration.
- [Privacy — crash reports](privacy.md) — what's collected, stripped,
  encrypted, and how to turn it all off.
