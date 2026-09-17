# `crash serve`: local only, everything escaped, no secret in a page.
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import fakeapi  # noqa: E402
import fakegithub  # noqa: E402
from d2vcrash import api, config, server  # noqa: E402

XSS = "<script>alert(1)</script>"


class BindingTest(unittest.TestCase):
    def test_refuses_every_host_but_the_loopback(self):
        cfg = config.Config("/nonexistent", {"API_BASE": "https://example.invalid", "ADMIN_TOKEN": "t"})
        for host in ("0.0.0.0", "", "192.168.1.10", "example.invalid", "::"):
            with self.subTest(host=host):
                with self.assertRaises(ValueError) as caught:
                    server.make_server(cfg, host=host, port=0)
                self.assertIn("127.0.0.1", str(caught.exception))

    def test_binds_the_loopback(self):
        cfg = config.Config("/nonexistent", {"API_BASE": "https://example.invalid", "ADMIN_TOKEN": "t"})
        httpd = server.make_server(cfg, host="127.0.0.1", port=0)
        try:
            self.assertEqual(httpd.server_address[0], "127.0.0.1")
        finally:
            httpd.server_close()

    def test_a_configuration_failure_leaves_no_socket_behind(self):
        # The Context reads the configuration: when it fails, the port must be
        # free again, or a second `crash serve` answers "address in use".
        import socket

        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        broken = config.Config("/nonexistent", {"API_BASE": "https://example.invalid"})   # no ADMIN_TOKEN
        error = None
        try:
            server.make_server(broken, host="127.0.0.1", port=port)
        except config.ConfigError as failure:
            error = failure                        # keeps the traceback, so a leaked socket stays open
        self.assertIsNotNone(error)
        self.assertIn("ADMIN_TOKEN", str(error))
        again = socket.socket()
        again.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            again.bind(("127.0.0.1", port))        # fails with "address in use" if the bind leaked
        finally:
            again.close()

    def test_the_command_line_refuses_another_host(self):
        with tempfile.TemporaryDirectory() as tmp:
            support.write_config(tmp, "https://example.invalid", "token")
            done = support.run_crash(tmp, "serve", "--host", "0.0.0.0")
            self.assertEqual(done.returncode, 2)
            self.assertIn("127.0.0.1", done.stderr)


