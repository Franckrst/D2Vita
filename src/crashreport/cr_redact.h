// src/crashreport/cr_redact.h — secret patterns, in-place redaction, stream scan.
//
// Patterns (spec §4.6), built at boot, kept in memory only, wiped after use:
//   * CD keys of keys.txt (classic, lod): the 16/26 key characters in any
//     letter case, with or without one '-' or ' ' between any two characters
//     (covers every dash grouping), in ASCII and UTF-16LE;
//   * owner names of keys.txt and Battle.net account names: case-insensitive,
//     ASCII and UTF-16LE. Names shorter than kMinSecretNameLen are ignored:
//     one or two characters identify nobody and would match everywhere.
// A match is replaced by 'X' characters of the same length (UTF-16LE code
// units become "X\0"). Only counts ever leave this module, never values, and
// no buffer that held a secret here goes back to the heap without being
// zeroed first (a later dump of the process must not carry one).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace d2cr {

constexpr size_t kMinSecretNameLen = 3;

struct SecretPattern {
  std::string units;         // characters, letters folded to upper case
  bool utf16 = false;        // each character followed by a 0x00 byte
  bool separators = false;   // key: one optional '-' or ' ' between characters
};

class SecretPatterns {
 public:
  SecretPatterns() = default;
  SecretPatterns(const SecretPatterns& o);
  SecretPatterns& operator=(const SecretPatterns& o);
  ~SecretPatterns();

  void add_key(const std::string& key);    // raw keys.txt value; ignored unless 16 or 26 alphanumerics
  void add_name(const std::string& name);  // trimmed; ignored when shorter than kMinSecretNameLen

  bool empty() const { return pats_.empty(); }
  size_t count() const { return pats_.size(); }
  size_t max_match_len() const { return max_len_; }
  const std::vector<SecretPattern>& patterns() const { return pats_; }

  // Match at data[i] with n bytes available: returns the matched length,
  // 0 if no pattern matches, or kNeedMore when the answer needs bytes past n
  // (only possible when !final). *utf16 tells how to redact the span.
  static constexpr size_t kNeedMore = static_cast<size_t>(-1);
  size_t match_at(const uint8_t* data, size_t n, size_t i, bool final, bool* utf16) const;

  void wipe();                              // zeroes and drops every pattern

 private:
  void add(const std::string& units, bool separators);
  void rebuild_index();
  std::vector<SecretPattern> pats_;
  std::vector<uint16_t> first_[256];        // pattern indices by folded first byte
  size_t max_len_ = 0;
};

// keys.txt as parsed by src/runtime/cdkeys_file.cpp:163-230 (name=value,
// '#' comments; classic/lod keys; owner/classic_owner/lod_owner names).
SecretPatterns build_patterns(const std::string& keys_txt, const std::vector<std::string>& accounts);

// "Last BNet" value of the game's registry.txt (format written by
// src/runtime/win32_shims_advapi32_d2.cpp:83-85: key|value|Name|type|hex),
// at most kMaxRegistryAccounts distinct names. Parsed in place, so the only
// copies of a name are the returned strings: callers wipe them (and their
// registry_txt, keys_txt buffers) once build_patterns has run.
constexpr size_t kMaxRegistryAccounts = 8;
std::vector<std::string> accounts_from_registry(const std::string& registry_txt);

// Replaces every non-overlapping match in place; returns the match count.
int redact_in_place(std::string* s, const SecretPatterns& p);

// Counts matches over a stream fed in arbitrary chunks. Keeps at most
// max_match_len() - 1 bytes between chunks, so a secret split across two
// chunks is still found, and counts exactly what redact_in_place would.
class StreamScanner {
 public:
  explicit StreamScanner(const SecretPatterns& p);
  ~StreamScanner();
  void feed(const uint8_t* data, size_t n);
  void finish();
  uint64_t hits() const { return hits_; }

 private:
  const SecretPatterns& p_;
  std::vector<uint8_t> carry_;   // undecided positions from the previous chunk
  uint64_t base_ = 0;            // stream offset of carry_[0]
  uint64_t skip_until_ = 0;      // end of the last match (non-overlapping count)
  uint64_t hits_ = 0;
  bool finished_ = false;
};

}  // namespace d2cr
