# The admin client against a fake API that answers contract bodies.
import io
import os
import sys
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import fakeapi  # noqa: E402
from d2vcrash import api  # noqa: E402


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class ApiTest(unittest.TestCase):
    def setUp(self):
        self.server = fakeapi.FakeAdminServer()
        self.server.__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)
        self.addCleanup(lambda: self.assertEqual(self.server.schema_errors, []))
        self.client = api.AdminClient(self.server.base_url, self.server.token)

    # -- signatures --------------------------------------------------------
    def test_lists_signatures_and_follows_the_cursor(self):
        data = self.server.data
        data.page_size = 1
        for index in range(3):
            id = "SZYGIRBIXGHOM3A" + "BCD"[index]
            data.signatures[id] = fakeapi.signature_detail(id=id, count=index)
        page = self.client.list_signatures()
        self.assertEqual(len(page.items), 1)
        self.assertIsNotNone(page.next_cursor)
        self.assertIsInstance(page.items[0], api.SignatureSummary)
        every = list(self.client.iter_signatures())
        self.assertEqual(len(every), 4)
        self.assertEqual([s.count for s in every], sorted((s.count for s in every), reverse=True))
        # The filters reach the query string of the request, and only the
        # parameters the route table of contract/README.md defines: the fake
        # server answers 400 to any other, as the Worker does (design 5.5).
        self.client.list_signatures(status="open", kind="halt", build="0.1.0+ab12cd34ef56", sort="last_seen")
        self.assertEqual(self.server.requests[-1]["query"],
                         {"status": "open", "kind": "halt", "build": "0.1.0+ab12cd34ef56", "sort": "last_seen"})
        self.client.list_bugs(status="open")
        for request in self.server.requests:
            self.assertEqual(set(request["query"]) - {"status", "kind", "build", "sort", "cursor"}, set(),
                             request["path"])

    def test_a_cursor_that_never_moves_stops_instead_of_spinning(self):
        # An API that answers the same non-empty page with the same cursor made
        # `crash list --all` loop for ever.
        self.server.data.sticky_cursor = "SAME"
        for listing in (self.client.iter_signatures, self.client.iter_bugs):
            with self.subTest(listing=listing.__name__):
                with self.assertRaises(api.ProtocolError) as caught:
                    list(listing())
                self.assertIn("SAME", str(caught.exception))
        # A caller that asked for a fixed number still gets it and stops.
        self.assertEqual(len(list(self.client.iter_signatures(max_items=1))), 1)

    def test_paging_is_the_cursor_of_the_contract_and_nothing_else(self):
        # `limit` is not in the route table of contract v1: asking for it would
        # be 400 invalid_payload against a Worker that checks its query string.
        with self.assertRaises(TypeError):
            self.client.list_signatures(limit=2)
        with self.assertRaises(TypeError):
            self.client.list_bugs(limit=2)

    def test_reads_a_signature_detail(self):
        detail = self.client.get_signature("SZYGIRBIXGHOM3AH")
        self.assertEqual(detail.id, "SZYGIRBIXGHOM3AH")
        self.assertEqual(detail.kind, "halt")
        self.assertEqual(detail.count, 7)
        self.assertEqual(detail.sample_report, "01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        self.assertEqual(detail.sample_artifacts[0].name, "crash_txt")
        self.assertEqual(detail.builds[0].build_id, "0.1.0+ab12cd34ef56")
        self.assertEqual(detail.recent_reports[0].action, "upload")
        self.assertEqual(detail.versions(), ["0.1.0", "0.1.1"])

    def test_patches_a_signature(self):
        detail = self.client.patch_signature("SZYGIRBIXGHOM3AH", status="fixed", fixed_in_version="0.2.0",
                                             note="fixed by the sprite fix")
        self.assertEqual(detail.status, "fixed")
        self.assertEqual(detail.fixed_in_version, "0.2.0")
        self.assertEqual(self.server.patches[-1][1],
                         {"status": "fixed", "fixed_in_version": "0.2.0", "note": "fixed by the sprite fix"})
        self.client.patch_signature("SZYGIRBIXGHOM3AH", resample=True)
        self.assertEqual(self.server.patches[-1][1], {"resample": True})
        self.client.patch_signature("SZYGIRBIXGHOM3AH", merged_into="SAAAABBBBCCCCDDD")
        self.assertEqual(self.server.patches[-1][1], {"merged_into": "SAAAABBBBCCCCDDD"})
        # None is a value (null), an omitted argument is not sent at all.
        self.client.patch_signature("SZYGIRBIXGHOM3AH", issue_url=None)
        self.assertEqual(self.server.patches[-1][1], {"issue_url": None})
        with self.assertRaises(ValueError):
            self.client.patch_signature("SZYGIRBIXGHOM3AH")

    # -- reports and artifacts --------------------------------------------
    def test_reads_a_report_and_streams_an_artifact(self):
        report = self.client.get_report("01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        self.assertEqual(report.claim["kind"], "halt")
        self.assertEqual(report.artifacts[0].bytes, 2210)
        blob = b"D2VSEAL1" + bytes(range(256)) * 8
        self.server.data.artifacts[("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt")] = blob
        sink = io.BytesIO()
        written = self.client.download_artifact("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt", sink)
        self.assertEqual(written, len(blob))
        self.assertEqual(sink.getvalue(), blob)
        self.assertEqual(self.client.get_artifact_bytes("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt"), blob)

    def test_an_identifier_that_is_not_the_contract_shape_is_refused(self):
        # Every one of these reaches a file path, a URL or a command line, and
        # each starts life in a claim any console on the internet can send.
        report, signature, bug = "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "SZYGIRBIXGHOM3AH", "B7QK2M4X9ZAB"
        cases = [
            ("artifact name", "name", dict(fakeapi.report_detail(),
                                           artifacts=[{"name": "../../ESCAPED.txt", "bytes": 7,
                                                       "sha256": "ab" * 32, "stored_unix": 1789284100}]),
             self.client.get_report, report),
            ("report id", "report_id", dict(fakeapi.report_detail(), report_id="../../etc/passwd"),
             self.client.get_report, report),
            ("build id of the claim", "build_id", dict(fakeapi.report_detail(),
                                                       claim=fakeapi.claim(build_id="../../.ssh")),
             self.client.get_report, report),
            ("sample report", "sample_report", fakeapi.signature_detail(sample_report="/etc/shadow"),
             self.client.get_signature, signature),
            ("signature id", "id", fakeapi.signature_detail(id="../../x"),
             self.client.get_signature, signature),
            ("report id of a recent report", "report_id", fakeapi.signature_detail(
                recent_reports=[{"report_id": "../escape", "build_id": "0.1.0+ab12cd34ef56",
                                 "received_unix": 1789284100, "action": "upload"}]),
             self.client.get_signature, signature),
            ("bug id", "id", dict(fakeapi.bug_item(id="../../bug"), v=1), self.client.get_bug, bug),
        ]
        for name, field, body, call, argument in cases:
            with self.subTest(case=name):
                self.server.force_body(200, body)
                with self.assertRaises(api.ProtocolError) as caught:
                    call(argument)
                self.assertIn(field, str(caught.exception))

    def test_an_artifact_bigger_than_its_cap_is_refused_while_it_streams(self):
        # The sha256 check only runs once everything is read: without a ceiling
        # a misbehaving API could hand `crash pull` an endless body.
        report = "01J9Z6T4Q8M3K7V2B5N0XWAYCD"
        self.server.data.artifacts[(report, "dump")] = b"x" * 4096
        sink = io.BytesIO()
        with self.assertRaises(api.ProtocolError) as caught:
            self.client.download_artifact(report, "dump", sink, max_bytes=1024)
        self.assertIn("1024", str(caught.exception))
        self.assertLessEqual(len(sink.getvalue()), 1024)
        # Contract v1 caps the largest artifact, the dump, at 2 MiB.
        self.assertEqual(api.MAX_SEALED_BYTES, 2097152)

    def test_an_artifact_whose_digest_does_not_match_is_refused(self):
        blob = b"sealed bytes"
        self.server.data.artifacts[("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "dump")] = blob
        with self.assertRaises(api.ProtocolError):
            self.client.download_artifact("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "dump", io.BytesIO(), expected_sha256="00" * 32)

    # -- bugs, builds, installs, stats, settings ---------------------------
    def test_bugs(self):
        page = self.client.list_bugs(status="open")
        self.assertEqual(page.items[0].id, "B7QK2M4X9ZAB")
        bug = self.client.get_bug("B7QK2M4X9ZAB")
        self.assertEqual(bug.title, "Crash when entering the Rogue Encampment")
        patched = self.client.patch_bug("B7QK2M4X9ZAB", status="fixed", issue_url="https://github.com/x/y/issues/1")
        self.assertEqual(patched.status, "fixed")
        self.assertEqual(list(self.client.iter_bugs())[0].id, "B7QK2M4X9ZAB")

    def test_registers_a_build_once_and_again(self):
        record = self.client.register_build("0.1.0+ab12cd34ef56", "0.1.0", "dev")
        self.assertTrue(record.created)
        self.assertEqual(record.channel, "dev")
        self.assertFalse(self.client.register_build("0.1.0+ab12cd34ef56", "0.1.0", "dev").created)

    def test_forget_install_repeats_while_the_api_answers_202(self):
        install = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e"
        self.server.data.erasure_rounds[install] = 2
        result = self.client.forget_install(install)
        self.assertEqual(result.calls, 3)
        self.assertEqual(result.reports_deleted, 6)      # 2 per call
        self.assertEqual(result.artifacts_deleted, 3)
        self.assertEqual(len([r for r in self.server.requests if r["method"] == "DELETE"]), 3)

    def test_forget_install_gives_up_after_the_call_limit(self):
        install = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e"
        self.server.data.erasure_rounds[install] = 50
        with self.assertRaises(api.ApiError):
            self.client.forget_install(install, max_calls=3)

    def test_stats_and_settings(self):
        stats = self.client.get_stats()
        self.assertEqual(stats.day, "2026-09-14")
        self.assertTrue(stats.accepting)
        self.assertEqual(stats.usage["claims"].used, 12)
        self.assertIsNone(stats.database_bytes)
        self.server.data.stats_extra = {"database_bytes": 471859200}
        self.assertEqual(self.client.get_stats().database_bytes, 471859200)
        updated = self.client.put_settings(accepting=False, disable_until_unix=1789300000)
        self.assertFalse(updated.accepting)
        self.assertEqual(updated.disable_until_unix, 1789300000)
        self.assertEqual(updated.caps["global_claims_per_day"], 2000)
        self.assertEqual(self.client.put_settings(caps={"global_claims_per_day": 10}).caps["global_claims_per_day"], 10)
        with self.assertRaises(ValueError):
            self.client.put_settings()

    # -- errors ------------------------------------------------------------
    def test_maps_every_error_body_to_an_exception(self):
        cases = [
            (401, "unauthorized", api.Unauthorized),
            (404, "not_found", api.NotFound),
            (400, "invalid_payload", api.InvalidPayload),
            (405, "method_not_allowed", api.ApiError),
            (500, "internal_error", api.ServerError),
        ]
        for status, code, kind in cases:
            with self.subTest(code=code):
                self.server.force_next(status, code, "nope")
                with self.assertRaises(kind) as caught:
                    self.client.get_stats()
                self.assertEqual(caught.exception.status, status)
                self.assertEqual(caught.exception.error, code)
                self.assertIn("nope", str(caught.exception))
        self.server.force_next(429, "rate_limited", "slow down", retry_after_s=30)
        with self.assertRaises(api.RateLimited) as caught:
            self.client.get_stats()
        self.assertEqual(caught.exception.retry_after_s, 30)
        self.server.force_next(503, "not_accepting", "kill switch", disable_until_unix=1789300000)
        with self.assertRaises(api.NotAccepting) as caught:
            self.client.get_stats()
        self.assertEqual(caught.exception.disable_until_unix, 1789300000)

    def test_a_wrong_token_is_unauthorized(self):
        client = api.AdminClient(self.server.base_url, "not-the-token")
        with self.assertRaises(api.Unauthorized):
            client.get_stats()

    def test_a_body_that_is_not_the_contract_shape_is_a_protocol_error(self):
        detail = dict(fakeapi.signature_detail())
        detail.pop("installs")
        self.server.force_body(200, detail)
        with self.assertRaises(api.ProtocolError) as caught:
            self.client.get_signature("SZYGIRBIXGHOM3AH")
        self.assertIn("installs", str(caught.exception))
        self.server.force_body(200, dict(fakeapi.signature_detail(), count="seven"))
        with self.assertRaises(api.ProtocolError):
            self.client.get_signature("SZYGIRBIXGHOM3AH")
        self.server.force_body(200, {"items": [], "next_cursor": None})     # no "v": 1
        with self.assertRaises(api.ProtocolError):
            self.client.list_signatures()
        self.server.force_body(200, [1, 2, 3])
        with self.assertRaises(api.ProtocolError):
            self.client.get_stats()

    def test_refuses_to_send_the_token_over_plain_http_to_another_host(self):
        with self.assertRaises(ValueError):
            api.AdminClient("http://crash.example.invalid", "token")
        api.AdminClient("https://crash.example.invalid", "token")       # fine
        api.AdminClient("http://127.0.0.1:8787", "token")               # local development

    def test_never_follows_a_redirect_with_the_token(self):
        # A redirect would resend "Authorization: Bearer ..." to wherever it
        # points; the client refuses instead.
        self.server.force_redirect(self.server.base_url + "/v1/admin/stolen")
        with self.assertRaises(api.ApiError) as caught:
            self.client.get_stats()
        self.assertEqual(caught.exception.status, 302)
        self.assertNotIn("/v1/admin/stolen", [r["path"] for r in self.server.requests])

    def test_network_failures_are_reported_as_such(self):
        client = api.AdminClient("http://127.0.0.1:1", "token", timeout=1)
        with self.assertRaises(api.NetworkError):
            client.get_stats()


if __name__ == "__main__":
    unittest.main()
