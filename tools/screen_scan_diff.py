#!/usr/bin/env python3
"""tools/screen_scan_diff.py — offline diff for the Task 3 title-screen spike.

Reads pairs of raw memory snapshots (see tools/tests/screen_scan_diff_test.py
for the exact file-naming contract: "<label>_a.bin" / "<label>_b.bin", same
length, 4-byte-word-aligned) captured via D2SCRIPT "memscan"
(D2_SCREENSCAN_LABEL=<label>), and reports every 4-byte offset whose word
value is:
  - identical between the target label's two snapshots (stable, not noise), and
  - different from that same offset's value in EVERY other label's snapshots
    (both of each, so a coincidental match in one run isn't enough).
"""
import struct
import os
import sys


def _load_words(path):
    with open(path, "rb") as f:
        data = f.read()
    n = len(data) // 4
    return struct.unpack(f"<{n}I", data[: n * 4])


def find_candidates(snapshot_dir, target_label, other_labels):
    def pair(label):
        a = os.path.join(snapshot_dir, f"{label}_a.bin")
        b = os.path.join(snapshot_dir, f"{label}_b.bin")
        for p in (a, b):
            if not os.path.isfile(p):
                raise FileNotFoundError(p)
        return _load_words(a), _load_words(b)

    ta, tb = pair(target_label)
    others = [pair(l) for l in other_labels]

    n = min(len(ta), len(tb), *(min(len(oa), len(ob)) for oa, ob in others))
    candidates = []
    for i in range(n):
        if ta[i] != tb[i]:
            continue  # noisy within the target label itself
        value = ta[i]
        if any(oa[i] == value or ob[i] == value for oa, ob in others):
            continue  # some other screen shares this value: not distinguishing
        candidates.append((i * 4, value))
    return candidates


def main(argv):
    if len(argv) < 3:
        print("usage: screen_scan_diff.py <snapshot_dir> <target_label> <other_label> [more_labels...]",
              file=sys.stderr)
        return 2
    snapshot_dir, target_label = argv[1], argv[2]
    other_labels = argv[3:]
    candidates = find_candidates(snapshot_dir, target_label, other_labels)
    if not candidates:
        print(f"no candidate offset distinguishes '{target_label}' from {other_labels}")
        return 1
    for off, value in candidates:
        print(f"Game+0x{off:06x} (relative to D2_SCREENSCAN_BASE) = 0x{value:08x} on '{target_label}', "
              f"differs on {other_labels}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
