# Controls-help overlay on the title screen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Vita-side overlay — a persistent hint icon on D2's literal title screen that opens a translucent panel listing the current effective controller bindings — without touching `Game.exe`.

**Architecture:** A new live "title screen active" signal read from guest memory (found empirically, Task 3), consumed by a new header-only overlay module (`controls_help.h`, modeled on the existing `vita_kb.h`/`radial_menu.h` overlays) that draws an icon in D2's menu-mode left letterbox band and, on tap, a scrollable panel built from the live `g_btn[]` binding table plus a small fixed list of non-remappable bindings.

**Tech Stack:** C++17 (host dev build + ARM cross-compile for the Vita/qemu-arm), the winx86 dynarec runtime's `Cpu` memory-read interface, existing D2SCRIPT scripted-input harness for deterministic qemu-arm repro, Python 3 for an offline snapshot-diff tool.

**Spec:** `docs/superpowers/specs/2026-09-22-controls-help-title-screen-design.md`

**Branch:** `ui/aide-controles-ecran-titre` (already checked out).

---

## Sequencing and the blocking gate

Tasks run in order. **Task 3 is a go/no-go gate**: if it does not end with a
verified, title-screen-only memory signal, implementation stops there and is
reported back — do not continue to Task 4 onward, do not fall back to a
broader signal, do not guess an unverified offset. Everything from Task 4
onward assumes Task 3 succeeded and `src/runtime/screen_state.h` exists with a
real, verified `d2_title_screen_active()`.

---

### Task 1: Give the per-frame input tick a guest-memory read handle

**Why first:** `src/platform/vita_present.cpp` (where the overlay will live)
never reads guest memory today — it only touches Vita platform APIs. The
`Cpu&` needed to call `read_u32()` already exists one call frame up, in the
`PeekMessageA` import shim (`src/runtime/win32_shims_user32_d2.cpp:190-213`),
which is proven to fire on every screen D2 redraws (menu, character
creation/select, options, in a game — see the comment at
`win32_shims_user32_d2.cpp:192-199`). This task threads that `Cpu*` one level
down, into `d2vita_input_tick()`, and proves the plumbing works by reading the
*already-trusted* "in a game" signal (the player pointer at
`Game+0x3a6a70`, `src/runtime/phase_hooks.cpp:438`) and cross-checking it
against the existing `d2res_active()` latch's known behavior.

**Files:**
- Modify: `src/runtime/scripted_input.h:13`
- Modify: `src/platform/vita_present.cpp:1680` (function definition) and its top-of-file includes
- Modify: `src/runtime/win32_shims_user32_d2.cpp:213`
- Modify: `tools/rt_boot.cpp` (a second call site, inside `dumpFrame`, and — Step 5 — the actual smoke-test read, corrected below: `vita_present.cpp` is `__vita__`-only and is NOT linked into the qemu-arm oracle at all, per `tools/rt_boot_srcs.sh`; `tools/rt_boot.cpp` is, and already has a per-frame `Cpu&c` in `dumpFrame`, which is the file this task's actual qemu-arm verification runs against)

- [ ] **Step 1: Widen the weak declaration**

In `src/runtime/scripted_input.h`, add a forward declaration and change the
weak extern signature:

```cpp
// scripted_input.h -- D2SCRIPT scripted input injection (drive the menu
// without a real mouse) + D2CMDFILE runtime command injection. See
// scripted_input.cpp for the full rationale. g_hwnd/g_wndProc/g_msgQ/g_injN
// stay in tools/rt_boot.cpp (win32_shims_user32_d2.cpp also needs them, see
// runtime/rt_host.h).
#pragma once
#include <cstdint>
#include <string>

namespace d2rt { class Cpu; }

extern bool g_snap;                     // one-shot frame dump request (D2SCRIPT "snap")
extern uint8_t g_keyState[256];         // VK states driven by injected input

// Vita physical-input tick (weak). `cpu` is the guest CPU handle of the
// thread currently running the PeekMessageA pump — valid on every screen D2
// redraws (menu, character select/creation, options, in a game).
extern "C" { __attribute__((weak)) void d2vita_input_tick(d2rt::Cpu* cpu); }
```

- [ ] **Step 2: Update the definition**

In `src/platform/vita_present.cpp`, add the include near the other
`runtime/*.h` includes (line 21 area):

```cpp
#include "runtime/cpu.h"                  // d2rt::Cpu — screen-state reads (controls_help)
```

Change the function definition at line 1680:

```cpp
extern "C" void d2vita_input_tick(d2rt::Cpu* cpu){
```

(the existing body is untouched by this step — `cpu` is unused until Task 3).

- [ ] **Step 3: Update the call site**

In `src/runtime/win32_shims_user32_d2.cpp:213`, the call is already inside a
lambda that captures `Cpu&c`:

```cpp
if(d2vita_input_tick) d2vita_input_tick(&c);
```

**A second call site exists and needs the identical fix**: `tools/rt_boot.cpp`'s
`dumpFrame` lambda (~line 3424) also calls `d2vita_input_tick()` with no
argument, inside a scope that already has `Cpu&c` in scope (`{ FuScope
_s(&g_fuInput); if(d2vita_input_tick) d2vita_input_tick(&c); }`) — this is
the qemu-arm-side counterpart of the same call. Without this fix the build
fails to link ("too few arguments"). Apply the same one-line change there.

- [ ] **Step 4: Build and boot-check**

```bash
tools/rt_boot_arm_check.sh
```

Expected: PASS, same as before this change (title screen reached, clean
exit) — this step only proves the signature change compiles and doesn't
regress the existing boot path. If it fails to compile, the most likely
cause is a missing `d2rt::` qualifier or include path; `phase_hooks.cpp`
already includes `runtime/cpu.h` the same way and can be used as the
reference.

- [ ] **Step 5: Smoke-test the read, cross-checked against the trusted signal**

**Correction found during implementation:** `src/platform/vita_present.cpp`
is compiled only for the real Vita target (`__vita__`, real `sceCtrl*`/
`sceTouch*`) — `tools/rt_boot_srcs.sh` (the shared source list for both
`rt_boot_arm_check.sh` and `rt_gameplay_arm_check.sh`) does **not** include
it; only `tools/build_rt_boot_vpk.sh` does. So `d2vita_input_tick`'s real
body (defined in `vita_present.cpp`) never runs under qemu-arm at all — the
weak symbol resolves to null there and `if(d2vita_input_tick) ...` is a
no-op by construction. A smoke test placed inside it, as originally written
here, can never execute under qemu-arm.

What IS linked into both the qemu-arm oracle and the VPK build is
`tools/rt_boot.cpp`, which already has its own per-frame `Cpu&c` in the
`dumpFrame` lambda (~line 3400-3430) — the same lambda that already calls
`{ FuScope _s(&g_fuInput); if(d2vita_input_tick) d2vita_input_tick(&c); }`.
Put the smoke test there instead, right after that existing line:

```cpp
        { FuScope _s(&g_fuInput); if(d2vita_input_tick) d2vita_input_tick(&c); }   // physical controls (Vita)
        // TEMPORARY (Task 1 smoke test, removed before commit): cross-check
        // Cpu::read against the already-trusted "in a game" signal, from a
        // TU that IS linked into the qemu-arm oracle (unlike vita_present.cpp).
        {
            static bool s_last = false;
            extern uint32_t g_d2base;
            extern const bool g_114;
            bool in_game = false;
            if (g_114 && g_d2base) {
                uint32_t pl = 0;
                c.read(g_d2base + 0x003a6a70u, &pl, 4);
                in_game = (pl != 0);
            }
            if (in_game != s_last) {
                s_last = in_game;
                std::printf(in_game ? "task1-smoke: player pointer NON-NUL (en jeu)\n"
                                     : "task1-smoke: player pointer NUL (menu)\n");
            }
        }
```

(`std::printf` here, not `d2vita_progress` — `tools/rt_boot.cpp` is the
qemu-arm orchestrator itself and prints straight to its own captured log,
same as the surrounding diagnostics in `dumpFrame`; `d2vita_progress` is the
Vita-only boot-progress-file writer and isn't what this TU's other probes
use.)

This still fully proves what Task 1 needs to prove: that `Cpu::read` at
`Game+0x3a6a70` correctly reflects "in a game" vs "at a menu" — the same
read `d2vita_input_tick` will perform for real once it's linked into the
actual VPK build. `vita_present.cpp`'s Steps 1-4 changes (the signature
widening itself) stay exactly as specified above and are still real,
needed, and already verified by the Step 4 qemu-arm PASS (compiles, doesn't
regress) — Step 5 only moves *where the temporary proof read runs*, not
what Steps 1-4 changed.

