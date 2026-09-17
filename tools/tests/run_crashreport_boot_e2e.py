#!/usr/bin/env python3
"""tools/tests/run_crashreport_boot_e2e.py — crash-report boot-wiring
properties, proven against a real (local) API, that
crashreport_transport_test.cpp's --live-upload does not cover (it
fabricates a ReportRecord directly to test only the upload state machine).
This drives tools/tests/crashreport_boot_e2e
(built from the same crashreport library as the console, through the POSIX
IoApi/NetApi) to exercise the EVIDENCE side for real:

  1. d2cr_boot_collect() reads the previous session's boot_progress.txt and
     copies evidence into the outbox BEFORE the caller performs the
     boot_progress.txt rotation (d2_boot_config.cpp:209-211) that would
     otherwise destroy it — proven by doing the exact rotation ourselves,
     in between, and then checking the outbox still has it;
  2. claim + pieces + complete reach the real API, and the pieces the admin
     API returns open (decrypt) to EXACTLY the bytes the console had, before
     any sealing;
  3. rebooting with the same underlying fault (same eip, i.e. same
     gfault|<eip>|<frames> canon) makes the second report count_only
     (ReportOutcome::Deleted, completed=0), not a second stored sample;
  4. D2SCRIPT set => should_suppress_dialog() true => no dialog, no network,
     report stays pending (the ABSOLUTE RULE, checked via the actual
     function cr_boot.cpp and cr_consent_vita.cpp both call);
  5. D2_CRASHREPORT=0 => d2cr_boot_collect()'s own early return => nothing
     collected at all (the outbox stays empty even though real evidence is
     sitting right there on disk).

Usage:
  run_crashreport_boot_e2e.py --reader BIN --api-url URL --api-dir DIR
                              --vectors DIR --fixtures DIR [--work DIR]

Prints SKIPPED (exit 0) if api-dir has no .dev.client.json (wrangler dev not
set up) — same convention as crash_dev_integration.py.
"""
import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crash_dev_integration import request, vector_value, verify_pieces  # noqa: E402


def run(reader, *args):
    r = subprocess.run([reader] + list(args), capture_output=True, text=True)
    print("   $ %s %s" % (os.path.basename(reader), " ".join(args)))
    for line in r.stdout.splitlines():
        print("     " + line)
    if r.returncode != 0:
        for line in r.stderr.splitlines():
            print("     stderr: " + line)
    return r


def raise_claim_caps(base, admin_token):
    """Test/dev builds already get a relaxed PER-INSTALL claim cap (limits.ts
    installCaps() picks prerelease_install_claims_per_day for channel="test",
    which register_build() below always uses), but the PER-IP and GLOBAL daily
    caps (ip_claims_per_day, global_claims_per_day) are not build-aware, and
    they are NOT reset between runs of this script: they live in the same
    local D1 under .wrangler/state, keyed by the real UTC day. Properties
    1-3 each spend one real claim (mode="always"), against both counters, so
    a handful of manual re-runs against the same persisted local API state on
    the same day -- this project's own iterative same-day testing habit -- is
    enough to exhaust ip_claims_per_day (default 10) and have some LATER run
    fail with outcome=gated/"rate limited" instead of the property it is
    actually checking, which is not a regression in the boot-wiring logic
    this harness exists to prove.
    Raised here, once, through the same admin API a real maintainer would
    use (PUT /v1/admin/settings), rather than by reaching into D1 by hand:
    this persists as a setting (like every other admin.v1 cap override), so
    it also fixes every later run against this same local D1 -- not only
    this one -- until an admin lowers it again."""
    status, body, _ = request(base + "/v1/admin/settings", admin_token, "PUT",
                              json.dumps({"caps": {"ip_claims_per_day": 1_000_000,
                                                    "global_claims_per_day": 1_000_000}}).encode())
    if status != 200:
        raise SystemExit("cannot raise claim caps for the test run (%d): %r" % (status, body[:200]))


def register_build(base, admin_token, build_id):
    status, body, _ = request(base + "/v1/admin/builds", admin_token, "POST",
                              json.dumps({"build_id": build_id, "version": build_id.split("+")[0],
                                          "channel": "test"}).encode())
    if status not in (200, 201):
        raise SystemExit("cannot register build %s (%d): %r" % (build_id, status, body[:200]))


def rand_build_id(rng):
    return "0.1.%d+%012x" % (rng.randrange(1000), rng.getrandbits(48))


def rand_install_id(rng):
    return "%032x" % rng.getrandbits(128)


