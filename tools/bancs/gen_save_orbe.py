#!/usr/bin/env python3
"""tools/bancs/gen_save_orbe.py — turn the player's bench save into a level-99
Sorceress built for the Frozen Orb bench (tools/bancs/orbe_givre.sh).

  gen_save_orbe.py <player_save.d2s> <out_dir> [NAME] [--trone --excel=<dir>]

The input is the player's own save (the Act V bench copy, $W/save_banc_ref/
<name>.d2s): its quests and waypoints are kept as they are, so it must have
every waypoint of Normal (the bench save does — Act V, all 3 difficulties).
What changes:
  - name (header + file name; D2 matches the two), class -> Sorceress;
  - level 99, max experience, energy/mana 8191 (the 21-bit stat ceiling);
  - skills: Frozen Orb 20 + its prerequisite chain, Warmth 20 (mana regen),
    Cold Mastery 20; Frozen Orb bound to the right button;
  - the 'WS' block: every waypoint bit of the 3 difficulties set.
--trone (Baal's Throne bench, BANC=trone tools/bancs/orbe_givre.sh): Hell,
Act V; Teleport 20 on F1, Frozen Orb on F2, Energy Shield on F3 (all right
button), Frost Nova 20; life 8191 (the 21-bit ceiling). Hell's waves outlast
that, so the build is deliberately absurd: Energy Shield 60 (absorption at
its cap) and Telekinesis 32 (its synergy brings the mana cost of an absorbed
point to 0), plus a worn ring of resistances / damage reduction at the
save-format ceilings — poison is not absorbed by the shield (console,
15/10: mana full, death by poison). The ring needs --excel=<dir>: the
ItemStatCost/Weapons/Armor/Misc tables extracted from the player's own MPQs.
Other items and the mercenary are left as they are. Size and checksum are
recomputed.
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

# --trone: extra skills (with their prerequisites) and F1..F3 bindings.
TRONE_SKILLS = {38: 1, 42: 1, 49: 1, 53: 1,             # Charged Bolt, Static, Lightning, Chain L.
                43: 32,                                 # Telekinesis: -6.25 %/lvl mana per absorbed point -> 0
                44: 20,                                 # Frost Nova
                54: 20,                                 # Teleport
                58: 60}                                 # Energy Shield, well past 20: absorption at its cap
TELEPORT, ENERGY_SHIELD = 54, 58
TRONE_RING = {39: 205, 41: 205, 43: 205, 45: 205,      # fire/light/cold/poison resist
              40: 31, 42: 31, 44: 31, 46: 31,          # their max resist
              110: 100,                                # poison length reduced
              36: 50, 34: 63, 35: 63,                  # damage reduced %, physical, magic
              76: 53, 74: 33}                          # +% life, life regeneration
HELL = 2

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    trone = "--trone" in sys.argv
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    src, outdir = args[0], args[1]
    name = args[2] if len(args) > 2 else "Givre"
    assert name.isalpha() and 2 <= len(name) <= 15, "nom D2 : 2..15 lettres"
    os.makedirs(outdir, exist_ok=True)
    tmp = os.path.join(outdir, ".stats.d2s")
    subprocess.run([sys.executable, os.path.join(HERE, "..", "d2s_stats.py"), "set", src, tmp,
                    "level=99", "exp=3520485254", "energy=1000", "statpts=0", "skillpts=0",
                    "mana=8191", "maxmana=8191"]
                   + (["hp=8191", "maxhp=8191"] if trone else []), check=True, stdout=subprocess.DEVNULL)
    d = bytearray(open(tmp, "rb").read()); os.remove(tmp)
    assert struct.unpack_from("<I", d, 0)[0] == 0xAA55AA55 and struct.unpack_from("<I", d, 4)[0] == 96
    d[20:36] = name.encode().ljust(16, b"\0")
    d[40] = SORC
    d[43] = 99
    struct.pack_into("<I", d, 120, 0)            # left skill: attack
    struct.pack_into("<I", d, 124, FROZEN_ORB)   # right skill: Frozen Orb
    sk = d.find(b"if", d.find(b"gf"))
    assert sk > 0 and d[sk + 32:sk + 34] == b"JM", "section 'if' (30 octets) puis 'JM' attendues"
    skills = {**SKILLS, **TRONE_SKILLS} if trone else SKILLS
    for i in range(30):
        d[sk + 2 + i] = skills.get(FIRST + i, 0)
    if trone:
        # F1..F16 bindings (16 dwords at 56): skill id, 0x8000 = left button.
        for k, sid in enumerate((TELEPORT, FROZEN_ORB, ENERGY_SHIELD)):
            struct.pack_into("<I", d, 56 + 4 * k, sid)
        # Bytes 168..170: Normal / Nightmare / Hell; bit 7 = the difficulty
        # the character last played (picked by default), low bits = act.
        act = d[168] & 7
        for k in range(3):
            d[168 + k] = (0x80 | act) if k == HELL else (d[168 + k] & 0x7f)
    ws = d.find(b"WS")
    assert ws > 0 and d[ws + 8:ws + 10] == b"\x02\x01", "section 'WS' inattendue"
    for diff in range(3):                        # 3 x 24 bytes: 02 01 + 5 bytes of bits + padding
        o = ws + 8 + diff * 24 + 2
        for k, m in enumerate(b"\xff\xff\xff\xff\x7f"):  # 39 waypoints (OR: keep set bits)
            d[o + k] |= m
    struct.pack_into("<I", d, 8, len(d)); struct.pack_into("<I", d, 12, checksum(d))
    out = os.path.join(outdir, name + ".d2s")
    open(out, "wb").write(d)
    if trone:
        # Hell poison goes past Energy Shield: a worn ring carries the rest
        # (stats at their save-format ceiling; tools/d2s_items.py equip).
        xd = next((a[8:] for a in sys.argv if a.startswith("--excel=")), None)
        assert xd, "--trone demande --excel=<dossier> (tables extraites des MPQ du joueur)"
        subprocess.run([sys.executable, os.path.join(HERE, "..", "d2s_items.py"), "equip", out, xd, out,
                        "rin@6"] + [f"{k}={v}" for k, v in TRONE_RING.items()], check=True)
    extra = ", Enfer, F1 Teleportation / F2 Orbe / F3 Bouclier d'energie" if trone else ""
    print(f"{out}: Sorciere niv. 99, Orbe de givre 20 (clic droit){extra}, {len(d)} octets")

if __name__ == "__main__":
    main()
