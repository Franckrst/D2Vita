# Public-safe GitHub issue drafts (design section 6).
#
# An issue is public forever. A draft is therefore built from an allowlist of
# fields, and every value is checked against the pattern the contract gives it
# (claim.v1: addresses, hex codes, module names, source locations, versions)
# before it can reach the text. Anything that does not match is dropped, so a
# log line, a dump byte string, an install id or a report id cannot end up in
# an issue even if the API served one in a field where it does not belong.
#
# Never in a draft: log lines, dump bytes, install ids or hashes, report ids,
# artifact digests, admin notes, contact details of a bug reporter.
import datetime
import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request

GITHUB_API_ENV = "D2VCRASH_GITHUB_API"
DEFAULT_GITHUB_API = "https://api.github.com"
USER_AGENT = "d2vita-crash-admin/1"

# Patterns of contract/schemas/claim.v1.schema.json.
ADDRESS = re.compile(r"^(Game|ABS|(?!(game|abs)\+)[a-z0-9_]{1,32})\+0x(0|[1-9a-f][0-9a-f]{0,7})$")
HEX32 = re.compile(r"^0x(0|[1-9a-f][0-9a-f]{0,7})$")
MODULE = re.compile(r"^[A-Za-z0-9_.]{1,32}$")
SOURCE_LOCATION = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}:(0|[1-9][0-9]{0,9})$")
IMPORT = re.compile(r"^[A-Za-z0-9_.!@?$#]{1,128}$")
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
BUILD_ID = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}(-dirty)?$")
SIGNATURE_ID = re.compile(r"^S[A-Z2-7]{15}$")
BUG_ID = re.compile(r"^B[0-9A-Z]{8,31}$")
DECIMAL = re.compile(r"^(0|[1-9][0-9]{0,9})$")
EXIT_REASONS = ("main_thread_fault", "unshimmed_import", "fatal_app_exit", "raise_exception", "exit_process")
KINDS = ("halt", "guest_fault", "host_fault", "abnormal_exit", "hang")
REGIONS = ("eboot", "jit", "sysmodule", "unknown")
MAX_FRAMES = 16
FOOTER = ("Filed by the D2Vita crash reporter. No log, no dump and no player data are published: "
          "the pieces stay sealed for the maintainer (see the privacy page of the site).")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Never follow a redirect: it would resend the GitHub token elsewhere."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class IssueError(Exception):
    """Creating the issue failed."""


class IssueDraft:
    """What would be posted; `body` is Markdown."""

    def __init__(self, title, body, labels=None):
        self.title = title
        self.body = body
        self.labels = list(labels or ())

    def as_dict(self):
        payload = {"title": self.title, "body": self.body}
        if self.labels:
            payload["labels"] = self.labels
        return payload

    def as_text(self):
        return f"title: {self.title}\n\n{self.body}"


def _safe(value, pattern):
    """The value when it matches the contract pattern, else None."""
    return value if isinstance(value, str) and pattern.match(value) else None


def _safe_int(value, low=0, high=2 ** 32 - 1):
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value if low <= value <= high else None


def _date(unix):
    if not isinstance(unix, int) or isinstance(unix, bool):
        return None
    return datetime.datetime.fromtimestamp(unix, datetime.timezone.utc).strftime("%Y-%m-%d")


def _addresses(values, limit=MAX_FRAMES):
    out = []
    for value in values or ():
        address = _safe(value, ADDRESS)
        if address:
            out.append(address)
        if len(out) >= limit:
            break
    return out


def canon_fields(canon):
    """The canon split into its fields, each one still unchecked."""
    return canon.split("|") if isinstance(canon, str) else []


def _host_address(value):
    """("eboot", "0x2f6e6e") from a claim HostAddress, or (None, None)."""
    if not isinstance(value, dict):
        return None, None
    region = value.get("region")
    module = _safe(value.get("module"), MODULE)
    offset = _safe(value.get("offset"), HEX32)
    if region not in REGIONS or not module or not offset:
        return None, None
    return module, offset


def _features(claim):
    return claim.get("features") if isinstance(claim, dict) and isinstance(claim.get("features"), dict) else {}


