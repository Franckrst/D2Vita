# `crash autopsy`: open the dump, then run tools/autopsie_psp2dmp.py on it.
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import fakeapi  # noqa: E402
from d2vcrash import autopsy  # noqa: E402

GENERATOR = os.path.join(support.REPO_ROOT, "tools", "tests", "gen_fake_psp2dmp.py")
BOOT_PROGRESS = """[   0.00s] env.txt: D2NET=1
[   0.00s] module: main a l'execution=0x81024730 (biais = cette valeur - nm(main))
[   1.00s] arene: base=0x84300000 membase=0x84000000 span=271 Mo reserve=284 Mo
[   2.00s] mode: 1.14 monolith
"""


class BootProgressScanTest(unittest.TestCase):
    def test_reads_the_arena_and_main_lines(self):
        facts = autopsy.scan_boot_progress(BOOT_PROGRESS)
        self.assertEqual(facts["arena_host_base"], 0x84000000)
        self.assertEqual(facts["main_runtime"], 0x81024730)

    def test_reads_a_nondefault_membase_line(self):
        line = "[   1.00s] arene: base=0x84300000 membase=0x85000000 span=271 Mo reserve=284 Mo\n"
        self.assertEqual(autopsy.scan_boot_progress(line)["arena_host_base"], 0x85000000)

    def test_says_nothing_when_the_log_says_nothing(self):
        self.assertEqual(autopsy.scan_boot_progress("[ 0.00s] boot\n"), {})


class SymbolsTest(unittest.TestCase):
    def test_finds_nm_and_the_elf_of_one_build(self):
        with tempfile.TemporaryDirectory() as root:
            build = os.path.join(root, "0.1.0+ab12cd34ef56")
            os.makedirs(build)
            self.assertEqual(autopsy.symbols_for("0.1.0+ab12cd34ef56", root), {})
            for name in ("nm.txt", "d2vita_boot.elf"):
                with open(os.path.join(build, name), "w", encoding="utf-8") as handle:
                    handle.write("81000730 T main\n")
            found = autopsy.symbols_for("0.1.0+ab12cd34ef56", root)
            self.assertEqual(found["nm"], os.path.join(build, "nm.txt"))
            self.assertEqual(found["elf"], os.path.join(build, "d2vita_boot.elf"))
            self.assertEqual(autopsy.symbols_for("0.9.9+000000000000", root), {})

    def test_a_build_id_that_is_not_one_looks_nowhere(self):
        # The build id comes from a claim; the client refuses a malformed one,
        # and the symbol lookup never walks out of its root either.
        with tempfile.TemporaryDirectory() as root:
            symbols = os.path.join(root, "symbols")
            outside = os.path.join(root, "outside")
            os.makedirs(symbols)
            os.makedirs(outside)
            with open(os.path.join(outside, "nm.txt"), "w", encoding="utf-8") as handle:
                handle.write("81000730 T main\n")
            for build in ("../outside", "./../outside", "outside/../../outside", "", None):
                with self.subTest(build=build):
                    self.assertEqual(autopsy.symbols_for(build, symbols), {})


