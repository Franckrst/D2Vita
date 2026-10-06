#!/usr/bin/env python3
"""tools/bancs/gen_save_orbe.py — turn the player's bench save into a level-99
Sorceress built for the Frozen Orb bench (tools/bancs/orbe_givre.sh).

  gen_save_orbe.py <player_save.d2s> <out_dir> [NAME]

The input is the player's own save (the Act V bench copy, $W/save_banc_ref/
<name>.d2s): its quests and waypoints are kept as they are, so it must have
every waypoint of Normal (the bench save does — Act V, all 3 difficulties).
What changes:
  - name (header + file name; D2 matches the two), class -> Sorceress;
  - level 99, max experience, energy/mana 8191 (the 21-bit stat ceiling);
  - skills: Frozen Orb 20 + its prerequisite chain, Warmth 20 (mana regen),
    Cold Mastery 20; Frozen Orb bound to the right button;
  - the 'WS' block: every waypoint bit of the 3 difficulties set.
Items and mercenary are left as they are (a level-99 Sorceress does not need
them in Normal). Size and checksum are recomputed.
Writes <out_dir>/<NAME>.d2s — player-derived data, never committed.
"""
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SORC = 1
FIRST = 36                       # first Sorceress skill id (Fire Bolt)
FROZEN_ORB = 64
SKILLS = {37: 20,                # Warmth: mana regeneration while casting
          39: 1, 44: 1, 45: 1, 55: 1, 59: 1,   # Ice Bolt, Frost Nova, Ice Blast, Glacial Spike, Blizzard
          64: 20,                # Frozen Orb
          65: 20}                # Cold Mastery

def checksum(d):
    c = 0
    for i, b in enumerate(d):
        if 12 <= i < 16: b = 0
        c = (((c << 1) | (c >> 31)) + b) & 0xFFFFFFFF
    return c

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    src, outdir = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else "Givre"
    assert name.isalpha() and 2 <= len(name) <= 15, "nom D2 : 2..15 lettres"
    os.makedirs(outdir, exist_ok=True)
    tmp = os.path.join(outdir, ".stats.d2s")
    subprocess.run([sys.executable, os.path.join(HERE, "..", "d2s_stats.py"), "set", src, tmp,
                    "level=99", "exp=3520485254", "energy=1000", "statpts=0", "skillpts=0",
                    "mana=8191", "maxmana=8191"], check=True, stdout=subprocess.DEVNULL)
    d = bytearray(open(tmp, "rb").read()); os.remove(tmp)
    assert struct.unpack_from("<I", d, 0)[0] == 0xAA55AA55 and struct.unpack_from("<I", d, 4)[0] == 96
    d[20:36] = name.encode().ljust(16, b"\0")
    d[40] = SORC
    d[43] = 99
    struct.pack_into("<I", d, 120, 0)            # left skill: attack
    struct.pack_into("<I", d, 124, FROZEN_ORB)   # right skill: Frozen Orb
    sk = d.find(b"if", d.find(b"gf"))
    assert sk > 0 and d[sk + 32:sk + 34] == b"JM", "section 'if' (30 octets) puis 'JM' attendues"
    for i in range(30):
        d[sk + 2 + i] = SKILLS.get(FIRST + i, 0)
    ws = d.find(b"WS")
    assert ws > 0 and d[ws + 8:ws + 10] == b"\x02\x01", "section 'WS' inattendue"
    for diff in range(3):                        # 3 x 24 bytes: 02 01 + 5 bytes of bits + padding
        o = ws + 8 + diff * 24 + 2
        for k, m in enumerate(b"\xff\xff\xff\xff\x7f"):  # 39 waypoints (OR: keep set bits)
            d[o + k] |= m
    struct.pack_into("<I", d, 8, len(d)); struct.pack_into("<I", d, 12, checksum(d))
    out = os.path.join(outdir, name + ".d2s")
    open(out, "wb").write(d)
    print(f"{out}: Sorciere niv. 99, Orbe de givre 20 (clic droit), {len(d)} octets")

if __name__ == "__main__":
    main()
