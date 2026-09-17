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
| **L (held)** | **Left click** |
| **R (held)** | **Right click** (also serves as a combo layer, see below) |
| Cross | R — toggle walk/run |
| Circle | Shift (held) — attack in place / forced cast |
| Square (held) | Alt — show items on the ground |
| Triangle | W — weapon swap |
| D-pad ↑ / ← / ↓ / → | Belt potions 1 / 2 / 3 / 4 |
| Start | Esc (menu / close) |
| **Select** | **Radial menu** (see below) |

## R layer (R held + …)

| Combo | Action |
|---|---|
| R + Triangle | **Virtual keyboard** (open; close with Select) |
| R + D-pad | F1 / F2 / F3 / F4 — quick skills |
| R + Select | Space — close all panels |

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
