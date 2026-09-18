# Typed client for the admin routes of contract v1 (contract/README.md).
#
# Every route of "Local admin" is here, reading the bodies that
# contract/schemas/admin.v1.schema.json defines. A body that is not shaped as
# the contract says raises ProtocolError rather than being half-read: the
# caller then knows the API and the contract disagree.
#
# Two answers of the live API have no definition in the frozen contract and
# are handled leniently, both noted where they are read:
#   * DELETE /v1/admin/installs/{id} may answer 202 {"done": false, ...} while
#     its erasure run hits the D1 statement budget; the client calls again;
#   * GET /v1/admin/stats may add database_bytes; the client reads it when it
#     is there (D1 Free stops accepting writes at 500 MB).
import hashlib
import json
import re
import socket
import urllib.error
import urllib.parse
import urllib.request

USER_AGENT = "d2vita-crash-admin/1"
DEFAULT_TIMEOUT = 30
MAX_PAGES = 1000                     # a listing that does not end by then is broken
MAX_SEALED_BYTES = 2097152           # the dump cap of contract/README.md, the largest artifact
LOCAL_HOSTS = ("127.0.0.1", "localhost", "::1", "[::1]")
UNSET = object()                     # "argument not given", so None can mean null

# Shapes of contract v1 (claim.v1, decision.v1, bug.v1, admin.v1). Every value
# in an admin body started as a claim that any console on the internet can
# send, and several of them become a file name, a directory, a URL or a command
# line argument here: what the contract describes is checked before it is used,
# never after. `\Z` and not `$`: in Python `$` also matches before a newline.
ARTIFACT_NAMES = ("dump", "crash_txt", "crash_log", "boot_progress")
REPORT_ID = re.compile(r"^[0-7][0-9A-HJKMNP-TV-Z]{25}\Z")
SIGNATURE_ID = re.compile(r"^S[A-Z2-7]{15}\Z")
BUG_ID = re.compile(r"^B[0-9A-Z]{8,31}\Z")
BUILD_ID = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}(-dirty)?\Z")
# admin.v1#HttpsUrl: at most 300 characters in all, so 292 after "https://".
HTTPS_URL = re.compile(r"^https://[!-~]{1,292}\Z")


# --------------------------------------------------------------------- errors

class ApiError(Exception):
    """An ErrorBody (contract/README.md, error codes)."""

    def __init__(self, status, error, message, body=None):
        super().__init__(f"{status} {error}: {message}")
        self.status = status
        self.error = error
        self.message = message
        self.body = body or {}
        self.retry_after_s = self.body.get("retry_after_s")
        self.disable_until_unix = self.body.get("disable_until_unix")
        self.report_id = self.body.get("report_id")
        self.artifact = self.body.get("artifact")


class Unauthorized(ApiError):
    """401: the admin token is missing or wrong."""


class NotFound(ApiError):
    """404: unknown route or resource."""


class InvalidPayload(ApiError):
    """400: the request body or a path parameter was refused."""


class RateLimited(ApiError):
    """429: a daily cap is reached."""


class NotAccepting(ApiError):
    """503: the kill switch is on."""


class ServerError(ApiError):
    """500: unexpected server failure."""


class ProtocolError(Exception):
    """The answer does not match the contract."""


class NetworkError(Exception):
    """The API could not be reached."""


_BY_CODE = {
    "unauthorized": Unauthorized,
    "not_found": NotFound,
    "invalid_payload": InvalidPayload,
    "rate_limited": RateLimited,
    "not_accepting": NotAccepting,
    "internal_error": ServerError,
}
_BY_STATUS = {400: InvalidPayload, 401: Unauthorized, 404: NotFound, 429: RateLimited, 500: ServerError,
              503: NotAccepting}


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Never follow a redirect: it would resend the admin token elsewhere."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def build_opener():
    return urllib.request.build_opener(_NoRedirect)


# ------------------------------------------------------------------- reading

