# Controller and keyboard

!!! note "Autopilot"
    The VPK (v5+) embeds an autopilot (menus → game) that automatically
    switches off on the player's **first physical action**: touch nothing
    and it drives all the way into play; touch anything and control is
    handed back immediately. It stays continuously active for headless
    testing.

## In game ("aim-assist" scheme, default since the manette v2 snapshot)

| Vita input | Diablo II action |
|---|---|
| Left stick | **Move only**: the character walks in the stick's direction (radius follows tilt), never attacking, talking or picking anything up by accident; release = hard stop |
| Right stick | **Free cursor**, as on PC: it clicks nothing by itself, and it reaches the HUD (belt, skill buttons) |
| Cross | **The action button**: the browsed ground item, else whatever you are pointing at, else a chest / door / portal / NPC within reach, else the nearest enemy |
| Circle / Square / Triangle | **Skills 1 to 3**. The cast snaps to the enemy inside a ±35° cone around the cursor (nearest one if the cursor sits on the character), then hands the cursor straight back; hold = repeat |
| R held + faces | Skills 4 to 7 |
| L held | **Stand still** (Shift): cast and attack without moving |
| L, short press | Toggle run/walk |
| L + D-pad → | Virtual keyboard |
| L + D-pad ↓ | **Right click** at the cursor — wherever it is, whatever is under it |
| L + D-pad ↑ | **Weapon swap** |
| R then L (held) | Alt — ground item labels ; D-pad = move the selection cursor from item to item (nearest to the cursor in that direction), Cross = pick up the selected item (cyan marker) |
| D-pad ↑ / ← / ↓ / → | Belt potions 1 / 2 / 3 / 4 |
| R + D-pad | Potion to the mercenary |
| Start | Escape |
| Select | Radial menu (see below); R + Select: Space |
| Touch screen | Absolute cursor; short tap = left click |

A diamond marks the current hostile target (gold once the game confirms the
hover). Assisted aiming only ever borrows the cursor: a cast moves it onto
its target and puts it back where you left it on release, and so does the
stop click that ends a walk. With no enemy in the cone, a skill is cast
exactly where the cursor points — which is how a ground-targeted skill
(teleport, meteor…) is aimed. Point the cursor at the character's own feet
and it goes out along the direction you last walked in instead.

`aim=0` in `controls.txt` turns the snap off and leaves a plain cursor.

**The cursor beats everything.** Whatever it sits on is what Cross acts on,
ahead of any cone. Distances are measured in the world, not in screen pixels:
the projection makes one pixel down worth two across, so a unit north of you
is not the near one it looks. And a unit the game refuses to hover — a
critter, a vulture still in the air — is dropped after a moment instead of
capturing every press.

**Assigning a skill to a button**: open the skill tree (radial menu), hover
the icon, then press the very gesture that will cast it — Circle for slot 1,
R + Cross for slot 4, and so on. **Cross clicks** there as everywhere else,
so it is still what spends a point.

**Open panels** (inventory, chest, vendor…): both sticks move the cursor, L
or Cross click, Triangle = right click, Square held = Shift (fast move),
Circle closes.

**What a slot aims at.** By default a skill looks for a live enemy. Two
families need something else, and `controls.txt` says so per slot:
`ground` never snaps and lands where the cursor points (teleport, meteor,
blizzard — snapping teleport onto a monster puts you *on* it), and `corpse`
looks for a dead one instead (corpse explosion, revive, raise skeleton,
which the live-enemy filter excludes by construction).

Out of a game (menus, character select), the old mouse scheme stays active.
`scheme=mouse` in `controls.txt` restores it everywhere (0.1.6's mapping:
L/R = clicks, Cross = walk/run, Circle = Shift, Square = Alt, Triangle = W,
R + D-pad = F1-F4).

## Mouse scheme (`scheme=mouse`, and outside a game)

This is the scheme that `controls.txt` remaps, the `R + …` / `L + …` layers and
the item assist below apply to. With the aim-assist scheme above, an in-game
button press is handled by that scheme first, so those remaps do not reach it.

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
focus, and otherwise keeps its own dedicated gesture — L + D-pad right under
the aim scheme (skill 7 there), R + Triangle under the legacy `scheme=mouse`.

## Virtual keyboard (L + D-pad right)

The keyboard **opens by itself** when a text field takes focus — character
name, Battle.net account and password, game name and password. Closing stays
manual (Select or the FERMER key — Start/Enter validates the field but
leaves the keyboard open), and a closed keyboard does not reopen until
another field takes focus (or the same one loses and regains it). L + D-pad
right still opens it at any time under the aim scheme (R + Triangle under
`scheme=mouse`). `D2_KBAUTO=0` in `ux0:data/d2vita/env.txt` turns
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
d2vita-specific: L + D-pad right under the aim scheme, R + Triangle under
`scheme=mouse`.

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
scheme=aim        # aim (default, aim-assist) | mouse (full legacy scheme)
aim=1              # 0 = no automatic target selection (aim scheme only)
orbit_min=40       # px (at 600 lines), orbit radius at a light stick push
orbit_max=110      # px, full stick
range_min=6        # subtiles, ground-cast distance when the cursor is parked on the character
range_max=20
cone=35            # aim cone half-angle around the cursor, degrees
hover_h=28         # default hover height above a unit's feet, px
hud_h=60           # bottom band ASSISTED clicks never enter, px (the cursor
                   # you drive does go there, or the belt would be unreachable)
slot1=hostile      # slot1..slot7: hostile (default) | ground | corpse
slot3=ground       #   e.g. teleport on slot 3, corpse explosion on slot 5
slot5=corpse
deadzone=0.25
sens=10            # free cursor speed (right stick in game, both sticks in panels)
```

`D2_PAD=0` (or `1`) in `env.txt` overrides `scheme=` — handy for an A/B test
without touching `controls.txt`.

The following keys, and the `cross=…`/`r+triangle=…` remaps below, only
apply to the legacy `scheme=mouse`:

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
