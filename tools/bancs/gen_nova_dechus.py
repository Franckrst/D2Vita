#!/usr/bin/env python3
"""tools/bancs/gen_nova_dechus.py — Frost Nova among Fallen packs (sound bench),
and the Maggot Lair exploration (BANC=nova / BANC=asticots of orbe_givre.sh).

    gen_nova_dechus.py                 measurement script (Frost Nova loop)
    gen_nova_dechus.py explore         same, with captures through the window
    gen_nova_dechus.py window          prints WIN_START WIN_END END ARRIVAL_SNAP
    gen_nova_dechus.py save <Trone.d2s> <out_dir> <NAME> <ACT>
        derive the bench save from the Throne bench save (gen_save_orbe.py
        --trone): name, Hell act (0 = Act I, 1 = Act II), F2 = Frost Nova
        (skill 44, level 20 in that save) on the right button. F1 Teleport
        and F3 Energy Shield stay. Player-derived data, never committed.

Players' report (10/2026): sound crackles / freezes when Frost Nova hits a
pack of 10+ Fallen, Act I. No route: D2_BANC_WARP=<level> (native_hooks_solo
.cpp) sends the character, on its first weapon swap (W), to the Den of Evil
(8, Hell: Fallen packs and their shamans). Energy Shield (F3) on the
character, then cycles of one Teleport (F1) in a fixed direction followed by
Frost Nova (F2) held on the right button, cast at the character.
NOVA_ACT / NOVA_LEVEL / NOVA_CYCLE override act, level and the cycle length.
"""
import os, struct, sys

BOOT = [(700, 400, 308), (1000, 690, 554), (1300, 400, 366)]   # Hell (same clicks as the Throne bench)
WARP_AT = 2600
SHIELD_AT = 2700
ME = (480, 250)
ARRIVAL_SNAP = 2740
CYCLE = int(os.environ.get("NOVA_CYCLE", "240"))   # frames: 1 teleport + Nova held
DIRS = [(700, 130), (800, 300), (640, 430), (300, 420), (170, 260), (300, 120)]
LOOP_AT = 2780
NCYC = 26
WIN_START = LOOP_AT + 2 * CYCLE
WIN_END = LOOP_AT + NCYC * CYCLE
END = WIN_END + 60

def script(explore=False):
    ev = []
    for f, x, y in BOOT:
        ev += [f"{f}:move:{x}:{y}", f"{f+5}:ldown:{x}:{y}", f"{f+15}:lup:{x}:{y}"]
    ev.append("2499:snap")
    ev.append(f"{WARP_AT}:key:87")
    x, y = ME
    ev += [f"{SHIELD_AT}:key:114", f"{SHIELD_AT+5}:move:{x}:{y}",
           f"{SHIELD_AT+10}:rdown:{x}:{y}", f"{SHIELD_AT+25}:rup:{x}:{y}"]
    ev.append(f"{ARRIVAL_SNAP}:snap")
    for i in range(NCYC):
        f = LOOP_AT + i * CYCLE
        tx, ty = DIRS[i % len(DIRS)]
        ev += [f"{f}:key:112", f"{f+4}:move:{tx}:{ty}", f"{f+8}:rdown:{tx}:{ty}", f"{f+20}:rup:{tx}:{ty}",
               f"{f+30}:key:113", f"{f+34}:move:{x}:{y}", f"{f+37}:key:113",
               f"{f+40}:rdown:{x}:{y}", f"{f+CYCLE-10}:rup:{x}:{y}"]
        if explore:
            ev.append(f"{f+CYCLE//2}:snap")
    if not explore:
        ev += [f"{f}:snap" for f in (WIN_START - 30, (WIN_START + WIN_END) // 2, WIN_END + 30)]
    return ",".join(ev)

def checksum(d):
    c = 0
    for i, b in enumerate(d):
        if 12 <= i < 16: b = 0
        c = (((c << 1) | (c >> 31)) + b) & 0xFFFFFFFF
    return c

def save(src, outdir, name, act):
    assert name.isalpha() and 2 <= len(name) <= 15
    d = bytearray(open(src, "rb").read())
    assert struct.unpack_from("<I", d, 0)[0] == 0xAA55AA55
    d[20:36] = name.encode().ljust(16, b"\0")
    struct.pack_into("<I", d, 56 + 4, 44)        # F2 = Frost Nova (right button)
    struct.pack_into("<I", d, 124, 44)           # right skill: Frost Nova
    d[168 + 2] = 0x80 | act                      # Hell, last played, act
    struct.pack_into("<I", d, 8, len(d)); struct.pack_into("<I", d, 12, checksum(d))
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, name + ".d2s")
    open(out, "wb").write(d)
    print(f"{out}: Enfer acte {act + 1}, F1 Teleportation / F2 Nova de givre / F3 Bouclier, {len(d)} octets")

if __name__ == "__main__":
    a = sys.argv[1] if len(sys.argv) > 1 else ""
    if a == "window":
        print(WIN_START, WIN_END, END, ARRIVAL_SNAP)
    elif a == "save":
        save(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    else:
        print(script(a == "explore"))
