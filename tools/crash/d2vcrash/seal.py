# D2VSEAL1, the sealed container of crash artifacts (contract/sealed-format.md).
#
# The console seals every artifact for the maintainer's X25519 public key; this
# module is the only place that holds the private key in memory. Sealing is here
# for the tests and for end-to-end checks: production sealing happens on the
# console (Monocypher).
#
# Layout (little endian), header = the first 72 bytes:
#   0  8  magic "D2VSEAL1"      8  8  key_id = BLAKE2b-256(recipient_pk)[0:8]
#   16 32 ephemeral X25519 pk   48 16 nonce prefix
#   64 4  chunk_size (u32)      68 4  reserved, zero
# key   = BLAKE2b-256("D2VSEAL1" || X25519(eph_sk, recipient_pk) || eph_pk || recipient_pk)
# chunk i = XChaCha20-Poly1305 IETF(nonce = prefix || u64(i), ad = header || last)
import os
import struct

from nacl import bindings
from nacl.exceptions import CryptoError
from nacl.hash import blake2b
from nacl.encoding import RawEncoder

MAGIC = b"D2VSEAL1"
HEADER_SIZE = 72
TAG_SIZE = 16
NONCE_PREFIX_SIZE = 16
KEY_ID_SIZE = 8
DEFAULT_CHUNK_SIZE = 65536
MIN_CHUNK_SIZE = 1
MAX_CHUNK_SIZE = 1 << 20

# The four results of sealed-format.md section 6, in the order they are checked.
TRUNCATED = "truncated"
MALFORMED = "malformed"
WRONG_KEY = "wrong_key"
TAMPERED = "tampered"


class SealedError(Exception):
    """A sealed object could not be opened. `kind` is one of the four results."""

    def __init__(self, kind, detail=""):
        super().__init__(f"{kind}: {detail}" if detail else kind)
        self.kind = kind
        self.detail = detail


def generate_secret_key():
    """A new X25519 private key (32 random bytes, clamped by the primitive)."""
    return os.urandom(32)


def public_key(secret_key):
    _check_key(secret_key, "secret key")
    return bindings.crypto_scalarmult_base(secret_key)


def shared_secret(secret_key, peer_public_key):
    _check_key(secret_key, "secret key")
    _check_key(peer_public_key, "public key")
    try:
        return bindings.crypto_scalarmult(secret_key, peer_public_key)
    except (CryptoError, RuntimeError) as exc:      # all-zero output (low order point)
        raise SealedError(MALFORMED, "X25519 gives an all-zero shared secret") from exc


def blake2b_256(data):
    return blake2b(data, digest_size=32, encoder=RawEncoder)


def key_id(recipient_public_key):
    return blake2b_256(recipient_public_key)[:KEY_ID_SIZE]


def derive_key(shared, eph_public_key, recipient_public_key):
    return blake2b_256(MAGIC + shared + eph_public_key + recipient_public_key)


def chunk_count(size, chunk_size=DEFAULT_CHUNK_SIZE):
    """k = floor(n / C) + 1: the last chunk is empty when n is a multiple of C."""
    return size // chunk_size + 1


def sealed_size(size, chunk_size=DEFAULT_CHUNK_SIZE):
    return HEADER_SIZE + size + TAG_SIZE * chunk_count(size, chunk_size)


def build_header(recipient_public_key, eph_public_key, nonce_prefix, chunk_size):
    return (MAGIC + key_id(recipient_public_key) + eph_public_key + nonce_prefix
            + struct.pack("<II", chunk_size, 0))


def chunk_nonce(nonce_prefix, index):
    return nonce_prefix + struct.pack("<Q", index)


