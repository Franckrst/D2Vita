# Privacy — crash reports

This page describes exactly what d2vita's crash-report system does, written
directly from the actual code (`src/crashreport/`), not a generic policy
template. The principle everything else follows from: **nothing leaves the
console without your explicit consent, and nothing is ever sent
automatically.**

## When a report is even proposed

Only after a real abnormal stop is detected on the following boot: a system
dump (native crash), a `Crash.txt` written by the game itself, an abnormal
exit (non-zero exit code, an unhandled import, etc.), or a "NATIVE FAULT"
line found in the boot log. Quitting normally, or with the PS button,
triggers nothing.

## What gets collected, if you agree

- An excerpt of the boot log (`boot_progress.txt`): the start and the end,
  plus the lines around the moment of the crash.
- The end of `crash.log` (the runtime's own internal log).
- The start of `Crash.txt`, the crash file Diablo II itself writes — the
  original is then deleted from the console.
- The Vita system dump (`.psp2dmp`), compressed, with a capped size.
- Technical metadata: crash kind, build version, session length, whether
  you were online, firmware version, and how many secrets were redacted (a
  count, never their content).

No screenshots, no audio recording, no background collection: gathering
happens exactly once, right after a detected crash.

## What gets stripped before anything ever leaves the console

Before anything is sent, a redaction pass finds and erases:
- the CD keys in your `keys.txt` (16 or 26 characters, with or without a
  dash, any letter case);
- the account names tied to those keys, and the last Battle.net account
  used (read from the game's own registry).

For the three text artifacts (boot log, crash log, `Crash.txt`), every
match is replaced with `X` characters before writing. The system dump is
handled differently, because rewriting bytes inside a structured binary
format is a worse risk than the problem it would solve: it is fully
decompressed and scanned for the same patterns, and if even one match is
found anywhere in it, **the entire dump is withheld and never sent** —
not partially redacted, dropped outright. Either way, the number of
matches goes into the report so redaction can be confirmed as having
worked; **the actual value never leaves process memory**, and the memory
that held it is wiped before it can be reused.

## Your consent comes first

A dialog appears on the boot following a crash, with three explicit
choices: send this once, send nothing (and discard every pending report),
or always send from now on. With no answer within a minute, nothing is
sent and the question comes back on the next boot (up to 3 times, after
which the report is dropped). This dialog **never** appears during a
scripted or automated bench session — in that case the report simply stays
pending.

## Encryption, and who can read what

A report is encrypted (a project-specific format, authenticated blocks)
before it ever leaves the console. Only the maintainer holds the
decryption key, kept solely on their own machine — never in the
repository, never on the server that receives reports. The hosting
infrastructure therefore only ever sees an unreadable encrypted blob, not
its content. As with any request on the internet, the console's IP address
is necessarily visible to the server at the moment of sending; it is not
stored alongside the report's content.

## How long it stays, and duplicates

On the console: at most 5 pending reports, 4 MiB total, 7 days of age —
beyond that, a pending report is simply dropped, never sent late. On the
server side, a report matching a crash already seen can be counted without
a full new copy being kept — storage doesn't grow linearly with how many
players hit the same bug.

## Identifiers used

Two technical identifiers travel with a report, neither tied to your
Battle.net account or a real identity:
- an install identifier, drawn at random once on first launch and kept on
  the console;
- a session identifier, drawn at random on every boot.

They exist only to link several reports from the same install together
(for example, to see that a crash keeps recurring), not to identify who
you are.

## Turning it off entirely

`D2_CRASHREPORT=0` disables collection and sending completely for the
session — no file is read, none is created. `D2_CRASHREPORT=ask` forgets a
previously-made "always send" choice, so the question comes back.

## Contact

*(to be filled in by the maintainer with a real contact address or link
before publication — left blank here rather than invented.)*
