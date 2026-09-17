// src/crashreport/cr_progress_parse.h — facts from boot_progress.txt / crash.log.
//
// Each line format comes from the code that writes it (file:line quoted next
// to its parser in cr_progress_parse.cpp). Lines may carry the
// "[%4u.%02us] " prefix of wx86_vita_progress (boot_progress) or not
// (crash.log); both are accepted. Malformed lines are ignored.
//
// The parser is a stream: it keeps one line at a time and, of everything
// else, only what the facts need (the first fault block, the last stop
// lines, the last beat and the beat where the final stall began). A
// multi-MiB log therefore costs a few KiB of heap on top of the read buffer
// (spec §4.9: at most 1 MiB for the whole collector).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "crashreport/cr_addr.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_types.h"

namespace d2cr {

// A line longer than this is ignored as a whole (still counted for line
// numbers). The longest writer line is the alive: heartbeat, built in a
// 512-byte buffer (vita_present.cpp:896).
constexpr size_t kProgressLineMax = 4096;
// Read size used by parse_progress_file.
constexpr size_t kProgressStreamChunk = 64 * 1024;

struct ProgressFacts {
  // First "NATIVE FAULT" of the log (native scheduler guest-thread fault).
  bool native_fault = false;
  int native_faults = 0;              // all NATIVE FAULT lines
  int native_fault_line = -1;         // 0-based line index of the first one, in the whole file
  uint32_t fault_thread = 0;
  bool fault_main = false;            // thread 1 = main guest thread
  uint32_t fault_eip = 0, fault_addr = 0;
  Addr fault_eip_addr;
  bool has_fault_regs = false;        // "fault thr N:" block attached
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0, ebp = 0, esp = 0;
  std::vector<GuestModule> fault_modules;
  std::vector<Addr> fault_frames;     // <= 16 stack words, module-relative

  // Last "scheduler stopped" line.
  bool sched_stopped = false;
  std::string stop_reason;
  bool has_main_exit = false;
  uint32_t main_exit = 0;
  bool clean_exit = false;            // proves nothing (spec §2), reported as seen

  // Controlled-stop lines of the Win32 shims.
  bool unshimmed = false;
  std::string unshimmed_import;       // "KERNEL32.dll!Name"
  bool fatal_app_exit = false;
  bool raise_exception = false;
  uint32_t raise_code = 0;
  bool exit_process = false, terminate_process = false;
  uint32_t exit_code = 0;
  std::vector<Addr> exit_chain;       // Game.exe return addresses of the exit call

  // "alive:" heartbeats of the watchdog (every 10 s).
  int beats = 0;
  int stalled_beats = 0;              // trailing beats whose frames= did not move
  bool has_last_eip = false;
  uint32_t last_eip = 0;
  Addr last_eip_addr;
  std::string runner_state;           // last beat's run=: "id:state[+]" ('+' = counter moved during the stall)
  std::string runner_class;           // during the stall: "starvation" (a counter moved), "deadlock" (none
                                      // moved, a runner in R), "blocked" (none moved, all waiting), "" unknown

  int64_t last_uptime_s = -1;         // seconds of the last prefixed line
  bool online = false;                // "env.txt: D2NET=<v>" with v not empty and not "0"
};

// Feed the log in chunks of any size, then finish() once.
class ProgressParser {
 public:
  explicit ProgressParser(uint32_t game_base);
  ~ProgressParser();
  ProgressParser(const ProgressParser&) = delete;
  ProgressParser& operator=(const ProgressParser&) = delete;

  void feed(const char* data, size_t n);
  ProgressFacts finish();             // takes a last unterminated line into account

 private:
  struct State;
  std::unique_ptr<State> st_;
};

ProgressFacts parse_progress(const std::string& text, uint32_t game_base);

// True for the line the parser takes as a NATIVE FAULT (trailing "\n" or
// "\r\n" allowed; lines over kProgressLineMax are never one).
bool progress_line_is_native_fault(const std::string& line);

// Streams a log file through ProgressParser (IoApi::read_stream). false when
// the file cannot be read to its end; *out is then left untouched.
bool parse_progress_file(IoApi& io, const std::string& path, uint32_t game_base, ProgressFacts* out,
                         size_t chunk = kProgressStreamChunk);

}  // namespace d2cr
