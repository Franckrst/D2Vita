# Controller and keyboard

!!! note "Autopilot"
    The VPK (v5+) embeds an autopilot (menus → game) that automatically
    switches off on the player's **first physical action**: touch nothing
    and it drives all the way into play; touch anything and control is
    handed back immediately. It stays continuously active for headless
    testing.

## In game

| Vita input | Diablo II action |
|---|---|
| Left stick | **Direct movement** (cursor orbits the character + held left click) |
| Right stick | Free mouse, no click (aiming, menus, hover) |
| Touch screen | Absolute cursor; short tap = left click |
| **L (held)** | **Left click** — remappable, see below (also a combo layer, see below) |
| **R (held)** | **Right click** — remappable, see below (also a combo layer, see below) |
| Cross | R — toggle walk/run |
| Circle | Shift (held) — attack in place / forced cast |
| Square (held) | Alt — show items on the ground (or, with `square=items`, [item assist](#item-assist)) |
| Triangle | W — weapon swap |
| D-pad ↑ / ← / ↓ / → | Belt potions 1 / 2 / 3 / 4 |
| Start | Esc (menu / close) |
| **Select** | **Radial menu** — remappable, see below |

L and R being remappable never changes their second role: R still arms
every `R + …` combo below, and L its own `L + …` combo layer, regardless
of what pressing either one *alone* currently does.

## R layer (R held + …) and L layer (L held + …)

| Combo | Action |
|---|---|
| R + Triangle | **Virtual keyboard** (open; close with Select — it also opens by itself on text fields, see below) |
| R + D-pad | F1 / F2 / F3 / F4 — quick skills |
| R + Select | Space — close all panels (fixed, not remappable) |
| L + Start | Screenshot (fixed, not remappable) |

R + Triangle, R + Select and L + Start always win: they're checked before
any `controls.txt` remap, so binding those same combos to something else
has no effect (the title-screen controls panel, see below, and
`controls.reference.txt` both call this out). Every other `R + …` slot
above starts bound; the mirror `L + …` layer starts **entirely free** —
nothing happens by default, it's there for `controls.txt` to fill in.

## Item assist

Off by default. Bind the `items` action to any button (`square=items` is
the natural one — it replaces plain Alt on Square) and **hold** it:

- the game shows the ground-item name labels, exactly as with Alt;
- the D-pad jumps the cursor from label to label (the nearest one in the
  direction pressed), so the highlighted item is the one the game itself
  reports as hovered;
- Cross picks the highlighted item up. The cursor goes onto the label, and
  the click is only sent once the game has reported the item as hovered:
  clicking earlier is read by the game as "walk there";
- with nothing on the ground, the D-pad and Cross keep their normal
  bindings (potions, walk/run), so holding the button never eats a potion;
- the right stick still moves the cursor by hand — landing on a label
  focuses it; the left stick (direct movement) cancels any pick-up.

`alt` stays available as a plain Alt for anyone who prefers it. The
buttons `items` can be bound to are the same as for any action: face
buttons, D-pad, `r+`/`l+` layers, `l=`, `r=`, `select=` (avoid binding it
to Cross or the D-pad themselves, since it takes those over while held).

Validated on the host only (logic tests); not yet on a console.

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
focus, and keeps its own dedicated gesture (R + Triangle) for everything
else.

## Virtual keyboard

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
winx86 engine (`src/platform/vita_kb.h`/`vita_kb_font.h`). The trigger (R +
Triangle) is d2vita-specific.

## Controls-help overlay (title screen)

A small "Controls" tab sits in the left letterbox band of the title
screen. Tap or click it to open a full-screen panel listing every
binding **as currently in effect** (defaults plus whatever
`controls.txt` changed) — D-pad or drag to scroll, Circle/Start to
close. It reads `controls.txt` back rather than repeating this page, so
it never goes stale relative to your own file.

## Remapping without a rebuild

File `ux0:data/d2vita/controls.txt`. A fully-commented reference copy is
seeded there automatically the very first time the game boots (never
overwrites a file you already have — delete yours and it comes back next
boot); the walkthrough below covers the same ground.

```
cross=rclick
r+triangle=f5
l+circle=perso   # L layer: entirely free by default, yours to fill in
l=rclick         # L's OWN action, held alone (default: lclick)
r=none           # R's OWN action, held alone (default: rclick) — "none" disables it
select=inv       # Select's OWN action, pressed alone (default: opens the radial menu)
orbit=70         # direct-movement radius (px)
sens=10          # right stick speed
deadzone=0.25
anchor_y=470     # character's vertical anchor (for a 1000-tall resolution)
```

**Buttons**: `cross`/`croix`, `circle`/`rond`, `square`/`carre`,
`triangle`, `up`, `down`, `left`, `right`, `start` (prefix with `r+` for
the R layer, `l+` for the L layer — never both on the same line). `l=`,
`r=` and `select=` are separate top-level keys (no button prefix): they
set what L, R and Select do *by themselves*, not a combo.

**Actions**: `lclick`, `rclick`, `alt`, `items`, `shift`, `tab`/`automap`,
`esc`/`echap`, `inv`, `perso`, `skills`, `quests`, `swap`, `space`, `run`,
`enter`, `pot1`-`pot4`, `f1`-`f8`, `vk:0xNN` (raw key code), `none`.

A line the game doesn't recognise — an unknown button, an unknown
action, or (`r+select=`/`l+select=`) a combo Select doesn't have — is
never applied, and now says so: check `boot_progress.txt` for
`controls.txt ignore "..."` lines, one per rejected line (capped at 8).
Before this, a bad line simply did nothing with no indication why —
the most common case being a `controls.txt` copied from a build with a
different set of keys than the one actually running.

**Not yet possible**: a single button driving more than one action in
sequence (e.g. "open the map and start running"), and a left stick that
moves without ever holding a click. Both turn out to need the same
per-frame reading of monster/item positions that a full aim-assist
scheme does — they're not simple additions to this file, see the
project's open work on an aim-assist controller mode.

## Diagnostics

`D2_INPUTLOG=1` in `env.txt` logs the raw button words received — useful
for debugging a remap that doesn't behave as expected. Only enable it for
diagnostics, not for normal play (logging has a cost).

!!! note "Known gotcha"
    If the chat box is open, Esc closes it first (a second press is needed
    to open the menu).
