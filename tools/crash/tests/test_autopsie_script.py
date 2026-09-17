# tools/autopsie_psp2dmp.py: faulting thread, truncated dumps, bases, symbols.
#
# The dumps are the synthetic ones of tools/tests/gen_fake_psp2dmp.py. The
# maintainer's real dumps are read only through D2V_CRASH_FIXTURES (they can
# hold CD keys and never enter the repository).
import os
import re
import subprocess
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402

SCRIPT = os.path.join(support.REPO_ROOT, "tools", "autopsie_psp2dmp.py")
GENERATOR = os.path.join(support.REPO_ROOT, "tools", "tests", "gen_fake_psp2dmp.py")
FIXTURES = os.environ.get("D2V_CRASH_FIXTURES")


def run_script(*args):
    return subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True, timeout=300)


class AutopsieScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dumps = os.path.join(cls.tmp.name, "dumps")
        done = subprocess.run([sys.executable, GENERATOR, "--out", cls.dumps], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def dump(self, name):
        return os.path.join(self.dumps, name + ".psp2dmp")

    def ok(self, *args):
        done = run_script(*args)
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout

    def faulting_tid(self, output):
        match = re.search(r"== FIL FAUTIF tid=(0x[0-9a-f]+)", output)
        self.assertIsNotNone(match, output)
        return int(match.group(1), 16)

    def frames(self, output):
        return [(int(m.group(2), 16), int(m.group(1), 16))
                for m in re.finditer(r"VA 0x([0-9a-f]+)\s+\(RVA 0x[0-9a-f]+, pour set_alternate\)\s+cadre=([0-9a-f]{8})",
                                     output)]

    # (a) the faulting thread ------------------------------------------------
    def test_takes_the_first_thread_whose_stop_reason_is_not_zero(self):
        # worker_withheld: three threads, the faulting one is the third.
        output = self.ok(self.dump("worker_withheld"))
        self.assertEqual(self.faulting_tid(output), 0x40020301)
        self.assertIn("d2_runner3", output)
        self.assertIn("stop=0x30004", output)

    def test_falls_back_to_the_first_thread_when_no_stop_reason_is_set(self):
        output = self.ok(self.dump("late_threads"), "--first-thread")
        self.assertEqual(self.faulting_tid(output), 0x40010003)

    def test_first_thread_option_keeps_the_old_rule(self):
        self.assertEqual(self.faulting_tid(self.ok(self.dump("worker_withheld"), "--first-thread")), 0x40010003)

    def test_the_thread_list_is_printed_with_the_faulting_one_marked(self):
        output = self.ok(self.dump("worker_withheld"))
        lines = [line for line in output.splitlines() if "d2_runner3" in line]
        self.assertTrue(any("FAUTIF" in line for line in lines), output)

    # (b) truncated dumps ----------------------------------------------------
    def test_a_dump_whose_notes_are_announced_past_the_end_still_reads(self):
        output = self.ok(self.dump("truncated"))
        self.assertEqual(self.faulting_tid(output), 0x40010003)
        self.assertIn("tronque", output.lower())

    def test_frames_of_a_truncated_dump_are_the_ones_that_are_there(self):
        output = self.ok(self.dump("truncated"))
        self.assertEqual([frame for frame, _ in self.frames(output)], [0x0f2e1028, 0x0f2e1100])

    def test_a_dump_cut_inside_its_program_headers_says_so(self):
        import gzip
        with gzip.open(self.dump("first_jit"), "rb") as handle:
            elf = handle.read()
        cut = os.path.join(self.tmp.name, "cut.psp2dmp")
        with open(cut, "wb") as handle:
            handle.write(gzip.compress(elf[:60]))          # header plus a quarter of one entry
        done = run_script(cut)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("THREAD_REG_INFO", done.stdout + done.stderr)

    def test_a_dump_that_is_not_an_elf_fails_with_a_message(self):
        done = run_script(self.dump("not_elf"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("ELF", done.stdout + done.stderr)

    # (c) bases --------------------------------------------------------------
    def test_default_bases_are_the_historical_constants(self):
        output = self.ok(self.dump("first_jit"))
        self.assertEqual([frame for frame, _ in self.frames(output)],
                         [0x0f2e1028, 0x0f2e1100, 0x0f2e1400, 0x0f2e2000, 0x0f2e4000])
        self.assertEqual(self.frames(output)[0][1], 0x451c23)      # VA of Game+0x51c23 (RVA + 0x400000)

    def test_another_arena_host_base_finds_no_chain(self):
        # 0x85000000 is a real, non-default membase value (seen on a real
        # console log): with the wrong one the guest stack is not where the
        # script looks.
        output = self.ok(self.dump("first_jit"), "--arena-host-base", "0x85000000")
        self.assertEqual(self.frames(output), [])
        self.assertIn("hors dump", output)

    def test_another_game_base_moves_every_frame(self):
        output = self.ok(self.dump("first_jit"), "--game-base", "0x01a00000")
        self.assertEqual(self.frames(output), [])                 # returns outside the new Game.exe span
        shifted = self.ok(self.dump("first_jit"), "--game-base", "0x01900000")
        self.assertEqual(len(self.frames(shifted)), 5)

    # symbols ----------------------------------------------------------------
    def nm_file(self):
        path = os.path.join(self.tmp.name, "nm.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("81000730 T main\n812d2e00 T d2vita_other\n812f6e00 T d2vita_present_flip\n"
                         "812fa100 T d2vita_upload\n         w __cxa_something\n")
        return path

    def test_symbolizes_the_eboot_once_the_bias_is_known(self):
        # eboot_nostamp: pc 0x812f6e6e, lr 0x812fa1f1, main linked at 0x81000730.
        output = self.ok(self.dump("eboot_nostamp"), "--nm", self.nm_file(), "--main-runtime", "0x81000730")
        self.assertIn("biais=0x0", output)
        self.assertIn("d2vita_present_flip+0x6e", output)
        self.assertIn("d2vita_upload+0xf1", output)

    def test_the_bias_moves_every_symbol(self):
        # rt_boot.cpp publishes "module: main a l'execution=0x81024730".
        output = self.ok(self.dump("eboot_nostamp"), "--nm", self.nm_file(), "--main-runtime", "0x81024730")
        self.assertIn("biais=0x24000", output)
        self.assertIn("d2vita_other+0x6e", output)
        self.assertNotIn("d2vita_present_flip", output)

    def test_without_a_bias_no_symbol_is_invented(self):
        output = self.ok(self.dump("eboot_nostamp"), "--nm", self.nm_file())
        self.assertIn("biais inconnu", output)
        self.assertNotIn("d2vita_present_flip", output)
        self.assertNotIn("sym:", output)

    def test_the_elf_link_address_gives_the_bias(self):
        import struct
        elf = os.path.join(self.tmp.name, "d2vita.elf")
        header = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8 + struct.pack(
            "<HHIIIIIHHHHHH", 2, 40, 1, 0x81000730, 52, 0, 0x05000000, 52, 32, 1, 0, 0, 0)
        phdr = struct.pack("<8I", 1, 0, 0x81000000, 0, 0x1000, 0x1000, 5, 4)
        with open(elf, "wb") as handle:
            handle.write(header + phdr)
        output = self.ok(self.dump("eboot_nostamp"), "--nm", self.nm_file(), "--elf", elf)
        self.assertIn("MODULE_INFO(d2vita.elf)", output)      # module base 0x81000000, link 0x81000000
        self.assertIn("d2vita_present_flip+0x6e", output)

    def test_names_the_module_of_the_faulting_address(self):
        output = self.ok(self.dump("d2_sysmodule"))
        self.assertIn("SceLibKernel", output)

    # real dumps -------------------------------------------------------------
    @unittest.skipUnless(FIXTURES, "D2V_CRASH_FIXTURES is not set")
    def test_reads_every_real_dump_of_the_fixtures_folder(self):
        dumps = sorted(f for f in os.listdir(FIXTURES) if f.endswith(".psp2dmp"))
        self.assertTrue(dumps, f"no *.psp2dmp in {FIXTURES}")
        for name in dumps:
            with self.subTest(dump=name):
                done = run_script(os.path.join(FIXTURES, name))
                self.assertEqual(done.returncode, 0, done.stderr[-400:])
                self.assertIn("FIL FAUTIF", done.stdout)


if __name__ == "__main__":
    unittest.main()
