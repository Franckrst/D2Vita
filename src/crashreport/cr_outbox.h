// src/crashreport/cr_outbox.h — report outbox (spec §4.2, §4.5).
//
// <root>/<report_id>/ holds, for one report:
//   boot_progress.txt  first 16 KiB + last 256 KiB of boot_progress (whole file
//                      when it fits), plus the lines around the first NATIVE
//                      FAULT when they fall in between; redacted in place
//   crash_log.txt      end of crash.log, redacted
//   crash_txt.txt      start of Crash.txt, redacted (the original is deleted)
//   dump.psp2dmp       the system dump, MOVED (rename on the same device),
//                      original gzip bytes, never rewritten
//   evidence.txt       ReportRecord: what the claim needs, key=value
//   state.txt          ReportState: attempts, prompts, dates, consent
// Bounds: 5 reports and 4 MiB in total (oldest go first), 7 days of age,
// 3 dialog presentations while the consent is still pending (§4.2), 3 failed
// sends (§4.8).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_evidence.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_types.h"

namespace d2cr {

constexpr size_t kProgressHeadBytes = 16 * 1024;
constexpr size_t kProgressTailBytes = 256 * 1024;
constexpr int kFaultWindowLines = 400;

// Sealed size caps of spec §4.5.
constexpr uint64_t kSealedCapBootProgress = 320 * 1024;
constexpr uint64_t kSealedCapCrashLog = 64 * 1024;
constexpr uint64_t kSealedCapCrashTxt = 64 * 1024;
constexpr uint64_t kSealedCapDump = 2 * 1024 * 1024;

// D2VSEAL1 (spec §8): 72-byte header, 64 KiB blocks each followed by a
// 16-byte tag, plus a last (possibly empty) block.
uint64_t sealed_size(uint64_t plain_bytes);
uint64_t max_plain_for_sealed(uint64_t sealed_cap);

struct OutboxLimits {
  int max_reports = 5;
  uint64_t max_total_bytes = 4 * 1024 * 1024;
  int64_t max_age_s = 7 * 24 * 3600;
  int max_prompts = 3;
  int max_attempts = 3;                 // spec 4.8: a report is abandoned after 3 failed sends
};

struct ReportState {
  int attempts = 0;
  int prompts = 0;
  int64_t created_unix = 0;
  int64_t next_attempt_unix = 0;
  std::string consent = "pending";   // pending | granted | denied
};
bool parse_report_state(const std::string& text, ReportState* out);
std::string serialize_report_state(const ReportState& s);

struct ArtifactFile {
  std::string name;   // claim artifact name: boot_progress, crash_log, crash_txt, dump
  std::string file;   // file name inside the report directory
  uint64_t bytes = 0; // plaintext bytes on disk
};

struct ReportRecord {
  std::string report_id, session_id, build_id;
  Kind kind = Kind::None;
  std::vector<Kind> hints;
  std::string features_json;
  int64_t started_unix = 0, uptime_s = -1;
  bool online = false;
  std::string fw;                    // "" when unknown
  int redactions = 0;
  std::string dump;                  // none | included | withheld | too_large | move_failed
  std::vector<ArtifactFile> artifacts;
};
bool parse_report_record(const std::string& text, ReportRecord* out);
std::string serialize_report_record(const ReportRecord& r);

struct CollectInputs {
  std::string report_id;
  SessionRecord session;
  Evidence evidence;
  EvidenceDetails details;
  int64_t now_unix = 0;
};

class Outbox {
 public:
  Outbox(IoApi& io, const std::string& root, const OutboxLimits& limits = OutboxLimits());

  // Copies the pieces into <root>/<report_id>/, moves the dump, deletes
  // Crash.txt. On failure nothing of the sources is touched and the partial
  // directory is removed.
  bool create(const CollectInputs& in, const SecretPatterns& patterns, ReportRecord* out, std::string* err);

  std::vector<std::string> list();                  // valid reports, oldest first
  bool load(const std::string& id, ReportRecord* rec, ReportState* st);
  bool save_state(const std::string& id, const ReportState& st);
  bool remove(const std::string& id);               // the directory and its files
  uint64_t report_bytes(const std::string& id);
  const std::string& root() const { return root_; }
  const OutboxLimits& limits() const { return limits_; }   // the sender applies the same bounds
  // Drops invalid, expired, refused and repeatedly failed reports, and
  // pending ones already presented max_prompts times, then the oldest ones
  // while over the count or size bound. Returns how many went.
  int enforce(int64_t now_unix);

 private:
  std::string dir(const std::string& id) const { return root_ + "/" + id; }
  // state.txt, or state.txt.new when an interrupted save_state left only
  // that one (*from_new is then true).
  bool read_state(const std::string& id, ReportState* st, bool* from_new);
  IoApi& io_;
  std::string root_;
  OutboxLimits limits_;
};

}  // namespace d2cr
