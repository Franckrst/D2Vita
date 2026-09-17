#!/usr/bin/env python3
"""tools/tests/text_size_report.py — .text of the crash reporter, per unit.

  text_size_report.py --size <arm-vita-eabi-size> --budget 153600 OBJ...

Prints one line per object, biggest first, then the total, and exits 1 when
the total is over the budget (spec section 4.9 aims at 150 KiB of added .text
for the whole reporter, zlib excluded).

--no-fail: still prints everything (including whether the total itself is
over budget) but always exits 0. Use this when the real gate is applied
elsewhere against a different figure — tools/tests/run_crashreport_tests.sh
leg 7 calls it this way and gates on REACHABLE (the --gc-sections figure)
instead: the per-object TOTAL below is a documented upper bound (Monocypher
is one translation unit that also carries Argon2, EdDSA signing and
Elligator, which the reporter never calls — see
tools/tests/crashreport_size_probe.cpp), so failing the build on THIS number
alone would reject a change that adds no real weight to the eboot.
"""
import argparse
import os
import subprocess
import sys


def text_sizes(size_tool, objects):
    out = subprocess.run([size_tool, "-A"] + objects, check=True, capture_output=True, text=True).stdout
    sizes, name = {}, None
    for line in out.splitlines():
        line = line.rstrip()
        if line.endswith(":"):
            name = os.path.basename(line[:-1])
            continue
        parts = line.split()
        # -ffunction-sections spreads the code over .text.<function>.
        if len(parts) >= 2 and name and (parts[0] == ".text" or parts[0].startswith(".text.")):
            sizes[name] = sizes.get(name, 0) + int(parts[1])
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", required=True)
    ap.add_argument("--budget", type=int, default=150 * 1024)
    ap.add_argument("--prefix", default="   ")
    ap.add_argument("--no-fail", action="store_true")
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args()
    sizes = text_sizes(a.size, a.objects)
    total = sum(sizes.values())
    for name in sorted(sizes, key=lambda k: (-sizes[k], k)):
        print("%s%-28s %8d" % (a.prefix, name, sizes[name]))
    print("%s%-28s %8d bytes of .text (budget %d)" % (a.prefix, "TOTAL", total, a.budget))
    if total > a.budget:
        label = "NOTE (upper bound, see --gc-sections REACHABLE below)" if a.no_fail else "FAIL"
        print("%s%s: over the %d KiB of spec 4.9" % (a.prefix, label, a.budget // 1024))
        return 0 if a.no_fail else 1
    print("%sOK: %d bytes left under the %d KiB of spec 4.9" % (a.prefix, a.budget - total, a.budget // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
