import struct
import tempfile
import os
import unittest

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from screen_scan_diff import find_candidates


def _write(path, words):
    with open(path, "wb") as f:
        f.write(struct.pack(f"<{len(words)}I", *words))


class FindCandidatesTest(unittest.TestCase):
    def test_single_offset_distinguishes_one_label_from_the_rest(self):
        with tempfile.TemporaryDirectory() as d:
            # offset 8 (word index 2) is 1 on both title runs, 0 everywhere else;
            # every other offset is noisy/shared and must NOT be reported.
            _write(os.path.join(d, "title_a.bin"),      [5, 9, 1, 3])
            _write(os.path.join(d, "title_b.bin"),      [7, 9, 1, 4])
            _write(os.path.join(d, "charselect_a.bin"), [5, 9, 0, 3])
            _write(os.path.join(d, "charselect_b.bin"), [7, 9, 0, 4])
            _write(os.path.join(d, "options_a.bin"),    [5, 9, 0, 3])
            _write(os.path.join(d, "options_b.bin"),    [7, 9, 0, 4])
            _write(os.path.join(d, "game_a.bin"),       [5, 9, 0, 3])
            _write(os.path.join(d, "game_b.bin"),       [7, 9, 0, 4])

            candidates = find_candidates(d, target_label="title",
                                          other_labels=["charselect", "options", "game"])

            self.assertEqual(candidates, [(8, 1)])   # (byte offset, title's stable value)

    def test_no_candidate_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as d:
            # nothing distinguishes title from the rest: every word is either
            # noisy within a label or identical across all labels.
            _write(os.path.join(d, "title_a.bin"),      [1, 1])
            _write(os.path.join(d, "title_b.bin"),      [2, 1])
            _write(os.path.join(d, "charselect_a.bin"), [1, 1])
            _write(os.path.join(d, "charselect_b.bin"), [2, 1])

            candidates = find_candidates(d, target_label="title",
                                          other_labels=["charselect"])

            self.assertEqual(candidates, [])

    def test_missing_snapshot_file_raises_a_clear_error(self):
        with tempfile.TemporaryDirectory() as d:
            _write(os.path.join(d, "title_a.bin"), [1])
            with self.assertRaises(FileNotFoundError):
                find_candidates(d, target_label="title", other_labels=["charselect"])


if __name__ == "__main__":
    unittest.main()
