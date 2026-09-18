# A fake admin API that answers exactly what contract/schemas/admin.v1 says.
#
# Every JSON body it sends is validated against its $def before it leaves the
# server (jsonschema_lite), and a body that fails is recorded and answered 500:
# a test that passes therefore proves the client reads contract bodies, not
# bodies shaped like the client.
#
# Two answers of the live API have no definition in the frozen contract and
# are marked below: the 202 of DELETE /v1/admin/installs/{id} (ForgetInstall
# plus "done": false) and the database_bytes of GET /v1/admin/stats. Both are
# validated with that single extra key removed.
import base64
import json
import os
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__))]

import support  # noqa: E402
import jsonschema_lite  # noqa: E402

ADMIN = "admin.v1.schema.json#/$defs/"
TOKEN = "test-admin-token"

SIGNATURE_ID = re.compile(r"^S[A-Z2-7]{15}$")
REPORT_ID = re.compile(r"^[0-7][0-9A-HJKMNP-TV-Z]{25}$")
BUG_ID = re.compile(r"^B[0-9A-Z]{8,31}$")
INSTALL_ID = re.compile(r"^[0-9a-f]{32}$")
ARTIFACT_NAMES = ("dump", "crash_txt", "crash_log", "boot_progress")


def sha256_hex(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------- fixtures

def signature_summary(id="SZYGIRBIXGHOM3AH", kind="halt", **over):
    summary = {
        "id": id,
        "kind": kind,
        "canon": "halt|1420|-|Game+0x1fedf4,Game+0x451c23,Game+0x44f570",
        "status": "open",
        "count": 7,
        "installs": 3,
        "first_seen_unix": 1789200000,
        "last_seen_unix": 1789284000,
        "sample_state": "stored",
        "fixed_in_version": None,
        "merged_into": None,
        "issue_url": None,
        "last_version": "0.1.0",
    }
    summary.update(over)
    return summary


def signature_detail(id="SZYGIRBIXGHOM3AH", **over):
    detail = dict(signature_summary(id), v=1, rules_version=1, note=None,
                  sample_report="01J9Z6T4Q8M3K7V2B5N0XWAYCD", lease_report=None, lease_expires_unix=None,
                  sample_artifacts=[{"name": "crash_txt", "bytes": 2210, "sha256": "ab" * 32, "stored_unix": 1789284100}],
                  builds=[{"build_id": "0.1.0+ab12cd34ef56", "count": 5, "first_seen_unix": 1789200000,
                           "last_seen_unix": 1789284000},
                          {"build_id": "0.1.1+0123456789ab", "count": 2, "first_seen_unix": 1789240000,
                           "last_seen_unix": 1789250000}],
                  recent_reports=[{"report_id": "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "build_id": "0.1.0+ab12cd34ef56",
                                   "received_unix": 1789284100, "action": "upload"}])
    detail.update(over)
    return detail


def claim(**over):
    body = {
        "v": 1,
        "report_id": "01J9Z6T4Q8M3K7V2B5N0XWAYCD",
        "install_id": "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e",
        "build_id": "0.1.0+ab12cd34ef56",
        "channel": "release",
        "platform": {"model": "vita", "fw": "3.65"},
        "session": {"started_unix": 1789284000, "uptime_s": 967, "online": False},
        "kind": "halt",
        "features": {"code": 1420, "location": None,
                     "frames": ["Game+0x1fedf4", "Game+0x451c23", "Game+0x44f570"]},
        "hints": ["guest_fault"],
        "artifacts": [{"name": "crash_txt", "bytes": 2210}, {"name": "crash_log", "bytes": 3104}],
    }
    body.update(over)
    return body


def report_detail(report_id="01J9Z6T4Q8M3K7V2B5N0XWAYCD", **over):
    detail = {
        "v": 1,
        "report_id": report_id,
        "signature": "SZYGIRBIXGHOM3AH",
        "install_hash": "cd" * 32,
        "received_unix": 1789284100,
        "rules_version": 1,
        "action": "upload",
        "completed_unix": 1789284200,
        "claim": claim(report_id=report_id),
        "artifacts": [{"name": "crash_txt", "bytes": 2210, "sha256": "ab" * 32, "stored_unix": 1789284100}],
    }
    detail.update(over)
    return detail


HOST_FAULT_SIGNATURE = "SHFAULTJIT23456"[:16].ljust(16, "A")
HOST_FAULT_REPORT = "01J9Z6T4Q8M3K7V2B5N0XWAYCE"


def host_fault_claim(**over):
    body = claim(
        report_id=HOST_FAULT_REPORT,
        kind="host_fault",
        features={
            "stop_reason": "0x30004",
            "thread_name": "d2_runner3",
            "pc": {"region": "jit", "module": "jit", "offset": "0x24184"},
            "lr": {"region": "jit", "module": "jit", "offset": "0x24101"},
            "guest_frames": ["Game+0x451c23", "Game+0x44f570", "Game+0x44b959"],
            "redaction": "clean",
        },
        hints=[],
        artifacts=[{"name": "dump", "bytes": 65536}, {"name": "crash_log", "bytes": 3104},
                   {"name": "boot_progress", "bytes": 278528}],
    )
    body.update(over)
    return body


def add_host_fault(dataset):
    """A host_fault signature whose sample carries a dump; returns (signature, report)."""
    dataset.signatures[HOST_FAULT_SIGNATURE] = signature_detail(
        id=HOST_FAULT_SIGNATURE, kind="host_fault",
        canon="hfault_jit|0x30004|Game+0x451c23,Game+0x44f570,Game+0x44b959",
        sample_report=HOST_FAULT_REPORT, sample_artifacts=[],
        recent_reports=[{"report_id": HOST_FAULT_REPORT, "build_id": "0.1.0+ab12cd34ef56",
                         "received_unix": 1789284100, "action": "upload"}])
    dataset.reports[HOST_FAULT_REPORT] = report_detail(
        report_id=HOST_FAULT_REPORT, signature=HOST_FAULT_SIGNATURE,
        claim=host_fault_claim(), artifacts=[])
    return HOST_FAULT_SIGNATURE, HOST_FAULT_REPORT


def bug_item(id="B7QK2M4X9ZAB", **over):
    item = {
        "id": id,
        "title": "Crash when entering the Rogue Encampment",
        "description": "The game closes after the loading screen.",
        "version": "0.1.0",
        "contact": None,
        "lang": "en",
        "status": "open",
        "issue_url": None,
        "created_unix": 1789284300,
    }
    item.update(over)
    return item


DEFAULT_CAPS = {
    "install_claims_per_day": 3,
    "install_artifact_bytes_per_day": 3145728,
    "prerelease_install_claims_per_day": 50,
    "prerelease_install_artifact_bytes_per_day": 67108864,
    "ip_claims_per_day": 10,
    "ip_bugs_per_day": 3,
    "global_claims_per_day": 2000,
    "global_artifact_bytes_per_day": 314572800,
    "global_new_signatures_per_day": 200,
    "global_bugs_per_day": 100,
}


def stats(**over):
    body = {
        "v": 1,
        "day": "2026-09-14",
        "accepting": True,
        "usage": {
            "claims": {"used": 12, "cap": 2000},
            "artifact_bytes": {"used": 1048576, "cap": 314572800},
            "new_signatures": {"used": 2, "cap": 200},
            "bugs": {"used": 0, "cap": 100},
        },
    }
    body.update(over)
    return body


def settings(**over):
    body = {"v": 1, "accepting": True, "disable_until_unix": None, "caps": dict(DEFAULT_CAPS)}
    body.update(over)
    return body


def paginate(items, cursor, data):
    """(page, next_cursor) as the contract pages: an opaque ?cursor=.

    With data.sticky_cursor set, the same non-empty page and the same cursor
    come back for ever: an API that never finishes paging.
    """
    try:
        start = int(base64.urlsafe_b64decode(cursor + "==").decode()) if cursor else 0
    except ValueError:
        start = 0
    page = items[start:start + data.page_size]
    if data.sticky_cursor:
        return (page or items[:1]), data.sticky_cursor
    following = start + data.page_size
    if following >= len(items):
        return page, None
    return page, base64.urlsafe_b64encode(str(following).encode()).decode().rstrip("=")


class Dataset:
    """What the fake API serves; tests change it before or between calls."""

    def __init__(self):
        self.signatures = {"SZYGIRBIXGHOM3AH": signature_detail()}
        self.reports = {"01J9Z6T4Q8M3K7V2B5N0XWAYCD": report_detail()}
        self.artifacts = {}                       # (report_id, name) -> sealed bytes
        self.bugs = {"B7QK2M4X9ZAB": bug_item()}
        self.builds = {}
        self.settings = settings()
        self.stats = stats()
        self.stats_extra = {}                     # e.g. {"database_bytes": ...}, outside the contract
        self.erasure_rounds = {}                  # install_id -> remaining 202 answers
        self.erasure_counts = (2, 1)              # (reports, artifacts) deleted per call
        self.page_size = 50
        # Hostile mode, to check what the client does with an API that does not
        # respect the contract (every value in a body starts in a claim that
        # any console on the internet can send):
        self.hostile_artifact_names = False       # serve any artifact name, not only claim.v1#ArtifactName
        self.sticky_cursor = None                 # answer this next_cursor on every page, on a page that is never empty


# ---------------------------------------------------------------- server

class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake-d2vita-admin/1"

    # -- plumbing ----------------------------------------------------------
    def log_message(self, *args):                 # keep the test output clean
        pass

    @property
    def state(self):
        return self.server.state

    def send_json(self, status, body, definition, extra_ok=()):
        problems = self.state.registry.errors(ADMIN + definition,
                                              {k: v for k, v in body.items() if k not in extra_ok})
        if problems:
            self.state.schema_errors.append((definition, problems))
            body, status, definition = {"v": 1, "error": "internal_error",
                                        "message": f"{definition}: {problems[0][1]}"}, 500, "ErrorBody"
        payload = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def send_error_body(self, status, code, message, **fields):
        self.send_json(status, {"v": 1, "error": code, "message": message, **fields}, "ErrorBody")

    def send_bytes(self, data, headers=()):
        self.send_response(200)
        self.send_header("content-type", "application/octet-stream")
        self.send_header("content-length", str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        length = int(self.headers.get("content-length") or 0)
        return self.rfile.read(length) if length else b""

    def json_body(self, definition):
        try:
            value = json.loads(self.read_body().decode("utf-8"))
        except ValueError:
            return None, "body is not JSON"
        problems = self.state.registry.errors(ADMIN + definition, value)
        return (value, None) if not problems else (None, f"{problems[0][0] or '/'}: {problems[0][1]}")

    def do_GET(self):
        self.dispatch("GET")

    def do_PATCH(self):
        self.dispatch("PATCH")

    def do_POST(self):
        self.dispatch("POST")

    def do_PUT(self):
        self.dispatch("PUT")

    def do_DELETE(self):
        self.dispatch("DELETE")

    # -- routing -----------------------------------------------------------
    def dispatch(self, method):
        url = urlparse(self.path)
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        self.state.requests.append({"method": method, "path": url.path, "query": query,
                                    "auth": self.headers.get("authorization")})
        forced = self.state.force_status.pop(0) if self.state.force_status else None
        if forced:
            status, code, message, fields = forced
            return self.send_error_body(status, code, message, **fields)
        location = self.state.force_redirect.pop(0) if self.state.force_redirect else None
        if location:
            self.send_response(302)
            self.send_header("location", location)
            self.send_header("content-length", "0")
            self.end_headers()
            return
        raw = self.state.force_raw.pop(0) if self.state.force_raw else None
        if raw:
            # Deliberately unvalidated: the only way to hand the client a body
            # the contract would refuse.
            status, body = raw
            payload = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            return self.wfile.write(payload)
        if self.headers.get("authorization") != f"Bearer {self.state.token}":
            return self.send_error_body(401, "unauthorized", "A valid admin token is required")
        data = self.state.data
        path = url.path
        try:
            if method == "GET" and path == "/v1/admin/signatures":
                return self.list_signatures(query)
            if method == "GET" and path.startswith("/v1/admin/signatures/"):
                return self.one_signature(path.rsplit("/", 1)[1])
            if method == "PATCH" and path.startswith("/v1/admin/signatures/"):
                return self.patch_signature(path.rsplit("/", 1)[1])
            if method == "GET" and path.startswith("/v1/admin/reports/"):
                report = data.reports.get(path.rsplit("/", 1)[1])
                if not report:
                    return self.send_error_body(404, "not_found", "No such report")
                return self.send_json(200, report, "ReportDetail")
            if method == "GET" and path.startswith("/v1/admin/artifacts/"):
                return self.artifact(path[len("/v1/admin/artifacts/"):])
            if method == "GET" and path == "/v1/admin/bugs":
                return self.list_bugs(query)
            if method == "GET" and path.startswith("/v1/admin/bugs/"):
                bug = data.bugs.get(path.rsplit("/", 1)[1])
                if not bug:
                    return self.send_error_body(404, "not_found", "No such bug")
                return self.send_json(200, dict(bug, v=1), "BugDetail")
            if method == "PATCH" and path.startswith("/v1/admin/bugs/"):
                return self.patch_bug(path.rsplit("/", 1)[1])
            if method == "POST" and path == "/v1/admin/builds":
                return self.post_build()
            if method == "DELETE" and path.startswith("/v1/admin/installs/"):
                return self.delete_install(path.rsplit("/", 1)[1])
            if method == "GET" and path == "/v1/admin/stats":
                return self.send_json(200, dict(data.stats, **data.stats_extra), "Stats",
                                      extra_ok=tuple(data.stats_extra))
            if method == "PUT" and path == "/v1/admin/settings":
                return self.put_settings()
        except _BadRequest as bad:
            return self.send_error_body(400, "invalid_payload", str(bad))
        self.send_error_body(404, "not_found", "No such route")

    # -- routes ------------------------------------------------------------
    def list_signatures(self, query):
        data = self.state.data
        # The route table of contract/README.md, and nothing else: design
        # section 5.5 makes an unknown field a 400 everywhere else.
        unknown = set(query) - {"status", "kind", "build", "sort", "cursor"}
        if unknown:
            raise _BadRequest(f"{sorted(unknown)[0]}: unknown query parameter")
        items = [dict((k, v) for k, v in detail.items() if k in signature_summary())
                 for detail in data.signatures.values()]
        if query.get("status"):
            items = [i for i in items if i["status"] == query["status"]]
        if query.get("kind"):
            items = [i for i in items if i["kind"] == query["kind"]]
        if query.get("build"):
            items = [i for i in items
                     if any(b["build_id"] == query["build"] for b in data.signatures[i["id"]]["builds"])]
        sort = query.get("sort") or "count"
        items.sort(key=lambda i: i["count"] if sort == "count" else i["last_seen_unix"], reverse=True)
        page, cursor = paginate(items, query.get("cursor"), data)
        self.send_json(200, {"v": 1, "items": page, "next_cursor": cursor}, "SignatureList")

    def one_signature(self, id):
        detail = self.state.data.signatures.get(id)
        if not SIGNATURE_ID.match(id) or not detail:
            return self.send_error_body(404, "not_found", "No such signature")
        self.send_json(200, detail, "SignatureDetail")

    def patch_signature(self, id):
        patch, problem = self.json_body("SignaturePatch")
        if problem:
            raise _BadRequest(problem)
        detail = self.state.data.signatures.get(id)
        if not detail:
            return self.send_error_body(404, "not_found", "No such signature")
        for key, value in patch.items():
            if key == "resample":
                if value:
                    detail["sample_state"] = "none"
                    detail["sample_report"] = None
                continue
            detail[key] = value
        self.state.patches.append((id, patch))
        self.send_json(200, detail, "SignatureDetail")

    def artifact(self, tail):
        report_id, _, name = tail.partition("/")
        known = name in ARTIFACT_NAMES or self.state.data.hostile_artifact_names
        if not REPORT_ID.match(report_id) or not known:
            return self.send_error_body(404, "not_found", "No such piece")
        blob = self.state.data.artifacts.get((report_id, name))
        if blob is None:
            return self.send_error_body(404, "not_found", "No such piece")
        self.send_bytes(blob, headers=(("x-d2v-sha256", sha256_hex(blob)),))

    def list_bugs(self, query):
        data = self.state.data
        unknown = set(query) - {"status", "cursor"}
        if unknown:
            raise _BadRequest(f"{sorted(unknown)[0]}: unknown query parameter")
        items = list(data.bugs.values())
        if query.get("status"):
            items = [i for i in items if i["status"] == query["status"]]
        items.sort(key=lambda i: i["created_unix"], reverse=True)
        page, cursor = paginate(items, query.get("cursor"), data)
        self.send_json(200, {"v": 1, "items": page, "next_cursor": cursor}, "BugList")

    def patch_bug(self, id):
        patch, problem = self.json_body("BugPatch")
        if problem:
            raise _BadRequest(problem)
        bug = self.state.data.bugs.get(id)
        if not bug:
            return self.send_error_body(404, "not_found", "No such bug")
        bug.update(patch)
        self.state.patches.append((id, patch))
        self.send_json(200, dict(bug, v=1), "BugDetail")

    def post_build(self):
        body, problem = self.json_body("BuildRegistration")
        if problem:
            raise _BadRequest(problem)
        created = body["build_id"] not in self.state.data.builds
        record = {"v": 1, "build_id": body["build_id"], "version": body["version"], "channel": body["channel"],
                  "registered_unix": 1789284400}
        self.state.data.builds[body["build_id"]] = record
        self.send_json(201 if created else 200, record, "BuildRecord")

    def delete_install(self, install_id):
        if not INSTALL_ID.match(install_id):
            raise _BadRequest("install_id: must be 32 lowercase hex characters")
        data = self.state.data
        left = data.erasure_rounds.get(install_id, 0)
        reports, artifacts = data.erasure_counts
        body = {"v": 1, "reports_deleted": reports, "artifacts_deleted": artifacts}
        if left > 0:
            data.erasure_rounds[install_id] = left - 1
            # Outside the frozen contract: the live API answers 202 with a
            # "done" flag while a run hits its D1 statement budget.
            return self.send_json(202, dict(body, done=False), "ForgetInstallResult", extra_ok=("done",))
        self.send_json(200, body, "ForgetInstallResult")

    def put_settings(self):
        body, problem = self.json_body("SettingsUpdate")
        if problem:
            raise _BadRequest(problem)
        current = self.state.data.settings
        for key, value in body.items():
            if key == "caps":
                current["caps"].update(value)
            else:
                current[key] = value
        self.state.patches.append(("settings", body))
        self.send_json(200, current, "Settings")


class _BadRequest(Exception):
    pass


class _State:
    def __init__(self, data, token, registry):
        self.data = data
        self.token = token
        self.registry = registry
        self.requests = []
        self.patches = []
        self.schema_errors = []
        self.force_status = []           # [(status, code, message, fields)] consumed in order
        self.force_raw = []              # [(status, body)] sent as is, without validation
        self.force_redirect = []         # [location] answered 302, to check the client refuses


class FakeAdminServer:
    """Serves the admin routes of contract v1 on 127.0.0.1."""

    def __init__(self, data=None, token=TOKEN):
        self.data = data or Dataset()
        self.registry = jsonschema_lite.Registry(support.SCHEMA_DIR)
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.state = _State(self.data, token, self.registry)
        self.token = token
        # A short poll interval: shutdown() waits for one, and the suite starts
        # a server per test.
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    @property
    def base_url(self):
        host, port = self.httpd.server_address[:2]
        return f"http://{host}:{port}"

    @property
    def requests(self):
        return self.httpd.state.requests

    @property
    def patches(self):
        return self.httpd.state.patches

    @property
    def schema_errors(self):
        return self.httpd.state.schema_errors

    def force_next(self, status, code, message, **fields):
        """Make the next request answer this error, whatever it asks."""
        self.httpd.state.force_status.append((status, code, message, fields))

    def force_body(self, status, body):
        """Make the next request answer this body, without validating it."""
        self.httpd.state.force_raw.append((status, body))

    def force_redirect(self, location):
        """Make the next request answer 302 to `location`."""
        self.httpd.state.force_redirect.append(location)

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)
