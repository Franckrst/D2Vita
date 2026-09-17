#!/usr/bin/env python3
# tools/tests/crashreport_contract_check.py — claims built by the extraction
# library, checked against the D2Vita-website contract.
#
#   crashreport_contract_check.py --contract DIR CLAIM.json...
#
# DIR is the contract/ directory of a D2Vita-website checkout (or the checkout
# itself). Each claim file holds the exact bytes of build_claim_json(); it
# must be at most 16384 bytes (design 4.4), valid against
# schemas/claim.v1.schema.json, and get a canon and a signature from the
# reference rules. All three come from the contract's own
# tools/check_schemas.py (schema_errors, canon, signature_id), so a contract
# change is picked up without touching this file.
#
# Validation needs the jsonschema package, which contract/.venv provides:
# run_crashreport_tests.sh starts this script with DIR/.venv/bin/python when
# it exists.
import argparse
import importlib.util
import json
import os
import sys

CLAIM_MAX_BYTES = 16384


def load_checker(contract_dir):
    path = os.path.join(contract_dir, 'tools', 'check_schemas.py')
    if not os.path.isfile(path):
        raise SystemExit('no tools/check_schemas.py under %s' % contract_dir)
    spec = importlib.util.spec_from_file_location('d2v_contract_check_schemas', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--contract', required=True)
    ap.add_argument('claims', nargs='+')
    a = ap.parse_args()
    contract_dir = a.contract
    if os.path.isdir(os.path.join(contract_dir, 'contract', 'schemas')):
        contract_dir = os.path.join(contract_dir, 'contract')
    cs = load_checker(contract_dir)
    failures = 0
    for path in a.claims:
        name = os.path.basename(path)
        with open(path, 'rb') as f:
            raw = f.read()
        if len(raw) > CLAIM_MAX_BYTES:
            print('  FAIL %s: %d bytes, over %d' % (name, len(raw), CLAIM_MAX_BYTES))
            failures += 1
            continue
        claim = json.loads(raw.decode('utf-8'))
        errors = cs.schema_errors('claim.v1', claim)
        if errors:
            failures += 1
            print('  FAIL %s: %d schema error(s)' % (name, len(errors)))
            for e in errors:
                print('       %s: %s' % (cs.json_pointer(e.absolute_path) or '/', e.message))
            continue
        canon = cs.canon(claim)
        print('  OK   %s: %s %s' % (name, cs.signature_id(canon), canon))
    print('   %d claim(s), %d failure(s)' % (len(a.claims), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
