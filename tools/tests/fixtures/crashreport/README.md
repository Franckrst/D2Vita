# Crash-report extraction fixtures

Inputs for `tools/tests/crashreport_test.cpp`. Every file is either a verbatim
extract of a real log (anonymized) or built from the exact format strings of
the code that writes it; the table says which. Real `psp2dmp` dumps never
live here (they may hold CD keys): the reader is tested on synthetic dumps
generated at test time by `tools/tests/gen_fake_psp2dmp.py`, and on the
maintainer's real dumps only through `D2V_CRASH_FIXTURES`.

Anonymization applied to real extracts: LAN addresses replaced by
documentation addresses (`192.0.2.x`); no account name was present (any would
be replaced by `FAKEACCOUNT`).

| File | Provenance | What it exercises |
|---|---|---|
| `progress_native_fault_real.txt` | real, console 2026-09-08 boot_progress, lines 1340-1534 | `NATIVE FAULT thr 1` (older build: no register/stack block), beats around it |
| `progress_hang_real.txt` | real, console 2026-09-08 boot_progress (whole file) | game frozen: `frames=38` on the last 5 `alive:` beats (4 without progress), watchdog thread dump |
| `progress_clean_exit_real.txt` | real, console 2026-09-14 boot_progress, lines 1-40 + 760-1012 | normal quit: `ExitProcess code=0`, `scheduler stopped: shutdown requested ... main-exit=0x00000000`, `CLEAN EXIT`, `env.txt: D2NET=1` |
| `progress_no_evidence_real.txt` | real, console 2026-09-08 boot_progress fragment (248 lines) | log that just stops: beats still progressing, nothing else |
| `progress_native_fault_block.txt` | real lines 1386-1446 of the 2026-09-08 log around a CONSTRUCTED fault block | current block format of `sched_native.cpp:607-630` (`fault thr`, `fault module`, `fault pile`, `NATIVE FAULT`) for worker thread 3, an unrelated line interleaved, and a second fault that must be ignored |
| `progress_abnormal_exit.txt` | real teardown lines 960-1012 of the 2026-09-14 log, with three CONSTRUCTED lines | abnormal exit after a Halt: `ExitProcess code=4294967295` (chain quoted in the Halt 904 investigation), `main-exit=0xffffffff` |
| `crash_txt_halt1420.txt` | RECONSTRUCTED Crash.txt (CRLF) | Halt 1420: layout of a real D2 1.14d crash log (Inspector header, `<Inspector.Summary:>`, `<Inspector.LineNumber>`, `<Inspector.Assertion:>` with `DBG-ADDR<%p>("%s")` frames, `Registers:`, `Threads:`); summary text, EDI and the 17-frame stack quoted in the Halt 1420 investigation notes; register values other than EDI are placeholders |
| `crash_txt_halt904_location.txt` | RECONSTRUCTED Crash.txt (CRLF) | Halt 904 with a `Location : %s, line #%d` line (format string present in Game.exe 1.14d but not referenced by its code: 1.14d Halt call sites pass an empty file name, so real files carry no location); frames from the Halt 904 exit chain, plus a non-Game frame that must be dropped |
| `crash_txt_empty.txt` | empty file | Crash.txt created but never flushed — seen during a Vita3K-only "Halt 316" investigation (map-seed-dependent, not reproduced on real hardware): the crash reporter itself was killed mid-write before it could flush |

Crash.txt format sources: format strings of Game.exe 1.14d
(`[%s] (%s) failed at %s(%i)` referenced at VA 0x4089d4 in the common
reporter, `<Inspector.LineNumber>%u`, `DBG-ADDR<%p>("%s")%s` referenced at VA
0x402dbf, `    Base:%08lXh  Size:%7lXh  Name:%-15.15s  Path:%s` referenced at
VA 0x40bc88) and the structure of a public 1.14d crash log.
