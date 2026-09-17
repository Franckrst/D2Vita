# The command line: text and --json, run as a session would run it.
import json
import os
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import fakeapi  # noqa: E402


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class CliTest(unittest.TestCase):
    def setUp(self):
        self.server = fakeapi.FakeAdminServer()
        self.server.__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)
        self.addCleanup(lambda: self.assertEqual(self.server.schema_errors, []))
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_dir = os.path.join(self.tmp.name, "config")
        self.public_key = support.write_config(self.config_dir, self.server.base_url, self.server.token)

    def crash(self, *args, **kwargs):
        return support.run_crash(self.config_dir, *args, **kwargs)

    def ok(self, *args, **kwargs):
        done = self.crash(*args, **kwargs)
        self.assertEqual(done.returncode, 0, f"{args}\nstdout: {done.stdout}\nstderr: {done.stderr}")
        return done

    def json_of(self, *args, **kwargs):
        return json.loads(self.ok(*args, "--json", **kwargs).stdout)

    # -- reading -----------------------------------------------------------
    def test_list_prints_one_line_per_signature_and_json_keeps_the_contract_body(self):
        done = self.ok("list")
        self.assertIn("SZYGIRBIXGHOM3AH", done.stdout)
        self.assertIn("halt", done.stdout)
        header, row = done.stdout.splitlines()[:2]
        self.assertEqual(header.split()[0], "SIGNATURE")
        self.assertEqual(row.split()[0], "SZYGIRBIXGHOM3AH")
        payload = self.json_of("list")
        self.assertEqual(payload["items"][0]["id"], "SZYGIRBIXGHOM3AH")
        self.assertEqual(payload["v"], 1)
        self.assertIn("next_cursor", payload)

    def test_list_passes_its_filters_and_can_follow_every_page(self):
        data = self.server.data
        data.page_size = 1
        for index in range(3):
            id = "SZYGIRBIXGHOM3A" + "BCD"[index]
            data.signatures[id] = fakeapi.signature_detail(id=id, count=index, kind="hang")
        self.ok("list", "--status", "open", "--kind", "halt", "--sort", "last_seen")
        self.assertEqual(self.server.requests[-1]["query"],
                         {"status": "open", "kind": "halt", "sort": "last_seen"})
        payload = self.json_of("list", "--all")
        self.assertEqual(len(payload["items"]), 4)
        # --limit is ours: contract v1 has no page size, so the request never
        # carries one and the cap is applied on what came back.
        self.assertEqual(len(self.json_of("list", "--all", "--limit", "2")["items"]), 2)
        data.page_size = 50
        self.assertEqual(len(self.json_of("list", "--limit", "3")["items"]), 3)
        self.assertIn("3 of 4 shown", self.ok("list", "--limit", "3").stdout)
        self.assertNotIn("limit", set().union(*(set(r["query"]) for r in self.server.requests)))

    def test_show_prints_the_detail_and_its_builds(self):
        done = self.ok("show", "SZYGIRBIXGHOM3AH")
        self.assertIn("signature    SZYGIRBIXGHOM3AH", done.stdout)
        self.assertIn("halt|1420|-|Game+0x1fedf4", done.stdout)
        self.assertIn("0.1.0+ab12cd34ef56", done.stdout)
        self.assertIn("01J9Z6T4Q8M3K7V2B5N0XWAYCD", done.stdout)
        self.assertEqual(self.json_of("show", "SZYGIRBIXGHOM3AH")["id"], "SZYGIRBIXGHOM3AH")

    def test_show_of_an_unknown_signature_fails_with_a_readable_message(self):
        done = self.crash("show", "SAAAAAAAAAAAAAAA")
        self.assertEqual(done.returncode, 1)
        self.assertIn("404", done.stderr)
        self.assertEqual(done.stdout, "")

    def test_stats_warns_above_450_mb_of_database(self):
        done = self.ok("stats")
        self.assertIn("claims", done.stdout)
        self.assertNotIn("WARNING", done.stdout)
        payload = self.json_of("stats")
        self.assertEqual(payload["usage"]["claims"]["used"], 12)
        self.server.data.stats_extra = {"database_bytes": 460 * 1000 * 1000}
        done = self.ok("stats")
        self.assertIn("WARNING", done.stdout)
        self.assertIn("500 MB", done.stdout)
        payload = self.json_of("stats")
        self.assertEqual(payload["database_bytes"], 460 * 1000 * 1000)
        self.assertTrue(payload["warnings"])
        self.server.data.stats_extra = {"database_bytes": 100 * 1000 * 1000}
        self.assertEqual(self.json_of("stats")["warnings"], [])

    def test_bugs_lists_shows_and_patches(self):
        done = self.ok("bugs")
        self.assertIn("B7QK2M4X9ZAB", done.stdout)
        done = self.ok("bugs", "B7QK2M4X9ZAB")
        self.assertIn("Crash when entering the Rogue Encampment", done.stdout)
        self.assertIn("The game closes after the loading screen.", done.stdout)
        self.ok("bugs", "B7QK2M4X9ZAB", "--set-status", "fixed")
        self.assertEqual(self.server.patches[-1], ("B7QK2M4X9ZAB", {"status": "fixed"}))
        self.assertEqual(self.json_of("bugs", "B7QK2M4X9ZAB")["status"], "fixed")

    # -- writing -----------------------------------------------------------
    def test_status_merge_and_resample(self):
        self.ok("status", "SZYGIRBIXGHOM3AH", "fixed", "--version", "0.2.0", "--note", "sprite fix")
        self.assertEqual(self.server.patches[-1][1],
                         {"status": "fixed", "fixed_in_version": "0.2.0", "note": "sprite fix"})
        failed = self.crash("status", "SZYGIRBIXGHOM3AH", "fixed")
        self.assertEqual(failed.returncode, 2)
        self.assertIn("--version", failed.stderr)
        self.ok("merge", "SZYGIRBIXGHOM3AH", "SAAAABBBBCCCCDDD")
        self.assertEqual(self.server.patches[-1][1], {"merged_into": "SAAAABBBBCCCCDDD"})
        self.ok("resample", "SZYGIRBIXGHOM3AH")
        self.assertEqual(self.server.patches[-1][1], {"resample": True})
        self.assertEqual(self.json_of("show", "SZYGIRBIXGHOM3AH")["sample_state"], "none")

    def test_builds_register(self):
        done = self.ok("builds", "register", "0.1.0+ab12cd34ef56", "--channel", "test")
        self.assertIn("0.1.0+ab12cd34ef56", done.stdout)
        self.assertIn("registered", done.stdout)
        payload = self.json_of("builds", "register", "0.1.0+ab12cd34ef56", "--channel", "test")
        self.assertEqual(payload["version"], "0.1.0")      # derived from the build id
        self.assertEqual(payload["channel"], "test")
        self.assertFalse(payload["created"])
        bad = self.crash("builds", "register", "not-a-build-id")
        self.assertEqual(bad.returncode, 2)

    def test_forget_install_asks_before_erasing_and_repeats_while_202(self):
        install = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e"
        self.server.data.erasure_rounds[install] = 1
        refused = self.crash("forget-install", install, stdin="n\n")
        self.assertEqual(refused.returncode, 1)
        self.assertEqual([r for r in self.server.requests if r["method"] == "DELETE"], [])
        done = self.ok("forget-install", install, stdin="yes\n")
        self.assertIn("2 calls", done.stdout)
        self.assertEqual(len([r for r in self.server.requests if r["method"] == "DELETE"]), 2)
        self.server.data.erasure_rounds[install] = 0
        payload = self.json_of("forget-install", install, "--yes")
        self.assertEqual(payload["reports_deleted"], 2)
        self.assertEqual(payload["calls"], 1)

    def test_settings_changes_the_kill_switch_and_the_caps(self):
        done = self.ok("settings", "--accepting", "no", "--disable-until", "1789300000")
        self.assertIn("not accepting", done.stdout)
        self.assertEqual(self.server.patches[-1], ("settings", {"accepting": False,
                                                               "disable_until_unix": 1789300000}))
        payload = self.json_of("settings", "--cap", "global_claims_per_day=10", "--accepting", "yes")
        self.assertEqual(payload["caps"]["global_claims_per_day"], 10)
        self.assertTrue(payload["accepting"])
        self.assertEqual(self.crash("settings").returncode, 2)
        self.assertEqual(self.crash("settings", "--cap", "nope=1").returncode, 2)

    # -- artifacts ---------------------------------------------------------
    def test_pull_decrypts_every_artifact_of_a_signature_sample(self):
        data = self.server.data
        support.store_artifact(data, "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt", b"Halt 1420\r\n", self.public_key)
        support.store_artifact(data, "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_log", b"log line\n", self.public_key)
        out = os.path.join(self.tmp.name, "pulled")
        payload = self.json_of("pull", "SZYGIRBIXGHOM3AH", "--dir", out)
        self.assertEqual(payload["report_id"], "01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        names = {f["name"]: f for f in payload["files"]}
        self.assertEqual(set(names), {"crash_txt", "crash_log"})
        with open(names["crash_txt"]["path"], "rb") as handle:
            self.assertEqual(handle.read(), b"Halt 1420\r\n")
        with open(os.path.join(out, "claim.json"), encoding="utf-8") as handle:
            self.assertEqual(json.load(handle)["kind"], "halt")
        text = self.ok("pull", "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "--dir", out).stdout
        self.assertIn("crash_txt", text)
        self.assertIn(out, text)

    def test_pull_never_writes_outside_the_directory_it_was_given(self):
        # The artifact name is served by the API but starts in a claim sent by
        # any console on the internet: it is checked against the four names of
        # claim.v1#ArtifactName before it can become a file name.
        import hashlib

        from d2vcrash import seal

        data = self.server.data
        data.hostile_artifact_names = True
        hostile = "../../ESCAPED.txt"
        report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCD"
        sealed = seal.seal(b"escaped plaintext", self.public_key)
        data.artifacts[(report_id, hostile)] = sealed
        self.server.force_body(200, dict(fakeapi.report_detail(), artifacts=[
            {"name": hostile, "bytes": len(sealed), "sha256": hashlib.sha256(sealed).hexdigest(),
             "stored_unix": 1789284100}]))
        out = os.path.join(self.tmp.name, "hostile", "out")
        done = self.crash("pull", report_id, "--dir", out)
        self.assertEqual(done.returncode, 1)
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "ESCAPED.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "hostile", "ESCAPED.txt")))

    def test_pull_says_when_a_signature_has_no_sample(self):
        self.server.data.signatures["SZYGIRBIXGHOM3AH"]["sample_report"] = None
        self.server.data.signatures["SZYGIRBIXGHOM3AH"]["sample_state"] = "none"
        done = self.crash("pull", "SZYGIRBIXGHOM3AH")
        self.assertEqual(done.returncode, 1)
        self.assertIn("no sample", done.stderr)

    def test_pull_reports_a_sealed_object_it_cannot_open(self):
        data = self.server.data
        support.store_artifact(data, "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt", b"Halt 1420\r\n", self.public_key)
        support.store_artifact(data, "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_log", b"log", self.public_key)
        sealed = bytearray(data.artifacts[("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_log")])
        sealed[-1] ^= 0x01
        import hashlib
        data.artifacts[("01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_log")] = bytes(sealed)
        for holder in (data.reports["01J9Z6T4Q8M3K7V2B5N0XWAYCD"]["artifacts"],
                       data.signatures["SZYGIRBIXGHOM3AH"]["sample_artifacts"]):
            for entry in holder:
                if entry["name"] == "crash_log":
                    entry["sha256"] = hashlib.sha256(bytes(sealed)).hexdigest()
        done = self.crash("pull", "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "--dir", os.path.join(self.tmp.name, "bad"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("tampered", done.stderr)

    # -- configuration -----------------------------------------------------
    def test_without_api_base_the_error_names_the_file(self):
        empty = os.path.join(self.tmp.name, "empty")
        os.makedirs(empty, mode=0o700)
        done = support.run_crash(empty, "list")
        self.assertEqual(done.returncode, 2)
        self.assertIn("admin.env", done.stderr)
        self.assertIn("API_BASE", done.stderr)

    def test_a_plain_http_api_base_is_refused_before_the_token_travels(self):
        risky = os.path.join(self.tmp.name, "risky")
        support.write_config(risky, "http://crash.example.invalid", "secret-token")
        done = support.run_crash(risky, "list")
        self.assertEqual(done.returncode, 2)
        self.assertIn("https", done.stderr)
        self.assertNotIn("secret-token", done.stdout + done.stderr)

    def test_no_token_ever_reaches_the_output(self):
        for args in (("list",), ("show", "SZYGIRBIXGHOM3AH"), ("stats",), ("config",)):
            done = self.crash(*args)
            self.assertNotIn(self.server.token, done.stdout + done.stderr, args)


class DownloadGuardTest(unittest.TestCase):
    """The second lock of the write path: handed a name the client did not
    check, download_artifacts still writes nothing outside its directory."""

    def test_an_unknown_artifact_name_never_becomes_a_path(self):
        from d2vcrash import cli

        class Stored:
            name, bytes, sha256 = "../../ESCAPED.txt", 7, "ab" * 32

        class Report:
            report_id, claim = "01J9Z6T4Q8M3K7V2B5N0XWAYCD", {}
            artifacts = [Stored()]

        class Client:
            def get_artifact_bytes(self, *args, **kwargs):
                raise AssertionError("the name must be refused before anything is downloaded")

        class Cfg:
            def secret_key(self):
                return bytes(32)

        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(cli.CommandError) as caught:
                cli.download_artifacts(Client(), Cfg(), Report(), os.path.join(tmp, "out"))
            self.assertIn("ESCAPED", str(caught.exception))
            self.assertFalse(os.path.exists(os.path.join(tmp, "ESCAPED.txt")))


if __name__ == "__main__":
    unittest.main()
