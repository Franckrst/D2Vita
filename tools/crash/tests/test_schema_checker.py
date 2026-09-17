# The test-side schema checker, proved on the contract's own claim vectors.
#
# The admin tests validate every fake-server response against admin.v1; that is
# only worth something if the checker itself is right. The contract ships 26
# claims that must pass and 70 that must fail at a named JSON pointer.
import json
import os
import sys
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
import jsonschema_lite  # noqa: E402

CLAIM = "claim.v1.schema.json"


def load_vector(name):
    with open(os.path.join(support.VECTOR_DIR, name), encoding="utf-8") as handle:
        return json.load(handle)


@unittest.skipUnless(os.path.isdir(support.SCHEMA_DIR), f"contract schemas not found in {support.SCHEMA_DIR}")
class SchemaCheckerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = jsonschema_lite.Registry(support.SCHEMA_DIR)

    def test_every_valid_claim_vector_passes(self):
        cases = load_vector("signatures.v1.json")["cases"]
        self.assertGreaterEqual(len(cases), 14)
        for case in cases:
            with self.subTest(case=case["name"]):
                self.assertEqual(self.registry.errors(CLAIM, case["claim"]), [])

    def test_every_invalid_claim_vector_fails_where_the_contract_says(self):
        cases = load_vector("claims-invalid.v1.json")["cases"]
        self.assertGreaterEqual(len(cases), 60)
        for case in cases:
            with self.subTest(case=case["name"]):
                errors = self.registry.errors(CLAIM, case["claim"])
                self.assertTrue(errors, "claim was accepted")
                self.assertIn(case["invalid_at"], {p for p, _ in errors})

    def test_admin_definitions_are_reachable_and_strict(self):
        stats = {"v": 1, "day": "2026-09-14", "accepting": True,
                 "usage": {"claims": {"used": 1, "cap": 2000}, "artifact_bytes": {"used": 0, "cap": 314572800},
                           "new_signatures": {"used": 0, "cap": 200}, "bugs": {"used": 0, "cap": 100}}}
        self.assertEqual(self.registry.errors("admin.v1.schema.json#/$defs/Stats", stats), [])
        stats["extra"] = 1
        self.assertTrue(self.registry.errors("admin.v1.schema.json#/$defs/Stats", stats))

    def test_an_unknown_keyword_in_a_schema_is_refused(self):
        with self.assertRaises(jsonschema_lite.SchemaError):
            jsonschema_lite._validate(self.registry, CLAIM, {"multipleOf": 2}, 4, [])

    def test_pattern_is_anchored_like_ecma_262(self):
        # Python's $ matches before a final newline; the contract's does not.
        schema = {"type": "string", "pattern": "^[a-z]+$"}
        self.assertTrue(jsonschema_lite._validate(self.registry, CLAIM, schema, "abc\n", []))
        self.assertEqual(jsonschema_lite._validate(self.registry, CLAIM, schema, "abc", []), [])


if __name__ == "__main__":
    unittest.main()