def _facts(detail, claim):
    """The safe facts of a signature: every value already checked."""
    fields = canon_fields(detail.canon)
    tag = fields[0] if fields else ""
    features = _features(claim)
    facts = {"kind": detail.kind if detail.kind in KINDS else "unknown", "tag": tag, "frames": [], "details": []}

    if tag == "halt":
        code = _safe_int(features.get("code"))
        if code is None and len(fields) > 1:
            code = int(fields[1]) if DECIMAL.match(fields[1]) else None
        location = _safe(features.get("location"), SOURCE_LOCATION) or (
            _safe(fields[2], SOURCE_LOCATION) if len(fields) > 2 else None)
        facts["code"] = code
        facts["location"] = location
        facts["details"] = [("Halt code", str(code) if code is not None else None), ("Location", location)]
    elif tag == "gfault":
        exception = _safe(features.get("exception"), HEX32) or (_safe(fields[1], HEX32) if len(fields) > 1 else None)
        eip = _safe(features.get("eip"), ADDRESS) or (_safe(fields[2], ADDRESS) if len(fields) > 2 else None)
        thread = features.get("thread") if features.get("thread") in ("main", "worker") else None
        facts["eip"] = eip
        facts["exception"] = exception
        facts["details"] = [("Exception", exception), ("EIP", eip), ("Thread", thread)]
    elif tag in ("hfault_jit", "hfault", "hfault_sys", "hfault_unknown"):
        stop = _safe(features.get("stop_reason"), HEX32) or (
            _safe(fields[1], HEX32) if tag == "hfault_jit" and len(fields) > 1 else None)
        pc_module, pc_offset = _host_address(features.get("pc"))
        lr_module, lr_offset = _host_address(features.get("lr"))
        if tag in ("hfault", "hfault_unknown") and not pc_offset and len(fields) > 2:
            pc_module = "eboot" if tag == "hfault" else "unknown"
            lr_module = pc_module
            pc_offset = _safe(fields[2], HEX32)
            lr_offset = lr_offset or (_safe(fields[3], HEX32) if len(fields) > 3 else None)
        if tag == "hfault_sys" and not pc_offset and len(fields) > 2:
            pc_module = _safe(fields[1], MODULE)
            pc_offset = _safe(fields[2], HEX32)
        pc = f"{pc_module}+{pc_offset}" if pc_module and pc_offset else None
        lr = f"{lr_module}+{lr_offset}" if lr_module and lr_offset else None
        facts["pc"] = pc
        facts["lr"] = lr
        facts["pc_module"] = pc_module
        facts["pc_offset"] = pc_offset
        facts["lr_offset"] = lr_offset
        facts["build_id"] = _safe(fields[1], BUILD_ID) if len(fields) > 1 else None
        facts["stop_reason"] = stop
        facts["details"] = [("Stop reason", stop), ("PC", pc), ("LR", lr)]
    elif tag == "exit":
        reason = fields[1] if len(fields) > 1 and fields[1] in EXIT_REASONS else None
        reason = features.get("reason") if features.get("reason") in EXIT_REASONS else reason
        imported = _safe(features.get("import"), IMPORT) or (
            _safe(fields[2], IMPORT) if len(fields) > 2 and fields[2] != "-" else None)
        code = _safe_int(features.get("code"))
        facts["reason"] = reason
        facts["import"] = imported
        facts["code"] = code
        facts["details"] = [("Reason", reason), ("Import", imported),
                            ("Exit code", str(code) if code is not None else None)]
    elif tag == "hang":
        eip = _safe(features.get("eip"), ADDRESS) or (_safe(fields[1], ADDRESS) if len(fields) > 1 else None)
        beats = _safe_int(features.get("stalled_beats"), 2, 1000000)
        facts["eip"] = eip
        facts["details"] = [("EIP", eip), ("Stalled beats", str(beats) if beats is not None else None)]

    frames = _addresses(features.get("frames") or features.get("guest_frames"))
    if not frames:                                    # no claim: the canon holds the first ones
        for field in fields[1:]:
            frames = _addresses(field.split(","))
            if frames:
                break
    facts["frames"] = frames
    return facts


def safe_grouping(facts):
    """The canon rebuilt from checked values: never a field as it was received."""
    tag = facts["tag"]

    def frames(count):
        return ",".join(facts["frames"][:count]) or "-"

    def value(name):
        got = facts.get(name)
        return str(got) if got not in (None, "") else "-"

    if tag == "halt":
        return f"halt|{value('code')}|{value('location')}|{frames(3)}"
    if tag == "gfault":
        return f"gfault|{value('exception')}|{value('eip')}|{frames(2)}"
    if tag == "hfault_jit":
        return f"hfault_jit|{value('stop_reason')}|{frames(3)}"
    if tag in ("hfault", "hfault_unknown"):
        return f"{tag}|{value('build_id')}|{value('pc_offset')}|{value('lr_offset')}"
    if tag == "hfault_sys":
        return f"hfault_sys|{value('pc_module')}|{value('pc_offset')}"
    if tag == "exit":
        return f"exit|{value('reason')}|{facts.get('import') or value('code')}|{frames(1)}"
    if tag == "hang":
        return f"hang|{value('eip')}"
    return ""