Build and run the title-only check, then a run that reaches a game (the
existing gameplay gate, which already drives D2SCRIPT far enough to create/
enter a game deterministically):

```bash
tools/rt_boot_arm_check.sh
tools/rt_gameplay_arm_check.sh
```

Expected: `boot_progress.txt` (or the check's captured log — see each
script's own `$LOG`/`$WRITE` variables) shows **no** `NON-NUL` line from
`rt_boot_arm_check.sh` (title screen only, never enters a game) and **one**
`NON-NUL` line from `rt_gameplay_arm_check.sh` at the point a game starts,
matching the point the existing `resolution ->` / `d2res_active()` behavior
already fires. This proves the new `cpu` handle reads real, correctly-based
guest memory from the tick.

- [ ] **Step 6: Remove the temporary block and commit**

Delete the smoke-test block added in Step 5 from `tools/rt_boot.cpp` (keep
the signature change in the other three files, and keep `tools/rt_boot.cpp`'s
own `d2vita_input_tick(&c)` call-site fix — see the note below).

```bash
git add src/runtime/scripted_input.h src/platform/vita_present.cpp \
        src/runtime/win32_shims_user32_d2.cpp tools/rt_boot.cpp
git commit -m "$(cat <<'EOF'
input: d2vita_input_tick recoit le Cpu du pompe de messages

Le pompe PeekMessageA tourne sur tout ecran que D2 redessine (menu,
creation/selection de perso, options, en jeu) — c'est le point d'entree
naturel pour lire de la memoire invitee depuis le cote presentation, qui
n'y touchait pas jusqu'ici. Verifie par une lecture jetable du pointeur
joueur deja documente comme fiable, comparee au comportement connu de
d2res_active() : NUL a l'ecran-titre (rt_boot_arm_check), NON-NUL une
fois en jeu (rt_gameplay_arm_check) — lecture faite depuis dumpFrame()
dans tools/rt_boot.cpp, seule unite liee a la fois dans l'oracle qemu-arm
et le build VPK (vita_present.cpp est __vita__ seulement).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 2: Build the one-shot memory-snapshot tool and its offline diff

**Why:** finding which memory field flags "title screen, specifically" needs
comparing guest memory across screens. There's no such tool in this codebase
today (confirmed by research) — but there IS a precedented one-shot-trigger
mechanism to model it on: D2SCRIPT's `"snap"` action
(`src/runtime/scripted_input.cpp:132`, sets a flag a consumer checks once).
This task adds a twin action, `"memscan"`, and a host-side diff script that is
fully deterministic and gets a real TDD oracle (no guest/qemu dependency).

**Files:**
- Modify: `src/runtime/scripted_input.h`
- Modify: `src/runtime/scripted_input.cpp:132`
- Modify: `tools/rt_boot.cpp` (consume the flag inside `dumpFrame` — **not**
  `vita_present.cpp`/`d2vita_input_tick`: per Task 1 Step 5's correction,
  that TU is `__vita__`-only and never runs under qemu-arm, which is exactly
  where this spike needs to run)
- Create: `tools/screen_scan_diff.py`
- Create: `tools/tests/screen_scan_diff_test.py`

- [ ] **Step 1: Add the `g_memscan` flag and D2SCRIPT action**

In `src/runtime/scripted_input.h`, next to `g_snap`:

```cpp
extern bool g_snap;                     // one-shot frame dump request (D2SCRIPT "snap")
extern bool g_memscan;                  // one-shot guest-memory snapshot request (D2SCRIPT "memscan")
```

In `src/runtime/scripted_input.cpp`, next to `bool g_snap=false;` (line 23):

```cpp
bool g_snap=false;                         // one-shot frame dump request
bool g_memscan=false;                      // one-shot guest-memory snapshot request
```

And next to the `"snap"` dispatch (line 132):

```cpp
    else if(act=="snap"){    g_snap=true; if(d2gxm_shot_request) d2gxm_shot_request(); }
    else if(act=="memscan"){ g_memscan=true; }
```

- [ ] **Step 2: Consume the flag in `dumpFrame` and write the snapshot**

In `tools/rt_boot.cpp`, inside the `dumpFrame` lambda, add (right after the
`d2vita_input_tick(&c)` line Task 1 Step 3 already touched — this block
stays permanently, it is env-gated and inert unless `D2_SCREENSCAN_LABEL` is
set, consistent with every other diagnostic in this codebase, e.g.
`D2_LAGMARK`):

```cpp
    // D2_SCREENSCAN: one-shot guest-memory snapshot, armed by D2SCRIPT
    // "memscan" and env-gated so it costs nothing when unset. Used only by
    // the Task 3 title-screen-signal spike; not part of the shipped feature.
    // Lives here (not vita_present.cpp) because this TU is linked into both
    // the qemu-arm oracle and the VPK build — see Task 1 Step 5.
    if (g_memscan) {
        g_memscan = false;
        extern uint32_t g_d2base;
        const char* label = getenv("D2_SCREENSCAN_LABEL");
        const char* baseS  = getenv("D2_SCREENSCAN_BASE");
        const char* lenS   = getenv("D2_SCREENSCAN_LEN");
        if (label && baseS) {
            uint32_t base = (uint32_t)strtoul(baseS, nullptr, 16);
            uint32_t len  = lenS ? (uint32_t)strtoul(lenS, nullptr, 16) : 0x20000u;
            std::vector<uint8_t> buf(len);
            c.read(g_d2base + base, buf.data(), len);
            char path[256];
            snprintf(path, sizeof path, "ux0:data/d2vita/screenscan_%s.bin", label);
            FILE* f = fopen(path, "wb");
            if (f) { fwrite(buf.data(), 1, len, f); fclose(f); }
            std::printf("screenscan: %s (%u Ko a Game+0x%x) -> %s\n",
                        label, len >> 10, base, path);
        }
    }
```

(`<vector>` may need an explicit `#include <vector>` at the top of
`tools/rt_boot.cpp` if the build complains it's missing — check first, this
file is large and may already pull it in transitively.)