def seal(plaintext, recipient_public_key, eph_sk=None, nonce_prefix=None, chunk_size=DEFAULT_CHUNK_SIZE):
    """Seal `plaintext` for `recipient_public_key`.

    eph_sk and nonce_prefix are only given by the test vectors: production
    seals draw both from the system CSPRNG and never reuse them.
    """
    _check_key(recipient_public_key, "recipient public key")
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise ValueError(f"chunk_size {chunk_size} outside [1, {MAX_CHUNK_SIZE}]")
    eph_sk = generate_secret_key() if eph_sk is None else eph_sk
    nonce_prefix = os.urandom(NONCE_PREFIX_SIZE) if nonce_prefix is None else nonce_prefix
    if len(nonce_prefix) != NONCE_PREFIX_SIZE:
        raise ValueError("nonce_prefix must be 16 bytes")
    eph_pk = public_key(eph_sk)
    key = derive_key(shared_secret(eph_sk, recipient_public_key), eph_pk, recipient_public_key)
    header = build_header(recipient_public_key, eph_pk, nonce_prefix, chunk_size)
    out = [header]
    total = chunk_count(len(plaintext), chunk_size)
    for index in range(total):
        chunk = plaintext[index * chunk_size:(index + 1) * chunk_size]
        last = b"\x01" if index == total - 1 else b"\x00"
        out.append(bindings.crypto_aead_xchacha20poly1305_ietf_encrypt(
            chunk, header + last, chunk_nonce(nonce_prefix, index), key))
    return b"".join(out)


def open_sealed(sealed, recipient_secret_key):
    """Open a sealed object, or raise SealedError with the contract's result.

    Checks run in the order of sealed-format.md section 6: size, header, key
    id, shared secret, last-chunk presence, then authentication.
    """
    _check_key(recipient_secret_key, "recipient secret key")
    if len(sealed) < HEADER_SIZE:
        raise SealedError(TRUNCATED, f"{len(sealed)} bytes, header is {HEADER_SIZE}")
    header = bytes(sealed[:HEADER_SIZE])
    chunk_size, reserved = struct.unpack_from("<II", header, 64)
    if header[:8] != MAGIC:
        raise SealedError(MALFORMED, "magic is not D2VSEAL1")
    if reserved != 0:
        raise SealedError(MALFORMED, "reserved bytes are not zero")
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise SealedError(MALFORMED, f"chunk_size {chunk_size} outside [1, {MAX_CHUNK_SIZE}]")
    recipient_pk = public_key(recipient_secret_key)
    if header[8:16] != key_id(recipient_pk):
        raise SealedError(WRONG_KEY, "sealed for another public key")
    eph_pk = header[16:48]
    nonce_prefix = header[48:64]
    key = derive_key(shared_secret(recipient_secret_key, eph_pk), eph_pk, recipient_pk)

    body = memoryview(sealed)[HEADER_SIZE:]
    framed = chunk_size + TAG_SIZE
    if len(body) % framed < TAG_SIZE:
        raise SealedError(TRUNCATED, "no last chunk at the end of the object")
    total = len(body) // framed + 1
    out = []
    for index in range(total):
        piece = bytes(body[index * framed:(index + 1) * framed])
        last = b"\x01" if index == total - 1 else b"\x00"
        try:
            out.append(bindings.crypto_aead_xchacha20poly1305_ietf_decrypt(
                piece, header + last, chunk_nonce(nonce_prefix, index), key))
        except CryptoError as exc:
            raise SealedError(TAMPERED, f"chunk {index} does not authenticate") from exc
    return b"".join(out)


def open_sealed_file(sealed_path, plaintext_path, recipient_secret_key):
    """Open `sealed_path` into `plaintext_path`; returns the plaintext size.

    Artifacts are capped at 2 MiB sealed (contract README), so the whole
    object is read at once; nothing here streams a multi-gigabyte file.
    """
    with open(sealed_path, "rb") as handle:
        sealed = handle.read()
    plaintext = open_sealed(sealed, recipient_secret_key)
    with open(plaintext_path, "wb") as handle:
        handle.write(plaintext)
    return len(plaintext)


def _check_key(value, what):
    if not isinstance(value, (bytes, bytearray)) or len(value) != 32:
        raise ValueError(f"{what} must be 32 bytes")