def _field(body, key, kinds, what, optional=False, null_ok=False):
    if key not in body:
        if optional:
            return None
        raise ProtocolError(f"{what}: missing {key!r}")
    value = body[key]
    if value is None and (null_ok or optional):
        return None
    if isinstance(value, bool) and bool not in kinds:
        raise ProtocolError(f"{what}: {key!r} is a boolean, expected {kinds[0].__name__}")
    if not isinstance(value, kinds):
        raise ProtocolError(f"{what}: {key!r} is {type(value).__name__}, expected {kinds[0].__name__}")
    return value


def _str(body, key, what, null_ok=False):
    return _field(body, key, (str,), what, null_ok=null_ok)


def _int(body, key, what, null_ok=False):
    return _field(body, key, (int,), what, null_ok=null_ok)


def _bool(body, key, what):
    return _field(body, key, (bool,), what)


def _list(body, key, what):
    return _field(body, key, (list,), what)


def _dict(body, key, what):
    return _field(body, key, (dict,), what)


def _shaped(body, key, pattern, what, null_ok=False):
    """A string the contract constrains: refused unless it matches."""
    value = _str(body, key, what, null_ok=null_ok)
    if value is not None and not pattern.match(value):
        raise ProtocolError(f"{what}: {key!r} is not the shape contract v1 gives it: {value[:80]!r}")
    return value


def _one_of(body, key, allowed, what, null_ok=False):
    value = _str(body, key, what, null_ok=null_ok)
    if value is not None and value not in allowed:
        raise ProtocolError(f"{what}: {key!r} is {value[:40]!r}, not one of {', '.join(allowed)}")
    return value


def _check_v(body, what):
    if not isinstance(body, dict):
        raise ProtocolError(f"{what}: body is {type(body).__name__}, expected an object")
    if body.get("v") != 1:
        raise ProtocolError(f"{what}: body has no \"v\": 1")
    return body


# -------------------------------------------------------------------- models

class StoredArtifact:
    """admin.v1#StoredArtifact: one sealed piece kept by the API."""

    def __init__(self, body):
        what = "StoredArtifact"
        # The name becomes a file name in `crash pull`: only the four names of
        # claim.v1#ArtifactName are ever accepted.
        self.name = _one_of(body, "name", ARTIFACT_NAMES, what)
        self.bytes = _int(body, "bytes", what)
        self.sha256 = _str(body, "sha256", what)
        self.stored_unix = _int(body, "stored_unix", what)


class BuildCount:
    """One row of SignatureDetail.builds."""

    def __init__(self, body):
        what = "SignatureDetail.builds[]"
        self.build_id = _shaped(body, "build_id", BUILD_ID, what)
        self.count = _int(body, "count", what)
        self.first_seen_unix = _int(body, "first_seen_unix", what)
        self.last_seen_unix = _int(body, "last_seen_unix", what)

    @property
    def version(self):
        return self.build_id.split("+", 1)[0]


class RecentReport:
    """One row of SignatureDetail.recent_reports."""

    def __init__(self, body):
        what = "SignatureDetail.recent_reports[]"
        self.report_id = _shaped(body, "report_id", REPORT_ID, what)
        self.build_id = _shaped(body, "build_id", BUILD_ID, what)
        self.received_unix = _int(body, "received_unix", what)
        self.action = _str(body, "action", what)


class SignatureSummary:
    """admin.v1#SignatureSummary."""

    WHAT = "SignatureSummary"

    def __init__(self, body):
        what = self.WHAT
        self.id = _shaped(body, "id", SIGNATURE_ID, what)
        self.kind = _str(body, "kind", what)
        self.canon = _str(body, "canon", what)
        self.status = _str(body, "status", what)
        self.count = _int(body, "count", what)
        self.installs = _int(body, "installs", what)
        self.first_seen_unix = _int(body, "first_seen_unix", what)
        self.last_seen_unix = _int(body, "last_seen_unix", what)
        self.sample_state = _str(body, "sample_state", what)
        self.fixed_in_version = _str(body, "fixed_in_version", what, null_ok=True)
        self.merged_into = _shaped(body, "merged_into", SIGNATURE_ID, what, null_ok=True)
        # Rendered as a link by the UI: only https, never another scheme.
        self.issue_url = _shaped(body, "issue_url", HTTPS_URL, what, null_ok=True)
        # VERSION part of the most recently seen build_id; null if no build is
        # registered against this signature yet.
        self.last_version = _str(body, "last_version", what, null_ok=True)
        self.raw = body