- [ ] **Step 3: Write the diff tool's failing tests first**

Create `tools/tests/screen_scan_diff_test.py`:

```python
import struct
import tempfile
import os
import unittest

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from screen_scan_diff import find_candidates


def _write(path, words):
    with open(path, "wb") as f:
        f.write(struct.pack(f"<{len(words)}I", *words))


class FindCandidatesTest(unittest.TestCase):
    def test_single_offset_distinguishes_one_label_from_the_rest(self):
        with tempfile.TemporaryDirectory() as d:
            # offset 8 (word index 2) is 1 on both title runs, 0 everywhere else;
            # every other offset is noisy/shared and must NOT be reported.
            _write(os.path.join(d, "title_a.bin"),      [5, 9, 1, 3])
            _write(os.path.join(d, "title_b.bin"),      [7, 9, 1, 4])
            _write(os.path.join(d, "charselect_a.bin"), [5, 9, 0, 3])
            _write(os.path.join(d, "charselect_b.bin"), [7, 9, 0, 4])
            _write(os.path.join(d, "options_a.bin"),    [5, 9, 0, 3])
            _write(os.path.join(d, "options_b.bin"),    [7, 9, 0, 4])
            _write(os.path.join(d, "game_a.bin"),       [5, 9, 0, 3])
            _write(os.path.join(d, "game_b.bin"),       [7, 9, 0, 4])

            candidates = find_candidates(d, target_label="title",
                                          other_labels=["charselect", "options", "game"])

            self.assertEqual(candidates, [(8, 1)])   # (byte offset, title's stable value)

    def test_no_candidate_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as d:
            # nothing distinguishes title from the rest: every word is either
            # noisy within a label or identical across all labels.
            _write(os.path.join(d, "title_a.bin"),      [1, 1])
            _write(os.path.join(d, "title_b.bin"),      [2, 1])
            _write(os.path.join(d, "charselect_a.bin"), [1, 1])
            _write(os.path.join(d, "charselect_b.bin"), [2, 1])

            candidates = find_candidates(d, target_label="title",
                                          other_labels=["charselect"])

            self.assertEqual(candidates, [])

    def test_missing_snapshot_file_raises_a_clear_error(self):
        with tempfile.TemporaryDirectory() as d:
            _write(os.path.join(d, "title_a.bin"), [1])
            with self.assertRaises(FileNotFoundError):
                find_candidates(d, target_label="title", other_labels=["charselect"])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 4: Run it to verify it fails**

```bash
python3 -m unittest tools/tests/screen_scan_diff_test.py -v
```

Expected: FAIL / ERROR — `screen_scan_diff` module (and `find_candidates`)
does not exist yet.

- [ ] **Step 5: Implement `tools/screen_scan_diff.py`**

```python
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
```

- [ ] **Step 6: Run the tests again to verify they pass**

```bash
python3 -m unittest tools/tests/screen_scan_diff_test.py -v
```

Expected: PASS (3 tests).

- [ ] **Step 7: Build and commit**

```bash
tools/rt_boot_arm_check.sh
git add src/runtime/scripted_input.h src/runtime/scripted_input.cpp tools/rt_boot.cpp \
        tools/screen_scan_diff.py tools/tests/screen_scan_diff_test.py
git commit -m "$(cat <<'EOF'
diag: memscan (D2SCRIPT) + diff hors-ligne pour le spike ecran-titre

memscan jumelle "snap" : un instantane brut de memoire invitee, borne par
D2_SCREENSCAN_BASE/LEN, ecrit sous un nom qui porte le label du run
(D2_SCREENSCAN_LABEL). screen_scan_diff.py compare des paires
d'instantanes par label et ne retient qu'un offset stable dans le label
cible et absent de tous les autres — outil deterministe, teste hors
console/qemu.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 3: Run the spike — find (or fail to find) the title-screen signal

**This is the go/no-go gate for the whole feature (see "Sequencing" above).**

