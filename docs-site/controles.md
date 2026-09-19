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
| Right stick | **Aim**: picks the enemy inside a ±35° cone; for a ground-targeted skill (teleport, meteor…), distance follows tilt |
| Cross / Circle / Square / Triangle | **Skills 1 to 4**, instant cast on the targeted enemy (nearest one if the right stick is idle); hold = repeat |
| R held + faces | Skills 5 to 8 |
| L | **Interact**: nearest item on the ground, else chest / door / NPC, else a basic attack on the target |
| R then L (held) | Alt — ground item labels ; D-pad = move the selection cursor from item to item (nearest to the cursor in that direction), Cross = pick up the selected item (cyan marker) |
| D-pad ↑ / ← / ↓ / → | Belt potions 1 / 2 / 3 / 4 |
| R + D-pad | Potion to the mercenary |
| Start | Escape; R + Start: weapon swap |
| Select | Radial menu (see below); R + Select: Space |
| Touch screen | Absolute cursor; short tap = left click |

A diamond marks the current hostile target (gold once the game confirms the
hover) and a dot marks a ground-targeted skill's impact point while the
right stick is pushed.

**Assigning a skill to a button**: open the skill tree (radial menu), tap
the icon, press the wanted button (R + button for slots 5 to 8). Spending a
point: L or tap.

**Open panels** (inventory, chest, vendor…): both sticks move the cursor, L
or Cross click, Triangle = right click, Square held = Shift (fast move),
Circle closes.

Out of a game (menus, character select), the old mouse scheme stays active.
`scheme=mouse` in `controls.txt` restores it everywhere (0.1.6's mapping:
L/R = clicks, Cross = walk/run, Circle = Shift, Square = Alt, Triangle = W,
R + D-pad = F1-F4).

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
deliberately not included — used too often to justify going through a menu
every time, it stays on its own dedicated gesture (R + Triangle).

## Virtual keyboard (R + Triangle)

| Input | Action |
|---|---|
| D-pad | Navigate keys |
| Cross | Type the selected key |
| Circle | Backspace |
| Start | Enter |
| Select (or the CLOSE key) | Close the keyboard |
| Touch tap on a key | Type it directly |

The virtual keyboard's own rendering (font, layout) comes from the generic
winx86 engine (`src/platform/vita_kb.h`/`vita_kb_font.h`). The trigger (R +
Triangle) is d2vita-specific.

## Remapping without a rebuild

File `ux0:data/d2vita/controls.txt`, one line per input:

```
scheme=aim        # aim (default, aim-assist) | mouse (full legacy scheme)
aim=1              # 0 = no automatic target selection (aim scheme only)
orbit_min=40       # px (at 600 lines), orbit radius at a light stick push
orbit_max=110      # px, full stick
range_min=6        # subtiles, ground-cast distance at a light stick push
range_max=20
cone=35            # aim cone half-angle, degrees
hover_h=28         # default hover height above a unit's feet, px
hud_h=60           # bottom band the cursor never enters, px
deadzone=0.25
sens=10            # free cursor (panels)
```

`D2_PAD=0` (or `1`) in `env.txt` overrides `scheme=` — handy for an A/B test
without touching `controls.txt`.

The following keys, and the `cross=…`/`r+triangle=…` remaps below, only
apply to the legacy `scheme=mouse`:

```
cross=rclick
r+triangle=f5
orbit=70         # direct-movement radius (px)
sens=10          # right stick speed
deadzone=0.25
anchor_y=470     # character's vertical anchor (for a 1000-tall resolution)
```

**Buttons**: `cross`/`croix`, `circle`/`rond`, `square`/`carre`,
`triangle`, `up`, `down`, `left`, `right`, `start`, `select` (prefix with
`r+` for the R layer).

**Actions**: `lclick`, `rclick`, `alt`, `shift`, `tab`/`automap`,
`esc`/`echap`, `inv`, `perso`, `skills`, `quests`, `swap`, `space`, `run`,
`enter`, `pot1`-`pot4`, `f1`-`f8`, `vk:0xNN` (raw key code), `none`.

## Diagnostics

`D2_INPUTLOG=1` in `env.txt` logs the raw button words received — useful
for debugging a remap that doesn't behave as expected. Only enable it for
diagnostics, not for normal play (logging has a cost).

!!! note "Known gotcha"
    If the chat box is open, Esc closes it first (a second press is needed
    to open the menu).