class SignatureDetail(SignatureSummary):
    """admin.v1#SignatureDetail: the summary plus what the admin tool works on."""

    WHAT = "SignatureDetail"

    def __init__(self, body):
        _check_v(body, self.WHAT)
        super().__init__(body)
        what = self.WHAT
        self.rules_version = _int(body, "rules_version", what)
        self.note = _str(body, "note", what, null_ok=True)
        self.sample_report = _shaped(body, "sample_report", REPORT_ID, what, null_ok=True)
        self.lease_report = _shaped(body, "lease_report", REPORT_ID, what, null_ok=True)
        self.lease_expires_unix = _int(body, "lease_expires_unix", what, null_ok=True)
        self.sample_artifacts = [StoredArtifact(a) for a in _list(body, "sample_artifacts", what)]
        self.builds = [BuildCount(b) for b in _list(body, "builds", what)]
        self.recent_reports = [RecentReport(r) for r in _list(body, "recent_reports", what)]

    def versions(self):
        """Affected versions (the VERSION part of every build), sorted."""
        return sorted({b.version for b in self.builds})

    def artifact(self, name):
        for stored in self.sample_artifacts:
            if stored.name == name:
                return stored
        return None


class SignatureList:
    """admin.v1#SignatureList."""

    def __init__(self, body):
        _check_v(body, "SignatureList")
        self.items = [SignatureSummary(item) for item in _list(body, "items", "SignatureList")]
        self.next_cursor = _str(body, "next_cursor", "SignatureList", null_ok=True)
        self.raw = body


class ReportDetail:
    """admin.v1#ReportDetail: the stored claim and what happened to it."""

    def __init__(self, body):
        what = "ReportDetail"
        _check_v(body, what)
        # The report id names the directory `crash pull` writes into.
        self.report_id = _shaped(body, "report_id", REPORT_ID, what)
        self.signature = _shaped(body, "signature", SIGNATURE_ID, what)
        self.install_hash = _str(body, "install_hash", what)
        self.received_unix = _int(body, "received_unix", what)
        self.rules_version = _int(body, "rules_version", what)
        self.action = _str(body, "action", what)
        self.completed_unix = _int(body, "completed_unix", what, null_ok=True)
        self.claim = _dict(body, "claim", what)
        # The build id of the claim picks the symbol directory of an autopsy.
        if "build_id" in self.claim:
            _shaped(self.claim, "build_id", BUILD_ID, f"{what}.claim")
        self.artifacts = [StoredArtifact(a) for a in _list(body, "artifacts", what)]
        self.raw = body

    @property
    def build_id(self):
        value = self.claim.get("build_id")
        return value if isinstance(value, str) else None

    @property
    def kind(self):
        value = self.claim.get("kind")
        return value if isinstance(value, str) else None

    def artifact(self, name):
        for stored in self.artifacts:
            if stored.name == name:
                return stored
        return None


class BugItem:
    """admin.v1#BugItem (and #BugDetail, which only adds v)."""

    def __init__(self, body):
        what = "BugItem"
        self.id = _shaped(body, "id", BUG_ID, what)
        self.title = _str(body, "title", what)
        self.description = _str(body, "description", what)
        self.version = _str(body, "version", what)
        self.contact = _str(body, "contact", what, null_ok=True)
        self.lang = _str(body, "lang", what)
        self.status = _str(body, "status", what)
        self.issue_url = _shaped(body, "issue_url", HTTPS_URL, what, null_ok=True)
        self.created_unix = _int(body, "created_unix", what)
        self.raw = body


class BugList:
    """admin.v1#BugList."""

    def __init__(self, body):
        _check_v(body, "BugList")
        self.items = [BugItem(item) for item in _list(body, "items", "BugList")]
        self.next_cursor = _str(body, "next_cursor", "BugList", null_ok=True)
        self.raw = body