- [ ] **Step 1: Build scripted paths that reach each of the four states, twice each**

Using the same deterministic D2SCRIPT approach as `tools/profile_rogue.sh`
(phased frame:action events) and `tools/rt_boot_arm_check.sh`/
`tools/rt_gameplay_arm_check.sh` as the harness, produce **eight** runs whose
scripts end with a `memscan` event once the target screen is reached and
stable:

| Label | Screen to reach before `memscan` |
|---|---|
| `title` | The literal title screen (this is what `rt_boot_arm_check.sh` already reaches and stops at — add one more D2SCRIPT frame at the end: `,<frame>:memscan`) |
| `charselect` | Character select (reachable via the same input path `rt_gameplay_arm_check.sh`/`profile_rogue.sh` use to get past the title screen, stopped one step earlier — before actually entering a game) |
| `options` | The options screen (navigate there from the title screen; same input-injection mechanism, new short script) |
| `game` | A frame or two after `rt_gameplay_arm_check.sh`'s own "in a game" point |

Run each twice (independent qemu-arm invocations of the *same* script — the
coop/deterministic scheduler must reproduce byte-identical guest state) with:

```bash
D2_SCREENSCAN_LABEL=title_a D2_SCREENSCAN_BASE=3a0000 D2_SCREENSCAN_LEN=20000 <the title run>
D2_SCREENSCAN_LABEL=title_b D2_SCREENSCAN_BASE=3a0000 D2_SCREENSCAN_LEN=20000 <the title run again>
# ...repeat for charselect_a/b, options_a/b, game_a/b
```

`D2_SCREENSCAN_BASE=3a0000` (relative to `g_d2base`) is a starting
hypothesis, not a known answer — it's the neighborhood of the two already-used
"screen offset" globals at `g_d2base+0x3a520c`/`0x3a5208`
(`phase_hooks.cpp:437`, read on the camera-draw hook — confirmed **not**
directly reusable, since that hook only fires in-game, but a reasonable place
to start looking nearby). If Step 3 below finds no candidate, widen
`D2_SCREENSCAN_LEN` or move `D2_SCREENSCAN_BASE` and repeat this step before
concluding nothing exists.

**Correction found during Task 2's code review:** the snapshot is written to
`g_writeRoot` (driven by env `D2WRITE`, matching the sibling `snap`/`dumpFrame`
FBDUMP path in `tools/rt_boot.cpp`), not the originally-planned
`ux0:data/d2vita/...` (that path doesn't exist under qemu-arm at all).
`tools/rt_boot_arm_check.sh`'s own header comment says it uses "a dedicated
fresh D2WRITE" per run for determinism — **read both
`tools/rt_boot_arm_check.sh` and `tools/rt_gameplay_arm_check.sh` first** to
see exactly how/whether each sets `D2WRITE`, since that decides where each of
these eight runs' `screenscan_<label>.bin` actually lands. If the scripts
auto-generate a fresh `D2WRITE` per invocation, either capture and note each
run's actual output path from its own log line (`tools/rt_boot.cpp`'s memscan
block prints the full path on success), or set `D2WRITE` explicitly yourself
to a known fixed directory for these eight runs so they all land somewhere
predictable (whichever is less invasive given how those two scripts are
written — read them before choosing). Collect the eight resulting `.bin`
files into one directory, e.g. `/tmp/screenscan/`, renaming/copying them
there if they didn't already land together.

- [ ] **Step 2: Run the diff tool**

```bash
python3 tools/screen_scan_diff.py /tmp/screenscan title charselect options game
```

- [ ] **Step 3: Evaluate the result**

- **If it prints one or more candidates**: for each, re-verify independently —
  re-run the `title` script a third time with a fresh label
  (`D2_SCREENSCAN_LABEL=title_c`), confirm the candidate offset still holds
  that exact value, and re-run one of the non-title labels a third time too,
  confirming it's still different. Pick the candidate that also makes sense
  as a small enum/flag-shaped value (prefer a small integer or bit pattern
  over an address-shaped value, which is more likely a transient pointer than
  a screen-id). This is the offset for Task 3's Step 4.
