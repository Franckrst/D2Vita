// src/crashreport/cr_verify.cpp — Ed25519 check of X-D2V-Signature.
#include "crashreport/cr_verify.h"

#include "monocypher-ed25519.h"   // third_party/monocypher

namespace d2cr {

namespace {

// RFC 4648 §4 value of one character, or -1.
int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

bool decode_signature(const std::string& b64, uint8_t out[kEd25519SignatureBytes]) {
    // 64 bytes encode as 21 full groups (84 characters) plus one byte written
    // as two characters and "==".
    if (b64.size() != kSignatureHeaderChars) return false;
    if (b64[86] != '=' || b64[87] != '=') return false;
    size_t written = 0;
    for (size_t i = 0; i < 84; i += 4) {          // 21 full groups, 63 bytes
        const int a = b64_value(b64[i]), b = b64_value(b64[i + 1]);
        const int c = b64_value(b64[i + 2]), d = b64_value(b64[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        out[written++] = (uint8_t)((a << 2) | (b >> 4));
        out[written++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
        out[written++] = (uint8_t)(((c & 0x03) << 6) | d);
    }
    const int a = b64_value(b64[84]), b = b64_value(b64[85]);   // last byte, four unused bits
    if (a < 0 || b < 0 || (b & 0x0F) != 0) return false;
    out[written++] = (uint8_t)((a << 2) | (b >> 4));
    return written == kEd25519SignatureBytes;
}

bool verify_response(const uint8_t public_key[kEd25519PublicKeyBytes], const std::string& signature_b64,
                     const std::string& body) {
    uint8_t signature[kEd25519SignatureBytes];
    if (!public_key || !decode_signature(signature_b64, signature)) return false;
    return crypto_ed25519_check(signature, public_key, (const uint8_t*)body.data(), body.size()) == 0;
}

}  // namespace d2cr
