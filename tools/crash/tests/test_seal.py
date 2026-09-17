# Opening and writing D2VSEAL1 objects, against the contract vectors.
import json
import os
import sys
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
from d2vcrash import seal  # noqa: E402

VECTORS = json.load(open(os.path.join(support.VECTOR_DIR, "sealed.v1.json"), encoding="utf-8"))


class SealedVectorTest(unittest.TestCase):
    """contract/vectors/sealed.v1.json: every case, produced and opened back."""

    def test_every_case_is_reproduced(self):
        self.assertEqual(len(VECTORS["cases"]), 10)
        for case in VECTORS["cases"]:
            with self.subTest(case=case["name"]):
                recipient_pk = bytes.fromhex(case["recipient_pk_hex"])
                eph_sk = bytes.fromhex(case["eph_sk_hex"])
                plaintext = bytes.fromhex(case["plaintext_hex"])
                # Intermediate values, so a disagreement says where it is.
                self.assertEqual(seal.public_key(eph_sk).hex(), case["eph_pk_hex"])
                self.assertEqual(seal.key_id(recipient_pk).hex(), case["key_id_hex"])
                self.assertEqual(seal.shared_secret(eph_sk, recipient_pk).hex(), case["shared_hex"])
                self.assertEqual(
                    seal.derive_key(bytes.fromhex(case["shared_hex"]), bytes.fromhex(case["eph_pk_hex"]), recipient_pk).hex(),
                    case["key_hex"],
                )
                sealed = seal.seal(plaintext, recipient_pk, eph_sk=eph_sk,
                                   nonce_prefix=bytes.fromhex(case["nonce_prefix_hex"]),
                                   chunk_size=case["chunk_size"])
                self.assertEqual(sealed.hex(), case["sealed_hex"])
                self.assertEqual(seal.open_sealed(sealed, bytes.fromhex(case["recipient_sk_hex"])), plaintext)

    def test_every_negative_case_fails_as_the_contract_says(self):
        self.assertEqual(len(VECTORS["negative_cases"]), 21)
        for case in VECTORS["negative_cases"]:
            with self.subTest(case=case["name"]):
                with self.assertRaises(seal.SealedError) as caught:
                    seal.open_sealed(bytes.fromhex(case["sealed_hex"]), bytes.fromhex(case["recipient_sk_hex"]))
                self.assertEqual(caught.exception.kind, case["expect"])

    def test_public_key_of_a_recipient_secret_key(self):
        case = VECTORS["cases"][0]
        self.assertEqual(seal.public_key(bytes.fromhex(case["recipient_sk_hex"])).hex(), case["recipient_pk_hex"])


class VectorCopyTest(unittest.TestCase):
    """tests/crashreport/vectors/ is a copy of the contract; keep it current."""

    @unittest.skipUnless(os.path.isdir(os.path.join(support.CONTRACT_DIR, "vectors")),
                         "the contract is not checked out next to this repository")
    def test_the_copied_vectors_are_the_contract_ones(self):
        import subprocess
        script = os.path.join(support.REPO_ROOT, "tools", "tests", "sync_contract_vectors.sh")
        done = subprocess.run([script, "--check", support.CONTRACT_DIR], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


class SealRoundTripTest(unittest.TestCase):
    def test_random_key_pair_round_trip_over_several_chunks(self):
        sk = seal.generate_secret_key()
        pk = seal.public_key(sk)
        plaintext = bytes(range(256)) * 700          # 179 200 bytes: three chunks
        sealed = seal.seal(plaintext, pk)
        self.assertEqual(len(sealed), seal.sealed_size(len(plaintext)))
        self.assertEqual(seal.open_sealed(sealed, sk), plaintext)

    def test_two_sealings_of_the_same_bytes_differ(self):
        sk = seal.generate_secret_key()
        pk = seal.public_key(sk)
        self.assertNotEqual(seal.seal(b"abc", pk), seal.seal(b"abc", pk))

    def test_open_file_writes_the_plaintext(self):
        import tempfile
        sk = seal.generate_secret_key()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "a.sealed")
            dst = os.path.join(tmp, "a.bin")
            with open(src, "wb") as handle:
                handle.write(seal.seal(b"hello dump", seal.public_key(sk)))
            self.assertEqual(seal.open_sealed_file(src, dst, sk), 10)
            with open(dst, "rb") as handle:
                self.assertEqual(handle.read(), b"hello dump")

    def test_sealed_size_matches_the_contract_formula(self):
        for n, chunk, expected in ((0, 65536, 72 + 16), (1, 65536, 72 + 1 + 16), (65536, 65536, 72 + 65536 + 32),
                                   (70000, 65536, 72 + 70000 + 32)):
            self.assertEqual(seal.sealed_size(n, chunk), expected)


if __name__ == "__main__":
    unittest.main()