- **If it prints no candidates** after widening the scan window at least once
  (Step 1's note): **stop here.** Do not proceed to Task 4. Report back: the
  spike found no title-screen-only signal in the scanned region(s), with the
  regions tried and the eight snapshot files kept for the user to inspect or
  extend the search themselves.

- [ ] **Step 4 (only if Step 3 found a verified candidate): write `src/runtime/screen_state.h`**

```cpp
// src/runtime/screen_state.h — is D2 on its literal title screen right now?
//
// Offset found empirically (see docs/superpowers/specs/
// 2026-09-22-controls-help-title-screen-design.md, section 1, and
// tools/screen_scan_diff.py) by comparing guest memory across the title
// screen, character select, options, and in a game — NOT a community/RE
// database value. Re-verify with the same method if 1.14d's Game.exe build
// ever changes.
#pragma once
#include <cstdint>
#include "runtime/cpu.h"

extern uint32_t g_d2base;
extern const bool g_114;

// True only on the literal title screen; false on character select, options,
// and in a game.
inline bool d2_title_screen_active(d2rt::Cpu* cpu) {
    if (!cpu || !g_114 || !g_d2base) return false;
    uint32_t v = 0;
    cpu->read(g_d2base + 0x00<OFFSET>u, &v, 4);   // <- Step 3's verified offset
    return v == 0x<VALUE>u;                        // <- Step 3's verified value
}
```

Replace `<OFFSET>`/`<VALUE>` with the exact values Step 3 verified — write the
real hex literals, not this placeholder text.

- [ ] **Step 5: Smoke-test it the same way Task 1 did**

Temporarily call `d2_title_screen_active(&c)` from `dumpFrame` in
`tools/rt_boot.cpp` (same location as Task 1 Step 5's smoke test, for the
same reason: `vita_present.cpp` never runs under qemu-arm) and log on change,
run `tools/rt_boot_arm_check.sh` (expect it stays true the whole title-screen
run) and the character-select/options/game scripts from Step 1 (expect false
throughout each). Remove the temporary block once confirmed.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/screen_state.h
git commit -m "$(cat <<'EOF'
runtime: signal ecran-titre verifie empiriquement (Game+0x<OFFSET>)

Trouve par comparaison memoire entre ecran-titre / selection de perso /
options / en jeu (tools/screen_scan_diff.py, 8 instantanes qemu-arm en
2 exemplaires par ecran). Reverifie independamment sur un 3e run avant
d'etre retenu. Voir la section 1 du design pour la methode complete.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 4: `controls_help.h` — pure logic (hit-rect, scroll, open/close state)

**Files:**
- Create: `src/platform/controls_help.h`
- Test: `tools/tests/controls_help_test.cpp`
- Create: `tools/oracle_controls_help.sh`

- [ ] **Step 1: Write the failing tests**

```cpp
// tools/tests/controls_help_test.cpp — host-side oracle for controls_help.h.
//
// controls_help.h never touches VitaSDK or a syscall, so everything below
// builds and runs on a dev machine. What still needs on-device checking is
// whether the icon/panel actually render above the frame, and the touch
// coordinates it receives are truly confined to the left letterbox band.
#include "platform/controls_help.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace d2ch;

static void t_icon_hit_rect_confined_to_left_band() {
    // The band is x in [0,117), full height (960x544 screen, design section 2).
    CHECK(icon_hit(50, 10) == true,   "inside the band should hit");
    CHECK(icon_hit(0, 0) == true,     "top-left corner of the band should hit");
    CHECK(icon_hit(116, 543) == true, "bottom-right corner of the band should hit");
    CHECK(icon_hit(117, 10) == false, "one pixel past the band must NOT hit");
    CHECK(icon_hit(400, 10) == false, "inside D2's own 800x600 canvas must NOT hit");
    CHECK(icon_hit(50, 544) == false, "one row past the bottom must NOT hit");
}

static void t_open_close_toggle() {
    State s; std::memset(&s, 0, sizeof s);
    CHECK(s.open == false, "starts closed");
    tap(s, 50, 10);
    CHECK(s.open == true, "tap inside the icon opens the panel");
    close(s);
    CHECK(s.open == false, "close() closes it");
    // A tap outside the band, while closed, must not open it.
    State s2; std::memset(&s2, 0, sizeof s2);
    tap(s2, 400, 10);
    CHECK(s2.open == false, "tap outside the band does not open the panel");
}

static void t_scroll_dpad_step() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 20; s.visible_rows = 6;
    CHECK(s.scroll == 0, "starts at top");
    scroll_dpad(s, +1);
    CHECK(s.scroll == 1, "one row down");
    for (int i = 0; i < 30; ++i) scroll_dpad(s, +1);
    CHECK(s.scroll == 14, "clamped to row_count - visible_rows (20-6=14)");
    for (int i = 0; i < 30; ++i) scroll_dpad(s, -1);
    CHECK(s.scroll == 0, "clamped at 0, never negative");
}

static void t_scroll_drag_delta() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 20; s.visible_rows = 6; s.row_px = 20;
    // Dragging UP (finger moves toward smaller y) scrolls the list DOWN
    // (higher row index) — same convention as a phone list.
    scroll_drag(s, /*dy_px=*/-45);
    CHECK(s.scroll == 2, "45px of upward drag at 20px/row => 2 rows (45/20 truncated)");
    scroll_drag(s, /*dy_px=*/+200);
    CHECK(s.scroll == 0, "large downward drag clamps at 0");
}

static void t_content_shorter_than_view_never_scrolls() {
    State s; std::memset(&s, 0, sizeof s);
    s.open = true; s.row_count = 3; s.visible_rows = 6;
    scroll_dpad(s, +5);
    CHECK(s.scroll == 0, "fewer rows than the view: max scroll is 0");
}

int main() {
    t_icon_hit_rect_confined_to_left_band();
    t_open_close_toggle();
    t_scroll_dpad_step();
    t_scroll_drag_delta();
    t_content_shorter_than_view_never_scrolls();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 2: Run it to verify it fails to compile**

```bash
g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$(pwd)/src" -o /tmp/controls_help_test tools/tests/controls_help_test.cpp
```

Expected: FAIL — `platform/controls_help.h` does not exist yet.

- [ ] **Step 3: Implement `src/platform/controls_help.h`**

```cpp
// src/platform/controls_help.h — title-screen "controls help" overlay:
// a persistent icon in D2's left letterbox band, and the panel it opens.
//
// No VitaSDK, no syscall — same "benign race, single writer" state model as
// vita_kb.h/radial_menu.h: State is written from the input tick, drawn from
// the presentation path.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace d2ch {

// Left letterbox band on the title screen (960x544 physical screen, D2's own
// 800x600 canvas centered at x in [117,842) — see design doc section 2).
constexpr int BAND_X0 = 0, BAND_X1 = 117, BAND_Y0 = 0, BAND_Y1 = 544;

inline bool icon_hit(int x, int y) {
    return x >= BAND_X0 && x < BAND_X1 && y >= BAND_Y0 && y < BAND_Y1;
}

struct State {
    bool open = false;
    int  row_count = 0;      // total bindings to show
    int  visible_rows = 0;   // how many fit on screen at once
    int  scroll = 0;         // topmost visible row index
    int  row_px = 20;        // pixel height of one row, for drag-to-scroll
};

inline int max_scroll(const State& s) {
    int m = s.row_count - s.visible_rows;
    return m > 0 ? m : 0;
}

inline void clamp_scroll(State& s) {
    if (s.scroll < 0) s.scroll = 0;
    int m = max_scroll(s);
    if (s.scroll > m) s.scroll = m;
}

inline void tap(State& s, int x, int y) {
    if (!s.open) {
        if (icon_hit(x, y)) s.open = true;
        return;
    }
}

inline void close(State& s) { s.open = false; }

inline void scroll_dpad(State& s, int rows) {
    s.scroll += rows;
    clamp_scroll(s);
}

inline void scroll_drag(State& s, int dy_px) {
    // Upward finger motion (negative dy_px) scrolls forward through the list.
    s.scroll += (-dy_px) / (s.row_px > 0 ? s.row_px : 1);
    clamp_scroll(s);
}

namespace draw_detail {
inline uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b); }
inline void rect(uint32_t* fb, int fw, int fh, int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > fw) x1 = fw; if (y1 > fh) y1 = fh;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            fb[y * fw + x] = color;
}
} // namespace draw_detail

