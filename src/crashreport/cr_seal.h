// src/crashreport/cr_seal.h — D2VSEAL1 sealing, one chunk at a time.
//
// Artifacts travel over plain HTTP and are stored by the API as opaque bytes:
// the console seals each of them for the maintainer's X25519 public key, built
// into the eboot (spec §4.7 and §8, D2Vita-website contract/sealed-format.md).
// Only the maintainer's private key opens them.
//
// The object is written as it is produced: a 72-byte header, then the
// plaintext cut into chunks of `chunk_bytes` (65536 on the console), each
// sealed with XChaCha20-Poly1305 IETF and followed by its 16-byte tag. The
// last chunk carries last = 1 and is EMPTY when the plaintext length is a
// multiple of the chunk size (including an empty plaintext), which is how a
// truncation is detected. So a full chunk is never the last one and can be
// sealed as soon as it is full: memory stays at one chunk whatever the size of
// the artifact, and sealed_size_for() gives the Content-Length of the upload
// before the first byte is read.
#pragma once
#include <cstddef>
#include <cstdint>

namespace d2cr {

constexpr size_t kSealHeaderBytes = 72;
constexpr size_t kSealTagBytes = 16;
constexpr size_t kSealKeyBytes = 32;
constexpr size_t kSealNoncePrefixBytes = 16;
constexpr uint32_t kSealChunkBytes = 65536;         // what the console writes
constexpr uint32_t kSealMaxChunkBytes = 1048576;    // what a reader must accept

// 72 + n + 16 * (n / chunk_bytes + 1). cr_outbox.h sealed_size(n) is this with
// the console chunk size.
uint64_t sealed_size_for(uint64_t plain_bytes, uint32_t chunk_bytes = kSealChunkBytes);

// Fills out[0, n) with cryptographic randomness; false when it cannot (the
// artifact is then not sent: a sealed object with predictable randomness is
// worse than no report). A Vita implementation is provided in cr_boot.cpp.
using RandomBytesFn = bool (*)(uint8_t* out, size_t n, void* ud);

// Receives the sealed bytes in order, starting with the header; false aborts
// the sealing.
using SealSinkFn = bool (*)(const uint8_t* data, size_t n, void* ud);

class Sealer {
 public:
  Sealer() = default;
  ~Sealer();
  Sealer(const Sealer&) = delete;
  Sealer& operator=(const Sealer&) = delete;

  // Draws a fresh ephemeral key pair and nonce prefix from `random`, writes
  // the header to the sink and gets ready for feed(). False leaves nothing
  // behind.
  bool begin(const uint8_t recipient_pk[32], RandomBytesFn random, void* random_ud,
             SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes = kSealChunkBytes);

  // Same with the ephemeral secret key and the nonce prefix imposed: this is
  // how the contract vectors are reproduced, and nothing else may use it.
  // Reusing either value with the same recipient would leak the plaintexts.
  bool begin_fixed(const uint8_t recipient_pk[32], const uint8_t eph_sk[32],
                   const uint8_t nonce_prefix[kSealNoncePrefixBytes],
                   SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes = kSealChunkBytes);

  // Feeds plaintext; chunks leave through the sink as they fill up.
  bool feed(const uint8_t* data, size_t n);
  // Seals the last (possibly empty) chunk and wipes the key material.
  bool finish();

  bool open() const { return state_ == State::Open; }
  uint64_t plain_bytes() const { return plain_bytes_; }
  uint64_t sealed_bytes() const { return sealed_bytes_; }

 private:
  enum class State : uint8_t { Idle, Open, Done, Failed };
  bool start(const uint8_t recipient_pk[32], const uint8_t eph_sk[32], const uint8_t nonce_prefix[16],
             SealSinkFn sink, void* sink_ud, uint32_t chunk_bytes);
  bool seal_chunk(bool last);
  void wipe();

  State state_ = State::Idle;
  SealSinkFn sink_ = nullptr;
  void* sink_ud_ = nullptr;
  uint32_t chunk_bytes_ = kSealChunkBytes;
  uint8_t key_[kSealKeyBytes] = {0};
  // The associated data of every chunk: the header, then the `last` byte.
  uint8_t ad_[kSealHeaderBytes + 1] = {0};
  uint64_t index_ = 0, plain_bytes_ = 0, sealed_bytes_ = 0;
  uint8_t* buf_ = nullptr;        // chunk_bytes_ + 16: sealed in place, then the tag
  size_t fill_ = 0;
};

}  // namespace d2cr
