# `tools/crash` — local admin tool of the crash reports

Private side of the crash-report system (design section 6). It reads the
Cloudflare API with the admin token, **opens the sealed artifacts with the
maintainer's X25519 private key**, runs the postmortem on a dump, and drafts
public GitHub issues that carry no log, no dump and no player data.

Everything here runs on the maintainer's machine. The token and the key never
leave it: nothing in this directory is shipped to a console, to Cloudflare or
to GitHub Pages.

- API contract: `github.com/Franckrst/D2Vita-website`, `contract/` (v1, frozen).
  Where this tool and the contract disagree, the contract is right.
- Console side: `src/crashreport/`; sealed format: `contract/sealed-format.md`.

## Setup

Python 3.12 and PyNaCl 1.5 (system package: `python3-nacl`). No other
dependency; tests use `unittest`.

```sh
tools/crash/crash.py keygen          # writes the X25519 key pair, prints the public key
$EDITOR ~/.config/d2vita-crash/admin.env
chmod 600 ~/.config/d2vita-crash/admin.env
tools/crash/crash.py config          # check what is set (never prints a secret)
```

`~/.config/d2vita-crash/admin.env`:

```sh
API_BASE=https://d2vita-crash.franck-rst-c3d.workers.dev
ADMIN_TOKEN=<the token whose SHA-256 is the Worker secret ADMIN_TOKEN_SHA256>
GITHUB_TOKEN=<fine-grained token, Issues: write on the issues repository>
ISSUES_REPO=Franckrst/D2Vita
```

| File | Role |
|---|---|
| `admin.env` | the four settings above (`ISSUES_REPO` defaults to `Franckrst/D2Vita`) |
| `admin_x25519.key` | the private key that opens every artifact, 64 hex characters |
| `admin_x25519.pub` | its public key: the one built into the eboot (design 4.7) |

Both files must be mode 600: the tool refuses to read a file group or others
can read. `keygen` never overwrites an existing key. **Back the private key up
offline**: without it, no crash artifact ever uploaded can be opened again.
`D2VCRASH_CONFIG_DIR` moves the whole directory (the tests use it; the real
one is never touched by a test).

## Commands

`crash.py <command> --help` documents each one. Every command takes `--json`
and then prints the contract body itself, so a session can read it.

| Command | What it does |
|---|---|
| `list [--status --kind --build --sort --limit --cursor --all]` | signatures, one line each (`--limit` caps what is shown, see below) |
| `show <signature>` | counters per build, distinct consoles, sample, recent reports |
| `pull <signature\|report> [--dir D] [--artifact N]` | downloads the sealed pieces, checks their digest, opens them into `D` (default `./<report_id>`) with the names they had on the console, plus `claim.json` |
| `autopsy <signature\|report> [--dir D] [--symbols R] [--game-base] [--arena-host-base]` | `pull`, then `tools/autopsie_psp2dmp.py` on the dump |
| `status <signature> open\|fixed\|ignored [--version V] [--note N]` | `fixed` needs `--version`: the API detects regressions with it |
| `merge <signature> <root>` | the family goes to `root`; later claims count there |
| `resample <signature>` | asks the next console that reports it for a new sample |
| `issue <signature\|bug> [--yes] [--dry-run] [--again]` | public-safe draft, then `POST /repos/<ISSUES_REPO>/issues`, then records `issue_url` |
| `bugs [<bug>] [--status --all] [--set-status S] [--issue-url U]` | bug reports of the public site |
| `builds register [<build_id>] [--version V] [--channel C]` | declares a build; **the API refuses claims from unknown builds** |
| `forget-install <install_id> [--yes]` | GDPR erasure; repeats while the API answers 202 |
| `stats` | today's quota use; warns above 450 MB of database (D1 Free stops writing at 500 MB) |
| `settings [--accepting yes\|no] [--disable-until UNIX] [--cap NAME=VALUE]` | kill switch and daily caps |
| `serve [--port P]` | the same features in a browser, on 127.0.0.1 only |
| `keygen`, `config` | the key pair, and what is configured |

Exit codes: `0` done, `1` the operation failed (API, network, a sealed object
that will not open, a refused confirmation), `2` the command line or the
configuration is wrong.