def rotate_progress(root):
    """Exactly d2_boot_config.cpp:209-211: remove the old _prev, rename the
    current file to _prev, remove the (now nonexistent) current file — a
    fresh run's state. This is done HERE, by the test, deliberately AFTER
    the 'collect' step above and not before, so the ordering property is
    actually demonstrated rather than assumed."""
    cur = os.path.join(root, "boot_progress.txt")
    prev = cur + "_prev"
    if os.path.exists(prev):
        os.remove(prev)
    if os.path.exists(cur):
        os.rename(cur, prev)
    if os.path.exists(cur):
        os.remove(cur)


def fresh_fault_copy(fixture, dest, rng, addr=None):
    """Copies the fixture with its one NATIVE FAULT address replaced by a
    freshly randomized one: guest_fault's canon (contract/signature-rules.v1
    #gfault) is gfault|<exception>|<eip>|<2 first frames>, with no build_id
    and no timestamp — the real fixture's literal address would otherwise
    collide, in D1, with whatever an EARLIER run of this same script already
    stored a sample for, breaking the "first boot uploads, second is
    count_only" property this test exists to demonstrate. Everything else
    in the fixture (a real, captured boot_progress.txt) stays untouched."""
    with open(fixture, encoding="utf-8") as f:
        text = f.read()
    addr = addr or ("%08x" % rng.getrandbits(32))
    new_text, n = re.subn(r"NATIVE FAULT thr (\d+) eip=[0-9a-f]{8} addr=[0-9a-f]{8}",
                          lambda m: "NATIVE FAULT thr %s eip=%s addr=%s" % (m.group(1), addr, addr), text)
    if n != 1:
        raise SystemExit("fixture %s: expected exactly one NATIVE FAULT line, found %d" % (fixture, n))
    with open(dest, "w", encoding="utf-8") as f:
        f.write(new_text)
    return addr


