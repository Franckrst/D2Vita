# Local web UI of the admin tool (design section 6): 127.0.0.1 only.
#
# The browser talks to this server, and only this server talks to the API: the
# admin token, the GitHub token and the X25519 private key never leave the
# process. Everything a page shows comes from the API or from a bug reporter,
# so every value is escaped and written as text; there is no JavaScript at all,
# which is why the design can say "any claim or bug content is rendered as
# text" (section 9, XSS row).
#
# Guards: the listening address must be the loopback; a request whose Host
# header is not this server is refused (a page on the internet must not be able
# to drive the admin tool through the browser); every POST carries a random
# form token created at start-up.
import html
import json
import os
import secrets
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import api, autopsy, cli, issue, render, seal

ALLOWED_HOSTS = ("127.0.0.1", "::1")
DEFAULT_PORT = 8765
STYLE = """
body { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; margin: 1.5rem auto; max-width: 70rem;
       background: #14161a; color: #d6dae0; line-height: 1.45; }
a { color: #7fb2ff; }
h1 { font-size: 1.2rem; } h2 { font-size: 1rem; margin-top: 1.6rem; }
nav a { margin-right: 1rem; }
table { border-collapse: collapse; width: 100%; margin: 0.6rem 0; }
th, td { text-align: left; padding: 0.25rem 0.6rem; border-bottom: 1px solid #2a2f37; vertical-align: top;
         white-space: nowrap; }
td.wrap, th.wrap { white-space: normal; }
pre { background: #0f1115; padding: 0.7rem; overflow-x: auto; border: 1px solid #2a2f37; white-space: pre-wrap; }
form { display: inline-block; margin: 0.2rem 1rem 0.2rem 0; }
input, select, button { background: #0f1115; color: #d6dae0; border: 1px solid #39404a; padding: 0.2rem 0.4rem;
                        font-family: inherit; }
.warn { color: #ffcf6b; }
.error { color: #ff8c8c; }
.small { color: #8b94a2; font-size: 0.85rem; }
"""


class Refused(Exception):
    """The request is refused (bad form token, foreign Host header)."""


def esc(value):
    return html.escape("" if value is None else str(value), quote=True)


def page(title, blocks):
    return ("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            f"<title>{esc(title)}</title><style>{STYLE}</style></head><body>"
            "<nav><a href=\"/\">signatures</a><a href=\"/bugs\">bugs</a><a href=\"/stats\">quotas</a>"
            "<a href=\"/config\">configuration</a></nav>"
            f"<h1>{esc(title)}</h1>" + "".join(blocks) + "</body></html>")


def table(headers, rows):
    head = "".join(f"<th>{esc(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>" for row in rows)
    return f"<table><tr>{head}</tr>{body}</table>"


def definitions(pairs):
    rows = [(esc(label), f"<td class=\"wrap\">{esc(value)}</td>") for label, value in pairs]
    return "<table>" + "".join(f"<tr><th>{label}</th>{value}</tr>" for label, value in rows) + "</table>"


def link(href, text):
    """A link to this server, or to an https URL; anything else stays text.

    html.escape neutralises quotes and angle brackets but not a scheme, so
    `javascript:` or `data:` in an href would survive it. api.py already
    refuses an issue_url that is not an admin.v1#HttpsUrl; this is the second
    lock, on the one value of a page that is not rendered as text.
    """
    target = "" if href is None else str(href)
    local = target.startswith("/") and not target.startswith("//")
    if not local and not target.startswith("https://"):
        return esc(text)
    return f"<a href=\"{esc(target)}\">{esc(text)}</a>"


def form(action, token, fields, button):
    inputs = [f"<input type=\"hidden\" name=\"_csrf\" value=\"{esc(token)}\">"]
    for field in fields:
        kind = field.get("type", "text")
        if kind == "select":
            options = "".join(f"<option value=\"{esc(v)}\">{esc(v)}</option>" for v in field["options"])
            inputs.append(f"<select name=\"{esc(field['name'])}\">{options}</select>")
        else:
            inputs.append(f"<input type=\"text\" name=\"{esc(field['name'])}\" "
                          f"placeholder=\"{esc(field.get('placeholder', field['name']))}\" "
                          f"value=\"{esc(field.get('value', ''))}\">")
    return (f"<form method=\"post\" action=\"{esc(action)}\">" + "".join(inputs)
            + f"<button type=\"submit\">{esc(button)}</button></form>")


