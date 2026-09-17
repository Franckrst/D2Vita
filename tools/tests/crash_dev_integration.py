#!/usr/bin/env python3
"""tools/tests/crash_dev_integration.py — one report against a real API.

Drives the console code (crashreport_transport_test --live-upload) against a
local `wrangler dev` of D2Vita-website/api, end to end:

  1. read .dev.client.json (admin token, response public key) written by the
     API's `npm run dev:secrets`;
  2. register a throwaway build with channel=test (the API refuses claims from
     builds it does not know);
  3. let the console send the report: claim, pieces, complete;
  4. read the stored claim back through the admin API, download one sealed
     piece and open it with the recipient key of contract/vectors/sealed.v1.json
     — it must be exactly the file the console had;
  5. erase the installation (DELETE /v1/admin/installs/{id}), so the run leaves
     nothing behind.

Usage:
  crash_dev_integration.py --url http://127.0.0.1:8787 --api-dir <D2Vita-website/api>
                           --reader <crashreport_transport_test> --work DIR --vectors DIR

Nothing here needs a Cloudflare token: `wrangler dev --local` runs on the
machine. The maintainer starts it with, in D2Vita-website/api:
  npm ci && npm run dev:secrets && npm run dev:migrate && npm run dev
"""
import argparse
import base64
import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_crash_api import open_sealed, seal  # noqa: E402  (the same D2VSEAL1 code)

CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

# What contract v1 allows in the bodies the console reads. Every schema is
# additionalProperties: false, and the console refuses anything else (the
# answer then counts as a network failure), so a member that is not here stops
# a report on a conforming console.
BODY_SHAPES = {
    "ArtifactStored": ({"v", "report_id", "name", "bytes"}, {"v", "report_id", "name", "bytes"}),
    "CompleteResponse": ({"v", "report_id", "sample_stored"}, {"v", "report_id", "sample_stored"}),
    "ErrorBody(exists)": ({"v", "error", "message", "report_id", "artifact"},
                          {"v", "error", "message", "report_id", "artifact"}),
}
ERROR_CODES = {"invalid_payload", "unauthorized", "unknown_build", "bad_token", "turnstile", "not_found",
               "method_not_allowed", "exists", "incomplete", "payload_too_large", "rate_limited", "internal_error",
               "not_accepting"}


def check_shape(label, body, shape):
    """Prints and counts what a conforming v1 console would refuse."""
    allowed, required = BODY_SHAPES[shape]
    keys = set(body)
    problems = []
    for extra in sorted(keys - allowed):
        problems.append("unknown member %r" % extra)
    for missing in sorted(required - keys):
        problems.append("missing member %r" % missing)
    if "error" in body and body["error"] not in ERROR_CODES:
        problems.append("error code %r is not one of admin.v1#ErrorBody" % body["error"])
    for p in problems:
        print("   CONTRACT: %s answers %s with %s" % (label, shape, p))
    return len(problems)


def ulid(rng):
    return "0" + "".join(rng.choice(CROCKFORD) for _ in range(25))


def request(url, token, method="GET", body=None, content_type="application/json"):
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", "Bearer " + token)
    if body is not None:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def vector_value(vectors, file_name, list_name, case_name, field):
    with open(os.path.join(vectors, file_name)) as f:
        doc = json.load(f)
    for case in doc[list_name]:
        if case["name"] == case_name:
            return case[field]
    raise SystemExit("no case %s in %s" % (case_name, file_name))


