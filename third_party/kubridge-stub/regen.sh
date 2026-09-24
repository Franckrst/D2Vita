#!/usr/bin/env bash
# Regenerates libkubridge_stub_weak.a from stubs.yml (vita-libs-gen, VitaSDK).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
vita-libs-gen "$HERE/stubs.yml" "$T"
make -C "$T" >/dev/null
cp "$T/libkubridge_stub_weak.a" "$HERE/"
echo "libkubridge_stub_weak.a regenerated"
