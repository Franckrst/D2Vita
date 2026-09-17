// src/crashreport/cr_seal.cpp — D2VSEAL1 sealing, one chunk at a time.
#include "crashreport/cr_seal.h"

#include <cstring>
#include <new>

#include "monocypher.h"   // third_party/monocypher, C linkage declared by the header

namespace d2cr {

namespace {

const uint8_t kMagic[8] = {'D', '2', 'V', 'S', 'E', 'A', 'L', '1'};

void store32_le(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)(v >> 16);
    out[3] = (uint8_t)(v >> 24);
}

void store64_le(uint8_t* out, uint64_t v) {
    store32_le(out, (uint32_t)v);
    store32_le(out + 4, (uint32_t)(v >> 32));
}

}  // namespace

uint64_t sealed_size_for(uint64_t plain_bytes, uint32_t chunk_bytes) {
    if (chunk_bytes == 0) return 0;
    return kSealHeaderBytes + plain_bytes + kSealTagBytes * (plain_bytes / chunk_bytes + 1);
}

Sealer::~Sealer() { wipe(); }

void Sealer::wipe() {
    crypto_wipe(key_, sizeof key_);
    if (buf_) {
        crypto_wipe(buf_, chunk_bytes_ + kSealTagBytes);   // never hand a plaintext back to the heap
        delete[] buf_;
        buf_ = nullptr;
    }
    fill_ = 0;
}

bool Sealer::begin(const uint8_t recipient_pk[32], RandomBytesFn random, void* random_ud,
                   SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes) {
    uint8_t eph_sk[32], nonce_prefix[kSealNoncePrefixBytes];
    bool ok = random && random(eph_sk, sizeof eph_sk, random_ud) &&
              random(nonce_prefix, sizeof nonce_prefix, random_ud);
    if (ok) ok = start(recipient_pk, eph_sk, nonce_prefix, sink, sink_ud, chunk_bytes);
    crypto_wipe(eph_sk, sizeof eph_sk);
    crypto_wipe(nonce_prefix, sizeof nonce_prefix);
    return ok;
}

bool Sealer::begin_fixed(const uint8_t recipient_pk[32], const uint8_t eph_sk[32],
                         const uint8_t nonce_prefix[kSealNoncePrefixBytes],
                         SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes) {
    return start(recipient_pk, eph_sk, nonce_prefix, sink, sink_ud, chunk_bytes);
}

bool Sealer::start(const uint8_t recipient_pk[32], const uint8_t eph_sk[32], const uint8_t nonce_prefix[16],
                   SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes) {
    wipe();
    state_ = State::Failed;
    if (!sink || !recipient_pk || chunk_bytes < 1 || chunk_bytes > kSealMaxChunkBytes) return false;

    uint8_t eph_pk[32], shared[32], hash[32], input[8 + 3 * 32];
    crypto_x25519_public_key(eph_pk, eph_sk);
    crypto_x25519(shared, eph_sk, recipient_pk);
    // A low-order recipient key would give every console the same key; the
    // opener refuses it too (contract sealed-format.md §6 step 4).
    uint8_t zero[32];
    std::memset(zero, 0, sizeof zero);
    if (crypto_verify32(shared, zero) == 0) {
        crypto_wipe(shared, sizeof shared);
        return false;
    }

    // Header: magic, key_id, eph_pk, nonce_prefix, chunk_size, reserved.
    std::memcpy(ad_, kMagic, 8);
    crypto_blake2b(hash, sizeof hash, recipient_pk, 32);
    std::memcpy(ad_ + 8, hash, 8);
    std::memcpy(ad_ + 16, eph_pk, 32);
    std::memcpy(ad_ + 48, nonce_prefix, kSealNoncePrefixBytes);
    store32_le(ad_ + 64, chunk_bytes);
    std::memset(ad_ + 68, 0, 4);
    ad_[kSealHeaderBytes] = 0;

    // key = BLAKE2b-256("D2VSEAL1" || shared || eph_pk || recipient_pk)
    std::memcpy(input, kMagic, 8);
    std::memcpy(input + 8, shared, 32);
    std::memcpy(input + 40, eph_pk, 32);
    std::memcpy(input + 72, recipient_pk, 32);
    crypto_blake2b(key_, sizeof key_, input, sizeof input);
    crypto_wipe(input, sizeof input);
    crypto_wipe(shared, sizeof shared);

    buf_ = new (std::nothrow) uint8_t[(size_t)chunk_bytes + kSealTagBytes];
    if (!buf_) {
        crypto_wipe(key_, sizeof key_);
        return false;
    }
    chunk_bytes_ = chunk_bytes;
    sink_ = sink;
    sink_ud_ = sink_ud;
    index_ = plain_bytes_ = sealed_bytes_ = 0;
    fill_ = 0;
    if (!sink_(ad_, kSealHeaderBytes, sink_ud_)) {
        wipe();
        return false;
    }
    sealed_bytes_ = kSealHeaderBytes;
    state_ = State::Open;
    return true;
}

bool Sealer::seal_chunk(bool last) {
    uint8_t nonce[24];
    std::memcpy(nonce, ad_ + 48, kSealNoncePrefixBytes);
    store64_le(nonce + kSealNoncePrefixBytes, index_);
    ad_[kSealHeaderBytes] = last ? 1 : 0;
    // In place: the ciphertext replaces the plaintext, the tag goes right
    // after it, which is the layout of a sealed chunk.
    crypto_aead_lock(buf_, buf_ + fill_, key_, nonce, ad_, kSealHeaderBytes + 1, buf_, fill_);
    const size_t sealed = fill_ + kSealTagBytes;
    fill_ = 0;
    ++index_;
    sealed_bytes_ += sealed;
    if (!sink_(buf_, sealed, sink_ud_)) {
        state_ = State::Failed;
        wipe();
        return false;
    }
    return true;
}

bool Sealer::feed(const uint8_t* data, size_t n) {
    if (state_ != State::Open) return false;
    if (n && !data) return false;
    size_t done = 0;
    while (done < n) {
        const size_t take = n - done < chunk_bytes_ - fill_ ? n - done : chunk_bytes_ - fill_;
        std::memcpy(buf_ + fill_, data + done, take);
        fill_ += take;
        done += take;
        plain_bytes_ += take;
        // A full chunk is never the last one (sealed-format.md §4), so it can
        // go out at once: one chunk of memory whatever the artifact size.
        if (fill_ == chunk_bytes_ && !seal_chunk(false)) return false;
    }
    return true;
}

bool Sealer::finish() {
    if (state_ != State::Open) return false;
    if (!seal_chunk(true)) return false;
    state_ = State::Done;
    wipe();
    return true;
}

}  // namespace d2cr