def pre(text):
    return f"<pre>{esc(text)}</pre>"


class Context:
    """What the handler needs: configuration, client, form token."""

    def __init__(self, cfg, client=None):
        self.cfg = cfg
        self.client = client or api.from_config(cfg)
        self.token = secrets.token_hex(16)
        self.default_dir = os.path.join(os.path.expanduser("~"), ".cache", "d2vita-crash")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "d2vita-crash-admin"

    def log_message(self, fmt, *args):
        # stderr: stdout stays the place where `crash serve` prints its address.
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    @property
    def context(self):
        return self.server.context

    # -- plumbing ----------------------------------------------------------
    def send_page(self, body, status=200):
        payload = body.encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "text/html; charset=utf-8")
        self.send_header("content-length", str(len(payload)))
        self.send_header("content-security-policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'")
        self.send_header("referrer-policy", "no-referrer")
        self.end_headers()
        self.wfile.write(payload)

    def error_page(self, status, message, title="Error"):
        self.send_page(page(title, [f"<p class=\"error\">{esc(message)}</p>"]), status)

    def check_host(self):
        """Refuse a request aimed at another name: a page on the internet must
        not be able to drive this tool through the browser."""
        host = (self.headers.get("host") or "").rsplit(":", 1)[0].strip("[]")
        if host and host not in ALLOWED_HOSTS:
            raise Refused(f"this server only answers to 127.0.0.1, not {host}")

    def read_form(self):
        length = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        fields = {k: v[0] for k, v in urllib.parse.parse_qs(raw, keep_blank_values=True).items()}
        if not secrets.compare_digest(fields.get("_csrf", ""), self.context.token):
            raise Refused("this form is not from this server (missing or stale form token)")
        return fields

    def do_GET(self):
        self.dispatch(self.get_route)

    def do_POST(self):
        self.dispatch(self.post_route)

    def dispatch(self, route):
        try:
            self.check_host()
            body = route()
        except Refused as refused:
            return self.error_page(403, str(refused), "Refused")
        except api.NotFound as missing:
            return self.error_page(404, str(missing), "Not found")
        except (api.ApiError, api.ProtocolError, api.NetworkError, seal.SealedError, issue.IssueError,
                cli.CommandError, cli.UsageError, OSError) as failure:
            return self.error_page(502, str(failure), "The API or a file failed")
        if body is None:
            return self.error_page(404, f"no page at {self.path}", "Not found")
        self.send_page(body)

    # -- routes ------------------------------------------------------------
    def get_route(self):
        url = urllib.parse.urlparse(self.path)
        query = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        parts = [p for p in url.path.split("/") if p]
        if not parts:
            return self.signatures_page(query)
        if parts[0] == "signature" and len(parts) == 2:
            return self.signature_page(parts[1])
        if parts[0] == "report" and len(parts) == 2:
            return self.report_page(parts[1])
        if parts[0] == "bugs" and len(parts) == 1:
            return self.bugs_page(query)
        if parts[0] == "bug" and len(parts) == 2:
            return self.bug_page(parts[1])
        if parts[0] == "stats":
            return self.stats_page()
        if parts[0] == "config":
            return self.config_page()
        if parts[0] == "issue" and len(parts) == 2:
            return self.issue_page(parts[1])
        return None

    def post_route(self):
        parts = [p for p in urllib.parse.urlparse(self.path).path.split("/") if p]
        fields = self.read_form()
        client = self.context.client
        if len(parts) == 3 and parts[0] == "signature":
            signature, action = parts[1], parts[2]
            if action == "status":
                patch = {"status": fields.get("status", "open")}
                if fields.get("version"):
                    patch["fixed_in_version"] = fields["version"]
                if fields.get("note"):
                    patch["note"] = fields["note"]
                client.patch_signature(signature, **patch)
                return self.signature_page(signature, banner=f"status set to {patch['status']}")
            if action == "resample":
                client.patch_signature(signature, resample=True)
                return self.signature_page(signature, banner="a new sample will be asked for")
            if action == "merge":
                client.patch_signature(signature, merged_into=fields.get("into") or None)
                return self.signature_page(signature, banner="merged")
            if action == "issue":
                return self.create_issue(signature)
            if action in ("pull", "autopsy"):
                return self.pull_page(signature, fields.get("dir"), autopsy_too=action == "autopsy")
        if len(parts) == 3 and parts[0] == "bug":
            bug_id, action = parts[1], parts[2]
            if action == "status":
                client.patch_bug(bug_id, status=fields.get("status", "open"))
                return self.bug_page(bug_id, banner="status changed")
            if action == "issue":
                return self.create_issue(bug_id)
        return None

    # -- pages -------------------------------------------------------------
    def signatures_page(self, query):
        client = self.context.client
        filters = {name: query.get(name) or None for name in ("status", "kind", "build", "sort")}
        page_of = client.list_signatures(**filters)
        rows = []
        for item in page_of.items:
            rows.append((link(f"/signature/{item.id}", item.id), esc(item.kind), esc(item.status), esc(item.count),
                         esc(item.installs), esc(render.utc(item.last_seen_unix, False)), esc(item.sample_state),
                         link(item.issue_url, "issue") if item.issue_url else "-",
                         f"<span class=\"small\">{esc(render.ellipsis(item.canon, 70))}</span>"))
        blocks = [table(("signature", "kind", "status", "count", "consoles", "last seen", "sample", "issue", "canon"),
                        rows)]
        if page_of.next_cursor:
            blocks.append(f"<p class=\"small\">more: {len(page_of.items)} shown; narrow with the filters below.</p>")
        blocks.append("<form method=\"get\" action=\"/\">"
                      "<input type=\"text\" name=\"status\" placeholder=\"status\">"
                      "<input type=\"text\" name=\"kind\" placeholder=\"kind\">"
                      "<input type=\"text\" name=\"build\" placeholder=\"build id\">"
                      "<input type=\"text\" name=\"sort\" placeholder=\"count|last_seen\">"
                      "<button type=\"submit\">filter</button></form>")
        return page("Signatures", blocks)

    def signature_page(self, signature_id, banner=None):
        detail = self.context.client.get_signature(signature_id)
        token = self.context.token
        blocks = []
        if banner:
            blocks.append(f"<p class=\"warn\">{esc(banner)}</p>")
        blocks.append(definitions([
            ("signature", detail.id), ("kind", detail.kind), ("status", detail.status),
            ("fixed in", detail.fixed_in_version or "-"), ("canon", detail.canon),
            ("count", f"{detail.count} claims from {detail.installs} console(s)"),
            ("first seen", render.utc(detail.first_seen_unix)), ("last seen", render.utc(detail.last_seen_unix)),
            ("versions", ", ".join(detail.versions()) or "-"), ("sample", render.sample_text(detail)),
            ("merged into", detail.merged_into or "-"), ("note", detail.note or "-"),
            ("rules", f"v{detail.rules_version}"),
        ]))
        if detail.issue_url:
            blocks.append(f"<p>issue: {link(detail.issue_url, detail.issue_url)}</p>")
        if detail.builds:
            blocks.append("<h2>builds</h2>" + table(
                ("build", "count", "first seen", "last seen"),
                [(esc(b.build_id), esc(b.count), esc(render.utc(b.first_seen_unix, False)),
                  esc(render.utc(b.last_seen_unix, False))) for b in detail.builds]))
        if detail.sample_artifacts:
            blocks.append("<h2>sample artifacts</h2>" + table(
                ("name", "sealed bytes", "stored", "sha256"),
                [(esc(a.name), esc(a.bytes), esc(render.utc(a.stored_unix, False)), esc(a.sha256[:16] + "..."))
                 for a in detail.sample_artifacts]))
        if detail.recent_reports:
            blocks.append("<h2>recent reports</h2>" + table(
                ("report", "build", "received", "action"),
                [(link(f"/report/{r.report_id}", r.report_id), esc(r.build_id),
                  esc(render.utc(r.received_unix, False)), esc(r.action)) for r in detail.recent_reports]))
        actions = [
            form(f"/signature/{detail.id}/status", token,
                 [{"name": "status", "type": "select", "options": ("open", "fixed", "ignored")},
                  {"name": "version", "placeholder": "fixed in version"},
                  {"name": "note", "placeholder": "note"}], "set status"),
            form(f"/signature/{detail.id}/resample", token, [], "ask for a new sample"),
            form(f"/signature/{detail.id}/merge", token, [{"name": "into", "placeholder": "root signature"}], "merge"),
            form(f"/signature/{detail.id}/pull", token,
                 [{"name": "dir", "placeholder": "directory", "value": self.pull_dir(detail.sample_report)}],
                 "open the artifacts"),
            form(f"/signature/{detail.id}/autopsy", token,
                 [{"name": "dir", "placeholder": "directory", "value": self.pull_dir(detail.sample_report)}],
                 "autopsy the dump"),
        ]
        blocks.append("<h2>actions</h2>" + "".join(actions)
                      + f"<p>{link(f'/issue/{detail.id}', 'draft a public issue')}</p>")
        return page(f"Signature {detail.id}", blocks)

    def pull_dir(self, report_id):
        return os.path.join(self.context.default_dir, report_id or "report")

    def report_page(self, report_id):
        report = self.context.client.get_report(report_id)
        blocks = [definitions([
            ("report", report.report_id), ("signature", report.signature),
            ("received", render.utc(report.received_unix)), ("action", report.action),
            ("completed", render.utc(report.completed_unix)), ("build", report.build_id or "-"),
            ("kind", report.kind or "-"), ("rules", f"v{report.rules_version}"),
        ])]
        blocks.append(f"<p>{link(f'/signature/{report.signature}', report.signature)}</p>")
        if report.artifacts:
            blocks.append("<h2>artifacts</h2>" + table(
                ("name", "sealed bytes", "stored", "sha256"),
                [(esc(a.name), esc(a.bytes), esc(render.utc(a.stored_unix, False)), esc(a.sha256[:16] + "..."))
                 for a in report.artifacts]))
        # The claim is written by a console: shown as text, never as markup.
        blocks.append("<h2>claim</h2>" + pre(json.dumps(report.claim, indent=2, ensure_ascii=False)))
        return page(f"Report {report.report_id}", blocks)

    def bugs_page(self, query):
        bugs = self.context.client.list_bugs(status=query.get("status") or None)
        rows = [(link(f"/bug/{bug.id}", bug.id), esc(bug.status), esc(render.utc(bug.created_unix, False)),
                 esc(bug.version), esc(bug.lang), link(bug.issue_url, "issue") if bug.issue_url else "-",
                 f"<span class=\"wrap\">{esc(render.ellipsis(bug.title, 80))}</span>") for bug in bugs.items]
        return page("Bug reports", [table(("bug", "status", "created", "version", "lang", "issue", "title"), rows)])

    def bug_page(self, bug_id, banner=None):
        bug = self.context.client.get_bug(bug_id)
        blocks = []
        if banner:
            blocks.append(f"<p class=\"warn\">{esc(banner)}</p>")
        blocks.append(definitions([
            ("bug", bug.id), ("status", bug.status), ("created", render.utc(bug.created_unix)),
            ("version", bug.version), ("language", bug.lang), ("contact", bug.contact or "-"),
            ("issue", bug.issue_url or "-"), ("title", bug.title),
        ]))
        blocks.append("<h2>description</h2>" + pre(bug.description))
        blocks.append("<h2>actions</h2>" + form(f"/bug/{bug.id}/status", self.context.token,
                                                [{"name": "status", "type": "select",
                                                  "options": ("open", "fixed", "ignored")}], "set status")
                      + f"<p>{link(f'/issue/{bug.id}', 'draft a public issue')}</p>")
        return page(f"Bug {bug.id}", blocks)

    def stats_page(self):
        stats = self.context.client.get_stats()
        lines, warnings = render.stats_lines(stats)
        blocks = [pre("\n".join(lines))]
        blocks += [f"<p class=\"warn\">WARNING: {esc(warning)}</p>" for warning in warnings]
        return page("Quotas", blocks)

    def config_page(self):
        described = self.context.cfg.describe()
        return page("Configuration", [definitions([
            ("config dir", described["config_dir"]), ("api base", described["api_base"] or "(unset)"),
            ("admin token", "set" if described["admin_token"] else "(unset)"),
            ("github token", "set" if described["github_token"] else "(unset)"),
            ("issues repo", described["issues_repo"]),
            ("private key", described["key_file"] or "(none: run crash keygen)"),
        ]), "<p class=\"small\">No token and no key ever reaches this page or the browser.</p>"])

    def draft_of(self, target):
        client = self.context.client
        if target.startswith("B"):
            bug = client.get_bug(target)
            return issue.draft_for_bug(bug), bug.issue_url
        detail = client.get_signature(target)
        claim = None
        report_id = detail.sample_report or (detail.recent_reports[0].report_id if detail.recent_reports else None)
        if report_id:
            try:
                claim = client.get_report(report_id).claim
            except (api.ApiError, api.ProtocolError):
                claim = None
        return issue.draft_for_signature(detail, claim), detail.issue_url

    def issue_page(self, target, banner=None):
        draft, existing = self.draft_of(target)
        blocks = []
        if banner:
            blocks.append(f"<p class=\"warn\">{esc(banner)}</p>")
        if existing:
            blocks.append(f"<p class=\"warn\">already filed: {link(existing, existing)}</p>")
        blocks.append(definitions([("repository", self.context.cfg.issues_repo), ("title", draft.title)]))
        blocks.append(pre(draft.body))
        where = "bug" if target.startswith("B") else "signature"
        blocks.append(form(f"/{where}/{target}/issue", self.context.token, [], "create this issue on GitHub"))
        return page(f"Issue draft for {target}", blocks)

    def create_issue(self, target):
        cfg = self.context.cfg
        token, repo = cfg.require_github()
        draft, existing = self.draft_of(target)
        if existing:
            # Filing twice from a click is too easy; the command line asks for
            # --again when that is really what the maintainer wants.
            raise Refused(f"{target} already points at {existing}; file another one with "
                          f"`crash issue {target} --again`")
        url = issue.create_issue(repo, token, draft)
        if target.startswith("B"):
            self.context.client.patch_bug(target, issue_url=url)
        else:
            self.context.client.patch_signature(target, issue_url=url)
        return page("Issue created", [f"<p>created {link(url, url)}</p>",
                                      f"<p>recorded on {esc(target)}</p>"])

    def pull_page(self, target, directory, autopsy_too=False):
        client = self.context.client
        cfg = self.context.cfg
        report, _ = cli.resolve_report(client, target, want_artifact="dump" if autopsy_too else None)
        directory = directory or self.pull_dir(report.report_id)
        written, claim_path = cli.download_artifacts(client, cfg, report, directory)
        blocks = [definitions([("report", report.report_id), ("directory", directory), ("claim", claim_path)]),
                  table(("artifact", "bytes", "file"),
                        [(esc(item["name"]), esc(item["bytes"]), esc(item["path"])) for item in written])]
        if autopsy_too:
            facts = autopsy.read_boot_progress(directory)
            symbols = autopsy.symbols_for(report.build_id)
            command = autopsy.build_command(os.path.join(directory, cli.ARTIFACT_FILES["dump"]),
                                            autopsy.DEFAULT_GAME_BASE,
                                            facts.get("arena_host_base", autopsy.DEFAULT_ARENA_HOST_BASE),
                                            symbols, facts.get("main_runtime"))
            code, output = autopsy.run(command)
            blocks.append("<h2>autopsy</h2>" + pre(" ".join(command)) + pre(output)
                          + f"<p class=\"small\">exit code {esc(code)}</p>")
        return page(f"Artifacts of {report.report_id}", blocks)


def make_server(cfg, host="127.0.0.1", port=DEFAULT_PORT, client=None):
    """A server bound to the loopback; any other address is refused.

    The context comes first: it reads the configuration, and a failure there
    must not leave a listening socket behind (the port would stay taken for
    the next `crash serve`).
    """
    if host not in ALLOWED_HOSTS:
        raise ValueError(f"the admin UI only listens on 127.0.0.1 (asked for {host!r}): it holds the admin token, "
                         f"the GitHub token and the private key")
    context = Context(cfg, client)
    httpd = ThreadingHTTPServer((host, port), Handler)
    httpd.context = context
    return httpd