`--limit` is ours, not the API's: contract v1 has no page size, so a request
only ever carries the parameters of the route table (`status`, `kind`,
`build`, `sort`, `cursor`) and the cap is applied to what came back. `--all`
follows `next_cursor`, and stops with an error if the API hands back the
cursor it was given.

### Reading a crash end to end

```sh
tools/crash/crash.py list --sort count           # what hurts most
tools/crash/crash.py show SZYGIRBIXGHOM3AH       # who and which builds
tools/crash/crash.py autopsy SZYGIRBIXGHOM3AH    # dump opened, postmortem run
tools/crash/crash.py issue SZYGIRBIXGHOM3AH      # draft, confirm, filed
tools/crash/crash.py status SZYGIRBIXGHOM3AH fixed --version 0.2.0
```

### Symbols

`autopsy` looks for `~/d2vita-symbols/<build_id>/nm.txt` and
`d2vita_boot.elf`, archived after each build (design 4.10);
`D2VCRASH_SYMBOLS_DIR` or `--symbols` move that root. The eboot is **not**
loaded at a fixed address, so a symbol is only printed once the bias is known:
it comes from the `module: main a l'execution=0x…` line of the report's own
`boot_progress`, or from the module list of the dump against the link address
of `d2vita_boot.elf`. Without either, the postmortem prints module offsets and
says the bias is unknown rather than a wrong name. The arena host base comes
from the `membase=0x…` line of the same log (0x84000000 by default; a real
console log has shown 0x85000000).

`--game-base` is the **load base** of `Game.exe` in the guest arena, default
`0x01900000` — the same meaning as `SessionRecord.game_base` in
`src/crashreport/cr_psp2dmp.cpp`, so the script, the console reader and the
oracle share one definition. It is not a bias: the printed VA is
`address − game_base + 0x400000` (the PE image base). Anyone scripting
`--game-base` passes the load base.

## What contract v1 does not define

Two answers of the live API have no definition in the frozen contract. The
client reads the contract's own field names and treats the rest as an
extension; neither was added to the contract, and both are a decision for its
owner:

- `DELETE /v1/admin/installs/{id}`: the contract answers 200
  `ForgetInstallResult` (`reports_deleted`, `artifacts_deleted`,
  `additionalProperties: false`) and knows no 202. `forget-install` calls again
  while the answer is 202 or says `done: false`, sums the counts of each call,
  and gives up with an error after 20. **A body whose counters are named
  otherwise (`deleted_reports`, `deleted_artifacts`) raises a protocol error
  naming the missing field**: that is the diagnostic to work from, not a client
  bug.
- `GET /v1/admin/stats`: `admin.v1#Stats` forbids extra properties and has no
  `database_bytes`, which the API sends because D1 Free refuses every write at
  500 MB. It is read when present and ignored otherwise.

## Guards on what comes back

Every value of an admin body starts life in a claim that any console on the
internet can send, and several of them become a file name, a directory, a URL
or a command line here. So the client checks the shape contract v1 gives them
before anything uses them — artifact names against the four of
`claim.v1#ArtifactName`, report, signature, bug and build ids against their
patterns, `issue_url` against `admin.v1#HttpsUrl` — and refuses the body
otherwise. On top of that, `pull` writes only under the directory it was
given, a download stops as soon as it passes the size the API said it stored
(never above the 2 MiB cap), and the UI renders an `href` only when it points
at this server or at `https://`.

## `crash serve`

```sh
tools/crash/crash.py serve            # http://127.0.0.1:8765
```

Signatures and their filters, a signature page with status, merge, resample,
pull, autopsy and the issue draft, reports, bugs, quotas. The browser talks
only to this process, which holds the tokens and the key: no secret is ever
written into a page. Two things stay on the command line on purpose: the kill
switch and the caps (`crash settings`), which a misclick must not touch, and
filing a second issue for a signature that already has one (`issue --again`).

- the listening address must be the loopback; `--host 0.0.0.0` is refused
  before anything binds;
- a request whose `Host` header is not this server is answered 403, so a page
  on the internet cannot drive the tool through the browser;
