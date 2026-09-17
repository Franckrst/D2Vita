# Architecture

## Relationship with winx86

```
d2vita (this repository)
  │
  ├── third_party/winx86/  (git submodule, generic engine)
  │     PE32 loader, ARMv7 dynarec, Cpu/Bridge, schedulers
  │
  └── everything else (specific to Diablo II)
```

The engine — PE32 loader, x86→ARMv7 dynarec derived from Box86, the `Cpu`
interface, `Bridge` (Win32 import bridge), guest thread schedulers — lives
in [winx86](https://winx86-136891.gitlab.io/) and has **no** knowledge of
Diablo II. d2vita adds it as a git submodule (`third_party/winx86`) and
links its own boot executable (`tools/rt_boot.cpp`) against `libwinx86.a`.

This separation was made by extracting everything with no knowledge of the
game out of this repository, validated at every step by `tools/shim_seq.sh`
(no shim table moved) and the qemu-arm oracle replayed twice (identical
deterministic verdict before/after).

## The Diablo-II-specific layers

### `tools/rt_boot.cpp` — the boot

The runtime's entry point: loads D2 1.14d's DLLs, registers Win32 shims
through winx86's extension API, starts the scheduler.

The **full list, with who serves what**, is on the [Win32 shim
list](shims.md) page — generated from the sources, so it's never stale.

!!! note "Sorting is done function by function, on the real body"
    A perfectly generic-looking API name (`SHGetFolderPathA`...) can carry
    game-specific content — a hardcoded `C:\Diablo II` path, for example.
    Migrating a shim to the generic repository on the strength of its name
    alone would be dishonest: classification is done **function by
    function, on the real body, never on the API name** (SHELL32/ADVAPI32,
    USER32, DDRAW/Bink/Smacker/ijl11, IMM32, GDI32 and the window/cursor,
    VERSION and PSAPI, WSOCK32/WS2_32). Every move is covered by
    `tools/shim_seq.sh` (set, winning body, order) and by the qemu-arm
    oracle replayed twice.

    What was genuinely specific stayed, and that's by design: the `C:\Diablo
    II` path of `SHGetFolderPathA` is still here.

The big unsorted chunk remains **KERNEL32**: the large majority of its keys
are still on the game side, with no audit having said which ones are
genuinely specific. That's the obvious next piece of work.

### Native hot-path hooks

Three files extracted from `rt_boot.cpp`, on the same pattern as winx86's
extension API (`Cpu::set_alternate` + `Bridge::shim_trap`):

- **`src/runtime/phase_hooks.cpp`** — render-phase hooks (`D2_PHASEPROF`,
  `D2_ONEDRAW`) and Glide ring tagging (`D2_REPLAY60`/`D2_RINGTAG`).
- **`src/runtime/native_hooks_codec.cpp`** — decoders (PKWARE compression,
  Huffman/ADPCM audio codecs), DCC sprite decoding, `fog_raise_snap`.
  Deliberately limited to **self-contained** hooks — no dependency on the
  parallelism engine below.
- **`src/runtime/native_hooks_cellengine.cpp`** — the fork-join engine
  porting the cell loop (`NATIVECELLLOOP`), with the lightgrid, blend, RLE,
  and collision lookup that depend on it. This is the block that carries
  most of the measured gains (see [Native ports and measured
  gains](gains.md)).

What's left in `rt_boot.cpp`: the boot wiring itself and everything not yet
judged isolated enough to extract safely.

### Rendering and platform

- **`src/glide_ring/*`** — D2's Glide rendering pipeline (ring buffer,
  texture atlas, replay) rendered on the GPU (sceGxm) rather than in
  software.
- **`src/runtime/vita_present.cpp`** — on-screen presentation (a mix of
  generic and specific, not yet audited in detail).
- **`src/platform/vita_net.cpp`** — what remains of the network layer on the
  game side. The primitives (socket table, name resolution, non-blocking
  setup, GIL-aware waiting) moved to winx86; this file keeps the
  self-test, which embeds Battle.net protocol checks.
- **`src/runtime/dcc_native.cpp`** — decoder for Blizzard's proprietary
  sprite format (DCC).
- **`src/runtime/ds_emul.cpp`** — DirectSound emulation (a fabricated COM
  vtable + game-specific mixing choices).

## How the boundary is kept

The pattern is always the same, and the network is its most complete
example: **the engine provides the mechanism, the game provides the
policy.**

- winx86 implements the Winsock→POSIX primitives (socket table, `connect`,
  `recv`, `send`, and real implementations of `accept`/`bind`/`listen` that
  used to be mere stubs). No Diablo II vocabulary appears in it: no port
  number, no notion of a gateway.
- winx86 exposes a **passive observer**: it reports what happens
  (connection, bytes sent, bytes received), it never asks for an opinion.
- d2vita registers **exactly one** observer, in **exactly one** place, which
  carries everything specific: Battle.net protocol decoding, connection
  state flags, clock switching.

The "exactly one place" isn't a style preference. `register_shim`
**overwrites** the key: a function registered twice has the last
registration win, silently. This pattern caused a real Battle.net
connection regression. The [Win32 shim list](shims.md) page permanently
lists keys registered more than once, so the case is seen rather than
discovered in production.

Two corollaries learned the hard way:

- **An API name says nothing.** Classification is done on the function's
  real body, never its Windows signature.
- **A shim isn't a translation layer you can skip.** The shim that
  translates a Win32 call into a POSIX call is mandatory — there is no
  Windows kernel under the Vita. What must disappear is the *business*
  logic slipped inside it. Example: pointing the game at a private server
  is done via the **registry's gateway list**, as on PC — not by rewriting
  the destination address inside `connect`.

## The three test levels

See [Three-level validation](validation.md) for the full method — it's
what makes any performance or correctness claim made on this project
credible.
