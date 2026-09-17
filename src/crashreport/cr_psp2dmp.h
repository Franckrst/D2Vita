// src/crashreport/cr_psp2dmp.h — streaming reader for PS Vita core dumps.
//
// A psp2dmp is a gzip-compressed ELF32 ARM core: PT_NOTE segments (thread,
// register, module and application notes) followed by PT_LOAD memory. The
// reader inflates it as a stream and never holds the dump in memory: it keeps
// the notes, the bytes around the faulting PC, and a guest stack window, then
// derives the guest EBP chain with the rules of tools/autopsie_psp2dmp.py.
// Every decompressed byte also goes through the secret scanner.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_io.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_types.h"

namespace d2cr {

struct DumpThread {
  uint32_t uid = 0;
  std::string name;
  uint32_t status = 0;        // 1 running, 8 waiting (observed)
  uint32_t stop_reason = 0;   // 0 for threads that did not stop the process
  uint32_t pc = 0;
};

// Host address in the shape of contract claim.v1 HostAddress: module is the
// region name for eboot, jit and unknown; for sysmodule it is the name of
// MODULE_INFO, which then matches ^[A-Za-z0-9_.]{1,32}$ (any other name makes
// the address region unknown). offset is absolute for unknown.
struct HostAddr {
  std::string region;         // "eboot", "jit", "sysmodule", "unknown"
  std::string module;         // "eboot", "jit", system module name, or "unknown"
  uint32_t offset = 0;        // from the region/segment base ("unknown": absolute)
};

struct GuestFrame { uint32_t frame = 0, ret = 0; };

enum class ThreadRule {
  StopReason,  // first thread whose THREAD_INFO stop reason is non-zero, else the first
  First,       // first THREAD_REG_INFO entry (tools/autopsie_psp2dmp.py:74)
};

struct DumpReadOptions {
  ThreadRule rule = ThreadRule::StopReason;
  bool notes_only = false;    // stop after the notes (no memory, no full secret scan)
};

// Guest window captured above guest ESP (and above EBP when EBP lies past it).
constexpr uint32_t kGuestStackWindow = 64 * 1024;
// tools/autopsie_psp2dmp.py:104-111: at most 24 frames, a return address must
// lie in [Game base, Game base + 0x400000), a link must grow by <= 0x10000.
constexpr int kEbpChainMaxFrames = 24;
constexpr uint32_t kEbpChainCodeSpan = 0x400000;
constexpr uint32_t kEbpChainMaxLink = 0x10000;

struct DumpFacts {
  bool ok = false;                  // ELF header and at least one registered thread read
  std::string error;                // why not ok, or what went wrong on the way
  bool truncated = false;           // stream or PT_LOAD data shorter than announced
  bool notes_truncated = false;     // notes over the in-memory cap were skipped
  uint64_t decompressed_bytes = 0;

  std::string app_title;            // APP_INFO title id, e.g. "DTWO00001"
  std::string fw;                   // SYSTEM_INFO firmware, e.g. "3.65"
  // Session stamp: "D2VSTAMP1 session_id=<32 lowercase hex> build_id=<id>"
  // ended by '\n' or NUL, inside a PT_NOTE segment (never taken from memory).
  // A notes-only read stops after the first note group when that group has
  // the thread notes, so it misses a stamp note placed after the memory.
  bool has_stamp = false;
  std::string stamp_session_id, stamp_build_id;   // build id "" when absent or malformed

  std::vector<DumpThread> threads;  // THREAD_INFO
  int fault_index = -1;             // THREAD_REG_INFO entry chosen by the rule
  uint32_t tid = 0;
  std::string thread_name;
  uint32_t stop_reason = 0;
  uint32_t r[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t sp = 0, lr = 0, pc = 0, cpsr = 0;
  HostAddr pc_addr, lr_addr;

  std::vector<GuestFrame> guest_chain;
  std::string chain_end;            // ret_outside_game, bad_link, max_frames, not_in_dump,
                                    // outside_capture, notes_only, no_thread, no_game_base
  std::vector<uint8_t> pc_bytes;    // [pc-0x20, pc+0x20) when present in the dump
  uint64_t secret_hits = 0;
};

// "data_abort" (0x30004), "prefetch_abort" (0x30003), "undefined_instruction"
// (0x30002), "none" for 0, "0x<hex>" otherwise. 0x30004 and 0x30002 were
// observed on data faults and on an abort() crash in real dumps; unverified
// codes stay numeric rather than get a guessed name.
std::string stop_reason_name(uint32_t code);

DumpFacts read_psp2dmp(IoApi& io, const std::string& path, const SessionRecord& session,
                       const SecretPatterns& patterns, const DumpReadOptions& opt = DumpReadOptions());

}  // namespace d2cr
