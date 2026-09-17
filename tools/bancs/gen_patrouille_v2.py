# Generates a ROBUST benchmark patrol.
# Differs from d2script_camp_patrouille.txt: a `space` (32) keypress is
# inserted before EVERY move. In D2, space closes any open panel (chest,
# inventory, skills); Escape, however, OPENS the game menu when nothing is
# open — that's why it isn't used.
# Without this, a single click landing on the chest freezes the bench on a
# static panel for the rest of the run.
ev = []
def at(f, s): ev.append((f, s))

# --- enter game: identical to the original ---
at(300,  "activate")
for f,x,y in [(400,400,308),(1500,400,300)]:
    at(f,   f"move:{x}:{y}"); at(f+50, f"ldown:{x}:{y}"); at(f+60, f"lup:{x}:{y}")
for i,c in enumerate("VITA"):
    at(2400+i*10, f"chr:{ord(c)}")
for i in range(5):
    at(2600+i*200, "keydown:13"); at(2620+i*200, "keyup:13")

# --- close any panel left open by loading, before starting ---
for i in range(3):
    at(3400+i*20, "keydown:32"); at(3410+i*20, "keyup:32")

# --- patrol: ring around the center, 8 points ---
ring = [(200,200),(600,200),(660,330),(600,430),(200,430),(140,330),(400,180),(400,420)]
f = 3600
STEP = 120          # same cadence as the original
while f < 8300:
    for (x,y) in ring:
        if f >= 8300: break
        at(f,    "keydown:32")      # closes any panel that might be open
        at(f+8,  "keyup:32")
        at(f+16, f"move:{x}:{y}")
        at(f+24, f"ldown:{x}:{y}")
        at(f+34, f"lup:{x}:{y}")
        f += STEP

ev.sort(key=lambda e: e[0])
print(",".join(f"{f}:{s}" for f,s in ev))