# A second report sent by this script, to read the exact bodies the console
# refuses and say which member is at fault.
def probe_conformance(base, build_id, install_id, report_id, recipient_pk, halt_code):
    piece = b"probe of the answers the console reads\n" * 4
    claim = {
        "v": 1, "report_id": report_id, "install_id": install_id, "build_id": build_id, "channel": "test",
        "platform": {"model": "vita", "fw": "3.65"},
        "session": {"started_unix": 1789284000, "uptime_s": 900, "online": False},
        "kind": "halt",
        "features": {"code": halt_code, "location": None, "frames": ["Game+0x1fedf4"]},
        "hints": [],
        "artifacts": [{"name": "crash_log", "bytes": 72 + len(piece) + 16}],
    }
    headers = {"X-D2V-Client": "d2vita/" + build_id, "X-D2V-Install": install_id,
               "Content-Type": "application/json"}
    status, body, _ = plain_request(base + "/v1/claims", "POST", json.dumps(claim).encode(), headers)
    if status != 200:
        print("   probe: the claim was refused (%d) %s" % (status, body[:160].decode("utf-8", "replace")))
        return 0
    decision = json.loads(body)
    if decision.get("action") != "upload":
        print("   probe: the API only counted this claim, nothing more to check")
        return 0
    token = decision["upload"]["token"]
    sealed = seal(piece, bytes.fromhex(recipient_pk), bytes(range(32)), bytes(range(16)))
    put_headers = dict(headers)
    put_headers["Content-Type"] = "application/octet-stream"
    put_headers["Authorization"] = "D2V-Upload " + token
    problems = 0
    url = base + "/v1/reports/%s/artifacts/crash_log" % report_id
    status, body, _ = plain_request(url, "PUT", sealed, put_headers)
    if status == 201:
        print("   probe: PUT 201 body %s" % body.decode("utf-8", "replace")[:200])
        problems += check_shape("PUT 201", json.loads(body), "ArtifactStored")
    else:
        print("   probe: PUT answered %d %s" % (status, body[:160].decode("utf-8", "replace")))
    status, body, _ = plain_request(url, "PUT", sealed, put_headers)
    if status == 409:
        print("   probe: PUT again 409 body %s" % body.decode("utf-8", "replace")[:200])
        problems += check_shape("PUT 409", json.loads(body), "ErrorBody(exists)")
    status, body, _ = plain_request(base + "/v1/reports/%s/complete" % report_id, "POST",
                                    json.dumps({"v": 1, "artifacts": ["crash_log"]}).encode(), put_headers)
    if status == 200:
        print("   probe: complete 200 body %s" % body.decode("utf-8", "replace")[:200])
        problems += check_shape("complete 200", json.loads(body), "CompleteResponse")
    else:
        print("   probe: complete answered %d %s" % (status, body[:160].decode("utf-8", "replace")))
    return problems


def verify_pieces(base, admin_token, report_id, work, recipient_sk, names, strict):
    """Reads each piece back through the admin API and opens it."""
    failures = 0
    for name in names:
        status, sealed, _ = plain_get(base + "/v1/admin/artifacts/%s/%s" % (report_id, name), admin_token)
        if status != 200:
            print("   piece %s: not stored (%d)" % (name, status))
            failures += 1 if strict else 0
            continue
        try:
            plain = open_sealed(sealed, bytes.fromhex(recipient_sk))
        except ValueError as e:
            print("   FAIL: piece %s does not open: %s" % (name, e))
            failures += 1
            continue
        with open(os.path.join(work, "pieces", name), "rb") as f:
            original = f.read()
        if plain != original:
            print("   FAIL: piece %s differs once opened (%d bytes for %d)" % (name, len(plain), len(original)))
            failures += 1
            continue
        print("   piece %s: %d sealed bytes stored, opened to the %d bytes the console had" %
              (name, len(sealed), len(plain)))
    return failures


def plain_get(url, token):
    return request(url, token)