class LinkTest(unittest.TestCase):
    """An href is the one value of a page that escaping alone does not make
    safe: html.escape leaves the scheme intact."""

    def test_a_link_only_points_at_this_server_or_at_an_https_url(self):
        self.assertIn('href="/signature/SZYGIRBIXGHOM3AH"', server.link("/signature/SZYGIRBIXGHOM3AH", "x"))
        self.assertIn('href="https://github.com/a/b/issues/1"',
                      server.link("https://github.com/a/b/issues/1", "issue"))
        for href in ("javascript:alert(1)", "JavaScript:alert(1)", "data:text/html,<script>alert(1)</script>",
                     "http://plain.example/", "//evil.example/x", "vbscript:x"):
            with self.subTest(href=href):
                rendered = server.link(href, "issue")
                self.assertNotIn("<a", rendered)
                self.assertNotIn("href", rendered)
                self.assertNotIn("<", rendered)


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class PagesTest(unittest.TestCase):
    def setUp(self):
        self.upstream = fakeapi.FakeAdminServer()
        self.upstream.__enter__()
        self.addCleanup(self.upstream.__exit__, None, None, None)
        self.addCleanup(lambda: self.assertEqual(self.upstream.schema_errors, []))
        self.github = fakegithub.FakeGitHub()
        self.github.__enter__()
        self.addCleanup(self.github.__exit__, None, None, None)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_dir = os.path.join(self.tmp.name, "config")
        self.public_key = support.write_config(self.config_dir, self.upstream.base_url, self.upstream.token,
                                               GITHUB_TOKEN="ghp_fake_token")
        os.environ["D2VCRASH_GITHUB_API"] = self.github.base_url
        self.addCleanup(os.environ.pop, "D2VCRASH_GITHUB_API", None)
        # Hostile text in every field that carries free text.
        data = self.upstream.data
        data.signatures["SZYGIRBIXGHOM3AH"]["note"] = XSS
        data.signatures["SZYGIRBIXGHOM3AH"]["canon"] = "halt|1420|-|" + XSS
        data.bugs["B7QK2M4X9ZAB"]["title"] = XSS
        data.bugs["B7QK2M4X9ZAB"]["description"] = "line one\n" + XSS
        data.reports["01J9Z6T4Q8M3K7V2B5N0XWAYCD"]["claim"] = fakeapi.host_fault_claim(
            report_id="01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        data.reports["01J9Z6T4Q8M3K7V2B5N0XWAYCD"]["claim"]["features"]["thread_name"] = XSS

        self.cfg = config.load(self.config_dir)
        self.httpd = server.make_server(self.cfg, host="127.0.0.1", port=0)
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=10) as answer:
            return answer.status, answer.read().decode("utf-8")

    def post(self, path, fields):
        data = urllib.parse.urlencode(fields).encode("utf-8")
        request = urllib.request.Request(self.base + path, data=data, method="POST")
        request.add_header("content-type", "application/x-www-form-urlencoded")
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                return answer.status, answer.read().decode("utf-8")
        except urllib.error.HTTPError as http_error:
            return http_error.code, http_error.read().decode("utf-8")

    def csrf(self, path="/"):
        _, page = self.get(path)
        marker = 'name="_csrf" value="'
        start = page.index(marker) + len(marker)
        return page[start:page.index('"', start)]

    # -- rendering ---------------------------------------------------------
    def test_the_list_escapes_hostile_text(self):
        status, page = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("SZYGIRBIXGHOM3AH", page)
        self.assertNotIn(XSS, page)
        self.assertIn("&lt;script&gt;", page)

    def test_the_signature_page_escapes_the_note_and_the_canon(self):
        _, page = self.get("/signature/SZYGIRBIXGHOM3AH")
        self.assertNotIn(XSS, page)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", page)
        self.assertIn("halt", page)

    def test_the_bug_page_escapes_the_title_and_the_description(self):
        _, page = self.get("/bugs")
        self.assertNotIn(XSS, page)
        _, page = self.get("/bug/B7QK2M4X9ZAB")
        self.assertNotIn(XSS, page)
        self.assertIn("line one", page)

    def test_the_report_page_escapes_the_claim(self):
        _, page = self.get("/report/01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        self.assertNotIn(XSS, page)
        self.assertIn("&lt;script&gt;", page)
        self.assertIn("host_fault", page)

    def test_no_page_carries_a_token_or_the_private_key(self):
        secret_key = self.cfg.secret_key().hex()
        for path in ("/", "/signature/SZYGIRBIXGHOM3AH", "/report/01J9Z6T4Q8M3K7V2B5N0XWAYCD", "/bugs",
                     "/bug/B7QK2M4X9ZAB", "/stats", "/config", "/issue/SZYGIRBIXGHOM3AH"):
            with self.subTest(path=path):
                _, page = self.get(path)
                self.assertNotIn(self.upstream.token, page)
                self.assertNotIn("ghp_fake_token", page)
                self.assertNotIn(secret_key, page)
                self.assertNotIn(self.cfg.public_key().hex(), page)

    def test_no_script_tag_at_all(self):
        for path in ("/", "/signature/SZYGIRBIXGHOM3AH", "/bugs", "/stats"):
            _, page = self.get(path)
            self.assertNotIn("<script", page.lower())

    def test_stats_shows_the_database_warning(self):
        self.upstream.data.stats_extra = {"database_bytes": 460 * 1000 * 1000}
        _, page = self.get("/stats")
        self.assertIn("WARNING", page)
        self.assertIn("500 MB", page)

    def test_an_unknown_page_is_a_404(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.get("/nope")
        self.assertEqual(caught.exception.code, 404)

    def test_an_api_failure_is_shown_as_text(self):
        self.upstream.force_next(404, "not_found", "No such signature")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.get("/signature/SAAAAAAAAAAAAAAA")
        self.assertIn("No such signature", caught.exception.read().decode("utf-8"))

    # -- actions -----------------------------------------------------------
    def test_a_status_change_needs_the_form_token(self):
        status, _ = self.post("/signature/SZYGIRBIXGHOM3AH/status", {"status": "ignored"})
        self.assertEqual(status, 403)
        self.assertEqual(self.upstream.patches, [])
        status, page = self.post("/signature/SZYGIRBIXGHOM3AH/status",
                                 {"status": "fixed", "version": "0.2.0", "note": "done",
                                  "_csrf": self.csrf("/signature/SZYGIRBIXGHOM3AH")})
        self.assertEqual(status, 200)
        self.assertEqual(self.upstream.patches[-1][1],
                         {"status": "fixed", "fixed_in_version": "0.2.0", "note": "done"})

    def test_resample_and_merge(self):
        token = self.csrf("/signature/SZYGIRBIXGHOM3AH")
        self.post("/signature/SZYGIRBIXGHOM3AH/resample", {"_csrf": token})
        self.assertEqual(self.upstream.patches[-1][1], {"resample": True})
        self.post("/signature/SZYGIRBIXGHOM3AH/merge", {"into": "SAAAABBBBCCCCDDD", "_csrf": token})
        self.assertEqual(self.upstream.patches[-1][1], {"merged_into": "SAAAABBBBCCCCDDD"})

    def test_a_bug_status_change(self):
        self.post("/bug/B7QK2M4X9ZAB/status", {"status": "fixed", "_csrf": self.csrf("/bug/B7QK2M4X9ZAB")})
        self.assertEqual(self.upstream.patches[-1], ("B7QK2M4X9ZAB", {"status": "fixed"}))

    def test_the_issue_page_shows_the_draft_and_creates_on_demand(self):
        _, page = self.get("/issue/SZYGIRBIXGHOM3AH")
        self.assertIn("Halt 1420", page)
        self.assertNotIn(XSS, page)
        status, page = self.post("/signature/SZYGIRBIXGHOM3AH/issue", {"_csrf": self.csrf("/issue/SZYGIRBIXGHOM3AH")})
        self.assertEqual(status, 200)
        self.assertEqual(len(self.github.requests), 1)
        self.assertIn("https://github.com/Franckrst/D2Vita/issues/41", page)

    def test_the_ui_refuses_to_file_a_second_issue(self):
        self.upstream.data.signatures["SZYGIRBIXGHOM3AH"]["issue_url"] = "https://github.com/x/y/issues/1"
        token = self.csrf("/issue/SZYGIRBIXGHOM3AH")
        status, page = self.post("/signature/SZYGIRBIXGHOM3AH/issue", {"_csrf": token})
        self.assertEqual(status, 403)
        self.assertIn("--again", page)
        self.assertEqual(self.github.requests, [])

    def test_pull_opens_the_artifacts_on_the_local_disk(self):
        support.store_artifact(self.upstream.data, "01J9Z6T4Q8M3K7V2B5N0XWAYCD", "crash_txt",
                               b"Halt 1420\r\n", self.public_key)
        out = os.path.join(self.tmp.name, "pulled")
        status, page = self.post("/signature/SZYGIRBIXGHOM3AH/pull",
                                 {"dir": out, "_csrf": self.csrf("/signature/SZYGIRBIXGHOM3AH")})
        self.assertEqual(status, 200)
        self.assertIn("crash_txt", page)
        with open(os.path.join(out, "Crash.txt"), "rb") as handle:
            self.assertEqual(handle.read(), b"Halt 1420\r\n")

    def test_a_request_for_another_host_header_is_refused(self):
        request = urllib.request.Request(self.base + "/", method="GET")
        request.add_header("host", "evil.example")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=10)
        self.assertEqual(caught.exception.code, 403)


if __name__ == "__main__":
    unittest.main()
