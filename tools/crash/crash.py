#!/usr/bin/env python3
# tools/crash/crash.py — local admin tool of the D2Vita crash reports.
#
#   crash.py list | show | pull | autopsy | status | merge | resample | issue |
#            bugs | builds | forget-install | stats | serve | keygen | config
#
# Runs on the maintainer's machine only: it holds the admin token and the
# X25519 private key that opens the sealed artifacts (design section 6).
# `crash.py <command> --help` documents each command.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from d2vcrash.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