def find_report_id(list_output, kind="guest_fault"):
    for line in list_output.splitlines():
        if line.startswith("REPORT ") and ("kind=%s" % kind) in line:
            return line.split()[1]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reader", required=True)
    ap.add_argument("--api-url", required=True)
    ap.add_argument("--api-dir", required=True)
    ap.add_argument("--vectors", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    base = a.api_url.rstrip("/")
    work = a.work or os.path.join("/tmp", "d2cr_boot_e2e_%d" % os.getpid())
    os.makedirs(work, exist_ok=True)

    client_path = os.path.join(a.api_dir, ".dev.client.json")
    if not os.path.isfile(client_path):
        print("SKIPPED: no %s (run `npm run dev:secrets` in the API directory)" % client_path)
        return 0
    with open(client_path) as f:
        client = json.load(f)
    admin_token = client["admin_token"]
    response_pk = client["response_public_key_hex"]
    raise_claim_caps(base, admin_token)
    recipient_sk = vector_value(a.vectors, "sealed.v1.json", "cases", "one_byte", "recipient_sk_hex")
    recipient_pk = vector_value(a.vectors, "sealed.v1.json", "cases", "one_byte", "recipient_pk_hex")

    fixture = os.path.join(a.fixtures, "progress_native_fault_real.txt")
    if not os.path.isfile(fixture):
        raise SystemExit("fixture not found: %s" % fixture)

    rng = random.Random()
    fails = []
    installs_to_forget = []

    def check(name, cond, detail=""):
        status = "OK" if cond else "FAIL"
        print("%s: %s %s" % (status, name, detail))
        if not cond:
            fails.append(name)

    # ================= property 1 + 2: evidence survives rotation, =========
    # ================= claim/pieces/complete reach a real API ==============
    build_a = rand_build_id(rng)
    register_build(base, admin_token, build_a)
    root1 = os.path.join(work, "boot1")
    fault_addr = fresh_fault_copy(fixture, os.path.join(_ensure(root1), "boot_progress.txt"), rng)
    now1 = int(time.time())
    run(a.reader, "init-session", "--root", root1, "--session-id", "prevsess1", "--build-id", build_a,
        "--started-unix", str(now1 - 120))
    out = run(a.reader, "collect", "--root", root1, "--now", str(now1), "--build-id", build_a)
    check("collect finds the NATIVE FAULT evidence", "CREATED: report" in out.stdout and "kind=guest_fault" in out.stdout)
    list_before = run(a.reader, "list", "--root", root1)
    report_id_1 = find_report_id(list_before.stdout)
    check("report is in the outbox before rotation", report_id_1 is not None)

    # The plaintext artifact, saved BEFORE rotation AND before upload (which
    # deletes the outbox entry once complete) — what verify_pieces compares
    # the admin-decrypted bytes against. verify_pieces() hardcodes
    # <work>/pieces/<name>, so each property gets its own work1/work3/... root.
    case1_work = _ensure(os.path.join(work, "case1"))
    pieces_dir = _ensure(os.path.join(case1_work, "pieces"))
    outbox_dir = os.path.join(root1, "reports", "outbox", report_id_1 or "")
    if report_id_1:
        shutil.copyfile(os.path.join(outbox_dir, "boot_progress.txt"), os.path.join(pieces_dir, "boot_progress"))

    rotate_progress(root1)
    check("the original boot_progress.txt is gone after rotation", not os.path.exists(os.path.join(root1, "boot_progress.txt")))
    list_after = run(a.reader, "list", "--root", root1)
    check("evidence SURVIVED the rotation (still in the outbox)",
         report_id_1 is not None and ("REPORT %s kind=guest_fault consent=pending" % report_id_1) in list_after.stdout)

    install_a = rand_install_id(rng)
    installs_to_forget.append(install_a)
    out = run(a.reader, "after-present", "--root", root1, "--mode", "always", "--url", base,
             "--response-pk", response_pk, "--recipient-pk", recipient_pk, "--install-id", install_a)
    check("claim+pieces+complete reached the real API", "completed=1" in out.stdout and "deleted=0" in out.stdout,
         out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "")
    if report_id_1:
        failures = verify_pieces(base, admin_token, report_id_1, case1_work, recipient_sk, ("boot_progress",), True)
        check("admin decrypts the piece identically to what the console had", failures == 0)

    # ================= property 3: same signature on reboot => count_only ===
    root2 = os.path.join(work, "boot2")
    fresh_fault_copy(fixture, os.path.join(_ensure(root2), "boot_progress.txt"), rng, addr=fault_addr)
    now2 = now1 + 300
    run(a.reader, "init-session", "--root", root2, "--session-id", "prevsess2", "--build-id", build_a,
        "--started-unix", str(now2 - 120))
    out = run(a.reader, "collect", "--root", root2, "--now", str(now2), "--build-id", build_a)
    check("second boot, same fault, evidence collected again", "CREATED: report" in out.stdout)
    rotate_progress(root2)
    install_b = rand_install_id(rng)
    installs_to_forget.append(install_b)
    out = run(a.reader, "after-present", "--root", root2, "--mode", "always", "--url", base,
             "--response-pk", response_pk, "--recipient-pk", recipient_pk, "--install-id", install_b)
    check("second report becomes count_only, not a second stored sample",
         "deleted=1" in out.stdout and "completed=0" in out.stdout,
         out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "")

    # ================= property 4: D2SCRIPT => zero dialog, zero network ====
    root3 = os.path.join(work, "boot3")
    fresh_fault_copy(fixture, os.path.join(_ensure(root3), "boot_progress.txt"), rng)
    now3 = now1 + 600
    run(a.reader, "init-session", "--root", root3, "--session-id", "prevsess3", "--build-id", build_a,
        "--started-unix", str(now3 - 120))
    run(a.reader, "collect", "--root", root3, "--now", str(now3), "--build-id", build_a)
    out = run(a.reader, "after-present", "--root", root3, "--mode", "script")
    check("D2SCRIPT => should_suppress_dialog() true, no dialog/network attempted",
         "OK: D2SCRIPT" in out.stdout and "outcome=" not in out.stdout)
    list3 = run(a.reader, "list", "--root", root3)
    check("report stays pending, untouched, under D2SCRIPT", "consent=pending" in list3.stdout)

    # ================= property 5: D2_CRASHREPORT=0 => nothing collected ====
    root4 = os.path.join(work, "boot4")
    fresh_fault_copy(fixture, os.path.join(_ensure(root4), "boot_progress.txt"), rng)
    now4 = now1 + 900
    run(a.reader, "init-session", "--root", root4, "--session-id", "prevsess4", "--build-id", build_a,
        "--started-unix", str(now4 - 120))
    out = run(a.reader, "collect", "--root", root4, "--now", str(now4), "--build-id", build_a,
             "--d2-crashreport", "0")
    check("D2_CRASHREPORT=0 => early return, nothing read/written", out.stdout.strip() == "DISABLED: D2_CRASHREPORT=0, nothing read, nothing written")
    list4 = run(a.reader, "list", "--root", root4)
    check("D2_CRASHREPORT=0 => nothing collected even though real evidence sits on disk", list4.stdout.strip() == "")

    for install_id in installs_to_forget:
        request(base + "/v1/admin/installs/" + install_id, admin_token, "DELETE")

    if fails:
        print("FAIL: %d/%d propert(y/ies) failed: %s" % (len(fails), 5, ", ".join(fails)))
        return 1
    print("PASS: all 5 crash-report boot-wiring properties proven against %s" % base)
    return 0


def _ensure(d):
    os.makedirs(d, exist_ok=True)
    return d


if __name__ == "__main__":
    sys.exit(main())