class RunTest(unittest.TestCase):
    def test_a_postmortem_that_overruns_is_reported_like_any_other_failure(self):
        # TimeoutExpired is not an OSError: uncaught, it would print a Python
        # traceback instead of a message, on the command line and in the UI.
        code, output = autopsy.run([sys.executable, "-c", "import time; time.sleep(30)"], timeout=0.5)
        self.assertEqual(code, 124)
        self.assertIn("0.5", output)
        self.assertIn("longer", output)


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class AutopsyCommandTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dumps = tempfile.TemporaryDirectory()
        done = subprocess.run([sys.executable, GENERATOR, "--out", cls.dumps.name], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
        with open(os.path.join(cls.dumps.name, "first_jit.psp2dmp"), "rb") as handle:
            cls.dump_bytes = handle.read()

    @classmethod
    def tearDownClass(cls):
        cls.dumps.cleanup()

    def setUp(self):
        self.server = fakeapi.FakeAdminServer()
        self.server.__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)
        self.addCleanup(lambda: self.assertEqual(self.server.schema_errors, []))
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_dir = os.path.join(self.tmp.name, "config")
        self.public_key = support.write_config(self.config_dir, self.server.base_url, self.server.token)
        self.signature, self.report = fakeapi.add_host_fault(self.server.data)
        support.store_artifact(self.server.data, self.report, "dump", self.dump_bytes, self.public_key)
        support.store_artifact(self.server.data, self.report, "boot_progress",
                               BOOT_PROGRESS.encode("utf-8"), self.public_key)
        self.symbols_root = os.path.join(self.tmp.name, "symbols")

    def crash(self, *args, **kwargs):
        env = {autopsy.SYMBOLS_DIR_ENV: self.symbols_root}
        env.update(kwargs.pop("env", {}))
        return support.run_crash(self.config_dir, *args, env=env, **kwargs)

    def test_opens_the_dump_and_runs_the_reference_autopsy(self):
        out = os.path.join(self.tmp.name, "work")
        done = self.crash("autopsy", self.signature, "--dir", out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("FIL FAUTIF", done.stdout)
        self.assertIn("CHAINE D APPEL INVITEE", done.stdout)
        self.assertIn("VA 0x451c23", done.stdout)                 # the guest chain of the dump
        with open(os.path.join(out, "dump.psp2dmp"), "rb") as handle:
            self.assertEqual(handle.read(), self.dump_bytes)

    def test_takes_the_arena_base_from_the_boot_progress_of_the_same_report(self):
        payload = json.loads(self.crash("autopsy", self.report, "--dir", os.path.join(self.tmp.name, "w2"),
                                        "--json").stdout)
        self.assertEqual(payload["bases"]["arena_host_base"], "0x84000000")
        self.assertEqual(payload["bases"]["source"], "boot_progress")
        self.assertIn("--arena-host-base", payload["command"])
        self.assertEqual(payload["exit_code"], 0)
        self.assertIn("FIL FAUTIF", payload["output"])

    def test_passes_the_symbols_of_that_build_when_they_are_archived(self):
        build = os.path.join(self.symbols_root, "0.1.0+ab12cd34ef56")
        os.makedirs(build)
        with open(os.path.join(build, "nm.txt"), "w", encoding="utf-8") as handle:
            handle.write("81000730 T main\n812f6e00 T d2vita_present_flip\n")
        payload = json.loads(self.crash("autopsy", self.report, "--dir", os.path.join(self.tmp.name, "w3"),
                                        "--json").stdout)
        self.assertEqual(payload["symbols"]["nm"], os.path.join(build, "nm.txt"))
        self.assertIn("--nm", payload["command"])
        self.assertIn("--main-runtime", payload["command"])       # the bias of that build
        self.assertIn("biais=0x24000", payload["output"])

    def test_says_where_symbols_would_go_when_there_are_none(self):
        done = self.crash("autopsy", self.report, "--dir", os.path.join(self.tmp.name, "w4"))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("0.1.0+ab12cd34ef56", done.stdout)
        self.assertIn("no symbols", done.stdout)

    def test_a_signature_without_a_dump_is_refused(self):
        done = self.crash("autopsy", "SZYGIRBIXGHOM3AH", "--dir", os.path.join(self.tmp.name, "w5"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("dump", done.stderr)

    def test_the_bases_can_be_forced_on_the_command_line(self):
        payload = json.loads(self.crash("autopsy", self.report, "--dir", os.path.join(self.tmp.name, "w6"),
                                        "--arena-host-base", "0x85000000", "--game-base", "0x01a00000",
                                        "--json").stdout)
        self.assertEqual(payload["bases"]["arena_host_base"], "0x85000000")
        self.assertEqual(payload["bases"]["game_base"], "0x1a00000")
        self.assertEqual(payload["bases"]["source"], "command line")


if __name__ == "__main__":
    unittest.main()
