#!/usr/bin/env python3
"""tools/bancs/gen_orbe_givre.py — D2SCRIPT of the Frozen Orb bench.

Prints one line: the D2SCRIPT value used by tools/bancs/orbe_givre.sh.

    gen_orbe_givre.py            measurement script
    gen_orbe_givre.py explore    same route, with captures at every step
                                 (to calibrate the coordinates below; a GXM
                                 capture stalls the game ~2.8 s, so never in
                                 a measured pass)

Save: tools/bancs/gen_save_orbe.py (level-99 Sorceress, Frozen Orb on the
right button, every waypoint), loaded in Normal, Act V: the game starts at
the Harrogath spawn.

Boot: the same menu clicks as the Act V patrol (gen_patrouille_acte5.py),
800x600 game space: Single Player, OK on character select (one character
in the save dir), Normal.
In game (960x544 native, clicks 1:1 with the captures):
    ROUTE      recorded by hand (see below): stairs, waypoint, destination
    CAST_AT    where the right button is held: Frozen Orb is cast again and
               again for as long as it stays down (D2 repeats a held skill)
Captures (measurement script): 2499 (spawn, Act V check) and ARRIVAL_SNAP
(destination check), both BEFORE the window.

Measurement window: frames WIN_START .. WIN_END (printed by `window`).
"""
import sys

BOOT = [(700, 400, 308), (1000, 690, 554), (1300, 400, 279)]
# The in-game route, recorded by hand on the console (D2_RECORD=1, touch
# screen, 05/10/2026), frames shifted so that its first click lands at 2600
# like the Act V patrol: down the stairs, onto the waypoint, then the
# destination row in the waypoint panel (the panel opens on the Act V tab).
REC_T0 = 2335
ROUTE = [(2335, 309, 447), (2375, 235, 455), (2421, 217, 461),   # stairs
         (2517, 356, 385), (2553, 344, 376),                     # waypoint
         (2933, 191, 149)]                                       # destination row
SHIFT = 2600 - REC_T0
ARRIVAL_SNAP = 3077 + SHIFT - 20  # recorded: first orb cast at 3077
CAST_AT = (603, 166)            # right button held (recorded first cast point)
CAST_FROM = ARRIVAL_SNAP + 40
WIN_START = CAST_FROM + 300     # orbs on screen, steady state
WIN_END = WIN_START + 3000      # ~2 min at 25 img/s of continuous casting
CAST_UNTIL = WIN_END + 100

def click(f, x, y):
    return [f"{f}:move:{x}:{y}", f"{f+5}:ldown:{x}:{y}", f"{f+15}:lup:{x}:{y}"]

def script(explore=False):
    ev = ["300:activate"]
    for f, x, y in BOOT:
        ev += click(f, x, y)
    ev.append("2499:snap")
    for f, x, y in ROUTE:
        if explore and f == ROUTE[-1][0]:
            ev.append(f"{f+SHIFT-20}:snap")      # waypoint panel open?
        ev += click(f + SHIFT, x, y)
    ev.append(f"{ARRIVAL_SNAP}:snap")
    x, y = CAST_AT
    ev += [f"{CAST_FROM}:move:{x}:{y}", f"{CAST_FROM+5}:rdown:{x}:{y}"]
    if explore:
        ev += [f"{f}:snap" for f in (WIN_START, (WIN_START + WIN_END) // 2)]
    ev.append(f"{CAST_UNTIL}:rup:{x}:{y}")
    return ",".join(ev)

if __name__ == "__main__":
    a = sys.argv[1] if len(sys.argv) > 1 else ""
    if a == "window":
        print(WIN_START, WIN_END, CAST_UNTIL, ARRIVAL_SNAP)
    else:
        print(script(a == "explore"))
