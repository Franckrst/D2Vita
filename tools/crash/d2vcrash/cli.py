# Command line of the local admin tool (design section 6).
#
# Every command prints stable text, or the contract body itself with --json (so
# a session can read it). Secrets never reach the output: no admin token, no
# GitHub token, no private key. Exit codes: 0 done, 1 the operation failed,
# 2 the configuration or the command line is wrong.
import argparse
import json
import os
import re
import subprocess
import sys

from . import api, autopsy, config, issue, render, seal

PROG = "crash"
SIGNATURE_ID = re.compile(r"^S[A-Z2-7]{15}$")
REPORT_ID = re.compile(r"^[0-7][0-9A-HJKMNP-TV-Z]{25}$")
BUILD_ID = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}(-dirty)?$")
INSTALL_ID = re.compile(r"^[0-9a-f]{32}$")
BUG_ID = re.compile(r"^B[0-9A-Z]{8,31}$")

# Where each opened artifact lands, with the name it had on the console.
ARTIFACT_FILES = {
    "dump": "dump.psp2dmp",
    "crash_txt": "Crash.txt",
    "crash_log": "crash.log",
    "boot_progress": "boot_progress.txt",
}


class CommandError(Exception):
    """The operation failed (exit 1)."""


class UsageError(Exception):
    """The command line or the configuration is wrong (exit 2)."""


# ---------------------------------------------------------------- plumbing

def add_json_flag(parser, top_level=False):
    parser.add_argument("--json", action="store_true",
                        default=False if top_level else argparse.SUPPRESS,
                        help="print JSON instead of text")


def emit(args, payload, lines):
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2, ensure_ascii=False))
    else:
        for line in lines:
            print(line)


def confirm(question, assume_yes):
    if assume_yes:
        return True
    sys.stdout.write(f"{question} [y/N] ")
    sys.stdout.flush()
    answer = sys.stdin.readline().strip().lower()
    return answer in ("y", "yes")


def client_of(cfg):
    return api.from_config(cfg)


def resolve_report(client, target, want_artifact=None):
    """(ReportDetail, SignatureDetail or None) for a signature or a report id."""
    if REPORT_ID.match(target):
        return client.get_report(target), None
    if not SIGNATURE_ID.match(target):
        raise UsageError(f"{target!r} is neither a signature id (S + 15 characters) nor a report id (26 characters)")
    detail = client.get_signature(target)
    report_id = detail.sample_report or detail.lease_report
    if not report_id:
        raise CommandError(f"{target}: no sample is stored (sample state {detail.sample_state}); "
                           f"ask for one with: {PROG} resample {target}")
    report = client.get_report(report_id)
    if want_artifact and not report.artifact(want_artifact):
        raise CommandError(f"{target}: its sample {report_id} has no {want_artifact} artifact")
    return report, detail


def artifact_path(directory, name):
    """Where an artifact of that name is written, or a refusal.

    The name arrives from the API, and before that from a claim that any
    console on the internet can send: only the four names of
    claim.v1#ArtifactName are turned into a path, and the path is checked to
    stay directly under the directory that was asked for.
    """
    filename = ARTIFACT_FILES.get(name)
    if filename is None:
        raise CommandError(f"the API offers an artifact named {name!r}, which contract v1 does not define "
                           f"(expected one of: {', '.join(ARTIFACT_FILES)}); nothing was written")
    path = os.path.join(directory, filename)
    if os.path.dirname(os.path.realpath(path)) != os.path.realpath(directory):
        raise CommandError(f"{path} would fall outside {directory}; nothing was written")
    return path


def download_artifacts(client, cfg, report, directory, names=None):
    """Open every stored artifact of `report` into `directory`.

    The sealed bytes are checked against the digest the API stored, then
    opened with the private key; only the opened file is kept.
    """
    secret_key = cfg.secret_key()
    os.makedirs(directory, exist_ok=True)
    written = []
    for stored in report.artifacts:
        if names and stored.name not in names:
            continue
        path = artifact_path(directory, stored.name)
        try:
            sealed = client.get_artifact_bytes(report.report_id, stored.name, expected_sha256=stored.sha256,
                                               max_bytes=stored.bytes)
            plaintext = seal.open_sealed(sealed, secret_key)
        except (seal.SealedError, api.ApiError, api.ProtocolError) as exc:
            raise CommandError(f"{report.report_id}/{stored.name}: {exc}") from exc
        with open(path, "wb") as handle:
            handle.write(plaintext)
        written.append({"name": stored.name, "path": path, "bytes": len(plaintext),
                        "sealed_bytes": stored.bytes, "sha256": stored.sha256})
    claim_path = os.path.join(directory, "claim.json")
    with open(claim_path, "w", encoding="utf-8") as handle:
        json.dump(report.claim, handle, indent=2, ensure_ascii=False)
    return written, claim_path