// Draws the persistent icon (when !s.open) or the full panel (when s.open).
// `labels`/`bindings` are `row_count` parallel arrays of already-formatted,
// null-terminated strings (built by the caller from g_btn[] + the fixed
// non-remappable list — see controls_help_format.h, Task 6).
inline void draw(const State& s, uint32_t* fb, int fw, int fh,
                  const char* const* labels, const char* const* bindings) {
    (void)labels; (void)bindings;   // text rendering: same primitive kit as vita_kb.h's draw_detail::text/ch, reused as-is
    using namespace draw_detail;
    if (!s.open) {
        rect(fb, fw, fh, BAND_X0 + 8, BAND_Y1 - 24, BAND_X1 - 8, BAND_Y1 - 8, rgb(40, 40, 40));
        return;
    }
    // Full-screen translucent panel background.
    rect(fb, fw, fh, 0, 0, fw, fh, rgb(10, 10, 10));
}

} // namespace d2ch
```

The text-rendering call (`labels[i]`/`bindings[i]` drawn as rows starting at
`s.scroll`) reuses `vita_kb.h`'s existing pixel-font primitives
(`draw_detail::text`/`ch`) rather than duplicating a font — wire that up
during Task 5, once the actual on-screen layout is being tuned on a real
framebuffer; the oracle in this task only proves the state machine and hit
math, not pixel output, matching `kb_test.cpp`'s own scope.

- [ ] **Step 4: Run the tests again to verify they pass**

```bash
g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$(pwd)/src" -o /tmp/controls_help_test tools/tests/controls_help_test.cpp
/tmp/controls_help_test
```

Expected: `5 checks` (well, count the individual `CHECK` calls — 6+4+4+2+1 =
17) `, 0 failed`, exit code 0.

- [ ] **Step 5: Write the oracle script**

Create `tools/oracle_controls_help.sh`, modeled directly on
`tools/oracle_clavier.sh`:

```bash
#!/usr/bin/env bash
# tools/oracle_controls_help.sh — HOST-SIDE ORACLE for the controls-help
# overlay (src/platform/controls_help.h). No console, no game.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${D2VITA_WORK:-/tmp}"; BIN="$W/controls_help_test.$$"
rc=0

echo "== 1. compilation stricte =="
if g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
       -I "$ROOT/src" -o "$BIN" "$ROOT/tools/tests/controls_help_test.cpp"; then echo "   OK"; else echo "   ECHEC"; rc=1; fi

echo "== 2. oracle de comportement =="
if [ -x "$BIN" ]; then "$BIN" || rc=1; else rc=1; fi
rm -f "$BIN"

exit $rc
```

```bash
chmod +x tools/oracle_controls_help.sh
tools/oracle_controls_help.sh
```

Expected: both legs OK, exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/platform/controls_help.h tools/tests/controls_help_test.cpp tools/oracle_controls_help.sh
git commit -m "$(cat <<'EOF'
ui: controls_help.h — logique pure de l'icone et du panneau

Rect de detection tactile confine a la bande gauche de l'ecran-titre,
bascule ouvert/ferme, defilement D-pad et glisse tactile (les deux
bornes a [0, row_count-visible_rows]). Oracle host, sans VitaSDK, sur
le meme modele que oracle_clavier.sh.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 5: Build the live binding-list text (from `g_btn[]` + the fixed six)

**Files:**
- Modify: `src/platform/vita_present.cpp`

- [ ] **Step 1: Add a formatter next to `load_controls_txt()`**

`BtnMap`/`Act` are private to the anonymous namespace in this file already
(they hold `g_btn[]`), so this formatter lives in the same translation unit —
no header can reach into them, which also means there's nothing to keep in
sync by hand (Task 4/6 boundary is exactly this: pure layout logic in the
header, content assembly here). Add, right after `load_controls_txt()`
(after line 1657):

```cpp
// Human-readable label for one Act, for the controls-help panel. Mirrors the
// vk-code choices already made in g_btn[]'s own comments (line 1576+).
const char* act_label(const Act& a) {
    if (a.kind == A_NONE) return "-";
    if (a.kind == A_LMB)  return "Left click";
    if (a.kind == A_RMB)  return "Right click";
    switch (a.vk) {
        case 0x52: return "R (walk/run)";
        case 0x10: return "Shift";
        case 0x12: return "Alt";
        case 0x57: return "W (weapon swap)";
        case 0x31: return "Potion 1"; case 0x32: return "Potion 2";
        case 0x33: return "Potion 3"; case 0x34: return "Potion 4";
        case 0x70: return "F1"; case 0x71: return "F2";
        case 0x72: return "F3"; case 0x73: return "F4";
        case 0x1B: return "Escape";
        case 0x20: return "Space";
        default:   return "?";
    }
}
const char* bit_label(uint32_t bit) {
    if (bit == B_CROSS)  return "Cross";
    if (bit == B_CIR)    return "Circle";
    if (bit == B_SQR)    return "Square";
    if (bit == B_TRI)    return "Triangle";
    if (bit == B_UP)     return "D-pad Up";
    if (bit == B_DOWN)   return "D-pad Down";
    if (bit == B_LEFT)   return "D-pad Left";
    if (bit == B_RIGHT)  return "D-pad Right";
    if (bit == B_START)  return "Start";
    return "?";
}
// Fills `out[i]` with up to `max` "<button>: <action>" lines: every g_btn[]
// entry with a bound base action, its R+ layer if bound, then the fixed set
// that is NOT remappable via controls.txt (kept in sync here, by hand, on
// purpose — see design doc section 4: these six never move).
int format_controls_help(char out[][64], int max) {
    int n = 0;
    for (const BtnMap& m : g_btn) {
        if (n >= max) break;
        if (m.base.kind != A_NONE)
            snprintf(out[n++], 64, "%s: %s", bit_label(m.bit), act_label(m.base));
        if (n < max && m.layer.kind != A_NONE)
            snprintf(out[n++], 64, "R+%s: %s", bit_label(m.bit), act_label(m.layer));
    }
    static const char* const kFixed[] = {
        "L: Left click (held)",
        "R: Right click (held)",
        "Select: Radial menu",
        "R+Select: Space",
        "R+Triangle: Virtual keyboard",
        "L+Start: Screenshot",
    };
    for (const char* f : kFixed) {
        if (n >= max) break;
        snprintf(out[n++], 64, "%s", f);
    }
    return n;
}
```

- [ ] **Step 2: Build**

**Correction — `tools/rt_boot_arm_check.sh` does NOT verify this step.** Per
Task 1 Step 5's finding, `tools/rt_boot_srcs.sh` (the qemu-arm oracle's
source list) does not include `src/platform/vita_present.cpp` at all — only
the real VPK build does. A `rt_boot_arm_check.sh` PASS after this change
would prove nothing about whether `format_controls_help()` even compiles;
reporting it as verification would be exactly the kind of false check
CLAUDE.md forbids. Use the real build instead:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
tools/build_glide_ring.sh    # one-time per fresh worktree: produces
                              # build-glide/glide3x.dll, which the VPK link
                              # step below requires and won't build itself
tools/build_rt_boot_vpk.sh
```

