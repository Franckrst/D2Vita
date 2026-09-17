---
name: vitasdk-build
description: Use when building or locally running THIS port's Vita package — the shipping VPK is NOT built by CMake but by tools/build_rt_boot_vpk.sh against the engine's static library. Covers the two-step build, its rebuild/jobs/tag knobs, the output names and title IDs, and where the game data must sit for a Vita3K run.
---

# Building and running this port's VPK

**The toolchain and the pipeline live in the engine**:
`third_party/winx86/.claude/skills/vitasdk-build/SKILL.md` (VitaSDK, the
elf→velf→self→eboot→vpk chain, common build errors) and
`third_party/winx86/.claude/skills/vita3k-testing/SKILL.md` (booting a VPK
headless in Vita3K, its traps, reading the log).

This file holds only what is specific to *this* port.

## The shipping VPK is not a CMake build

There is a CMake target, but the VPK that actually ships comes from a shell
script that compiles `rt_boot` plus this repo's game-specific units and links
them against the engine's static library. Two steps, in order:

```bash
TARGET=vita bash third_party/winx86/build.sh      # -> third_party/winx86/build-vita/libwinx86_vita.a
bash tools/build_rt_boot_vpk.sh                   # -> build-vita/d2vita_boot.vpk
```

The first step is the engine; it used to be a script in this repo
(`tools/build_dynarec86.sh`) and no longer is. If a build fails on a missing
symbol from the runtime, rebuild the engine before looking anywhere else.

Defaults of the second step: `TITLE=DTWO00001`, `APPNAME="D2Vita Boot"`,
`VPKOUT=d2vita_boot.vpk`, output directory `build-vita/`.

## Its knobs

| Variable | Effect |
|---|---|
| `D2VPK_REBUILD=1` (or `--rebuild`) | Full build, ignore the object cache |
| `D2VPK_JOBS=N` | Parallel compilations (default: `nproc-1`) |
| `D2VPK_OUT=/path` | Output directory |
| `D2VPK_PROF=1` | Build **with** `-DPROF_COUNTERS` (default: without) |
| `D2VPK_TAG=<TAG>` | Bench flavour — separate log and save dir, see `hardware-ab-bench` |

The script is incremental: a unit is recompiled only if it or one of its headers
changed, and the link/velf/fself/sfo/vpk tail is skipped entirely when no object
moved. So a "build" that prints almost nothing is a cache hit, not a failure.

## Running it locally

`tools/vita3k_isole.sh` boots the VPK in an isolated Vita3K prefix so a test run
cannot disturb the shared one:

```bash
V3K2ROOT=/tmp/vita3k_isole TITLE=DTWO00077 bash tools/vita3k_isole.sh init
VPK=build-vita/d2vita_boot.vpk ... install
DUR=90 ... launch      # then `cap file.png`, `stop`
... purge              # removes the witness folder from the shared tree
```

Game data must sit in `ux0/data/d2vita` inside that prefix. A
`stat_file: Missing file at ".../ux0/data/d2vita"` in the log is expected when
the archives are not installed — and remember that a *missing* archive shows up
at runtime as an endless "Insert Expansion Disc" retry, not as an error (see
`runtime-debugging`).