def local_build_id():
    """<VERSION>+<12 hex of HEAD>[-dirty] from the working tree, if it can be read."""
    root = autopsy.REPO_ROOT
    version_file = os.path.join(root, "VERSION")
    if not os.path.exists(version_file):
        raise UsageError("no VERSION file in the repository: give the build id on the command line")
    with open(version_file, encoding="utf-8") as handle:
        version = handle.read().strip()
    try:
        commit = subprocess.run(["git", "-C", root, "rev-parse", "--short=12", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", root, "status", "--porcelain"],
                               capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise UsageError(f"cannot read the commit of {root}: {exc}") from exc
    return f"{version}+{commit}" + ("-dirty" if dirty else "")


# ---------------------------------------------------------------- commands

def keep(items, limit):
    """--limit is ours, not the API's: contract v1 has no page size."""
    return items[:limit] if limit is not None else items


def more_line(shown, page):
    """The line that says what was left out, if anything was."""
    if len(shown) < len(page.items):
        return [f"({len(shown)} of {len(page.items)} shown: raise --limit for the rest)"]
    if page.next_cursor:
        return [f"(more: --cursor {page.next_cursor}, or --all)"]
    return []


def cmd_list(args, cfg):
    client = client_of(cfg)
    filters = {"status": args.status, "kind": args.kind, "build": args.build, "sort": args.sort}
    if args.all:
        items = list(client.iter_signatures(max_items=args.limit, **filters))
        payload = {"v": 1, "items": [item.raw for item in items], "next_cursor": None}
        lines = render.signature_list(items)
    else:
        page = client.list_signatures(cursor=args.cursor, **filters)
        items = keep(page.items, args.limit)
        payload = page.raw if len(items) == len(page.items) else dict(page.raw, items=[i.raw for i in items])
        lines = render.signature_list(items) + more_line(items, page)
    emit(args, payload, lines)
    return 0


def cmd_show(args, cfg):
    detail = client_of(cfg).get_signature(args.signature)
    emit(args, detail.raw, render.signature_detail(detail))
    return 0


def cmd_pull(args, cfg):
    client = client_of(cfg)
    report, detail = resolve_report(client, args.target)
    directory = args.dir or os.path.join(os.getcwd(), report.report_id)
    written, claim_path = download_artifacts(client, cfg, report, directory, names=args.artifact or None)
    payload = {"report_id": report.report_id, "signature": report.signature, "dir": directory,
               "claim": claim_path, "files": written}
    lines = [f"report {report.report_id} ({report.signature}, {report.kind or '?'}, build {report.build_id or '?'})",
             f"opened into {directory}"]
    lines += [f"  {item['name']:<14} {item['bytes']:>9} bytes  {os.path.basename(item['path'])}" for item in written]
    lines.append(f"  {'claim':<14} {'':>9}        {os.path.basename(claim_path)}")
    if not written:
        lines.append("  (no stored artifact)")
    if detail is not None:
        lines.append(f"sample of {detail.id}, {detail.count} claims from {detail.installs} console(s)")
    emit(args, payload, lines)
    return 0


def cmd_autopsy(args, cfg):
    client = client_of(cfg)
    report, _ = resolve_report(client, args.target, want_artifact="dump")
    if not report.artifact("dump"):
        raise CommandError(f"{report.report_id}: this report has no dump artifact "
                           f"(only a host_fault claim with a clean redaction offers one)")
    directory = args.dir or os.path.join(os.getcwd(), report.report_id)
    download_artifacts(client, cfg, report, directory)
    dump_path = os.path.join(directory, ARTIFACT_FILES["dump"])

    facts = autopsy.read_boot_progress(directory)
    source = "boot_progress" if facts else "defaults"
    game_base = args.game_base if args.game_base is not None else autopsy.DEFAULT_GAME_BASE
    arena = args.arena_host_base
    if arena is None:
        arena = facts.get("arena_host_base", autopsy.DEFAULT_ARENA_HOST_BASE)
    if args.game_base is not None or args.arena_host_base is not None:
        source = "command line"
    symbols = autopsy.symbols_for(report.build_id, args.symbols)
    command = autopsy.build_command(dump_path, game_base, arena, symbols, facts.get("main_runtime"))
    code, output = autopsy.run(command)

    notes = []
    if not symbols:
        notes.append(f"no symbols for build {report.build_id} in "
                     f"{os.path.join(autopsy.symbols_root(args.symbols), report.build_id or '')} "
                     f"(archive nm.txt and d2vita_boot.elf there after each build)")
    if "main_runtime" not in facts and symbols.get("nm") and not symbols.get("elf"):
        notes.append("the boot log of this report does not publish the runtime address of main: "
                     "symbols stay unresolved")
    payload = {"report_id": report.report_id, "signature": report.signature, "dir": directory,
               "build_id": report.build_id, "symbols": symbols,
               "bases": {"game_base": hex(game_base), "arena_host_base": hex(arena), "source": source},
               "command": command, "exit_code": code, "output": output, "notes": notes}
    lines = [f"report {report.report_id} ({report.signature}, build {report.build_id or '?'})",
             f"dump opened into {dump_path}",
             f"bases: game 0x{game_base:x}, arena host 0x{arena:x} ({source})",
             f"symbols: {symbols.get('nm') or '-'}"]
    lines += [f"note: {note}" for note in notes]
    lines += ["", *output.splitlines()]
    emit(args, payload, lines)
    return 0 if code == 0 else 1


def cmd_status(args, cfg):
    if args.state == "fixed" and not args.version:
        raise UsageError("status fixed needs --version (the API detects regressions with it)")
    patch = {"status": args.state}
    if args.version:
        patch["fixed_in_version"] = args.version
    if args.note is not None:
        patch["note"] = args.note
    detail = client_of(cfg).patch_signature(args.signature, **patch)
    emit(args, detail.raw, [f"{detail.id} is now {detail.status}"
                            + (f" (fixed in {detail.fixed_in_version})" if detail.fixed_in_version else "")])
    return 0


def cmd_merge(args, cfg):
    detail = client_of(cfg).patch_signature(args.signature, merged_into=args.into)
    emit(args, detail.raw, [f"{detail.id} is merged into {detail.merged_into}"])
    return 0


def cmd_resample(args, cfg):
    detail = client_of(cfg).patch_signature(args.signature, resample=True)
    emit(args, detail.raw, [f"{detail.id}: a new sample will be asked from the next console that reports it "
                            f"(sample state {detail.sample_state})"])
    return 0


def cmd_issue(args, cfg):
    client = client_of(cfg)
    repo = cfg.issues_repo                   # the token is only needed to publish
    target = args.target
    if BUG_ID.match(target):
        bug = client.get_bug(target)
        existing, draft = bug.issue_url, issue.draft_for_bug(bug)
    elif SIGNATURE_ID.match(target):
        detail = client.get_signature(target)
        claim = None
        report_id = detail.sample_report or (detail.recent_reports[0].report_id if detail.recent_reports else None)
        if report_id:
            try:
                claim = client.get_report(report_id).claim
            except (api.ApiError, api.ProtocolError):
                claim = None                       # the canon alone still describes the bug
        existing, draft = detail.issue_url, issue.draft_for_signature(detail, claim)
    else:
        raise UsageError(f"{target!r} is neither a signature id nor a bug id (B...)")
    if existing and not args.again:
        raise CommandError(f"{target} already points at {existing}; pass --again to open another issue")

    dry_run = args.dry_run or (getattr(args, "json", False) and not args.yes)
    payload = {"target": target, "repo": repo, "title": draft.title, "body": draft.body,
               "created": False, "issue_url": None}
    if dry_run:
        emit(args, payload, ["(draft only, nothing was created)", "", draft.as_text()])
        return 0
    token, _ = cfg.require_github()
    if not args.yes:
        print(draft.as_text())
        print("")
        if not confirm(f"Create this issue on {repo}?", False):
            raise CommandError("nothing was created")
    try:
        url = issue.create_issue(repo, token, draft)
    except issue.IssueError as exc:
        raise CommandError(str(exc)) from exc
    if BUG_ID.match(target):
        client.patch_bug(target, issue_url=url)
    else:
        client.patch_signature(target, issue_url=url)
    payload.update(created=True, issue_url=url)
    emit(args, payload, [f"created {url}", f"recorded on {target}"])
    return 0


def cmd_bugs(args, cfg):
    client = client_of(cfg)
    if args.bug:
        if args.set_status or args.issue_url is not None:
            patch = {}
            if args.set_status:
                patch["status"] = args.set_status
            if args.issue_url is not None:
                patch["issue_url"] = args.issue_url or None
            bug = client.patch_bug(args.bug, **patch)
        else:
            bug = client.get_bug(args.bug)
        emit(args, bug.raw, render.bug_detail(bug))
        return 0
    if args.all:
        items = list(client.iter_bugs(max_items=args.limit, status=args.status))
        payload = {"v": 1, "items": [bug.raw for bug in items], "next_cursor": None}
        lines = render.bug_list(items)
    else:
        page = client.list_bugs(status=args.status, cursor=args.cursor)
        items = keep(page.items, args.limit)
        payload = page.raw if len(items) == len(page.items) else dict(page.raw, items=[i.raw for i in items])
        lines = render.bug_list(items) + more_line(items, page)
    emit(args, payload, lines)
    return 0


def cmd_builds(args, cfg):
    build_id = args.build_id or local_build_id()
    if not BUILD_ID.match(build_id):
        raise UsageError(f"{build_id!r} is not a build id (<VERSION>+<12 hex>[-dirty])")
    version = args.version or build_id.split("+", 1)[0]
    record = client_of(cfg).register_build(build_id, version, args.channel)
    payload = dict(record.raw, created=record.created)
    emit(args, payload, [f"{record.build_id} {'registered' if record.created else 'already registered'} "
                         f"({record.version}, {record.channel})"])
    return 0


def cmd_forget_install(args, cfg):
    if not INSTALL_ID.match(args.install_id):
        raise UsageError("an install id is 32 lowercase hex characters (the one the dialog showed the player)")
    question = (f"Erase every claim and artifact of installation {args.install_id}? "
                f"This cannot be undone.")
    if not confirm(question, args.yes):
        raise CommandError("nothing was erased")
    result = client_of(cfg).forget_install(args.install_id)
    payload = {"v": 1, "install_id": args.install_id, "reports_deleted": result.reports_deleted,
               "artifacts_deleted": result.artifacts_deleted, "calls": result.calls}
    emit(args, payload, [f"erased {result.reports_deleted} report(s) and {result.artifacts_deleted} artifact(s) "
                         f"in {result.calls} calls"])
    return 0


def cmd_stats(args, cfg):
    stats = client_of(cfg).get_stats()
    lines, warnings = render.stats_lines(stats)
    emit(args, dict(stats.raw, warnings=warnings), lines + [f"WARNING: {w}" for w in warnings])
    return 0


def cmd_serve(args, cfg):
    from . import server                      # imported late: it pulls in http.server

    try:
        httpd = server.make_server(cfg, host=args.host, port=args.port)
    except ValueError as exc:
        raise UsageError(str(exc)) from exc
    host, port = httpd.server_address[:2]
    print(f"admin UI on http://{host}:{port} (Ctrl-C to stop)")
    print("The browser never receives the admin token, the GitHub token or the private key.")
    try:
        httpd.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        print("")
    finally:
        httpd.server_close()
    return 0


CAP_NAMES = ("install_claims_per_day", "install_artifact_bytes_per_day", "prerelease_install_claims_per_day",
             "prerelease_install_artifact_bytes_per_day", "ip_claims_per_day", "ip_bugs_per_day",
             "global_claims_per_day", "global_artifact_bytes_per_day", "global_new_signatures_per_day",
             "global_bugs_per_day")


def cmd_settings(args, cfg):
    """The kill switch and the daily caps (admin.v1#SettingsUpdate)."""
    patch = {}
    if args.accepting is not None:
        patch["accepting"] = args.accepting == "yes"
    if args.disable_until is not None:
        patch["disable_until_unix"] = args.disable_until or None
    caps = {}
    for pair in args.cap or ():
        name, _, value = pair.partition("=")
        if name not in CAP_NAMES or not value.isdigit():
            raise UsageError(f"--cap wants <name>=<number>, name being one of: {', '.join(CAP_NAMES)}")
        caps[name] = int(value)
    if caps:
        patch["caps"] = caps
    if not patch:
        raise UsageError("nothing to change: give --accepting, --disable-until or --cap")
    settings = client_of(cfg).put_settings(**patch)
    lines = [f"accepting: {'yes' if settings.accepting else 'NO - consoles are answered 503 not accepting'}",
             f"disabled until: {render.utc(settings.disable_until_unix)}"]
    lines += [f"  {name:<42} {settings.caps.get(name, '-')}" for name in CAP_NAMES]
    emit(args, settings.raw, lines)
    return 0


def cmd_keygen(args, cfg):
    public_key, key_path = config.generate_key_pair(cfg.config_dir)
    emit(args, {"public_key": public_key, "key_file": key_path, "config_dir": cfg.config_dir}, [
        f"public key: {public_key}",
        f"private key written to {key_path} (mode 600)",
        "Back up the private key offline: without it no crash artifact can ever be opened.",
        "Build the public key into the eboot (design section 4.7).",
    ])
    return 0


def cmd_config(args, cfg):
    described = cfg.describe()
    emit(args, described, [
        f"config dir:   {described['config_dir']}",
        f"api base:     {described['api_base'] or '(unset)'}",
        f"admin token:  {'set' if described['admin_token'] else '(unset)'}",
        f"github token: {'set' if described['github_token'] else '(unset)'}",
        f"issues repo:  {described['issues_repo']}",
        f"private key:  {described['key_file'] or '(none: run crash keygen)'}",
    ])
    return 0


# ------------------------------------------------------------------ parser

def build_parser():
    parser = argparse.ArgumentParser(prog=PROG, description="D2Vita crash reports, local admin tool")
    parser.add_argument("--config-dir", default=None,
                        help="directory of admin.env and admin_x25519.key (default: ~/.config/d2vita-crash)")
    add_json_flag(parser, top_level=True)
    subparsers = parser.add_subparsers(dest="command", required=True)

    listing = subparsers.add_parser("list", help="list signatures")
    listing.add_argument("--status", choices=("open", "fixed", "ignored", "regressed"))
    listing.add_argument("--kind", choices=("halt", "guest_fault", "host_fault", "abnormal_exit", "hang"))
    listing.add_argument("--build", help="only signatures seen on this build id")
    listing.add_argument("--sort", choices=("count", "last_seen"))
    listing.add_argument("--limit", type=int, help="show at most this many (ours: the API chooses the page size)")
    listing.add_argument("--cursor", help="continue a listing")
    listing.add_argument("--all", action="store_true", help="follow every page")
    add_json_flag(listing)
    listing.set_defaults(run=cmd_list)

    show = subparsers.add_parser("show", help="one signature in detail")
    show.add_argument("signature")
    add_json_flag(show)
    show.set_defaults(run=cmd_show)

    pull = subparsers.add_parser("pull", help="download and open the artifacts of a signature or report")
    pull.add_argument("target", help="signature id or report id")
    pull.add_argument("--dir", help="where to write (default: ./<report_id>)")
    pull.add_argument("--artifact", action="append", choices=tuple(ARTIFACT_FILES),
                      help="only this artifact (repeatable)")
    add_json_flag(pull)
    pull.set_defaults(run=cmd_pull)

    autopsy_cmd = subparsers.add_parser("autopsy", help="open the dump of a signature and run the postmortem")
    autopsy_cmd.add_argument("target", help="signature id or report id")
    autopsy_cmd.add_argument("--dir", help="where to write (default: ./<report_id>)")
    autopsy_cmd.add_argument("--symbols", help="symbol root (default: $%s, else ~/d2vita-symbols)"
                                               % autopsy.SYMBOLS_DIR_ENV)
    autopsy_cmd.add_argument("--game-base", type=lambda v: int(v, 0), help="default 0x%x" % autopsy.DEFAULT_GAME_BASE)
    autopsy_cmd.add_argument("--arena-host-base", type=lambda v: int(v, 0),
                             help="membase of the boot log; default 0x%x" % autopsy.DEFAULT_ARENA_HOST_BASE)
    add_json_flag(autopsy_cmd)
    autopsy_cmd.set_defaults(run=cmd_autopsy)

    status = subparsers.add_parser("status", help="change the status of a signature")
    status.add_argument("signature")
    status.add_argument("state", choices=("open", "fixed", "ignored"))
    status.add_argument("--version", help="version the fix ships in (required for fixed)")
    status.add_argument("--note", help="free note kept with the signature")
    add_json_flag(status)
    status.set_defaults(run=cmd_status)

    merge = subparsers.add_parser("merge", help="merge a signature into another")
    merge.add_argument("signature")
    merge.add_argument("into", help="signature that becomes the root")
    add_json_flag(merge)
    merge.set_defaults(run=cmd_merge)

    resample = subparsers.add_parser("resample", help="ask for a new sample of a signature")
    resample.add_argument("signature")
    add_json_flag(resample)
    resample.set_defaults(run=cmd_resample)

    issue_cmd = subparsers.add_parser("issue", help="draft and create a public GitHub issue")
    issue_cmd.add_argument("target", help="signature id or bug id")
    issue_cmd.add_argument("--yes", action="store_true", help="create without showing the draft first")
    issue_cmd.add_argument("--dry-run", action="store_true", help="show the draft and stop")
    issue_cmd.add_argument("--again", action="store_true", help="open another issue for something that has one")
    add_json_flag(issue_cmd)
    issue_cmd.set_defaults(run=cmd_issue)

    bugs = subparsers.add_parser("bugs", help="bug reports from the public site")
    bugs.add_argument("bug", nargs="?", help="one bug id; without it, the list")
    bugs.add_argument("--status", choices=("open", "fixed", "ignored"))
    bugs.add_argument("--limit", type=int, help="show at most this many")
    bugs.add_argument("--cursor")
    bugs.add_argument("--all", action="store_true", help="follow every page")
    bugs.add_argument("--set-status", choices=("open", "fixed", "ignored"), help="change the status of one bug")
    bugs.add_argument("--issue-url", help="record the issue of one bug (empty string clears it)")
    add_json_flag(bugs)
    bugs.set_defaults(run=cmd_bugs)

    builds = subparsers.add_parser("builds", help="builds known to the API")
    build_actions = builds.add_subparsers(dest="action", required=True)
    register = build_actions.add_parser("register", help="declare a build (the API refuses unknown ones)")
    register.add_argument("build_id", nargs="?", help="default: VERSION + the commit of this working tree")
    register.add_argument("--version", help="default: the VERSION part of the build id")
    register.add_argument("--channel", choices=("release", "dev", "test"), default="dev")
    add_json_flag(register)
    register.set_defaults(run=cmd_builds)

    forget = subparsers.add_parser("forget-install", help="erase everything one installation ever sent")
    forget.add_argument("install_id")
    forget.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    add_json_flag(forget)
    forget.set_defaults(run=cmd_forget_install)

    stats = subparsers.add_parser("stats", help="today's quota use")
    add_json_flag(stats)
    stats.set_defaults(run=cmd_stats)

    settings = subparsers.add_parser("settings", help="kill switch and daily caps")
    settings.add_argument("--accepting", choices=("yes", "no"), help="no: every console claim is answered 503")
    settings.add_argument("--disable-until", type=int, metavar="UNIX",
                          help="Unix time until which consoles stay quiet (0 clears it)")
    settings.add_argument("--cap", action="append", metavar="NAME=VALUE", help="one daily cap (repeatable)")
    add_json_flag(settings)
    settings.set_defaults(run=cmd_settings)

    serve = subparsers.add_parser("serve", help="local web UI, on 127.0.0.1 only")
    serve.add_argument("--host", default="127.0.0.1", help="must be a loopback address")
    serve.add_argument("--port", type=int, default=8765)
    add_json_flag(serve)
    serve.set_defaults(run=cmd_serve)

    keygen = subparsers.add_parser("keygen", help="create the maintainer X25519 key pair")
    add_json_flag(keygen)
    keygen.set_defaults(run=cmd_keygen)

    show_config = subparsers.add_parser("config", help="show the local configuration (never its secrets)")
    add_json_flag(show_config)
    show_config.set_defaults(run=cmd_config)

    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        cfg = config.load(args.config_dir)
        return args.run(args, cfg)
    except (config.ConfigError, UsageError) as exc:
        print(f"{PROG}: {exc}", file=sys.stderr)
        return 2
    except (api.ApiError, api.ProtocolError, api.NetworkError, seal.SealedError, CommandError, OSError) as exc:
        print(f"{PROG}: {exc}", file=sys.stderr)
        return 1
    except BrokenPipeError:                      # `crash list | head`
        return 0
