---
name: hardware-ab-bench
description: Use when running an A/B perf measurement on the real Vita for THIS port — driving the console over VitaCompanion, swapping eboots per flavour without reinstalling, the D2VITA_PATROL scripted route and its measurement window, the save-dir files that hang character select, and the stall windows that invalidate a run.
---

# Hardware A/B bench — driving this port's console

**The protocol lives in the engine**, not here:
`third_party/winx86/.claude/skills/hardware-ab-bench/SKILL.md` — which bench
applies to which scheduler, the determinism gate, the auto-normalized metric,
the validity guard, measuring movement rather than a still frame, and why
absolute values do not travel between sessions.

This file holds only the concrete piloting of *this* port.

## The console

Console at its LAN IP (`$IP` below). Command port **1338**, VitaShell FTP
**1337**.

```bash
cmd(){ timeout 10 bash -c "exec 3<>/dev/tcp/$IP/1338; printf '$1\n' >&3; timeout 3 head -c 120 <&3" 2>/dev/null; }
cmd destroy          # kill the running app
cmd "nosleep on"     # REQUIRED — sleep mid-run wrecks the timing window
cmd "launch DTWO00001"
```

Verbs: `help, destroy, launch, kill, nosleep, press, reboot, release, screen,
version, wait`. **There is no remote VPK install** — hence the eboot swap below.

## One bubble, many flavours

Do **not** install one bubble per flavour. Install one title (`DTWO00001`),
then swap `ux0:app/DTWO00001/eboot.bin` by FTP between runs:

```bash
unzip -q build-vita/d2vita_$TAG.vpk -d /tmp/ex
curl -s -T /tmp/ex/eboot.bin "ftp://$IP:1337/ux0:/app/DTWO00001/eboot.bin"
```

Each flavour is built with `D2VPK_TAG=<TAG>` so its log
(`boot_progress_<TAG>.txt`) and save dir (`save_<TAG>`) stay separate — no
cross-contamination even though they share a bubble.

Pre-create `save_<TAG>/` with `VITA.key`, `default.key` and `registry.txt`
copied from an existing save dir. To A/B a single knob without rebuilding, reuse
the same eboot and change `env.txt` (see the `runtime-debugging` skill).

## The scripted patrol

`D2VITA_PATROL` in `src/platform/vita_present.cpp`: 8 waypoints looping, one
click every 120 frames, from frame 3600. **Metric = fps over frames
3600–8400.**

Do **not** use "time to reach frame N" — the game is real-time driven, so that
measure breaks in-game.

> The patrol is guarded by `#ifdef D2VPK_TAG`. It must never ship in a play
> build — an unconditional patrol takes over the first 12000 frames of
> anyone's game.

## FBHASH ne valide PAS tout — il empreinte le DIB, pas l'écran

`FBHASH=N` est l'oracle d'exactitude au pixel de ce portage, et il est utilisé
partout. Mais il est calculé dans `tools/rt_boot.cpp`, **au site du blit**, sur
les octets du DIB que le jeu vient d'écrire — c'est-à-dire **en amont de la
couche de présentation**.

Donc il est aveugle, par construction, à tout ce qui se passe après :
`do_scale_and_flip` (expansion de palette, mise à l'échelle, cadrage), les
incrustations, le `sceDisplaySetFrameBuf`. Pour ces chemins-là il répondra
« identique » quoi que tu casses — y compris un écran entièrement noir.

> Piège déjà rencontré en validant une réécriture du scaler de présentation :
> réutiliser FBHASH était le réflexe évident, et il aurait signé n'importe quoi.

**Ce qui marche pour la couche de présentation** : `D2_PRESENT_CMP=1` fait
tourner les deux chemins sur **la même entrée dans le même appel** et compare
leurs octets de sortie, sur console, sur les vraies images. Comparer deux runs
ne marche pas — le jeu est piloté en temps réel, deux passes ne sont jamais à la
même image.

Et prouve que le comparateur coupe avant de le croire : `D2_PRESENT_CMPTEST=1`
corrompt un pixel de la sortie candidate. Résultat attendu : 100 % des images
divergentes, 1 pixel chacune. Un oracle qui n'a jamais échoué ne prouve rien.

**Le mode de rendu décide quel chemin est exercé.** Sous `-3dfx` (le défaut du
banc, `GLIDE=1`) l'image part par le GPU et `do_scale_and_flip` n'est
**jamais** appelé — un oracle de présentation y compte zéro image et a l'air de
passer. Mettre `GLIDE=0` pour exercer le chemin logiciel.

## Two things that silently kill a run

**The boot script lands wrong.** A single scripted `Enter` sometimes lands on
the main menu where it means "Exit" → `CLEAN EXIT (game path, frame=2608)`.
Both mitigations are needed: five spaced `Enter`s in the boot script (harmless
if the screen isn't there), *and* a retry inside the driver that relaunches the
same flavour under identical conditions rather than recording a dead run. About
2 runs in 8 need it.

**A stray file in the save dir.** A `._VITA.d2s` (macOS metadata) is counted as
a character and hangs the script at character select.

## Reading the result

```python
# fps over the patrol window, from the watchdog 'alive:' beats
w=[(t,f) for t,f in rows if 3600<=f<=8400]
fps=(w[-1][1]-w[0][1])/(w[-1][0]-w[0][0])
```

On this bench the dispersion between the two runs should be **under 0,5 %**; a
flavour that disperses more is not trustworthy. Report it next to the mean.

Before trusting a window, check for sleep stalls: if the console slept, time
advances while frames do not. Stalls at **112–142 s** are the level load and sit
*outside* the window — those are fine.

A known-good negative control for this port: `SANSBLEND` cuts a shim whose
counter is `blend=0` in every run. It measured −0,15 %, inside the noise. A
bench that finds an effect where none can exist is lying about its other numbers
too.
