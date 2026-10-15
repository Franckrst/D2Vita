#!/usr/bin/env python3
"""tools/bancs/gen_trone_baal.py — D2SCRIPT of the Baal's Throne bench (Hell).

    gen_trone_baal.py            measurement script
    gen_trone_baal.py explore    same, with captures through the window
    gen_trone_baal.py window     prints WIN_START WIN_END END ARRIVAL_SNAP

Save: tools/bancs/gen_save_orbe.py --trone (level-99 Sorceress, Hell, Act V,
F1 Teleport / F2 Frozen Orb / F3 Energy Shield, 8191 life and mana).

No route: the runtime knob D2_BANC_WARP=131 (native_hooks_solo.cpp) turns the
first weapon swap of a solo game (W) into the game's own level change to the
Throne of Destruction. In Hell the throne's monsters attack at once: Energy
Shield (F3, cast on the character) puts the hits on the 8191 mana, then
Frozen Orb (F2) is held on the right button for the whole window.

Boot: the Act V patrol's menu clicks (800x600 game space), Hell instead of
Normal. In game, 960x544 native.
"""
import os, sys

BOOT = [(700, 400, 308), (1000, 690, 554), (1300, 400, 366)]   # Hell
PLAYERS = int(os.environ.get("TRONE_PLAYERS", "1"))   # >1: "/players N" typed in town
CHAT_AT = 2510
WARP_AT = 2600                 # W (VK 0x57): the warp
SHIELD_AT = 2700               # F3 + right click on the character
ME = (480, 250)
TELE_AT = 2760                 # F1, then one Teleport every TELE_STEP frames
TELE_STEP = 60
TELE = [(640, 110)] * 18 + [(620, 360)] * 2   # up the hall, then down into the room centre
ORB_AT = TELE_AT + 10 + TELE_STEP * len(TELE)   # F2, then right button held
CAST_AT = (560, 300)           # just right of the character: orbs sweep the room
ARRIVAL_SNAP = 2740            # after the shield, before the window
WIN_START = ORB_AT + 300       # waves under way (calibrated by explore)
WIN_END = WIN_START + 6000     # ~4 min at 25 img/s: several waves
CAST_UNTIL = WIN_END + 100

def click(f, x, y):
    return [f"{f}:move:{x}:{y}", f"{f+5}:ldown:{x}:{y}", f"{f+15}:lup:{x}:{y}"]

def script(explore=False):
    ev = []
    for f, x, y in BOOT:
        ev += click(f, x, y)
    ev.append("2499:snap")
    if PLAYERS > 1:
        # monster life as in an N-player game: fewer kills per orb, more
        # monsters alive at once (single player's own command)
        ev += [f"{CHAT_AT}:keydown:13", f"{CHAT_AT+2}:keyup:13"]
        for i, ch in enumerate(f"/players {PLAYERS}"):
            ev.append(f"{CHAT_AT+6+4*i}:chr:{ord(ch)}")
        f = CHAT_AT + 6 + 4 * len(f"/players {PLAYERS}") + 4
        ev.append(f"{f}:chr:13")          # inside the chat box only WM_CHAR acts (console, 15/10)
    ev.append(f"{WARP_AT}:key:87")
    x, y = ME
    ev += [f"{SHIELD_AT}:key:114", f"{SHIELD_AT+5}:move:{x}:{y}",
           f"{SHIELD_AT+10}:rdown:{x}:{y}", f"{SHIELD_AT+25}:rup:{x}:{y}"]
    ev.append(f"{ARRIVAL_SNAP}:snap")
    ev.append(f"{TELE_AT}:key:112")
    for i, (x, y) in enumerate(TELE):
        f = TELE_AT + 10 + TELE_STEP * i
        ev += [f"{f}:move:{x}:{y}", f"{f+5}:rdown:{x}:{y}", f"{f+15}:rup:{x}:{y}"]
        if explore:
            ev.append(f"{f+TELE_STEP-5}:snap")
    if explore:
        ev.append(f"{ORB_AT-3}:snap")
    x, y = CAST_AT
    # F2 twice: a single press was lost once with Teleport held to the end
    # (console, 15/10: the pass went on teleporting instead of casting)
    ev += [f"{ORB_AT}:key:113", f"{ORB_AT+5}:move:{x}:{y}", f"{ORB_AT+8}:key:113",
           f"{ORB_AT+12}:rdown:{x}:{y}"]
    if explore:
        ev += [f"{f}:snap" for f in range(WIN_START, WIN_END + 1, 500)]
    else:
        # what the window held (the scene varies from pass to pass): before,
        # middle, after — one capture inside the window, a few ms in 4 min
        ev += [f"{f}:snap" for f in (WIN_START - 30, (WIN_START + WIN_END) // 2, WIN_END + 30)]
    ev.append(f"{CAST_UNTIL}:rup:{x}:{y}")
    return ",".join(ev)

if __name__ == "__main__":
    a = sys.argv[1] if len(sys.argv) > 1 else ""
    if a == "window":
        print(WIN_START, WIN_END, CAST_UNTIL, ARRIVAL_SNAP)
    else:
        print(script(a == "explore"))
