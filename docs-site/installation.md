# Installation

!!! danger "Personal use only"
    d2vita distributes neither a VPK nor game files. This page assumes a
    **legitimately-owned** copy of Diablo II: Lord of Destruction, on
    personal Vita hardware. See the legal notice on the home page.

## Game files

d2vita contains **no Blizzard file**. 1.13c is retired — only the genuine
1.14d executables and MPQs are supported. Both go together, exactly like a
real PC install, under a `1.14d` subfolder (the loader reads from there
specifically, not from `ux0:data/d2vita/` directly):

```
ux0:data/d2vita/1.14d/
├── Game.exe
├── d2data.mpq      (required)
├── d2exp.mpq       (required, LoD expansion)
├── patch_d2.mpq    (required — 1.14d patch)
├── d2char.mpq
├── d2sfx.mpq
├── d2music.mpq
├── d2speech.mpq    (voice — content is per-language, filename never changes)
├── d2video.mpq
├── d2xmusic.mpq
├── d2xtalk.mpq
└── d2xvideo.mpq
```

That's the full list. 1.14d ships as a single monolithic `Game.exe` with
everything statically linked in — d2vita never reads `Fog.dll`, `Storm.dll`,
`D2Win.dll`, `D2Client.dll`, `D2Common.dll` or `D2gfx.dll` from disk, even
though a PC install ships them alongside it.

Your copy of the folder may still contain those split DLLs and a few extra
launchers (`Diablo II.exe`, `BNUpdate.exe`, `SystemSurvey.exe`,
`BlizzardError.exe`) — a whole PC/Mac install usually does. **That's fine:
d2vita ignores them.** It never loads a stray 1.13c DLL next to the 1.14d
monolith (loading that old, incompatible code is what used to break rendering
or crash the boot — d2vita now refuses to). You can delete them to tidy the
folder if you like, but you don't have to; only `Game.exe`, the MPQs and your
`.key` files actually do anything.

On every boot, d2vita checks that `Game.exe` and each required MPQ above
actually exist, and writes down exactly which one is missing if any is —
see [Verifying the install](#verifying-the-install) below for where that
check is recorded.

It also checks that `Game.exe` is the **official 1.14d build** (3 618 792
bytes, linked 2016-05-31 — both are written to the log on every boot). Any
other build is refused on screen before anything is loaded: a 1.14a/b/c
monolith, a 1.13c launcher, a repacked or relinked file. d2vita patches
`Game.exe` at 1.14d-specific offsets, so another build does not "mostly
work", it fails later in some unrelated way (a 1.14b `Game.exe` used to stop
on Diablo's own *"Error 1: Unsupported graphics mode"* dialog with nothing
on screen). If you see the "invalid game files" screen, replace `Game.exe`
with the one from Blizzard's official 1.14d installer or patch; the MPQs can
stay.

Nothing else needs to be supplied: `ddraw.dll`, `checkrevision.dll` and
`d2vhost.dll` are all faked or shimmed by d2vita itself, never read from disk.
`glide3x.dll` — d2vita's own Glide renderer, which the game genuinely *does*
`LoadLibrary` from disk (D2 is launched with `-3dfx`) — travels inside the VPK
(`app0:glide3x.dll`, `build_rt_boot_vpk.sh`), so you do not supply it either.
Same for `ux0:data/d2vita/shaders/` — the precompiled GPU shaders already
travel inside the VPK. A copy of `glide3x.dll` or of a shader in the game
folder is only an optional on-console override, not something a normal install
needs to populate.

CD keys (optional) go one level up, flat in `ux0:data/d2vita/keys.txt` — see
[Online play](en-ligne.md#the-cd-key-mechanism).

## Getting the VPK

Download `d2vita.vpk` from the [Releases
page](https://github.com/Franckrst/D2Vita/releases) — each release is built
and published automatically by CI.

Building from source instead (for development, or to track `main` between
releases) is documented in the [repository
README](https://github.com/Franckrst/D2Vita#building); the result lands at
`build-vita/d2vita.vpk`.

## Installing on the console

Install the VPK with VitaShell, like any homebrew.

## Configuration (`env.txt`)

The runtime's behavior is configured through an `env.txt` file placed
alongside the MPQs (`ux0:data/d2vita/env.txt`), one variable per line. The
chosen game configuration is documented in [Native ports and measured
gains](gains.md#chosen-game-configuration) — `tools/bancs/env_jeu_glide.txt`
is its reference version, to be copied as-is for normal play (not a
measurement bench). It includes `D2_SON=1`, which turns on sound
(DirectSound, natively-ported Storm codecs) — without it the runtime
presents itself as a machine with no sound card and stays silent.

The **play** environment (perf/config) and the **diagnostic** environment
(`D2_INPUTLOG`, profiling probes, etc.) are kept separate — don't enable the
latter for normal use, they have a cost.

One flag lives outside that reference block on purpose: `D2_LOCAL_ONLY=1`
restricts the runtime to a private/local server instead of official
Battle.net, which is the default. It's a network-policy choice, not a
performance lever, so it isn't part of the config above — add it to
`env.txt` yourself if you want it. Details: [Online play](en-ligne.md).

### The picture

Nothing to set: once you're in a game the game itself draws at the screen's
own 960×544, one texel per pixel. The knobs exist only to undo that.
`D2_RES=0` puts the game back to its 800×600 (so back to the side bars);
`D2_RES=1280x720` forces another size. `D2_ASPECT=etire` stretches the
menus to full width instead of bordering them — menu art is fixed-size, so
it is never redrawn at 960×544. `D2_HUDFILL=0` stops the HUD bar from
filling the two gaps that its 800-wide art leaves on a 960-wide screen,
and stops the black column between two open panels (character sheet plus
inventory or skill tree) from being filled with the frame's own stone.
`D2_RES_PANNEAUX=0` leaves the side panels where the game itself puts
them (items, clicks and border frame 160 px left of the panel art).

The wider field of view a native resolution gives is an advantage in play:
keep it to solo and private servers.

## Verifying the install

Every boot writes a plain-text log to:

```
ux0:data/d2vita/boot_progress.txt
```

Read it with VitaShell's built-in text viewer, or pull it over FTP — it is
a file, not an on-screen message. If `Game.exe` or any required MPQ (see
above) is missing, the very first lines name exactly which file and the
full path it was expected at, before the game gives up. If the game closes
right after launch and the MPQ list above looks right, this file is the
first thing to check.

A boot that stops silently with *nothing at all* written to this file —
not even those first lines — usually means the console itself is stuck in
an inconsistent state rather than a bad install: reboot before digging
further (a documented project pitfall: an empty log doesn't mean the
previous binary booted correctly).