class BuildRecord:
    """admin.v1#BuildRecord; `created` is true when the answer was a 201."""

    def __init__(self, body, status=200):
        what = "BuildRecord"
        _check_v(body, what)
        self.build_id = _str(body, "build_id", what)
        self.version = _str(body, "version", what)
        self.channel = _str(body, "channel", what)
        self.registered_unix = _int(body, "registered_unix", what)
        self.created = status == 201
        self.raw = body


class Usage:
    """admin.v1#Usage."""

    def __init__(self, body, what):
        self.used = _int(body, "used", what)
        self.cap = _int(body, "cap", what)

    @property
    def share(self):
        return self.used / self.cap if self.cap else 0.0


class Stats:
    """admin.v1#Stats, plus database_bytes when the API adds it."""

    def __init__(self, body):
        what = "Stats"
        _check_v(body, what)
        self.day = _str(body, "day", what)
        self.accepting = _bool(body, "accepting", what)
        usage = _dict(body, "usage", what)
        self.usage = {name: Usage(_dict(usage, name, "Stats.usage"), f"Stats.usage.{name}")
                      for name in ("claims", "artifact_bytes", "new_signatures", "bugs")}
        # Outside contract v1: D1 Free refuses every write at 500 MB, so the
        # API reports the size when it knows it (api/README.md).
        size = body.get("database_bytes")
        self.database_bytes = size if isinstance(size, int) and not isinstance(size, bool) else None
        self.raw = body


class Settings:
    """admin.v1#Settings."""

    def __init__(self, body):
        what = "Settings"
        _check_v(body, what)
        self.accepting = _bool(body, "accepting", what)
        self.disable_until_unix = _int(body, "disable_until_unix", what, null_ok=True)
        self.caps = _dict(body, "caps", what)
        self.raw = body


class ForgetInstallResult:
    """admin.v1#ForgetInstallResult, summed over the calls it took."""

    def __init__(self, reports_deleted, artifacts_deleted, calls):
        self.reports_deleted = reports_deleted
        self.artifacts_deleted = artifacts_deleted
        self.calls = calls


# -------------------------------------------------------------------- client