def _title(detail, facts):
    kind = facts["kind"]
    tag = facts["tag"]
    first = facts["frames"][0] if facts["frames"] else None
    if tag == "halt":
        what = f"Halt {facts['code']}" if facts.get("code") is not None else "Halt"
        where = facts.get("location") or first
        head = f"{what} at {where}" if where else what
    elif tag == "gfault":
        head = "Guest fault" + (f" at {facts['eip']}" if facts.get("eip") else "")
    elif tag == "hfault_jit":
        head = "Native fault in translated code" + (f", guest {first}" if first else "")
    elif tag == "hfault":
        head = "Native fault at " + (facts.get("pc") or "eboot")
    elif tag == "hfault_sys":
        head = "Native fault in " + (facts.get("pc") or "a system module")
    elif tag == "hfault_unknown":
        head = "Native fault in an unknown region" + (f" at {facts['pc']}" if facts.get("pc") else "")
    elif tag == "exit":
        if facts.get("reason") == "unshimmed_import":
            head = "Abnormal exit: unshimmed import" + (f" {facts['import']}" if facts.get("import") else "")
        else:
            head = "Abnormal exit" + (f": {facts['reason']}" if facts.get("reason") else "")
    elif tag == "hang":
        head = "Hang" + (f" at {facts['eip']}" if facts.get("eip") else "")
    else:
        head = f"Crash ({kind})"
    signature = _safe(detail.id, SIGNATURE_ID) or "unknown signature"
    return f"{head} ({signature})"


def draft_for_signature(detail, claim=None):
    """A public-safe draft for one signature, optionally enriched by a claim."""
    facts = _facts(detail, claim if isinstance(claim, dict) else None)
    signature = _safe(detail.id, SIGNATURE_ID) or "unknown"
    versions = sorted({v for v in (b.version for b in detail.builds) if _safe(v, VERSION)})
    lines = [
        f"Signature `{signature}`, kind `{facts['kind']}`, rules v{_safe_int(detail.rules_version) or 1}.",
        "",
        f"- Occurrences: {_safe_int(detail.count) or 0} claims from {_safe_int(detail.installs) or 0} "
        f"distinct consoles",
        f"- First seen: {_date(detail.first_seen_unix) or 'unknown'} (UTC)",
        f"- Last seen: {_date(detail.last_seen_unix) or 'unknown'} (UTC)",
        f"- Affected versions: {', '.join(versions) if versions else 'unknown'}",
    ]
    known = [(label, value) for label, value in facts["details"] if value]
    if known:
        lines += ["", "### Details", ""]
        lines += [f"- {label}: `{value}`" for label, value in known]
    if facts["frames"]:
        lines += ["", "### Frames (innermost first)", "", "```"]
        lines += facts["frames"]
        lines += ["```"]
    grouping = safe_grouping(facts)
    if grouping:
        lines += ["", "### Grouping", "", f"`{grouping}`"]
    lines += ["", FOOTER]
    return IssueDraft(_title(detail, facts), "\n".join(lines))


def draft_for_bug(bug):
    """A draft from a bug report of the public site; the contact never travels."""
    bug_id = _safe(bug.id, BUG_ID) or "unknown"
    title = " ".join((bug.title or "").split())[:120] or f"Bug report {bug_id}"
    description = "\n".join(line.rstrip() for line in (bug.description or "").splitlines())[:4000]
    lines = [
        f"Reported through the bug form of the site ({bug_id}, {_date(bug.created_unix) or 'unknown date'}, "
        f"language `{bug.lang if bug.lang in ('fr', 'en') else '?'}`).",
        "",
        f"- Version given by the reporter: `{' '.join((bug.version or '').split())[:40] or 'unknown'}`",
        "",
        "### Description",
        "",
        description,
        "",
        "Filed by the D2Vita crash reporter. The contact details of the reporter, when they gave any, stay private.",
    ]
    return IssueDraft(f"{title} ({bug_id})", "\n".join(lines))


def github_api_base():
    return os.environ.get(GITHUB_API_ENV) or DEFAULT_GITHUB_API


def create_issue(repo, token, draft, api_base=None, timeout=30, opener=None):
    """POST the draft; returns the issue URL. Nothing else is ever sent."""
    base = (api_base or github_api_base()).rstrip("/")
    url = f"{base}/repos/{urllib.parse.quote(repo)}/issues"
    payload = json.dumps(draft.as_dict()).encode("utf-8")
    request = urllib.request.Request(url, data=payload, method="POST")
    request.add_header("authorization", f"Bearer {token}")
    request.add_header("accept", "application/vnd.github+json")
    request.add_header("x-github-api-version", "2022-11-28")
    request.add_header("content-type", "application/json")
    request.add_header("user-agent", USER_AGENT)
    try:
        # No redirect: it would resend the GitHub token to wherever it points.
        with (opener or urllib.request.build_opener(_NoRedirect)).open(request, timeout=timeout) as answer:
            body = json.loads(answer.read().decode("utf-8"))
    except urllib.error.HTTPError as http_error:
        raw = http_error.read().decode("utf-8", "replace")[:300]
        message = raw
        try:
            message = json.loads(raw).get("message", raw)
        except ValueError:
            pass
        raise IssueError(f"GitHub answered {http_error.code}: {message}") from None
    except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
        raise IssueError(f"GitHub could not be reached: {exc}") from exc
    except ValueError as exc:
        raise IssueError("GitHub answered something that is not JSON") from exc
    url = body.get("html_url")
    if not isinstance(url, str) or not url.startswith("https://"):
        raise IssueError("GitHub answered without an issue URL")
    return url
