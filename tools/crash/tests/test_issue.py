# The public-safe issue draft, and `crash issue` against a fake GitHub.
import json
import os
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import fakeapi  # noqa: E402
import fakegithub  # noqa: E402
from d2vcrash import api, issue  # noqa: E402

# Everything a draft must never carry, planted in every field an API could
# ever fill: log lines, dump bytes, install and report ids.
LOG_LINE = "[  12.34s] keys.txt: present - classic=oui lod=oui owner=FAKEACCOUNT"
DUMP_BYTES = "7f454c46010101000000"
INSTALL_ID = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e"
INSTALL_HASH = "cd" * 32
REPORT_ID = "01J9Z6T4Q8M3K7V2B5N0XWAYCD"
POISON = (LOG_LINE, DUMP_BYTES, INSTALL_ID, INSTALL_HASH, REPORT_ID, "FAKEACCOUNT", "keys.txt")


def detail_of(**over):
    return api.SignatureDetail(fakeapi.signature_detail(**over))


class DraftTest(unittest.TestCase):
    def test_a_halt_draft_names_the_code_and_the_frames(self):
        draft = issue.draft_for_signature(detail_of())
        self.assertIn("Halt 1420", draft.title)
        self.assertIn("Game+0x1fedf4", draft.title)
        self.assertIn("SZYGIRBIXGHOM3AH", draft.title)
        self.assertIn("7 claims", draft.body)
        self.assertIn("3 distinct consoles", draft.body)
        self.assertIn("0.1.0, 0.1.1", draft.body)
        self.assertIn("2026-09-12", draft.body)                  # first seen
        self.assertIn("Game+0x451c23", draft.body)

    def test_every_kind_gets_a_title(self):
        cases = {
            "halt|904|Codec.cpp:1377|Game+0x97b4": "Halt 904",
            "gfault|0xc0000005|Game+0x2f040|Game+0x51c23": "Guest fault",
            "hfault_jit|0x30004|Game+0x451c23,Game+0x44f570": "translated code",
            "hfault|0.1.0+ab12cd34ef56|0x2f6e6e|0x2fa1f1": "eboot+0x2f6e6e",
            "hfault_sys|SceLibKernel|0x6304": "SceLibKernel+0x6304",
            "hfault_unknown|0.1.0+ab12cd34ef56|0x96924184|0x96924101": "unknown region",
            "exit|unshimmed_import|KERNEL32.dll!GetNumaHighestNodeNumber|-": "unshimmed import",
            "hang|Game+0xfa60c": "Hang",
        }
        kinds = {"halt": "halt", "gfault": "guest_fault", "hfault_jit": "host_fault", "hfault": "host_fault",
                 "hfault_sys": "host_fault", "hfault_unknown": "host_fault", "exit": "abnormal_exit",
                 "hang": "hang"}
        for canon, expected in cases.items():
            with self.subTest(canon=canon):
                draft = issue.draft_for_signature(detail_of(canon=canon, kind=kinds[canon.split("|")[0]]))
                self.assertIn(expected, draft.title)
                self.assertIn("|", draft.body or "")             # the canon block is quoted, not the title

    def test_the_claim_gives_the_whole_frame_list(self):
        claim = fakeapi.claim(features={"code": 1420, "location": "Codec.cpp:1377",
                                        "frames": [f"Game+0x{0x1fe000 + 0x40 * i:x}" for i in range(16)]})
        draft = issue.draft_for_signature(detail_of(), claim)
        self.assertIn("Game+0x1fe3c0", draft.body)
        self.assertIn("Codec.cpp:1377", draft.body)
        self.assertEqual(draft.body.count("Game+0x"), 16 + 3)     # 16 frames, 3 in the canon block

    def test_a_draft_built_from_a_report_full_of_log_text_keeps_none_of_it(self):
        claim = fakeapi.claim(
            install_id=INSTALL_ID,
            report_id=REPORT_ID,
            features={"code": 1420, "location": LOG_LINE,
                      "frames": ["Game+0x1fedf4", LOG_LINE, DUMP_BYTES, "Game+0x" + INSTALL_ID]},
            artifacts=[{"name": "crash_log", "bytes": 3104}],
        )
        detail = detail_of(canon=f"halt|1420|{LOG_LINE}|{DUMP_BYTES}", note=LOG_LINE,
                           sample_report=REPORT_ID,
                           sample_artifacts=[{"name": "crash_log", "bytes": 3104, "sha256": "ab" * 32,
                                              "stored_unix": 1789284100}],
                           recent_reports=[{"report_id": REPORT_ID, "build_id": "0.1.0+ab12cd34ef56",
                                            "received_unix": 1789284100, "action": "upload"}])
        draft = issue.draft_for_signature(detail, claim)
        text = draft.title + "\n" + draft.body
        for secret in POISON:
            self.assertNotIn(secret, text, f"{secret!r} reached the draft")
        self.assertIn("Game+0x1fedf4", text)                      # the one address that is well formed
        self.assertIn("SZYGIRBIXGHOM3AH", text)

    def test_the_draft_says_no_log_or_dump_is_published(self):
        body = issue.draft_for_signature(detail_of()).body.lower()
        self.assertIn("no log", body)

    def test_a_bug_draft_never_carries_the_contact(self):
        bug = api.BugItem(fakeapi.bug_item(contact="player@example.invalid", description="It crashes\nevery time."))
        draft = issue.draft_for_bug(bug)
        self.assertIn("Crash when entering the Rogue Encampment", draft.title)
        self.assertIn("It crashes", draft.body)
        self.assertNotIn("player@example.invalid", draft.title + draft.body)
        self.assertIn("B7QK2M4X9ZAB", draft.body)


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class IssueCommandTest(unittest.TestCase):
    def setUp(self):
        self.server = fakeapi.FakeAdminServer()
        self.server.__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)
        self.addCleanup(lambda: self.assertEqual(self.server.schema_errors, []))
        self.github = fakegithub.FakeGitHub()
        self.github.__enter__()
        self.addCleanup(self.github.__exit__, None, None, None)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_dir = os.path.join(self.tmp.name, "config")
        support.write_config(self.config_dir, self.server.base_url, self.server.token,
                             GITHUB_TOKEN="ghp_fake_token")

    def crash(self, *args, **kwargs):
        env = {issue.GITHUB_API_ENV: self.github.base_url}
        env.update(kwargs.pop("env", {}))
        return support.run_crash(self.config_dir, *args, env=env, **kwargs)

    def test_creates_the_issue_and_records_its_url(self):
        done = self.crash("issue", "SZYGIRBIXGHOM3AH", "--yes")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(self.github.requests), 1)
        request = self.github.requests[0]
        self.assertEqual(request["path"], "/repos/Franckrst/D2Vita/issues")
        self.assertEqual(request["auth"], "Bearer ghp_fake_token")
        self.assertIn("Halt 1420", request["body"]["title"])
        self.assertIn("3 distinct consoles", request["body"]["body"])
        self.assertEqual(self.server.patches[-1],
                         ("SZYGIRBIXGHOM3AH", {"issue_url": "https://github.com/Franckrst/D2Vita/issues/41"}))
        self.assertIn("https://github.com/Franckrst/D2Vita/issues/41", done.stdout)

    def test_shows_the_draft_and_stops_when_the_answer_is_no(self):
        done = self.crash("issue", "SZYGIRBIXGHOM3AH", stdin="n\n")
        self.assertEqual(done.returncode, 1)
        self.assertIn("Halt 1420", done.stdout)                   # the draft was shown
        self.assertEqual(self.github.requests, [])

    def test_json_without_yes_is_a_dry_run(self):
        payload = json.loads(self.crash("issue", "SZYGIRBIXGHOM3AH", "--json").stdout)
        self.assertFalse(payload["created"])
        self.assertIn("Halt 1420", payload["title"])
        self.assertEqual(self.github.requests, [])

    def test_refuses_a_signature_that_already_has_an_issue(self):
        self.server.data.signatures["SZYGIRBIXGHOM3AH"]["issue_url"] = "https://github.com/x/y/issues/1"
        done = self.crash("issue", "SZYGIRBIXGHOM3AH", "--yes")
        self.assertEqual(done.returncode, 1)
        self.assertIn("issues/1", done.stderr)
        self.assertEqual(self.github.requests, [])
        self.assertEqual(self.crash("issue", "SZYGIRBIXGHOM3AH", "--yes", "--again").returncode, 0)

    def test_without_a_github_token_it_says_so(self):
        plain = os.path.join(self.tmp.name, "plain")
        support.write_config(plain, self.server.base_url, self.server.token)
        done = support.run_crash(plain, "issue", "SZYGIRBIXGHOM3AH", "--yes")
        self.assertEqual(done.returncode, 2)
        self.assertIn("GITHUB_TOKEN", done.stderr)

    def test_a_draft_can_be_read_without_a_github_token(self):
        # Reviewing what would be published is exactly the operation that must
        # work before anything is configured to publish it.
        plain = os.path.join(self.tmp.name, "reader")
        support.write_config(plain, self.server.base_url, self.server.token)
        for extra in (("--dry-run",), ("--json",)):
            with self.subTest(extra=extra):
                done = support.run_crash(plain, "issue", "SZYGIRBIXGHOM3AH", *extra)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn("Halt 1420", done.stdout)
                self.assertEqual(self.github.requests, [])

    def test_a_github_failure_is_reported_and_nothing_is_recorded(self):
        self.github.answer_with(422, {"message": "Validation Failed"})
        done = self.crash("issue", "SZYGIRBIXGHOM3AH", "--yes")
        self.assertEqual(done.returncode, 1)
        self.assertIn("422", done.stderr)
        self.assertEqual(self.server.patches, [])

    def test_a_bug_becomes_an_issue_too(self):
        done = self.crash("issue", "B7QK2M4X9ZAB", "--yes")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("Crash when entering the Rogue Encampment", self.github.requests[0]["body"]["title"])
        self.assertEqual(self.server.patches[-1],
                         ("B7QK2M4X9ZAB", {"issue_url": "https://github.com/Franckrst/D2Vita/issues/41"}))

    def test_the_repository_can_be_changed_in_admin_env(self):
        other = os.path.join(self.tmp.name, "other")
        support.write_config(other, self.server.base_url, self.server.token,
                             GITHUB_TOKEN="ghp_fake_token", ISSUES_REPO="Franckrst/D2Vita-website")
        done = support.run_crash(other, "issue", "SZYGIRBIXGHOM3AH", "--yes",
                                 env={issue.GITHUB_API_ENV: self.github.base_url})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.github.requests[0]["path"], "/repos/Franckrst/D2Vita-website/issues")

    def test_no_token_reaches_the_output(self):
        done = self.crash("issue", "SZYGIRBIXGHOM3AH", "--yes")
        self.assertNotIn("ghp_fake_token", done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
