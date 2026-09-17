import os, subprocess, random, sys
sys.path.insert(0, os.path.dirname(__file__) or ".")
import cdkey_ref as C
def wine(*a):
    r = subprocess.run(["unshare","-rn","wine","harness.exe",*a], capture_output=True, text=True)
    return r.stdout.strip().replace("\r","")
alpha = "246789BCDEFGHJKMNPRTVWXYZ"
cases = []
for i in range(6):   # ARBITRARY 26-char strings from the alphabet, no validity search
    cases.append(("".join(random.choice(alpha) for _ in range(26)) + "\0").encode())
cases += [b"Fake Owner\0", b"\0", b"x", os.urandom(63), os.urandom(64), os.urandom(65), os.urandom(200),
          os.urandom(513), os.urandom(1030), os.urandom(1100)]
fails = 0
for p in cases:
    ge = wine("enc", p.hex()); pe = C.encrypt(p).hex()
    gd = wine("dec", pe)            # the GAME decrypts the Python blob
    exp = "len=%d hex=%s" % (len(p), p.hex())
    gd = gd.split(" ",1)[1]   # ok= (0x5232B0) depends on strnlen<=0x80: checked separately for text cases
    okg = wine("dec", pe).split(" ")[0]
    if p.endswith(b"\0") and len(p) < 0x80: assert okg == "ok=1", okg
    n, pp = C.decrypt(bytes.fromhex(ge))   # Python decrypts the game's blob
    good = (ge == pe) and gd == exp and (n, pp) == (len(p), p)
    fails += not good
    print("len=%4d enc_eq=%s game_dec_py=%s py_dec_game=%s" % (len(p), ge == pe, gd == exp, (n, pp) == (len(p), p)))
print("FAIL" if fails else "PASS", len(cases), "cas")
