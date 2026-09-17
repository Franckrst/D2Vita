# Three-level validation

Non-negotiable project rule: **no claim of correctness or performance has
any value if it hasn't been verified at the right level.** Three levels,
each seeing something the others don't.

## Level 1 — qemu-arm (fast, correctness only)

The real ARMv7 binary runs under `qemu-arm` on a Linux development machine,
with no Vita hardware. Fast to iterate, useful for checking that a change
doesn't break anything deterministic (the "oracle": a replayed boot scenario
that must produce a bit-identical verdict from one run to the next —
number of scheduler switches, number of live threads, clean exit, frame
count).

!!! danger "Performance numbers under qemu-arm are NEVER comparable to console"
    qemu-arm emulates the ARM CPU itself on top of running the x86→ARM
    dynarec — a double layer of translation with no relationship to the
    real cost on silicon. A gain or regression measured under qemu says
    nothing about what will happen on the real console. This project has
    documented at least one case where an instrument measured a plausible
    gain under qemu and a **zero** gain on console (`D2_MEMINTRIN`, see the
    gains catalog) — not an exception, a reminder that qemu only measures
    correctness.

Two consecutive runs of the oracle must produce an identical verdict before
trusting anything else — a PASS that doesn't replay is noise, not proof.

## Level 2 — Vita3K (the same ARM eboot, on PC)

[Vita3K](https://vita3k.org/) runs the **same ARM eboot** installed on the
real console — not a recompile, the shipping binary — driven by a
`cmd.txt`. It's the local reproduction bench: closer to reality than qemu
(real ARM ABI, real behavior of the emulated Vita libraries), but still not
a valid performance measurement — Vita3K doesn't have the same cost profile
as the real hardware either.

## Level 3 — real console (the only source of truth for performance)

Only the real PS Vita is authoritative for any claim of a performance gain
or loss. With a measured, documented constraint: **run-to-run dispersion, on
a strictly identical binary, is about 8%** (control-vs-control noise brought
down from 8.6% to 1.6% only after correcting a bench bias — an inventory
panel left open on a dead character, and a metric in wall-clock seconds
rather than frames). Below this noise floor, nothing is measurable in
isolation: either group several interleaved A/B/A measurements on the same
binary, or look for a slope rather than an absolute value.

Practical consequences that recur throughout the gains catalog:

- A published number always states **at which level** it was measured —
  never presented as a console result if it comes from qemu.
- We say "removed guest work" rather than "faster" until level 3 has
  confirmed the gain — one fewer trap round-trip isn't automatically one
  more frame per second.
- An absolute value never carries across a measurement campaign: the
  binary changes, the console drifts thermally (1.5-10%). Only the
  control/variant gaps **within the same interleaved campaign** mean
  anything.
- A software counter doesn't see a wrongly-rendered texture: on the GPU
  path, fidelity is proven by image capture, not by counters.

## Why all three, not just the hardware

Hardware is slow to iterate on (physical access to the console, no
screen/memory capture as convenient as in emulation) and noisy (8%).
Without qemu-arm to filter correctness regressions upstream, every
iteration would cost a hardware round-trip to discover a trivial bug.
Without Vita3K, the first time a change met the real ARM eboot would be on
the console itself. The three levels exist because none alone is enough.

## Reproducing level 2 locally (Vita3K headless)

Vita3K runs the shipped VPK (`build-vita/d2vita.vpk`) via `-r TITLE_ID`,
with no interactive GUI — useful for validating a build without physical
console access.

**One-time setup**: extract the Vita3K AppImage, launch the app once to
install the required Vita firmware ("Download Pre-Install Firmware" +
"Download Firmware Font Package"), then in `~/.config/Vita3K/config.yml`
disable the welcome screen (`initial-setup: false`, `show-welcome: false`)
and pick the OpenGL renderer (`backend-renderer: OpenGL` — llvmpipe exposes
OpenGL 4.5, which is enough).

**Install and run a VPK** (a VPK is a zip):

```bash
V=~/.local/share/Vita3K/Vita3K/ux0/app/DVITA0001
mkdir -p "$V" && 7z x -y -o"$V" build-vita/d2vita.vpk

D=~/.local/share/Vita3K/Vita3K/ux0/data/d2vita
mkdir -p "$D"
for f in ~/d2-vita-refs/<version>/*.[mM][pP][qQ]; do ln -sf "$f" "$D/$(basename "$f")"; done

export APPDIR=~/tools/vita3k/squashfs-root APPIMAGE=fake DISPLAY=:0
export PATH="$APPDIR/usr/bin:$PATH" LD_LIBRARY_PATH="$APPDIR/usr/lib" QT_PLUGIN_PATH="$APPDIR/usr/plugins"
$APPDIR/usr/bin/Vita3K -l 0 -r DVITA0001
```

Log: `~/.cache/Vita3K/vita3k.log`.

Pitfalls encountered, in the order they cost the most time:

- **`AppRun.wrapped` does nothing without `$APPIMAGE`** — export
  `APPIMAGE=fake`, or call `usr/bin/Vita3K` directly.
- **Only one Vita3K process at a time**: two instances interleave their
  writes to `boot_progress.txt` (an unreadable timeline). Kill by exact
  name (`pkill -x Vita3K`) — `pkill -f` also matches the current shell's own
  command line.
- **The log only fills in on a clean stop**: a `pkill -9` truncates it;
  prefer `SIGTERM` (`pkill`'s default), with a short delay. Vita3K
  sometimes ignores SIGTERM when stuck headless — wrap with `timeout -k 10
  90 …` to guarantee a fallback SIGKILL.
- **Logs can fill the disk in a few minutes** (10+ GiB): a guest thread
  stuck in a fault loop spams `|E|` lines regardless of the configured
  level. Keep `log-level: 4` (errors only) in `config.yml` — a one-off run
  with `-l 0` (TRACE) **persists** that setting for every subsequent run.
- **ext4 case sensitivity**: the game's exact filenames (`patch_d2.mpq` vs
  `Patch_D2.mpq` on disk) pass straight through Vita3K's virtual filesystem
  without `rt_boot`'s case-insensitive fallback. Create lowercase aliases
  for every data file. A console's real exFAT doesn't have this problem —
  it's an emulator artifact, not a porting one.
- **Writes are only durable on `fclose`** — to trace a freeze step by step,
  write a marker file per step and close it immediately; a plain `fflush`
  may never reach the host.
