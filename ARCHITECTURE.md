# D2Vita — architecture

_State: the runtime boots, plays solo (Rogue Encampment) on real console, and
joins a realm/lobby online against a private test server; see
[ROADMAP.md](ROADMAP.md) for the current detail. This page describes the
repository's structure, not its functional progress._

## Overview

D2Vita does not reimplement Diablo II: it **executes the original Blizzard
binary** (`Game.exe` 1.14d, monolithic since 1.14) under a homegrown
Win32/x86 runtime — a PE32 loader, an import bridge, and an x86→ARMv7
dynarec derived from Box86 — replacing only the system layer
(KERNEL32/USER32/GDI/DirectX/Winsock/…) with native ARM/VitaSDK shims.
Guiding principle: **run Diablo, don't rewrite Diablo** — it supersedes the
earlier reimplementation/decompilation strategies, abandoned before they
shipped anything.

Everything with no knowledge of Diablo II has been extracted into a
separate repository:
[`third_party/winx86`](https://gitlab.com/claude5564407/winx86) (git
submodule). This repository (`d2-vita`) keeps only what is game-specific.

## Diagram

```
                     Game.exe 1.14d (Blizzard, unmodified)
                                   │
                     ┌─────────────▼─────────────┐
                     │   third_party/winx86       │   PE32 loader, Bridge (imports),
                     │   (submodule, generic)      │   x86→ARMv7 dynarec, schedulers
                     └─────────────┬─────────────┘
                                   │ Win32 imports not resolved by execution
                                   │ direct x86→x86 (Storm/Fog/D2*)
                     ┌─────────────▼─────────────┐
                     │  tools/rt_boot.cpp          │   boot wiring, shim table,
                     │  + src/runtime/             │   native-ported hot paths,
                     │                             │   DCC decoder, DirectSound
                     └───────┬───────────┬─────────┘
                             │           │
                   ┌─────────▼───┐ ┌─────▼─────────┐
                   │ src/glide_ring│ │ src/platform  │   vitaGL/sceGxm presentation,
                   │ (GPU rendering)│ │ (input, network, boot config) │
                   └───────────────┘ └───────────────┘
```

## Repo layout

```
d2-vita/
├── CMakeLists.txt              host-only: MPQ tools (StormLib) + winx86 (pe_analyze);
│                                 the production VPK does NOT go through this file (see
│                                 its header comment and README.md > Building)
├── cmake/
│
├── src/
│   ├── runtime/                 D2-specific: native hot-path hooks
│   │   │                         (cells, blits, collision), native DCC
│   │   │                         decoder, DirectSound (ds_emul), PKWARE,
│   │   │                         "_d2" Win32 shims (advapi32, shell32,
│   │   │                         user32) whose body depends on the game
│   │   └── phase_hooks.*, d2_intrin_114.cpp, rt_host.h, …
│   │
│   ├── glide_ring/               GPU rendering: Glide3x ring (glide3x_ring.c),
│   │                              atlas, GPU host (gx_host), 60 Hz replay
│   │
│   ├── platform/                 presentation (vita_present, vita_gxm),
│   │   │                         networking (vita_net), boot config
│   │   │                         (d2_boot_config)
│   │   └── probe/                 gxm_probe (GPU diagnostic tool)
│
├── third_party/
│   ├── StormLib/                 (submodule, MIT) — MPQ reading, host tools
│   ├── pklib/                    (vendored, explode.c) — PKWARE decompression
│   └── winx86/                   (submodule) — generic engine: PE32 loader,
│                                   Bridge, x86→ARMv7 dynarec, coop/native
│                                   schedulers
│
├── tools/                        build scripts (build_rt_boot_vpk.sh,
│                                   build_oracle_arm.sh, …), measurement
│                                   benches (tools/bancs/), MPQ/DC6/DCC
│                                   utilities, dumpers, profile/log analysis
│
├── docs/                          what doesn't fit a docs-site page but
│   └── release/                   still needs a home: the release process
│                                   (VPK/source publication checklist,
│                                   VitaDB submission draft)
│
├── docs-site/ + mkdocs.yml        published site: architecture, validation,
│                                   measured gains, installation, online play,
│                                   controls, generated shim list
│
├── sce_sys/                       LiveArea assets (icon, background, template.xml)
│
├── .claude/skills/                this repo's own recipes (port-specific);
│                                   the engine's generic skills live in
│                                   third_party/winx86/.claude/skills/
│
├── CLAUDE.md, README.md, CONTRIBUTING.md, ARCHITECTURE.md, ROADMAP.md
└── LICENSE                        GPL-3.0
```

