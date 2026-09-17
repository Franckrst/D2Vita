// src/crashreport/cr_api.h — reading the answers of API v1.
//
// The console reads a body with the schema its status announces, and treats a
// body that does not match like a network failure (D2Vita-website
// contract/README.md, response signatures). "Does not match" is taken
// literally here: every schema of the contract says
// additionalProperties: false, so an unknown member, a value outside its
// pattern or range, or a conditional field that is missing (retry_after_s on
// 429, report_id and artifact on 409 exists) makes the answer unusable. That
// is what keeps v1 frozen: a Worker that adds a field to a body writes v2.
//
// Nothing here trusts the body about which request it answers; the caller
// compares the names it carries with what it sent (cr_upload.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace d2cr {

// The error codes of admin.v1#ErrorBody.
enum class ApiError {
  None = 0,
  InvalidPayload,
  Unauthorized,
  UnknownBuild,
  BadToken,
  Turnstile,
  NotFound,
  MethodNotAllowed,
  Exists,
  Incomplete,
  PayloadTooLarge,
  RateLimited,
  InternalError,
  NotAccepting,
};
const char* api_error_name(ApiError e);
// The status the contract gives that code (0 for None).
int api_error_status(ApiError e);

struct ArtifactRequest {
  std::string name;
  uint64_t max_bytes = 0;
};

// decision.v1: the answer to POST /v1/claims.
struct Decision {
  std::string report_id, signature;
  bool upload = false;                  // action: upload (false = count_only)
  std::string token;
  uint64_t expires_unix = 0;
  std::vector<ArtifactRequest> artifacts;
  bool has_retry_after = false;
  uint64_t retry_after_s = 0;
  bool has_disable_until = false;
  uint64_t disable_until_unix = 0;
};

// decision.v1#ArtifactStored: the 201 of a piece upload.
struct ArtifactStored {
  std::string report_id, name;
  uint64_t bytes = 0;
};

// decision.v1#CompleteResponse.
struct CompleteResponse {
  std::string report_id;
  bool sample_stored = false;
};

// admin.v1#ErrorBody.
struct ErrorBody {
  ApiError error = ApiError::None;
  bool has_retry_after = false;
  uint64_t retry_after_s = 0;
  bool has_disable_until = false;
  uint64_t disable_until_unix = 0;
  std::string report_id, artifact;      // empty when absent
};

// False when the body is not exactly what its schema allows.
bool parse_decision(const std::string& body, Decision* out);
bool parse_artifact_stored(const std::string& body, ArtifactStored* out);
bool parse_complete_response(const std::string& body, CompleteResponse* out);
bool parse_error_body(const std::string& body, ErrorBody* out);

// Shapes of claim.v1 the answers reuse.
bool valid_report_id(const std::string& s);        // ULID, first character 0-7
bool valid_artifact_name(const std::string& s);    // dump | crash_txt | crash_log | boot_progress
// Sealed size cap of an artifact (spec §4.5), 0 when the name is unknown.
uint64_t artifact_sealed_cap(const std::string& name);

}  // namespace d2cr
