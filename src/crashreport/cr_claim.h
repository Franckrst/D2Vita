// src/crashreport/cr_claim.h — report id and claim JSON (spec §4.4).
//
// The claim is the only part of a report sent in clear. Its exact schema is
// contract/schemas/claim.v1.schema.json (D2Vita-website); this library does
// not validate against it at runtime — that is checked in
// tools/tests/run_crashreport_tests.sh (leg 4) against the real schema when
// D2V_CONTRACT is set.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_outbox.h"
#include "crashreport/cr_types.h"

namespace d2cr {

constexpr size_t kClaimMaxBytes = 16 * 1024;

// ULID: 48-bit Unix time in milliseconds then 80 random bits, 26 characters
// of uppercase Crockford base32. Empty when now_ms is outside [0, 2^48).
std::string new_ulid(int64_t now_ms, const uint8_t rand10[10]);

struct ClaimArtifact {
  std::string name;          // dump, crash_txt, crash_log, boot_progress
  uint64_t bytes = 0;        // size of the sealed object (Content-Length of its upload)
};

struct ClaimInputs {
  std::string report_id, install_id, build_id, channel;
  std::string platform_model = "unknown";   // vita | pstv | unknown
  std::string platform_fw = "unknown";      // "X.YY" | unknown
  int64_t started_unix = 0;
  int64_t uptime_s = -1;                    // < 0: unknown (null)
  bool online = false;
  Kind kind = Kind::None;
  std::vector<Kind> hints;
  std::string features_json;                // Evidence::features_json
  std::vector<ClaimArtifact> artifacts;
  int redactions = -1;                      // < 0: omitted
};

// Claim JSON in the §4.4 field order. Hints that are not strictly less
// severe than kind, duplicates, unknown artifact names and invalid platform
// values are dropped or replaced by "unknown". Empty string when the claim
// would exceed kClaimMaxBytes or when kind is None.
std::string build_claim_json(const ClaimInputs& in);

// Claim inputs of an outbox report: artifacts sized as sealed objects,
// uptime/online/fw/redactions from the record.
ClaimInputs claim_inputs_from_record(const ReportRecord& rec, const std::string& install_id,
                                     const std::string& channel, const std::string& platform_model);

}  // namespace d2cr