- every form carries a random token created at start-up;
- there is no JavaScript at all and every value goes through `html.escape`:
  claims and bug reports are written by strangers and are rendered as text.

## Public issues

An issue is public forever, so a draft is built from an allowlist and every
value is checked against the pattern the contract gives it (addresses, hex
codes, module names, source locations, versions) before it can reach the
text; even the grouping line is rebuilt from checked values. A draft carries
the signature id, the kind, the counters, the distinct consoles, the affected
versions, the first and last sighting and the frames — **never** a log line,
dump bytes, an install id, a report id, an admin note, or the contact of a bug
reporter.

The draft is shown for confirmation; `--yes` skips it, `--dry-run` (or
`--json` without `--yes`) stops after showing it. Once filed, `issue_url` is
recorded on the signature or the bug, and the tool refuses to file a second
one unless `--again` says so.

## Layout

| File | Role |
|---|---|
| `crash.py` | entry point |
| `d2vcrash/cli.py` | commands, text and `--json` output, exit codes |
| `d2vcrash/api.py` | typed client of every admin route; `ErrorBody` becomes an exception |
| `d2vcrash/seal.py` | D2VSEAL1: opening (and sealing, for the tests) |
| `d2vcrash/config.py` | `admin.env`, the key files, `keygen` |
| `d2vcrash/autopsy.py` | symbols, bases read from the boot log, the postmortem command |
| `d2vcrash/issue.py` | public-safe drafts and the GitHub call |
| `d2vcrash/server.py` | the local web UI |
| `d2vcrash/render.py` | the text shapes shared by the CLI and the UI |
| `tests/` | `unittest`; `fakeapi.py` answers contract bodies, `fakegithub.py` stands in for GitHub |

## Tests

```sh
python3 -m unittest discover -s tools/crash/tests          # from the repository root
python3 -m unittest discover -s tools/crash/tests -v -k Seal
```

No test ever talks to Cloudflare or to GitHub: the admin API and GitHub are
fake servers on the loopback, and the keys are either the contract vectors or
throwaway keys in a temporary directory (`HOME` included, for `keygen`).

What the suite proves, besides the commands themselves:

- `test_seal.py`: the ten sealed vectors of the contract are reproduced byte
  for byte and opened back; the twenty-one negative ones fail with the very
  result `sealed-format.md` section 6 names; the copy of the vectors in
  `tests/crashreport/vectors/` still matches the contract.
- `test_schema_checker.py`: the small JSON Schema checker the tests use
  (`jsonschema` is not installed system-wide) accepts the contract's 26 valid
  claims and rejects its 70 invalid ones at the pointer each vector names.
- `test_api.py`: every route against a fake API that **validates every body it
  sends** against `admin.v1`, so the client is proved against the contract and
  not against itself.
- `test_issue.py`: a draft built from a report stuffed with log lines, dump
  bytes, install ids and report ids contains none of them.
- `test_server.py`: `0.0.0.0` refused, hostile text escaped, no token or key in
  any page, form token required.
- `test_autopsie_script.py`: the postmortem script picks the faulting thread,
  survives a dump whose notes are announced past the end of the file, honours
  both bases and only symbolizes with a known bias. With
  `D2V_CRASH_FIXTURES=<dir>` it also reads the maintainer's real dumps, which
  never enter the repository.

The oracle that compares the script with the console reader
(`tools/tests/run_crashreport_tests.sh`, leg 3) runs both thread rules on the
synthetic dumps, and on the real ones when `D2V_CRASH_FIXTURES` is set:

```sh
D2V_CRASH_FIXTURES=/tmp tools/tests/run_crashreport_tests.sh
```

Adding `D2V_CONTRACT=<D2Vita-website>/contract` turns on leg 4, which
validates the end-to-end claims against the contract itself. That leg needs
the `jsonschema` package, which lives in the contract's own virtualenv and
nowhere else: run `make -C contract venv` in the D2Vita-website checkout once,
or leg 4 fails with `the jsonschema package is missing`. The admin suite above
needs none of it: it carries `tests/jsonschema_lite.py`, proved on the
contract's own claim vectors.