Already confirmed working end to end in this exact worktree (VitaSDK present
at `/usr/local/vitasdk`, both commands run successfully, produced
`build-vita/d2vita.vpk`) — so this is a real, available verification path,
not a hypothetical. This build compiles the whole real Vita target including
`vita_present.cpp`, incrementally (only changed units recompile). Expected:
it completes and produces `build-vita/d2vita.vpk` with no compile error for
`vita_present.cpp` — this function isn't called from anywhere yet (Task 6
wires it in), so a clean compile is all this step proves, but it needs to be
a REAL compile, not the qemu-arm oracle. If either command fails for an
environment reason (not a code reason), report that precisely rather than
substituting `rt_boot_arm_check.sh` and calling it equivalent.

- [ ] **Step 3: Commit**

```bash
git add src/platform/vita_present.cpp
git commit -m "$(cat <<'EOF'
ui: format_controls_help() — la liste de bindings depuis g_btn[]

Lit g_btn[] (donc les surcharges de controls.txt) plus les six raccourcis
cables en dur qui ne passent pas par cette table (L/R clic, Select, R+
Select/Triangle, L+Start) — jamais une copie statique qui pourrait
diverger des vrais bindings.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 6: Wire the icon/panel into the input tick and presentation path

**Files:**
- Modify: `src/platform/vita_present.cpp`

- [ ] **Step 1: Add includes and state**

Near the other overlay includes (line 15-16 area):

```cpp
#include "platform/controls_help.h"
#include "runtime/screen_state.h"
```

Near `g_ctl_prev`/`g_ctl_init` (around line 1593), add:

```cpp
d2ch::State g_ch;
char g_ch_labels[64][64];
int  g_ch_count = 0;
bool d2ch_title_active_cached = false;   // written every ~4 ticks below, read
                                          // by Step 2's gate and Step 3's draw
```

**Correction — the sample in the original plan draft used
`d2ch_title_active_cached` in Step 2 without ever declaring it. Declare it
here** (added above) — this is a real bug in the plan text, not a hint to
invent your own name; use exactly this one so Step 2 and Step 3 below agree.

- [ ] **Step 2: Update state every tick, gated by the title-screen signal**

Inside `d2vita_input_tick`, after the existing touch-sampling code that
produces the current tap/touch coordinates (reuse the same
`SCE_TOUCH_PORT_FRONT` sample already read for the D2 cursor — see
`vita_present.cpp:1897` area), add, before that sample is forwarded to
`pad_to_game()`/the D2 cursor injection:

```cpp
    bool title = d2ch_title_active_cached;   // updated below, read here to gate this frame's handling
    {
        static int s_gate = 0;
        if ((++s_gate & 3) == 0)   // recompute at ~1/4 tick rate: this signal doesn't need every-frame freshness
            d2ch_title_active_cached = d2_title_screen_active(cpu);
    }
    if (title && g_ch.open) {
        // Panel open: D-pad scroll, Circle/Start close, and this frame's
        // touch (if any) drives drag-scroll instead of the D2 cursor.
        if (pad_pressed(B_UP))    d2ch::scroll_dpad(g_ch, -1);
        if (pad_pressed(B_DOWN))  d2ch::scroll_dpad(g_ch, +1);
        if (pad_pressed(B_CIR) || pad_pressed(B_START)) d2ch::close(g_ch);
        if (touched) d2ch::scroll_drag(g_ch, touch_dy);
        return;   // consumed: nothing this frame reaches D2's own input path
    }
    if (title && !g_ch.open && touched && tap_is_short) {
        d2ch::tap(g_ch, touch_x, touch_y);
        if (g_ch.open) {
            g_ch_count = format_controls_help(g_ch_labels, 64);
            g_ch.row_count = g_ch_count;
            g_ch.visible_rows = 20;   // tuned on-device in Task 8
            return;   // this tap opened the panel: don't also forward it as a D2 click
        }
    }
```

The exact identifiers `pad_pressed`, `touched`, `touch_x`/`touch_y`,
`touch_dy`, `tap_is_short` must match whatever this function already calls
its D-pad-edge and touch-sample locals (`vita_present.cpp`'s existing
`g_ctl_prev`/edge-loop and the touch block around line 1897-1907, per the
Task-1-referenced research) — read that existing code first and match its
naming exactly rather than introducing parallel state; this step's job is to
route into `d2ch::` at the right point, not to re-derive touch/pad sampling
that already exists.

- [ ] **Step 3: Draw the overlay**

Alongside the existing `draw_keyboard(fb)` call in the presentation path (the
call site referenced by Task 1's research, same file), add:

```cpp
    if (d2ch_title_active_cached)
        d2ch::draw(g_ch, fb, fw, fh, /*labels=*/nullptr, /*bindings=*/nullptr);
```

(`labels`/`bindings` wiring to `g_ch_labels` — splitting each formatted
"button: action" string into two columns for `draw()`, or simplifying
`draw()` to take one pre-joined string per row — gets finalized in Task 8
once this is checked against a real framebuffer on qemu-arm/Vita3K; note
that discrepancy here rather than silently picking one.)

- [ ] **Step 4: Build**

**Same correction as Task 5 Step 2**: `tools/rt_boot_arm_check.sh` does not
compile `vita_present.cpp` and cannot verify this step. Use:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
tools/build_glide_ring.sh    # skip if build-glide/glide3x.dll already exists
                              # from Task 5's build
tools/build_rt_boot_vpk.sh
```