class AdminClient:
    """The admin routes, one method each."""

    def __init__(self, base_url, token, timeout=DEFAULT_TIMEOUT, opener=None):
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc:
            raise ValueError(f"API_BASE must be an http(s) URL, got {base_url!r}")
        host = parsed.hostname or ""
        if parsed.scheme == "http" and host not in LOCAL_HOSTS:
            raise ValueError(f"refusing to send the admin token in clear to {host}: use https")
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.opener = opener or build_opener()

    # -- plumbing ----------------------------------------------------------
    def _url(self, path, query=None):
        clean = {k: v for k, v in (query or {}).items() if v is not None}
        suffix = f"?{urllib.parse.urlencode(clean)}" if clean else ""
        return f"{self.base_url}{path}{suffix}"

    def _open(self, method, path, query=None, body=None, accept="application/json"):
        payload = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(self._url(path, query), data=payload, method=method)
        request.add_header("authorization", f"Bearer {self.token}")
        request.add_header("accept", accept)
        request.add_header("user-agent", USER_AGENT)
        if payload is not None:
            request.add_header("content-type", "application/json")
            request.add_header("content-length", str(len(payload)))
        try:
            return self.opener.open(request, timeout=self.timeout)
        except urllib.error.HTTPError as http_error:
            raise self._api_error(http_error) from None
        except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError) as exc:
            raise NetworkError(f"{method} {path}: {exc}") from exc

    def _api_error(self, http_error):
        raw = http_error.read()
        http_error.close()
        try:
            body = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            body = None
        if not isinstance(body, dict) or not isinstance(body.get("error"), str):
            return ApiError(http_error.code, "unexpected", (raw[:200].decode("utf-8", "replace") or http_error.reason))
        kind = _BY_CODE.get(body["error"]) or _BY_STATUS.get(http_error.code) or ApiError
        message = body.get("message") if isinstance(body.get("message"), str) else ""
        return kind(http_error.code, body["error"], message, body)

    def _json(self, method, path, query=None, body=None):
        with self._open(method, path, query, body) as answer:
            raw = answer.read()
            status = answer.status
        try:
            return json.loads(raw.decode("utf-8")), status
        except (ValueError, UnicodeDecodeError) as exc:
            raise ProtocolError(f"{method} {path}: answer is not JSON") from exc

    @staticmethod
    def _patch_body(**fields):
        body = {name: value for name, value in fields.items() if value is not UNSET}
        if not body:
            raise ValueError("nothing to change: give at least one field")
        return body

    # -- signatures --------------------------------------------------------
    def list_signatures(self, status=None, kind=None, build=None, sort=None, cursor=None):
        """One page of GET /v1/admin/signatures.

        Only the parameters of the route table exist: contract v1 has no page
        size, and a Worker that checks its query string answers 400 to an
        invented one. The API decides how big a page is; `cursor` walks them.
        """
        query = {"status": status, "kind": kind, "build": build, "sort": sort, "cursor": cursor}
        body, _ = self._json("GET", "/v1/admin/signatures", query)
        return SignatureList(body)

    def _pages(self, fetch, max_items=None):
        """Walk next_cursor, and stop rather than spin.

        An API that answers the same non-empty page with the same cursor would
        otherwise loop for ever; so would one that never says it is done.
        """
        seen, cursor, pages = 0, None, 0
        while True:
            page = fetch(cursor)
            pages += 1
            for item in page.items:
                yield item
                seen += 1
                if max_items is not None and seen >= max_items:
                    return
            following = page.next_cursor
            if not following or not page.items:
                return
            if following == cursor:
                raise ProtocolError(f"the listing hands back the cursor it was given ({following!r}): "
                                    f"the API is not paging")
            if pages >= MAX_PAGES:
                raise ProtocolError(f"the listing has not ended after {MAX_PAGES} pages (cursor {following!r})")
            cursor = following

    def iter_signatures(self, max_items=None, **filters):
        """Every signature, page after page, following next_cursor."""
        return self._pages(lambda cursor: self.list_signatures(cursor=cursor, **filters), max_items)

    def get_signature(self, signature_id):
        body, _ = self._json("GET", f"/v1/admin/signatures/{urllib.parse.quote(signature_id)}")
        return SignatureDetail(body)

    def patch_signature(self, signature_id, status=UNSET, fixed_in_version=UNSET, merged_into=UNSET,
                        issue_url=UNSET, note=UNSET, resample=UNSET):
        body = self._patch_body(status=status, fixed_in_version=fixed_in_version, merged_into=merged_into,
                                issue_url=issue_url, note=note, resample=resample)
        answer, _ = self._json("PATCH", f"/v1/admin/signatures/{urllib.parse.quote(signature_id)}", body=body)
        return SignatureDetail(answer)

    # -- reports and artifacts --------------------------------------------
    def get_report(self, report_id):
        body, _ = self._json("GET", f"/v1/admin/reports/{urllib.parse.quote(report_id)}")
        return ReportDetail(body)

    def download_artifact(self, report_id, name, sink, expected_sha256=None, chunk=64 * 1024, max_bytes=None):
        """Stream the sealed bytes into `sink`; returns how many were written.

        The artifact is never decrypted here: sealed bytes go to disk, and
        seal.py opens them with the private key. The stream is refused as soon
        as it passes `max_bytes` (the size the API stored, when the caller
        knows it) or the 2 MiB of the contract: the digest can only be checked
        once everything has been read, so the ceiling comes first.
        """
        path = f"/v1/admin/artifacts/{urllib.parse.quote(report_id)}/{urllib.parse.quote(name)}"
        ceiling = MAX_SEALED_BYTES if max_bytes is None else min(max_bytes, MAX_SEALED_BYTES)
        digest = hashlib.sha256()
        written = 0
        with self._open("GET", path, accept="application/octet-stream") as answer:
            header_digest = answer.headers.get("x-d2v-sha256")
            while True:
                piece = answer.read(chunk)
                if not piece:
                    break
                if written + len(piece) > ceiling:
                    raise ProtocolError(f"{report_id}/{name}: the answer passes {ceiling} bytes "
                                        f"(contract v1 caps a sealed artifact at {MAX_SEALED_BYTES})")
                digest.update(piece)
                sink.write(piece)
                written += len(piece)
        got = digest.hexdigest()
        for expected, where in ((expected_sha256, "the stored digest"), (header_digest, "X-D2V-SHA256")):
            if expected and expected != got:
                raise ProtocolError(f"{report_id}/{name}: sha256 {got} does not match {where} {expected}")
        return written

    def get_artifact_bytes(self, report_id, name, expected_sha256=None, max_bytes=None):
        import io
        buffer = io.BytesIO()
        self.download_artifact(report_id, name, buffer, expected_sha256=expected_sha256, max_bytes=max_bytes)
        return buffer.getvalue()

    # -- bugs --------------------------------------------------------------
    def list_bugs(self, status=None, cursor=None):
        body, _ = self._json("GET", "/v1/admin/bugs", {"status": status, "cursor": cursor})
        return BugList(body)

    def iter_bugs(self, max_items=None, **filters):
        return self._pages(lambda cursor: self.list_bugs(cursor=cursor, **filters), max_items)

    def get_bug(self, bug_id):
        body, _ = self._json("GET", f"/v1/admin/bugs/{urllib.parse.quote(bug_id)}")
        _check_v(body, "BugDetail")
        return BugItem(body)

    def patch_bug(self, bug_id, status=UNSET, issue_url=UNSET):
        body = self._patch_body(status=status, issue_url=issue_url)
        answer, _ = self._json("PATCH", f"/v1/admin/bugs/{urllib.parse.quote(bug_id)}", body=body)
        _check_v(answer, "BugDetail")
        return BugItem(answer)

    # -- builds, installs, quotas -----------------------------------------
    def register_build(self, build_id, version, channel):
        body, status = self._json("POST", "/v1/admin/builds",
                                  body={"build_id": build_id, "version": version, "channel": channel})
        return BuildRecord(body, status)

    def forget_install(self, install_id, max_calls=20):
        """Erase one installation, calling again while the API answers 202.

        The contract answers 200 ForgetInstallResult; the live API may stop
        early on its D1 statement budget and answer 202 with "done": false,
        counts being per call (api/README.md).
        """
        path = f"/v1/admin/installs/{urllib.parse.quote(install_id)}"
        reports = artifacts = 0
        for call in range(1, max_calls + 1):
            body, status = self._json("DELETE", path)
            _check_v(body, "ForgetInstallResult")
            reports += _int(body, "reports_deleted", "ForgetInstallResult")
            artifacts += _int(body, "artifacts_deleted", "ForgetInstallResult")
            if status != 202 and body.get("done") is not False:
                return ForgetInstallResult(reports, artifacts, call)
        raise ApiError(202, "incomplete", f"the erasure of {install_id} is still unfinished after {max_calls} calls; "
                                         f"deleted so far: {reports} reports, {artifacts} artifacts")

    def get_stats(self):
        body, _ = self._json("GET", "/v1/admin/stats")
        return Stats(body)

    def put_settings(self, accepting=UNSET, disable_until_unix=UNSET, caps=UNSET):
        body = self._patch_body(accepting=accepting, disable_until_unix=disable_until_unix, caps=caps)
        answer, _ = self._json("PUT", "/v1/admin/settings", body=body)
        return Settings(answer)


def from_config(cfg, timeout=DEFAULT_TIMEOUT):
    """Client built from ~/.config/d2vita-crash/admin.env."""
    from .config import ConfigError

    base_url, token = cfg.require_api()
    try:
        return AdminClient(base_url, token, timeout=timeout)
    except ValueError as exc:
        raise ConfigError(f"{cfg.env_path}: {exc}") from exc