def plain_request(url, method, body, headers):
    req = urllib.request.Request(url, data=body, method=method)
    for name, value in headers.items():
        req.add_header(name, value)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--api-dir", required=True)
    ap.add_argument("--reader", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--vectors", required=True)
    a = ap.parse_args()
    base = a.url.rstrip("/")

    client_path = os.path.join(a.api_dir, ".dev.client.json")
    if not os.path.isfile(client_path):
        print("   SKIPPED: no %s (run `npm run dev:secrets` in the API directory)" % client_path)
        return 0
    with open(client_path) as f:
        client = json.load(f)
    admin_token = client["admin_token"]
    response_pk = client["response_public_key_hex"]

    rng = random.Random()
    build_id = "0.0.%d+%012x" % (rng.randrange(100), rng.getrandbits(48))
    install_id = "%032x" % rng.getrandbits(128)
    report_id = ulid(rng)
    recipient_sk = vector_value(a.vectors, "sealed.v1.json", "cases", "one_byte", "recipient_sk_hex")
    recipient_pk = vector_value(a.vectors, "sealed.v1.json", "cases", "one_byte", "recipient_pk_hex")

    status, body, _ = request(base + "/v1/admin/builds", admin_token, "POST",
                              json.dumps({"build_id": build_id, "version": build_id.split("+")[0],
                                          "channel": "test"}).encode())
    if status not in (200, 201):
        print("   FAIL: cannot register the build (%d): %s" % (status, body[:200].decode("utf-8", "replace")))
        return 1
    print("   build %s registered (channel test)" % build_id)

    # A fresh Halt code every run: a signature the API already has a sample
    # for is only counted, and this leg is about the whole upload path.
    halt_code = rng.randrange(1000, 60000)
    run = subprocess.run([a.reader, "--live-upload", "--url", base, "--response-pk", response_pk,
                          "--recipient-pk", recipient_pk, "--work", a.work, "--build-id", build_id,
                          "--install-id", install_id, "--report-id", report_id, "--halt-code", str(halt_code)],
                         capture_output=True, text=True)
    for line in run.stdout.splitlines():
        print("   " + line)
    if run.returncode != 0:
        print("   what reached the API before the console refused an answer:")
        verify_pieces(base, admin_token, report_id, a.work, recipient_sk, ("crash_log", "boot_progress"), False)
        print("   the exact bodies the console refused:")
        problems = probe_conformance(base, build_id, install_id, ulid(rng), recipient_pk,
                                     rng.randrange(1000, 60000))
        request(base + "/v1/admin/installs/" + install_id, admin_token, "DELETE")
        if problems:
            print("   FAIL: this API does not answer contract v1 (%d point(s) above); the console is right to "
                  "refuse — it is the conformance track's to fix" % problems)
        else:
            print("   FAIL: the console side returned %d" % run.returncode)
        if run.stderr.strip():
            print("   " + run.stderr.strip()[:500])
        return 1

    status, body, _ = request(base + "/v1/admin/reports/" + report_id, admin_token)
    if status != 200:
        print("   FAIL: the API does not know the report (%d)" % status)
        return 1
    stored = json.loads(body)
    # admin.v1#ReportDetail is flat; this route is not one the console reads,
    # so a different shape is a note for the conformance track, not a failure
    # of the send.
    detail = stored.get("report", stored)
    missing = {"v", "report_id", "signature", "install_hash", "received_unix", "rules_version", "action",
               "completed_unix", "claim", "artifacts"} - set(stored)
    if missing:
        print("   NOTE: GET /v1/admin/reports/{id} is not admin.v1#ReportDetail (missing %s at the top level); "
              "admin route, the console never reads it" % ", ".join(sorted(missing)))
    print("   the API kept the claim: signature %s, action %s, kind %s" %
          (detail.get("signature"), detail.get("action"), detail.get("claim", {}).get("kind")))

    failures = verify_pieces(base, admin_token, report_id, a.work, recipient_sk,
                             ("crash_log", "boot_progress"), True)

    status, body, _ = request(base + "/v1/admin/installs/" + install_id, admin_token, "DELETE")
    print("   erasure of the test installation: %d %s" % (status, body[:120].decode("utf-8", "replace")))
    if failures:
        return 1
    print("   OK: claim, pieces and complete went through a real API")
    return 0


if __name__ == "__main__":
    sys.exit(main())
