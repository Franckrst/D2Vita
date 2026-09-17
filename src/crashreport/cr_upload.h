// src/crashreport/cr_upload.h — sending a report (spec §4.8).
//
// Per report: POST /v1/claims, then, when the API answers action=upload, one
// PUT of each requested piece (sealed on the way out) and a POST .../complete;
// action=count_only means the API already knows this bug and the report is
// deleted at once. At most 3 reports per boot.
//
// Rules the contract adds (D2Vita-website contract/README.md):
//   * every answer is verified (Ed25519) before it is read, and must match its
//     schema and name this very request; anything else is a network failure,
//     which is repeated — every console request is safe to repeat;
//   * a pending report is deleted only after a verified body that names it: a
//     decision with action=count_only, or a CompleteResponse;
//   * 429 retry_after_s and 503 disable_until_unix are not per report: they
//     hold the whole outbox back and are written to <outbox>/state.txt, so a
//     report collected later waits too;
//   * 409 exists counts the piece as stored, complete is idempotent, and 409
//     incomplete means a piece is missing: upload again, then complete again.
//
// Failures cost the report one attempt and a longer wait (next_attempt_unix,
// doubling); the outbox drops it after 3 failures or 7 days (spec §4.2, §4.8).
// 5xx answers and cut connections are retried inside the run while the upload
// token is still valid, then left for the next boot.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_api.h"
#include "crashreport/cr_http.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_net.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_seal.h"
#include "crashreport/cr_verify.h"

namespace d2cr {

struct UploadKeys {
  uint8_t response_pk[kEd25519PublicKeyBytes] = {0};   // Ed25519, answers of the API
  uint8_t recipient_pk[kSealKeyBytes] = {0};           // X25519, sealing of the pieces
};

// Production address of the API (spec §3). Plain HTTP on purpose: the pieces
// are sealed, the claim carries nothing nominative, the answers are signed,
// and the console has no usable TLS (spec §2 and §4.8). D2_CRASHREPORT_URL
// replaces it for a local server or staging.
constexpr const char* kDefaultApiUrl = "http://d2vita-crash.franck-rst-c3d.workers.dev";

struct UploadConfig {
  std::string base_url = kDefaultApiUrl;   // http://host[:port][/prefix] (D2_CRASHREPORT_URL)
  std::string install_id;               // 32 lowercase hex digits
  std::string channel = "release";      // release | dev | test
  std::string platform_model = "vita";  // vita | pstv | unknown
  int max_reports_per_run = 3;          // spec §4.8: at most 3 reports per boot
  int max_request_retries = 2;          // repeats of one request inside the run
  uint32_t retry_delay_ms = 2000;       // doubled at every repeat
  int64_t backoff_base_s = 300;         // between boots: 5, 10, 20 minutes
  uint32_t token_margin_s = 20;         // stop retrying when the token is about to expire
  HttpLimits http;
};

// What the state machine needs from the platform besides files and sockets.
struct UploadEnv {
  int64_t (*now_unix)(void* ud) = nullptr;                      // wall clock, seconds
  bool (*random_bytes)(uint8_t* out, size_t n, void* ud) = nullptr;   // sealing entropy
  bool (*should_stop)(void* ud) = nullptr;                      // teardown asked
  void* ud = nullptr;
};

// <outbox root>/state.txt: what the whole outbox waits for. Report directories
// are the only directories there, so a file next to them is ignored by the
// outbox itself.
struct OutboxGate {
  int64_t not_before_unix = 0;
  std::string reason;                   // rate_limited | not_accepting | ""
};
bool parse_outbox_gate(const std::string& text, OutboxGate* out);
std::string serialize_outbox_gate(const OutboxGate& g);

enum class ReportOutcome {
  Skipped = 0,     // consent, age, attempts or next_attempt_unix say not now
  Deleted,         // count_only: the API only counted it
  Completed,       // pieces stored and complete acknowledged
  Failed,          // network, 5xx, 400, 403 bad_token: kept for the next boot
  Gated,           // 429 or 503: the whole outbox waits
  UnknownBuild,    // 403 unknown_build: nothing more for this build in this run
  Stopped,         // teardown asked
};
const char* report_outcome_name(ReportOutcome o);

struct UploadStats {
  int considered = 0;     // reports looked at
  int skipped = 0;
  int attempted = 0;      // reports that got at least one request
  int deleted = 0;        // count_only
  int completed = 0;
  int failed = 0;
  int requests = 0;
  int artifacts_stored = 0;
  int64_t gate_unix = 0;  // what the outbox waits for after the run
  ReportOutcome last = ReportOutcome::Skipped;
};

class Uploader {
 public:
  Uploader(IoApi& io, NetApi& net, Outbox& outbox, const UploadConfig& cfg, const UploadKeys& keys,
           const UploadEnv& env);

  // Sends at most max_reports_per_run granted reports, oldest first.
  UploadStats run();

  // One report; the tests drive this directly.
  ReportOutcome send_report(const std::string& id);

  const OutboxGate& gate() const { return gate_; }
  const std::string& detail() const { return detail_; }   // why the last report ended that way
  const UploadStats& stats() const { return stats_; }

 private:
  struct Answer;
  enum class Shape { Decision, Stored, Complete };
  bool stopped() const { return env_.should_stop && env_.should_stop(env_.ud); }
  int64_t now() const { return env_.now_unix ? env_.now_unix(env_.ud) : 0; }
  std::string gate_path() const { return outbox_.root() + "/state.txt"; }
  void load_gate();
  void save_gate(int64_t not_before, const char* reason);
  bool gate_closed() const { return gate_.not_before_unix > now(); }
  // Sends one request, checks the signature, the schema and the binding.
  bool call(Shape shape, const HttpRequest& req, const std::string& report_id, const std::string& artifact,
            uint64_t sent_bytes, Answer* out);
  // Same, repeated while the answer deserves another try (5xx, no verified
  // answer) and the token, when there is one, stays valid.
  bool call_retrying(Shape shape, const HttpRequest& req, const std::string& report_id, const std::string& artifact,
                     uint64_t sent_bytes, int64_t token_expires, Answer* out);
  // One pass over the pieces the decision asks for; false ends the report
  // with *outcome.
  bool upload_pieces(const std::string& id, const ReportRecord& rec, ReportState* st, const Decision& d,
                     std::vector<std::string>* stored, ReportOutcome* outcome);
  ReportOutcome fail(const std::string& id, ReportState* st, const char* why);
  ReportOutcome gated(const ErrorBody& e, const char* why);
  std::vector<std::string> headers_for(const std::string& build_id) const;
  static bool stop_thunk(void* ud);

  IoApi& io_;
  NetApi& net_;
  Outbox& outbox_;
  UploadConfig cfg_;
  UploadKeys keys_;
  UploadEnv env_;
  HttpClient http_;
  OutboxGate gate_;
  std::string detail_;
  std::string blocked_build_;    // 403 unknown_build: skipped for the rest of the run
  bool network_down_ = false;    // resolve or connect failed: stop the run
  bool client_ready_ = false;    // the base URL is parsed once, keeping the resolved address
  UploadStats stats_;
};

}  // namespace d2cr
