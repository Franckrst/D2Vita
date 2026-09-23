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
every time, it stays on its own dedicated gesture (L + D-pad right; R +
Triangle in the legacy `scheme=mouse`, where it is skill 7 under the aim
scheme).

## Virtual keyboard (L + D-pad right)

| Input | Action |
|---|---|
| D-pad | Navigate keys |
| Cross | Type the selected key |
| Circle | Backspace |
| Start | Enter |
| Select (or the CLOSE key) | Close the keyboard |
| Touch tap on a key | Type it directly |

The virtual keyboard's own rendering (font, layout) comes from the generic
winx86 engine (`src/platform/vita_kb.h`/`vita_kb_font.h`). The trigger is
d2vita-specific: L + D-pad right under the aim scheme, R + Triangle under
`scheme=mouse`.

## Remapping without a rebuild

File `ux0:data/d2vita/controls.txt`, one line per input:

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
