#!/usr/bin/env python3
"""tools/bancs/gen_patrouille_acte5.py — D2SCRIPT of the Act V patrol (Harrogath).

Prints one line: the D2SCRIPT value used by tools/bancs/patrouille_acte5.sh.

Boot (coordinates found on console from GXM captures, 26/09/2026): the menus
are drawn at 800x600, so their clicks are in 800x600 game space.
    700  Single Player          (400,308)
    1000 OK on character select (690,554)  -- the bench save holds ONE character
    1300 Normal in "Select Difficulty" (400,279)  -- Normal is the difficulty
         saved at Act V; Nightmare/Hell are unlocked but sit at Act I
    2499 snap: must match the Harrogath spawn reference (Act V check)
In game the resolution is 960x544 native: clicks are 1:1 with the captures.

Patrol: one click from the spawn A down the stairs to the waypoint B, then
10 laps B -> E (huts) -> B, one click every 150 frames. The route never comes
back to the spawn: the stash stands there, and a drifting return click opened
the stash + inventory panels (lap 6 in one pass, lap 3 in another — console,
26/09/2026), a different scene for the rest of the pass.
Each click is move / ldown (+5) / lup (+15): a click whose down and up land
in the same frame is not taken.
NO capture inside the window: a GXM capture stalls the game ~2.8 s. Only two:
2499 (Act V check against the spawn reference) and 5760 (end of route, checked
against the reference pass: a pass whose route diverged is not measured).

Measurement window: frames 2800 .. 5700.
"""
BOOT = [(700, 400, 308), (1000, 690, 554), (1300, 400, 279)]
TO_B = (330, 430)                       # A -> B, once
LEGS = [(650, 420), (310, 60)]          # B -> E, E -> B
START, LEG, LAPS = 2600, 150, 10
END_SNAP = 5760

def click(f, x, y):
    return [f"{f}:move:{x}:{y}", f"{f+5}:ldown:{x}:{y}", f"{f+15}:lup:{x}:{y}"]

ev = ["300:activate"]
for f, x, y in BOOT:
    ev += click(f, x, y)
ev.append("2499:snap")
ev += click(START, *TO_B)
f = START + LEG
for lap in range(LAPS):
    for x, y in LEGS:
        ev += click(f, x, y)
        f += LEG
ev.append(f"{END_SNAP}:snap")
print(",".join(ev))
