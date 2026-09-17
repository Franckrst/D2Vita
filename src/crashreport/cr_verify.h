// src/crashreport/cr_verify.h — Ed25519 check of X-D2V-Signature.
//
// Reports travel over plain HTTP (spec §4.8), so a hostile network could
// answer for the API: every console answer is signed and the console checks it
// before reading the body. A missing or invalid signature is handled like a
// network failure — retry later — and never applies retry_after_s or
// disable_until_unix (D2Vita-website contract/README.md, response signatures).
//
// Algorithm: Ed25519 of RFC 8032 (SHA-512, no prehash, no context) over the
// exact bytes of the response body, transported as standard base64 with
// padding (88 characters). The API's public key is built into the eboot.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace d2cr {

constexpr size_t kEd25519PublicKeyBytes = 32;
constexpr size_t kEd25519SignatureBytes = 64;
constexpr size_t kSignatureHeaderChars = 88;   // base64 of 64 bytes, padded

// Decodes the X-D2V-Signature header value. Exactly 88 characters of the
// RFC 4648 §4 alphabet ending with "==", no whitespace, and the unused bits of
// the last character must be zero: the Worker writes nothing else, so anything
// else is refused rather than guessed.
bool decode_signature(const std::string& b64, uint8_t out[kEd25519SignatureBytes]);

// True when `signature_b64` signs the exact bytes of `body` under
// `public_key`. False on any decoding or verification failure.
bool verify_response(const uint8_t public_key[kEd25519PublicKeyBytes], const std::string& signature_b64,
                     const std::string& body);

}  // namespace d2cr
