// src/crashreport/cr_evidence.h — what the previous session left behind (spec §4.1).
//
//   evidence                                                      kind
//   psp2core dump stamped with the session id (fallback: file
//   epoch in [started_unix, now] and APP_INFO title DTWO00001)    host_fault
//   Crash.txt modified at or after started_unix                   halt
//   abnormal stop (main thread exit code != 0, unshimmed import,
//   FatalAppExit, RaiseException, ExitProcess or TerminateProcess
//   with a non-zero code)                                         abnormal_exit
//   "NATIVE FAULT" line in boot_progress                          guest_fault
//   state=running and nothing else                                none (PS button)
//
// Automatic hang detection (state=running and >= 2 final alive: beats
// without frames -> Kind::Hang) was removed: real-hardware testing showed
// the general watchdog already covers a genuine freeze, so a dedicated
// crash-report path for it added complexity without a corresponding
// benefit — a stuck game can still be reported manually. A stalled
// heartbeat alone is therefore no longer evidence of anything: it falls
// straight through to the "nothing else" row above. Kind::Hang the enum
// value stays defined (cr_types.h) for the frozen v1 contract and any
// already-stored reports; no code path here produces it from a live boot
// any more.
//
// The most severe kind is kept (host_fault > halt > abnormal_exit >
// guest_fault); the others become hints, most severe first.
// features_json follows contract/schemas/claim.v1.schema.json (D2Vita-website):
// unknown values are null, codes are "0x" hex strings, and abnormal_exit
// reasons use the contract's closed vocabularies, guest addresses use the
// module tokens Game and ABS (cr_addr.h) and host addresses the HostAddress
// shape (cr_psp2dmp.h).
#pragma once
#include <cstdint>
#include <string>

#include "crashreport/cr_io.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_types.h"

namespace d2cr {

// Title id of the D2Vita VPK (tools/build_rt_boot_vpk.sh default TITLE).
constexpr const char* kD2VitaTitleId = "DTWO00001";
// Crash.txt bytes read for parsing (the summary and stack sit at the top).
constexpr size_t kCrashTxtParseBytes = 256 * 1024;

struct EvidenceDetails {
  int64_t uptime_s = -1;        // last "[N.00s]" of boot_progress
  bool online = false;          // env.txt: D2NET enabled
  int native_fault_line = -1;   // 0-based line index of the first NATIVE FAULT in boot_progress
  std::string fw;               // SYSTEM_INFO of the matched dump
  int64_t dump_epoch = 0;       // epoch in the dump file name
  uint64_t dump_bytes = 0;      // compressed size of the dump file
  bool dump_by_stamp = false;   // matched by session stamp rather than by epoch + title
};

// Paths: progress = prev.progress_path, Crash.txt and crash.log under
// prev.write_root, dumps in dumps_dir named psp2core-<epoch>-0x<id>-*.psp2dmp.
// Evidence paths are set only for files that exist.
Evidence build_evidence(IoApi& io, const SessionRecord& prev, int64_t now_unix, const std::string& dumps_dir,
                        const SecretPatterns& patterns, EvidenceDetails* details = nullptr);

}  // namespace d2cr