There is no longer a `spec/`, `src/engine/`, `src/formats/`, `src/renderer/`,
`src/input/`, `src/audio/`, `src/network/`, nor a `tests/` in the sense of
the old clean-room architecture: those directories belonged to the ADR 002
strategy (reimplementation + D2MOO), abandoned before it shipped.
`COMPATIBILITY.md` and `REVERSE_ENGINEERING.md`, mentioned by an earlier
version of this page, no longer exist at the root. [`THIRD_PARTY.md`](THIRD_PARTY.md)
does exist, as the third-party-license inventory the publication checklist
requires.

Earlier still, `docs/` also held dated internal investigation and
performance-campaign reports (`docs/audit/`, `docs/perf/`), session-journal
notes (`docs/journal/`), workflow scaffolding (`docs/superpowers/`), and the
reverse-engineering paper trail of the abandoned decompilation strategy
(`docs/re/`, superseded by ADR 004). None of that is part of the public V1
release; what is listed above is what remains.

## Boundaries and responsibilities

### `third_party/winx86` (submodule) — the generic engine

PE32 loader, import bridge (`Bridge`), x86→ARMv7 dynarec, cooperative and
native schedulers. Has **no** knowledge of any Diablo II function name or
address; usable for any Win32/x86→ARM port. Audited and documented
separately (its own README/skills).

### `src/runtime/` — what knows Diablo II

Native hooks for the hot paths identified by profiling (the cell loop,
blits, collision, the DCC decoder), the DirectSound backbone (`ds_emul`),
the Win32 shims whose **body** depends on the game (e.g.
`win32_shims_advapi32_d2.cpp` for D2's registry use,
`win32_shims_shell32_d2.cpp` for `SHGetFolderPathA`). The line between what
stays here and what moves to `winx86` is drawn function by function; the
exact list, generated from the sources, is
[`docs-site/shims.md`](docs-site/shims.md) (`tools/gen_shim_doc.py`, checked
in CI).

### `src/glide_ring/` and `src/platform/` — presentation

`glide_ring/` reconstructs a minimal Glide3x pipeline on the guest side and
renders it on the GPU (sceGxm) via `gx_host`. `platform/` ports Vita input
(pad/touch → `vita_present`), networking (`vita_net`), and boot
configuration (`d2_boot_config`).

### `tools/`

Build scripts (Vita VPK, desktop-repro qemu-arm binary), measurement benches
(`tools/bancs/`), MPQ/DC6/DCC/D2S utilities, profile and log analysis tools.
Nothing here is a game engine — these are development scripts.

## Cross-cutting rules

1. **Zero proprietary assets in the repo.** No Blizzard sprites, sounds, or
   binaries (`*.exe`, `*.dll`, `*.mpq`). `.gitignore` is a first net; human
   review is the second. The reference binary (`Game.exe` 1.14d and the
   MPQs) lives outside the repo, under `~/d2-vita-refs/1.14d/`.
2. **Never fabricate a response to make a check pass.** The guiding
   principle — run the real code rather than work around it — also applies
   to anti-cheat checks: see [Fidelity Warden / anti-cheat](https://franckrst.github.io/D2Vita/fidelite-warden/).
3. **No per-frame allocation in the hot path.** Performance discipline and
   pools/LRUs: see the generic engine's skills,
   [`third_party/winx86/.claude/skills/`](third_party/winx86/.claude/skills/).
4. **The VPK does not bundle any MPQ.** The user drops them in
   `ux0:data/d2vita/`. Detected at boot, with a clear message if missing.
5. **Anything borrowed from a third party goes in `third_party/`**, with its
   license preserved (StormLib MIT, pklib, winx86 — see their own license
   headers).

## Progress state

See [ROADMAP.md](ROADMAP.md).
