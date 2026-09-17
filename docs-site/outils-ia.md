# The project's AI tooling

The engine and the game are two repositories, and the tooling follows that
boundary: **the method belongs to the engine, the recipe belongs to the
port.**

Everything about running x86 code on ARM — the dynarec, the schedulers,
guest hooks, the validation ladder, proving an instrument before trusting
it, the A/B protocol, the VitaSDK toolchain — now lives on the winx86 side
and names no game:
[the equivalent page on winx86](https://winx86-136891.gitlab.io/outils-ia/).

What stays here is what only this port can answer: which address, which
setting, which marker, which file.

Source: `.claude/skills/README.md`.

## This repository's skills

| Skill | Role |
|---|---|
| `runtime-debugging` | This port's own recipes: the reproduction command under qemu-arm, `D2_REALCLOCK`/`D2SCRIPT`, resolving a guest EIP against `Game.exe`'s original base (loaded with no relocation), the watchdog log, `env.txt`, and which marker means "in game" on each platform. |
| `hardware-ab-bench` | Driving the real console: VitaCompanion verbs, swapping eboots by flavor without reinstalling, the `D2VITA_PATROL` path and its 3600–8400 measurement window, the stray file that blocks character selection, the pauses that invalidate a reading. |
| `vitasdk-build` | Building and running this port's VPK locally: the two-step build against the engine's library, its settings, title IDs and output names, where the game data must sit. |
| `mpq-reading` | Reading Blizzard MPQ archives with StormLib (host-side tools) — extracting, listing, diagnosing. |

The first three open with a pointer to their engine-side counterpart: read
the engine's skill for the *how*, this one for *what to type*.

## Agents

A skill tells you *how* to do something; an agent is dispatched to *do* it.

| Agent | Role |
|---|---|
| `doc-updater` | Bring `docs-site/`, `docs/`, and `ROADMAP.md` back in line with the code after a change — and the rule that a commit message is not a source: every claim gets verified against the code. |
| `online-validation` | Prove a change to an **active** network path is safe: local BNCS server, byte-for-byte A/B on real traffic, and the short-run trap that made two control runs lie. |
| `local-validation` | Prove a correctness or performance claim for everything else: the qemu-arm/Vita3K/console ladder, which script covers which level, and why a qemu number is never a console result. |

The engine has its own agents, including the generic/specific split
methodology used to move code across the boundary.

!!! note "Historical skills removed"
    Skills tied to abandoned strategies (native D2MOO reimplementation, AOT
    static lifting) were removed along with those strategies, in favor of
    directly running the original binaries.
