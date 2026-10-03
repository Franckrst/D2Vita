# Controller and keyboard

!!! note "Autopilot"
    The VPK (v5+) embeds an autopilot (menus → game) that automatically
    switches off on the player's **first physical action**: touch nothing
    and it drives all the way into play; touch anything and control is
    handed back immediately. It stays continuously active for headless
    testing.

## Default controls

The mapping below is what `controls.txt` remaps, and the `R + …` / `L + …`
layers and the item assist further down apply to it. Since 0.1.18-beta the
built-in default binds the [`aim` action](#aim-assist) to **L**, so the
sticks run in aim mode. This default is a beta, shipped to gather community
feedback. To get the previous scheme back, put `l=lclick` in `controls.txt`
(with no other `aim` binding): see [Aim assist](#aim-assist).

| Vita input | Diablo II action |
|---|---|
| Left stick | **Walk only** in game (never attacks or picks anything up); out of a game, the plain stick behaviour |
| Right stick | **Free cursor** (aiming, menus, panels, HUD), no click by itself |
| Touch screen | Absolute cursor; short tap = left click |
| **L (held)** | **Aim assist** in game (acts on the best target); a plain **left click** in menus, open panels and out of game — remappable, see below (also a combo layer, see below) |
| **R (held)** | **Right click** — remappable, see below (also a combo layer, see below) |
| Cross | R — toggle walk/run |
| Circle | Shift (held) — attack in place / forced cast |
| Square (held) | [Item assist](#item-assist) — show ground items, browse, pick up (`square=alt` gives plain Alt back) |
| Triangle | W — weapon swap |
| D-pad ↑ / ← / ↓ / → | Belt potions 1 / 2 / 3 / 4 |
| Start | Esc (menu / close) |
| **Select** | **Radial menu** — remappable, see below |

Cross alone no longer clicks in menus: **L does**.

L and R being remappable never changes their second role: R still arms
every `R + …` combo below, and L its own `L + …` combo layer, regardless
of what pressing either one *alone* currently does.

## R layer (R held + …) and L layer (L held + …)

| Combo | Action |
|---|---|
| R + Triangle | **Virtual keyboard** (open; close with Select — it also opens by itself on text fields, see below) |
| R + D-pad | F1 / F2 / F3 / F4 — quick skills |
| L + Cross | I — inventory |
| L + Circle | Tab — automap |
| L + Square | O — mercenary inventory |
| R + Select | Space — close all panels (fixed, not remappable) |
| L + Start | Screenshot (fixed, not remappable) |

R + Triangle, R + Select and L + Start always win: they're checked before
any `controls.txt` remap, so binding those same combos to something else
has no effect (the title-screen controls panel, see below, and
`controls.reference.txt` both call this out). Every other `R + …` slot
above starts bound; the mirror `L + …` layer starts with three bindings
(inventory, automap, mercenary inventory) and every other `L + …` slot is
free for `controls.txt` to fill in. Note that L is also the aim button: the
`L + …` combos fire the aim action too while L is down (as `l=lclick` clicked
during L combos before).

## Item assist

On Square by default (`square=items`; `square=alt` restores plain Alt).
Bind the `items` action to any other button if you prefer, and **hold** it:

- the game shows the ground-item name labels, exactly as with Alt;
- the D-pad jumps the cursor from label to label (the nearest one in the
  direction pressed), so the highlighted item is the one the game itself
  reports as hovered;
- **L** (the aim button) or Cross picks the highlighted item up — with
  `aim` on L, hold Square and press L, no need to reach for Cross. The
  cursor goes onto the label, and the click is only sent once the game has reported the item as hovered:
  clicking earlier is read by the game as "walk there";
- with nothing on the ground, the D-pad, Cross and L keep their normal
  bindings (potions, walk/run, aim), so holding the button never eats a potion;
- the right stick still moves the cursor by hand — landing on a label
  focuses it; the left stick (direct movement) cancels any pick-up.

`alt` stays available as a plain Alt for anyone who prefers it. The
buttons `items` can be bound to are the same as for any action: face
buttons, D-pad, `r+`/`l+` layers, `l=`, `r=`, `select=` (avoid binding it
to Cross or the D-pad themselves, since it takes those over while held).

Validated on the host (logic tests); on a console only through the maintainer's
own play so far.

## Aim assist

On by default, bound to **L** (`l=aim`). It costs frame time: the camera and
unit hooks that feed it are installed whenever the bindings carry `aim`, and
their cost is not benchmarked yet. Bind `aim` to another button — for example
`cross=aim` — and **hold** it. To opt out, put `l=lclick` in `controls.txt`
(with no other `aim` binding): the hooks are then not installed and the
sticks go back to the plain scheme below.

While `aim` is bound (the default), the **sticks** run in aim mode, in game:

| Vita input | With `aim` bound |
|---|---|
| Left stick | **Move only**: the character walks in the stick's direction (radius follows tilt), never attacking, talking or picking anything up by accident; release = hard stop |
| Right stick | **Free cursor**, as on PC: it clicks nothing by itself, and it reaches the HUD (belt, skill buttons) |
| `aim` button (held) | Acts on the best target: your own corpse within reach, else whatever the cursor sits on, else the enemy closest to the axis inside a ±35° cone around the cursor (never one outside it), else the nearest chest / door / portal / NPC (or, outside a town, the nearest enemy), else a level exit, else your own corpse anywhere on screen, else a plain left click at the cursor (held with the button; the right stick keeps aiming) |

Without `aim` bound (opt-out), the sticks keep the earlier plain behaviour
(left stick = direct movement: the cursor orbits the character with a held
left click; right stick = free mouse, no click) and L is a plain left click.
Out of a game (menus, character select) the plain stick behaviour always
applies, and the `aim` button is a plain **left click** there — which is why
L now clicks in the menus.

A diamond marks the current hostile target (gold once the game confirms the
hover). It can be hidden: `diamond=off` in `controls.txt` (the aim itself is
unchanged; `diamond=on` is the default). Assisted aiming only ever borrows the cursor: it moves onto the
target, holds the click once the game reports the hover, and puts the cursor
back where you left it on release — as does the stop click that ends a walk.
**The cursor beats everything**: whatever it sits on is what `aim` acts on,
ahead of any cone. Distances are measured in the world, not in screen pixels,
and a unit the game refuses to hover — a critter, a vulture still in the air —
is dropped after a moment instead of capturing every press (up to 32 such
units are remembered, so a pack of critters no longer hides the monster
behind it).

**Open panels** (inventory, chest, vendor, NPC menus, Larzuk's add-socket
window…): both sticks move the cursor and `aim` is a left click. The list of
panels is read from the game's UI flags; the add-socket window (index `0x0E`)
was added in 0.1.18-beta and has only had a first console test.

Everything else — potions, skills, Alt, item assist, the radial menu — stays
with the ordinary `controls.txt` mapping, and the two combine: the item
assist's cyan marker shows the focused ground item whether or not `aim` is
bound.

Tuning keys (all optional, `controls.txt`):

```
orbit_min=40       # px (at 600 lines), walk-ring radius at a light stick push
orbit_max=110      # px, full stick
cone=35            # aim cone half-angle around the cursor, degrees
hover_h=28         # default hover height above a unit's feet, px
diamond=on          # off = hide the aim marker (the aim still works)
hud_h=60           # bottom band ASSISTED clicks never enter, px (the cursor
                   # you drive does go there, or the belt would be unreachable)
reach=300          # how far `aim` reaches for a chest/door/NPC, world units
hostile_reach=800  # how far `aim` reaches for a monster (in or out of the cone), world units
```

**Not carried over from the earlier `scheme=aim` experiment**: skill slots that
snap their cast onto a target (and the `slot1..7` = hostile/ground/corpse
choice), dedicated panel-mode face buttons, L held = stand still, and the
D-pad loot browse with native labels (the item assist replaces it). Those live
on in the git history and the `manette/pad-core-port` branch. `scheme=` and
`D2_PAD` no longer exist.

Validation: host logic tests (pad_core), plus the maintainer's hands-on
checks on a console (NPC menus, touch, title-screen menus). The frame-time
cost of the hooks is **not** measured, and the default change is not yet
tested by the community.

## Radial menu (Select)

Pressing **Select** immediately opens a 7-sector menu centered on screen.
The **left stick** points at a sector (it lights up gold); **releasing
Select** confirms whichever sector is pointed at that exact instant.
Bringing the stick back to center before releasing Select **cancels** (no
sector is then selected, even if one had been hovered just before).

| Sector | D2 action |
|---|---|
| Skills (top) | T — skill tree |
| Quests | Q — quest log |
| Map | Tab — automap |
| Inventory | I — inventory |
| Chat | Enter — chat line |
| Party | P — party screen |
| Character | C — character sheet |

This menu replaces the old dedicated shortcuts for each of these actions
(character, skills, quests, automap, inventory): one gesture for all
seven, instead of seven combinations to remember. The virtual keyboard is
deliberately not included: it opens by itself whenever a text field takes
focus, and otherwise keeps its own dedicated gesture — R + Triangle.

## Virtual keyboard (R + Triangle)

The keyboard **opens by itself** when a text field takes focus — character
name, Battle.net account and password, game name and password. Closing stays
manual (Select or the FERMER key — Start/Enter validates the field but
leaves the keyboard open), and a closed keyboard does not reopen until
another field takes focus (or the same one loses and regains it). R + Triangle
still opens it at any time. `D2_KBAUTO=0` in `ux0:data/d2vita/env.txt` turns
the automatic opening off. The keyboard draws at 80% opacity by default — the
character/menu stays faintly visible behind it — adjustable with
`D2_KBALPHA=0-100` (100 = the old fully opaque look). Below 100, blending is
done on the GPU (a separate textured layer, composed alongside the radial
menu) whenever that path is armed; if it isn't, the keyboard falls back to
drawing translucency on the CPU, which reads the screen to blend and costs
noticeably more per frame — opaque (100) is always the fast path either way.
The runtime reads the game's own "focused control"
state to know this (read-only, the control's header only — never the text).
Each transition is logged in `boot_progress.txt` as `clavier: focus champ ON`
or `clavier: focus champ OFF` (capped at 64 lines per session). In-game chat
is not covered yet (phase 2).

| Input | Action |
|---|---|
| D-pad | Navigate keys |
| Cross | Type the selected key |
| Circle | Backspace |
| Start | Enter |
| Select (or the FERMER key) | Close the keyboard |
| Touch tap on a key | Type it directly |

The virtual keyboard's own rendering (font, layout) comes from the generic
winx86 engine (`src/platform/vita_kb.h`/`vita_kb_font.h`). The trigger is
d2vita-specific: R + Triangle.

## Controls panel (title screen)

A small "Controls" tab sits in the left letterbox band of the title
screen. Tap it to open a full-screen editor with two tabs:

- **Buttons** — a grid of every button (L, R, Select, face buttons, D-pad,
  Start) by layer: plain, with R held, with L held. Each cell is the action
  that combination fires. The three fixed ones (R+Select = Space,
  R+Triangle = keyboard, L+Start = screenshot) are shown greyed out.
- **Tuning** — the aim cone, monster and object reach, hover height, HUD
  band, aim marker, cursor speed, deadzone and walk-ring sizes, each with
  its default and range.

Colours: white = default, gold = customised, green = changed and not saved
yet. Changes apply at once; closing the panel writes **only what you
changed** into `ux0:data/d2vita/controls.txt` (your comments and other
lines are kept; an old alias such as `croix=` is replaced, not duplicated).

| Input | Effect |
|---|---|
| D-pad | Buttons: move in the grid. Tuning: Up/Down pick, Left/Right change (hold to repeat) |
| Cross / Square | Next / previous action (or value +/−), hold to repeat |
| Triangle | Put the focused item back to its default |
| L / R | Switch tab |
| Circle / Start | Save and close |
| Touch | Tap a cell to focus it, tap again to cycle; the `<` `>` ends of a cell step it (hold to repeat); tap a tab or the footer buttons |

Binding `aim` to a button when none was bound at launch needs a restart of
the game (the aim hooks are armed at boot); the panel says so.

## Remapping without a rebuild

File `ux0:data/d2vita/controls.txt`. A fully-commented reference copy is
seeded there automatically the very first time the game boots (never
overwrites a file you already have — delete yours and it comes back next
boot); the walkthrough below covers the same ground.

```
cross=rclick
r+triangle=f5
l+circle=perso   # L layer (defaults: l+cross=inv, l+circle=automap, l+square=vk:0x4F)
l=lclick         # L's OWN action, held alone (default: aim; lclick = opt out of aim assist)
r=none           # R's OWN action, held alone (default: rclick) — "none" disables it
select=inv       # Select's OWN action, pressed alone (default: opens the radial menu)
cross=aim        # any button, layer or l=/r=/select= can carry `aim` (see Aim assist)
orbit=70         # direct-movement radius (px), plain mode
sens=18          # right stick speed (default 18)
deadzone=0.15    # default 0.15
anchor_y=470     # character's vertical anchor (for a 1000-tall resolution)
diamond=off      # hide the aim assist's diamond (default on)
```

**Accepted ranges** for the numeric keys — a value outside it is refused (the
default stays) and reported in `boot_progress.txt`: `orbit` 10–400, `sens`
1–100, `deadzone` 0–0.9 (a fraction: `0.15`, not `15`), `anchor_y` 100–900,
`orbit_min`/`orbit_max` 5–400, `cone` 5–90, `hover_h` 0–150, `hud_h` 0–300,
`reach` 50–2000, `hostile_reach` 100–2000. In particular `orbit=1000`, which some older builds used,
is refused: it would push the walk point off the screen and nothing could be
controlled any more. A UTF-8 byte-order mark at the start of the file (added
by some Windows editors) is ignored.

**Buttons**: `cross`/`croix`, `circle`/`rond`, `square`/`carre`,
`triangle`, `up`, `down`, `left`, `right`, `start` (prefix with `r+` for
the R layer, `l+` for the L layer — never both on the same line). `l=`,
`r=` and `select=` are separate top-level keys (no button prefix): they
set what L, R and Select do *by themselves*, not a combo.

**Actions**: `lclick`, `rclick`, `alt`, `items`, `aim`, `shift`, `tab`/`automap`,
`esc`/`echap`, `inv`, `perso`, `skills`, `quests`, `swap`, `space`, `run`,
`enter`, `pot1`-`pot4`, `f1`-`f8`, `vk:0xNN` (raw key code), `none`.

A line the game doesn't recognise — an unknown key or button, an unknown
action, a number outside its range, or (`r+select=`/`l+select=`) a combo Select doesn't have — is
never applied, and now says so: check `boot_progress.txt` for
`controls.txt ignore "..."` lines, one per rejected line (capped at 8).
Before this, a bad line simply did nothing with no indication why —
the most common case being a `controls.txt` copied from a build with a
different set of keys than the one actually running.

**Not yet possible**: a single button driving more than one action in
sequence (e.g. "open the map and start running").

## Diagnostics

`D2_INPUTLOG=1` in `env.txt` logs the raw button words received — useful
for debugging a remap that doesn't behave as expected. Only enable it for
diagnostics, not for normal play (logging has a cost).

!!! note "Known gotcha"
    If the chat box is open, Esc closes it first (a second press is needed
    to open the menu).
