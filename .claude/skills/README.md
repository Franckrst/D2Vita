# D2Vita skills

Project-local skills. Loaded automatically by Claude Code when working in this
repo.

## Where the know-how lives

The engine and the game are two repositories, and the skills follow that
boundary. **The method belongs to the engine; the recipe belongs to the port.**

Everything about running x86 code on ARM — the dynarec, the schedulers, guest
hooks, the validation ladder, proving an instrument before trusting it, the A/B
protocol, the VitaSDK pipeline — now lives in the engine, at
[`third_party/winx86/.claude/skills/`](../../third_party/winx86/.claude/skills/).
Those skills are written for *any* port, so they name no game.

What stays here is what only this port can answer: which address, which knob,
which marker, which file.

## The skills in this repo

| Skill | Purpose |
|---|---|
| [`runtime-debugging`](runtime-debugging/SKILL.md) | This port's recipes: the qemu-arm repro command, `D2_REALCLOCK`/`D2SCRIPT`, resolving a guest EIP against Game.exe's own unrelocated base, the watchdog log, `env.txt`, and which marker means "in a game" on each platform |
| [`hardware-ab-bench`](hardware-ab-bench/SKILL.md) | Driving the real console: VitaCompanion verbs, swapping eboots per flavour without reinstalling, the `D2VITA_PATROL` route and its 3600–8400 window, the save-dir file that hangs character select, the stalls that invalidate a run |
| [`vitasdk-build`](vitasdk-build/SKILL.md) | Building and locally running this port's VPK: the two-step build against the engine library, its knobs, the title IDs and output names, where the game data must sit |
| [`mpq-reading`](mpq-reading/SKILL.md) | Reading Blizzard MPQ archives with StormLib (host-side tools) — extract, list, diagnose |

Each of the first three opens with a pointer to its engine counterpart. Read the
engine skill for *how*, this one for *what to type*.

## Agents

`.claude/agents/` holds agent definitions — a skill tells you *how* to do
something, an agent is dispatched to *do* it.

| Agent | Purpose |
|---|---|
| [`doc-updater`](../agents/doc-updater.md) | Bring `docs-site/`, `docs/`, `ROADMAP.md` back in line with the code after a change — and the rule that a commit message is not a source, every claim gets verified against the code |
| [`online-validation`](../agents/online-validation.md) | Prove a change to an ACTIVE network path is safe: local BNCS server, byte-for-byte A/B on real traffic, and the short-run trap that made two control runs lie |
| [`local-validation`](../agents/local-validation.md) | Prove a correctness or performance claim for everything else: the qemu-arm/Vita3K/console ladder, which script covers which level, and why a qemu number is never a console result |

The engine has its own agents at
[`third_party/winx86/.claude/agents/`](../../third_party/winx86/.claude/agents/),
including the generic/specific split methodology used to move code across the
boundary.

## When they're loaded

Claude Code auto-discovers `.claude/skills/*/SKILL.md`. Descriptions match
against the current task — no manual invocation needed.

## Adding new ones

Follow `superpowers:writing-skills` (loaded on demand). Keep triggers concrete,
bodies focused on judgment and hard-won gotchas — not mechanical facts a
compiler enforces. Ground every claim in something the project actually paid
for.

And before writing one here, ask whether it is really about *this game*. If it
would read the same for another port, it belongs in the engine.
