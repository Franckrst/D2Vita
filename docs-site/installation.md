# Installation

!!! danger "Personal use only"
    d2vita distributes neither a VPK nor game files. This page assumes a
    **legitimately-owned** copy of Diablo II: Lord of Destruction, on
    personal Vita hardware. See the legal notice on the home page.

## Game files

d2vita contains **no Blizzard file**. You must supply your own MPQs from a
legitimate D2 + LoD install, placed on the console's memory card:

```
ux0:data/d2vita/
├── d2data.mpq      (required)
├── d2exp.mpq       (required, LoD expansion)
├── patch_d2.mpq    (recommended — 1.14d patch)
├── d2char.mpq
├── d2sfx.mpq
├── d2music.mpq
├── d2xmusic.mpq
├── d2xtalk.mpq
└── d2xvideo.mpq
```

The boot-time diagnostic screen tells you precisely which file is missing,
if any is.

The genuine 1.13c→1.14d executables go in their own subfolder, since the
loader looks for them there specifically (not alongside the MPQs above):

```
ux0:data/d2vita/1.14d/
├── Game.exe
├── Fog.dll
├── Storm.dll
├── D2Win.dll
├── D2Client.dll
├── D2Common.dll
└── D2gfx.dll
```

Nothing else needs to be supplied: `ddraw.dll`, `glide3x.dll`,
`checkrevision.dll` and `d2vhost.dll` are all faked or shimmed by d2vita
itself, never read from disk. Same for `ux0:data/d2vita/shaders/` — the
precompiled GPU shaders already travel inside the VPK
(`build_rt_boot_vpk.sh`), that folder is only an optional on-console
override, not something a normal install needs to populate.

CD keys (optional) go in a separate, non-`.mpq` location — see
[Online play](en-ligne.md#the-cd-key-mechanism).

## Building the VPK

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

TARGET=vita bash third_party/winx86/build.sh   # builds the engine (winx86 + dynarec) for the Vita target
bash tools/build_rt_boot_vpk.sh                # produces the final VPK (d2vita.vpk)
```

The root `CMakeLists.txt` does **not** build the shipping binary — only
host-side development utilities (`pe_analyze`, etc.). The real build
pipeline goes through the `tools/build_*.sh` scripts above.

## Installing on the console

Install the produced VPK (`build-vita/d2vita.vpk` or equivalent
depending on the script used) with VitaShell, like any homebrew.

## Configuration (`env.txt`)

The runtime's behavior is configured through an `env.txt` file placed
alongside the MPQs (`ux0:data/d2vita/env.txt`), one variable per line. The
chosen game configuration is documented in [Native ports and measured
gains](gains.md#chosen-game-configuration) — `tools/bancs/env_jeu_glide.txt`
is its reference version, to be copied as-is for normal play (not a
measurement bench).

The **play** environment (perf/config) and the **diagnostic** environment
(`D2_INPUTLOG`, profiling probes, etc.) are kept separate — don't enable the
latter for normal use, they have a cost.

## Verifying the install

The first boot shows a diagnostic screen listing found/missing MPQs before
launching the game. A boot that stops silently before this screen, with
nothing written to the log, usually means the console is stuck in an
inconsistent state — reboot before digging further (a documented project
pitfall: an empty log doesn't mean the previous binary booted correctly).
