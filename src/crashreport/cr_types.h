// src/crashreport/cr_types.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace d2cr {
// Order = increasing severity (the retained kind is the max).
enum class Kind { None = 0, Hang, GuestFault, AbnormalExit, Halt, HostFault };
const char* kind_name(Kind k);          // "hang","guest_fault","abnormal_exit","halt","host_fault"
struct Addr { std::string module; uint32_t offset = 0; };
std::string format_addr(const Addr& a); // "Game+0x1fedf4" (lowercase hex, no leading zeros)
struct SessionRecord {                   // reports/session.txt (key=value, one per line)
  std::string session_id, build_id, write_root, progress_path, state, stop_reason;
  int64_t started_unix = 0;
  uint32_t game_base = 0, main_exit = 0, arena_host_base = 0;
  uint32_t eboot_base = 0, eboot_size = 0, jit_lo = 0, jit_hi = 0;
  bool has_main_exit = false;
};
bool parse_session(const std::string& text, SessionRecord* out);
std::string serialize_session(const SessionRecord& s);
struct Evidence {
  Kind kind = Kind::None;
  std::vector<Kind> hints;               // other kinds found
  std::string features_json;             // "features" JSON object conforming to claim.v1
  std::string dump_path, crash_txt_path, crash_log_path, progress_path;
  bool dump_withheld = false;
  int redactions = 0;
};
}  // namespace d2cr
