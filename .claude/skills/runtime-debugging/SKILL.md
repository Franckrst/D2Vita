---
name: runtime-debugging
description: Use when debugging THIS port (rt_boot running the genuine Blizzard 1.14d Game.exe on the Vita) — the concrete recipes the generic method needs to be executed here: the qemu-arm repro command, D2_REALCLOCK / D2SCRIPT, resolving a guest EIP against Game.exe's own unrelocated base, the watchdog log, env.txt, and which marker means "in a game" on each platform.
---

# Runtime debugging — this port's recipes

**The method lives in the engine**, not here:
`third_party/winx86/.claude/skills/runtime-debugging/SKILL.md` — desktop-first
reproduction, the 3-level validation ladder, the byte-identity gate, watchdog
attribution, resolving a guest EIP, the generic gotchas.

This file holds only what is specific to *this* port: the commands, the
addresses and the markers. Read the engine skill first; come here for the
values to type.

## Reproducing on the desktop

`D2_REALCLOCK=1` puts the qemu-arm binary on the same real-time clock the Vita
uses. Iteration drops from ~10 min to ~2 min.

**Do not retype the build command** — the ARM translation-unit list changes
every time a file moves between this repo and the engine. `tools/rt_boot_arm_check.sh`
*is* the recipe: it builds the engine (`third_party/winx86/build.sh`), then
compiles `rt_boot.cpp` plus this port's units against
`third_party/winx86/build-arm/libwinx86.a`, and runs the check twice.

Drive gameplay deterministically with `D2SCRIPT` (frame:action events, see
`tools/profile_rogue.sh`). A compressed input script reaches "in a game" in
~2 min and reproduces most load-path bugs.

## Resolving a guest EIP / crash address

On the desktop (sparse layout) `Game.exe` loads unrelocated at its preferred
base `0x00400000` — byte-identical to disk (see `ARCHITECTURE.md`) — so
`objdump -D -b binary -m i386 --adjust-vma=0x00400000` directly on `Game.exe`
turns any `eip=` from the watchdog log or a crash report into
`Game.exe+0x…`, no layout arithmetic needed.

**On console (and under `D2LAYOUT=haut`) it is relocated into the module
window** of the compact pack: base `0x03900000` since 2026-09-24,
`0x02100000` from 0.1.7 to 0.1.11-beta5, `0x01900000` up to 0.1.6. A raw
console `eip=` or a `Crash.txt` DBG-ADDR is `base + RVA`; `tools/x86dis.py`
knows the three windows and rebases for you. Crash reports pulled with
`tools/crash/crash.py` are already `Game+0x…`.

`CheckRevision.dll` is the one other module the runtime loads, and only
dynamically, at Battle.net connect — resolve its EIPs the same way, against
its own reported load base, not `Game.exe`'s.

(Older notes describing a compact multi-DLL layout packed from `0x01D00000`
— D2Game/D2CMP/D2Client as separate modules with fixed ImageBases — predate
the move to running 1.14d's single, monolithic `Game.exe`; that layout no
longer applies to the shipping runtime.)

## Progress markers and the watchdog

`d2vita_watchdog_start` logs every 10 s to `boot_progress.txt`:

```
alive: pump= frames= reads= eip= sw= io= jit= sync= fail=
```

Deltas between lines localize a freeze — the engine skill explains how to read
them. What is specific here is which marker means "reached the interesting
state" on each level of the ladder:

- **On device**, `resolution ->` prints; on the desktop it never does.
- **On the desktop**, the "in a game" marker is `CreateDIBSection 640` — grep
  for that when scripting qemu batteries.

Getting this backwards makes a scripted battery measure the wrong window in
silence.

## Flipping probes without recompiling

`ux0:data/d2vita/env.txt` is parsed at boot, one `KEY=VALUE` per line, and wins
over knobs baked into the build. To flip a probe on a device build, edit that
file instead of rebuilding. Verify it landed by grepping the log for
`env.txt: <KNOB>=`.

## The gotcha that looks like an engine bug and isn't

**A missing MPQ reads as an infinite "Insert Expansion Disc" loop**, not an
error — a hang at a low `reads=`, not a crash. Check the archives are installed
before debugging anything else.
