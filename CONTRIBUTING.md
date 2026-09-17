# Contributing

D2Vita is maintained by one person, with a large share of the implementation
and investigative work produced with AI assistance (Claude Code) under the
maintainer's direction and review — see
[`docs-site/outils-ia.md`](docs-site/outils-ia.md) for what that tooling
actually is. Issues and pull requests are welcome; response time will vary
since this is not a team project.

## Workflow

1. Work on a topic branch for anything non-trivial; keep commits focused.
2. Keep both build paths green:
   - **Host-side dev tools** (desktop): `cmake -B build && cmake --build build`
     — builds the StormLib MPQ tools and `third_party/winx86`'s desktop
     checks only. There is no desktop game binary.
   - **Vita (shipping VPK)**: **not** a CMake target —
     `third_party/winx86/build.sh` then `tools/build_rt_boot_vpk.sh`. See
     [README.md](README.md) > Building and the header comment in
     `CMakeLists.txt`.
   - **Desktop repro of the actual guest code**: `tools/build_oracle_arm.sh`
     (qemu-arm) — the fast way to iterate on anything that touches the
     dynarec or a native hot-path port.
3. Validate at the right level before claiming a change works — this
   project has been burned more than once by a result that only held at one
   level:
   - **qemu-arm**: fast, correctness only. A deterministic boot oracle
     (replayed twice, same verdict both times) catches regressions cheaply.
     Performance numbers from qemu-arm are **never** representative of the
     console and must never be reported as such.
   - **Vita3K**: the same ARM eboot that ships, run on the desktop. Closer
     to real hardware behavior than qemu, still not a valid performance
     measurement.
   - **Real console**: the only source of truth for performance, and the
     final check for anything touching threading, timing, networking, or
     GPU presentation.

   See `third_party/winx86/.claude/skills/` for the full validation ladder
   and A/B measurement protocol, and `.claude/skills/` in this repo for this
   port's own recipes (concrete addresses, knobs, markers — see below).
4. Update [ROADMAP.md](ROADMAP.md) when a subsystem's state changes or
   shifts. Don't invent a fake/synthetic
   response to make something look done — see
   [Fidelity Warden / anti-cheat](https://franckrst.github.io/D2Vita/fidelite-warden/) for why this matters
   beyond ordinary correctness (Warden/anti-cheat fidelity).
5. Commit with a message that explains *why*, not just what changed.

## Skills and agents

Project reference material lives in
[`.claude/skills/`](.claude/skills/README.md): this port's own recipes
(runtime debugging, hardware A/B bench, VitaSDK build, MPQ reading). The
generic engine's skills (dynarec, schedulers, validation ladder, A/B
protocol) live in
[`third_party/winx86/.claude/skills/`](third_party/winx86/.claude/skills/)
instead — the method belongs to the engine, the recipe belongs to the port.
[`.claude/agents/`](.claude/agents/) holds task-specific agents (bringing the
docs back in line with the code, validating a change to an active network
path). All of it is written to be equally usable as plain human-readable
documentation, whether or not you use Claude Code yourself.

## What never enters the repo

- Blizzard binaries and assets (`*.exe`, `*.dll`, `*.mpq`, `*.dc6`, `*.dcc`,
  `*.dt1`, `*.ds1`, `*.pl2`, cinematics, audio) — `.gitignore` filters the
  known extensions; treat that as a first net, not a substitute for looking
  at what you're about to commit.
- CD keys, account passwords, or any other player secret — real or test.
  `keys.txt` and the keystore live under `ux0:data/d2vita_secret/` on-device,
  never in git.
- Raw Ghidra output or any other artifact of the abandoned
  decompilation-based strategy.
