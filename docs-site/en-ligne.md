# Online play

Online mode has been validated **end-to-end, on real console**, against
both a private test server and the real **official Battle.net**:
connection, realm selection, character selection, game creation, act
loading, a game actually played.

!!! danger "Access to official Battle.net: on by default"
    Access to the real Battle.net is on by default — this unofficial
    client connects to it directly, with no environment variable to
    activate. `D2_LOCAL_ONLY=1` restricts the runtime to a private/local
    server if you don't want that. Playing on your official account with an
    unofficial client carries a real risk of sanction under Blizzard's
    terms of service (Warden can be pushed in-game by the server; its
    behavior against this runtime is not proven). That's a decision for the
    player to make, not something this project encourages or guarantees to
    be risk-free.

## What's proven

| Step | Evidence (real console) |
|---|---|
| CheckRevision | **Real verification**, not a fabricated response — hash computed on the real binary, different for every seed sent by the server |
| Authenticode | **Real** PKCS#7/X.509 chain-of-trust verification — `Get-AuthenticodeSignature` on the same binary reports `Valid` on real Windows; this runtime answers the same way |
| CD keys | Recognized by the server as real keys (mechanism below) |
| Connect → realm → character → game | Full path played autonomously, including against official Battle.net |
| The game binary during play | Unchanged: 0 bytes of code modified in memory, verified by a dedicated probe |

## Principle: nothing is fabricated

Non-negotiable project rule, including where cheating would be most
tempting: **no fabricated response, no forged packet, no special memory
view to bypass an anti-cheat check.** What can't be honestly represented is
documented as a known limitation, never quietly worked around — see
[Fidelity Warden / anti-cheat](fidelite-warden.md) for the technical
backlog of gaps still open relative to a real Windows process (structured
exception handling, memory introspection). None of these gaps is exploited
to give the player an advantage; they're accepted limitations, not
shortcuts.

## The CD key mechanism

Contrary to popular belief, Diablo II **does not store its CD keys in the
registry** — `BLIZZARDKEY`, found there, is actually an RSA public key used
to sign MPQ files, not a player secret. The real secret lives encrypted in
three small files written by the original installer (inside the game's MPQ
file chain, 72 bytes each); the game decrypts them itself, through its own
code, with encryption that depends on no machine-specific data.

This port provides an honest alternative mechanism for entering your own
keys: a text file (`ux0:data/d2vita/keys.txt`, alongside the MPQs) that the
player fills in themselves. Its content is
re-encoded exactly in the format the Blizzard installer would have
produced, then **the game decrypts it itself, through its own path** —
nothing is injected into its memory after the fact. Validated end-to-end
with real keys on console: the server receives exactly the same public
values as with a classic install.

## What remains open

- **Warden**, the anti-cheat module Battle.net can push in-game: its exact
  behavior against this runtime is not proven — it has never been seen
  activating during testing so far, which doesn't prove it never would.
- A small number of known gaps relative to a real Windows process
  (structured exception handling, memory/process introspection) remain
  documented as such rather than hidden — none affects normal play today.
  Full technical detail: [Fidelity Warden / anti-cheat](fidelite-warden.md).

## To contribute to this part of the code

The network protocol, the private test server, and the A/B validation
protocol (byte-for-byte traffic comparison before/after any change
touching an active network path) are documented in the repository —
`.claude/agents/online-validation.md`.
