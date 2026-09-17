# D2Vita — ground rules

D2Vita runs the genuine, unmodified **Diablo II: Lord of Destruction 1.14d**
`Game.exe` on PS Vita hardware: a custom Win32/x86 runtime — a PE32 loader, a
Win32 import bridge, and an x86-to-ARMv7 dynamic recompiler derived from
Box86 — executes the real binary directly, and only the OS layer (KERNEL32,
USER32, GDI, DirectDraw/DirectSound, Winsock, …) is reimplemented natively
for the Vita. **Run Diablo, don't rewrite Diablo.**

Two earlier strategies — a from-scratch reimplementation via D2MOO plus
Ghidra decompilation, then static AOT lifting of the x86 code to C — were
abandoned before shipping anything. Don't follow their workflow or cite them
as current; see [ARCHITECTURE.md](ARCHITECTURE.md) for the current picture.

This is a public repository: both the source and, separately, compiled VPK
releases. See [README.md](README.md) for build instructions,
[ROADMAP.md](ROADMAP.md) for
the coarse checklist, [ARCHITECTURE.md](ARCHITECTURE.md) for the repo layout
and the boundary with the [`third_party/winx86`](third_party/winx86) engine
submodule, and [CONTRIBUTING.md](CONTRIBUTING.md) for the day-to-day
workflow.

## Non-negotiable rules

- **No Blizzard assets or binaries ever committed** — no `*.mpq`, `*.exe`,
  `*.dll`, `*.dc6`, `*.dcc`, cinematics, audio, or fonts belonging to the
  game. `.gitignore` filters the known extensions, but that is a first net,
  not a substitute for reading a diff before committing it. Players supply
  their own legitimately-owned copy of Diablo II + Lord of Destruction.
- **No CD keys, no player secrets, no real account credentials committed.**
  Anything of that shape stays on-device, out of git (`keys.txt` and the
  keystore live under `ux0:data/d2vita_secret/`, gitignored), and out of
  commit messages and docs. See [`docs-site/en-ligne.md`](docs-site/en-ligne.md)
  for how keys are actually handled.
- **Never fabricate a response to make a check pass.** This applies to
  ordinary tests as much as to anti-cheat/CheckRevision fidelity work —
  see [Fidelity Warden / anti-cheat](https://d2-vita-bb378b.gitlab.io/fidelite-warden/). Represent the real guest
  state honestly, or document the gap; don't synthesize a convenient answer
  to make an observer believe otherwise.
- **Never claim a test or build succeeded unless it was actually run**, and
  never replace missing functionality with a stub marked complete.
- **Both build paths stay green**: the host-side dev tools (CMake) and the
  Vita shipping build (`third_party/winx86/build.sh` then
  `tools/build_rt_boot_vpk.sh` — see README.md > Building).
- **A performance or correctness claim names its validation level** — qemu-
  arm, Vita3K, or real console — and a qemu/Vita3K number is never presented
  as a console result. See `.claude/skills/` and
  `third_party/winx86/.claude/skills/` for the validation ladder and the A/B
  protocol this project relies on.
- Keep [ROADMAP.md](ROADMAP.md) accurate as the code changes; it is what
  gets read first for "what actually works today," ahead of this file.

## Legal posture

D2Vita executes Blizzard's own unmodified `Game.exe` rather than
redistributing or reimplementing its logic, and bundles no Blizzard assets;
players must own a legitimate copy of Diablo II: Lord of Destruction to use
it. Each public release meets a fixed bar before shipping: no Blizzard file
or embedded Blizzard data in the VPK, licensing of every third-party
component actually linked in, secrets hygiene, build hygiene.
D2Vita is not affiliated with, endorsed by, or sponsored by Blizzard
Entertainment; Diablo II and Diablo II: Lord of Destruction are trademarks
of Blizzard Entertainment.
