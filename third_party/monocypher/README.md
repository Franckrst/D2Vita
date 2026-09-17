# Monocypher 4.0.3 (vendored)

Crypto used by the crash reporter (`src/crashreport/`): X25519 and BLAKE2b-256
for the D2VSEAL1 key of a sealed artifact, XChaCha20-Poly1305 for its chunks
(`cr_seal.cpp`), and Ed25519 to verify `X-D2V-Signature` on every API answer
(`cr_verify.cpp`). Nothing else links it.

## Files

Copied unmodified from the official tarball
`https://monocypher.org/download/monocypher-4.0.3.tar.gz` (2026-06-15):

| File | Source in the tarball |
|---|---|
| `monocypher.c`, `monocypher.h` | `src/` |
| `monocypher-ed25519.c`, `monocypher-ed25519.h` | `src/optional/` (Ed25519 with SHA-512, the RFC 8032 variant the contract requires) |
| `LICENCE.md` | tarball root (BSD-2-Clause or CC0-1.0, choose either) |

`SHA256SUMS` holds the SHA-256 of the tarball and of each copied file, computed
here; `sha256sum --check --ignore-missing SHA256SUMS` verifies the copies, and
`tools/tests/run_crashreport_tests.sh` runs that check on every run.

## Provenance

monocypher.org publishes BLAKE2b and SHA-512 checksums, not SHA-256. Both
matched the downloaded tarball:

```text
blake2b  5c586019e8e78ce6fb70f73f4ded10c852335138394b22fb704d0b4379edfc1adaea342f0e4c4b8c52fcf053df401d1fe47812ee5cb5062813de19fc51f8ae93
sha512   40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
sha256   8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66
```

Those two come from the same server as the tarball, so the release asset of tag
`4.0.3` on GitHub (another origin) was downloaded as well: it is byte-identical,
same SHA-256.

## Why 4.0.3 and not the 4.0.2 of the contract

`D2Vita-website/contract/tools/c_check/` pins 4.0.2 for its byte-compatibility
check. 4.0.3 changes no output: its CHANGELOG lists one security fix ("Fixed
timing leak vulnerability in EdDSA/Ed25519 signatures" — `fe_cswap` and
`fe_ccopy` now use a `volatile` mask and an unrolled loop so that a compiler
cannot turn the constant-time swap into a secret-dependent branch), plus
documentation, build-system and warning fixes. The console runs that ladder
with its ephemeral X25519 secret key on every sealed artifact, so it takes the
hardened version; the contract (`contract/README.md`, section Monocypher) says
the vectors hold for 4.0.3 as well.

That is checked, not assumed: `tools/tests/crashreport_transport_test.cpp`
reproduces every case of `tests/crashreport/vectors/sealed.v1.json` and
`response-sig.v1.json` with these sources, and, when `D2V_CONTRACT` points at a
D2Vita-website checkout, the test script also builds the contract's own
`check_sealed.c` against them and runs it over the contract vectors.

## Configuration

**Never compile `monocypher.c` directly: compile
`src/crashreport/cr_monocypher.c`**, which sets `BLAKE2_NO_UNROLLING` and then
includes it. `tools/crashreport_srcs.sh` names that unit, and every build — the
tests here and the eboot of track G — takes its list from there.

The flag changes no output (the tests that reproduce the contract vectors link
the sources built with it) and it takes 17.7 KiB off `.text` at `-Os` for the
Vita (`monocypher.o`: 49,869 bytes without it, 32,177 with), because the
unrolled BLAKE2b compression function is 20 KiB on its own. The reporter hashes
104 bytes twice per sealed artifact, so the speed the unrolling buys is worth
nothing here. Spec section 4.9 budgets 150 KiB of added `.text` for the whole
reporter and leg 7 of `tools/tests/run_crashreport_tests.sh` measures 144,744
of them: this flag is most of the margin, which is why it lives in a source
file, where a build cannot forget it, rather than in a build script. Leg 5
checks exactly that, on the PC compiler and on ARM: given no define at all, the
reporter's unit must come out as the rolled build, and the rolled build must be
the smaller one.

A build that also passes `-ffunction-sections -fdata-sections` and links with
`--gc-sections` drops what the reporter never calls (Argon2, EdDSA signing,
Elligator, HKDF): the same leg reports that figure too.

`crypto_wipe` is used on every key buffer; the caller keeps secrets off the
heap (`cr_seal.cpp` wipes its chunk buffer before freeing it).