Expected: completes, produces `build-vita/d2vita.vpk`, no compile error. If identifiers
from Step 2 don't match the real local names in this file, this is where
that surfaces as a compile error — fix by matching the existing names, not
by inventing new state.

- [ ] **Step 5: Commit**

```bash
git add src/platform/vita_present.cpp
git commit -m "$(cat <<'EOF'
ui: l'icone/panneau controles branches sur le pompe d'entree

Actif seulement quand d2_title_screen_active() repond vrai. Tap dans la
bande gauche ouvre le panneau (et n'atteint plus D2) ; D-pad et glisse
tactile font defiler ; Rond/Start ferment (consommes, jamais transmis
a D2). Le detail de layout (colonnes, hauteur de ligne visible) reste a
regler sur un vrai framebuffer, note dans la tache suivante.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 7: Register the new oracle in the gate ladder

**Files:**
- Modify: `tools/bancs/portes.sh`

- [ ] **Step 1: Add a new leg next to the existing keyboard oracle**

In `tools/bancs/portes.sh`, following the exact pattern at the "Leg 0" block
(around line 25-38):

```bash
echo "== oracle controles $(date +%H:%M)"
bash tools/oracle_controls_help.sh > "$W/porte_controles.log" 2>&1; r_ch=$?
grep -E "^PASS|^FAIL" "$W/porte_controles.log"; echo "   rc=$r_ch"
```

Add `r_ch` to whichever final pass/fail aggregation `portes.sh` already does
for `r0`/the other leg results (read that aggregation first — mirror its
exact form, don't invent a different exit-code convention).

- [ ] **Step 2: Run the full gate**

```bash
tools/bancs/portes.sh
```

Expected: every leg, including the new one, reports `rc=0`.

- [ ] **Step 3: Commit**

```bash
git add tools/bancs/portes.sh
git commit -m "$(cat <<'EOF'
ci: enregistrer oracle_controls_help dans la porte hote

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

### Task 8: On-device validation (qemu-arm → Vita3K → console)

This task has no pre-written code — it's the validation ladder itself, and
its job is specifically to catch what the host oracles above cannot: real
pixels, real touch coordinates, real timing. Follow the
`local-validation`/`runtime-debugging` skills' method at each level; this
task records what to check, not fabricated expected output.

- [ ] **Step 1: qemu-arm**

```bash
tools/rt_boot_arm_check.sh
```

Confirm: boots to title screen, no crash, no new watchdog stall, with the
overlay wired in (icon drawing every title-screen frame now, unlike every
earlier task's steps).

- [ ] **Step 2: Vita3K screenshot**

Follow the `vita3k-testing` skill to boot the VPK and capture a screenshot at
the title screen. Confirm: the icon is visible in the left band and does not
visually overlap D2's own title-screen art; tap it (Vita3K's touch
simulation) and capture a second screenshot with the panel open, confirming
the binding list is legible and matches what `format_controls_help()`
produces (cross-check a couple of lines by hand against `docs-site/controles.md`).

- [ ] **Step 3: Console**

Follow the `hardware-ab-bench`/`runtime-debugging` skills' console protocol.
Confirm, in order:
1. The icon appears on the title screen and nowhere else (character select,
   options, in a game — tap in the same screen region on each and confirm
   nothing opens).
2. A custom `ux0:data/d2vita/controls.txt` remap (e.g. remapping Cross to
   `f5`) shows up correctly in the panel text after a fresh boot.
3. Both D-pad and touch-drag scroll the same list, and scrolling clamps at
   both ends without wrapping or crashing.
4. Circle and Start both close the panel; after closing, confirm neither an
   Escape nor a Shift keystroke reached D2 (e.g. that closing the panel with
   Start does not also back out of a menu D2 is showing underneath).

- [ ] **Step 4: Update ROADMAP.md**

Per `CLAUDE.md`'s "Keep ROADMAP.md accurate" rule, add an entry recording
this feature's status (validated at which of the three levels) — use the
`doc-updater` agent for this rather than hand-editing, since it already knows
this file's conventions.

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
ui: aide aux controles a l'ecran-titre — valide qemu-arm/Vita3K/console

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Xgm4MrSgSUDrUAFqVXvVN9
EOF
)"
```

---

## Self-review notes

- **Spec coverage**: section 1 (detection) → Tasks 1-3; section 2 (icon) →
  Task 4 (`icon_hit`) + Task 6 (wiring) + Task 8 (visual check); section 3
  (open/close) → Task 4 (`tap`/`close`) + Task 6; section 4 (content, live
  from `g_btn[]`) → Task 5; section 5 (scrolling, both input paths) → Task 4
  (`scroll_dpad`/`scroll_drag`); section 6 (implementation shape, file
  location resolved to `src/platform/`) → Tasks 4-6; section 7 (validation
  ladder) → Task 8.
- **Known open ends, called out rather than hidden**: Task 6 Step 2's exact
  local variable names (`touched`, `touch_x`, `tap_is_short`, …) are not
  independently re-derived — the step says explicitly to read the existing
  code and match it, because guessing them would risk silently inventing
  parallel/duplicate state. Task 6 Step 3's `labels`/`bindings` column
  layout is explicitly deferred to Task 8's on-device check rather than
  guessed pixel positions. Both are flagged inline at the point they occur,
  not left implicit.
- **Type consistency checked**: `Act`/`BtnMap`/`A_NONE`/`A_LMB`/`A_RMB`/
  `A_KEY`/`B_CROSS` etc. (Task 5) match the real declarations read directly
  from `vita_present.cpp:1558-1590` during planning, not paraphrased.
  `d2rt::Cpu`/`g_d2base`/`g_114` (Tasks 1, 3) match `rt_host.h:130-131` and
  `phase_hooks.cpp`'s `using namespace d2rt;` convention. `d2ch::State`
  fields (`open`, `row_count`, `visible_rows`, `scroll`, `row_px`) are used
  identically across Task 4's test, implementation, and Task 6's wiring.
