# Lexicon

Terms specific to d2vita / Diablo II. For generic engine terms (PE32, RVA,
dynarec, shim, GIL, trap, `set_alternate`...), see [winx86's
lexicon](https://winx86-136891.gitlab.io/lexique/).

**MPQ** — Blizzard's proprietary archive format holding the game's data
(graphics, sound, maps). Read with StormLib. d2vita distributes none — see
[Installation](installation.md).

**DCC** — *Direct Cell Compression*, Blizzard's proprietary animated sprite
format (characters, items). Decoded by `src/runtime/dcc_native.cpp`.

**DC6** — an older Blizzard sprite format (UI, some scenery), simpler than
DCC.

**D2NET** — Diablo II's proprietary network stack (as opposed to Battle.net
itself).

**BNCS** — *Battle.net Chat Server*, the classic Battle.net
connection/chat protocol (not the in-game protocol itself). The BNCS
handshake is what `src/platform/vita_net.cpp` must reproduce to connect —
which is why this file stays d2vita-specific rather than joining winx86.

**Warden** — Battle.net's anti-cheat system, which inspects the connected
client. A project audit established that `CheckRevision` (the function
involved) inspects neither the import table, nor loaded modules, nor
process memory in this port's context — the practical risk is therefore
judged theoretical, not zero.

**CheckRevision** — the integrity-check function called at Battle.net
login.

**`g_114`** — internal variable signaling that the loaded binary is
specifically the 1.14d patch (as opposed to other versions the project
historically targeted); gates certain shims/hooks that are only valid for
this exact version.

**Oracle (qemu-arm)** — a deterministic boot scenario replayed to verify
that a change hasn't broken anything; see [Three-level
validation](validation.md).

**Bench** — a scripted performance-measurement scenario, as opposed to a
"real play" measurement. The scripted bench historically reads ~30% above
real play — a bench number is never a play number without saying so
explicitly.

**GIL** — see winx86's lexicon. In the context of this project's
performance measurements, "GIL take" and "µs per take" come up constantly
as the unit of cost for crossing between native and translated code.

**Autopilot** — a mode built into the VPK that automatically drives the
game from the menus into play, disabled as soon as the player takes a
physical action; stays active for headless testing. See [Controller and
keyboard](controles.md).
