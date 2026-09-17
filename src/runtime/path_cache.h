// path_cache.h -- Win32 path resolution for the KERNEL32 file shims that stay
// in tools/rt_boot.cpp: host_path() (case-insensitive, write-dir-first
// resolution), the per-directory index that makes host_path() pay one
// readdir per directory generation instead of one per file (D2_PATHCACHE),
// and the small FindFirstFileA support helpers. See path_cache.cpp for the
// full rationale.
//
// g_pc/g_pcDirs/PcLock/pc_on/kMemoRescan are exposed (not just host_path/
// pc_dirty/pc_report/pc_announce) because the D2_PATHCACHETEST self-test in
// tools/rt_boot.cpp inspects the index's internals directly.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
namespace d2rt { struct Cpu; }

// Directory-index counters (see path_cache.cpp for what each one means).
struct PcStats {
    uint32_t builds, invalidations, lookups, hitW, hitD, memo, fallbacks, stale, rearm, misses;
    uint32_t scans, selfnames, blind, races;
    uint32_t lastLookups;
    uint64_t lastReport;
};
extern PcStats g_pc;
extern std::string g_dataRoot;   // data (read-only, Blizzard files) root; set in main()

struct DirIndex {
    bool built=false;    // a readdir has been attempted for the current generation
    bool usable=false;   // ... and it SUCCEEDED (otherwise: BLIND directory, see pc_lookup)
    std::map<std::string,std::string> byLower;   // lowercase name -> real on-disk name
    std::set<std::string> exact;                 // names as-is (exact-name precedence)
    std::map<std::string,uint32_t> absent;       // lowercase name -> "absent" answers since the last verification
    uint32_t entries=0, collisions=0;
    uint32_t gen=0;      // incremented on EVERY build — see pc_note_absent / pc_invalidate_stale
};
extern std::map<std::string,DirIndex> g_pcDirs;

static constexpr uint32_t kMemoRescan = 16;   // at most 16 "absent" answers per name between re-verifications

// LEAF lock over g_pc/g_pcDirs: acquires only g_pcMu (path_cache.cpp), never
// any other project lock, and never calls d2_crashlog while held.
struct PcLock {
    PcLock();
    ~PcLock();
    PcLock(const PcLock&)=delete; PcLock& operator=(const PcLock&)=delete;
};

bool pc_on();                                   // D2_PATHCACHE escape hatch (default: index active)
void pc_dirty(const std::string& root);         // marks a directory's index for rebuild
void pc_report(bool force);                     // periodic (or forced) summary line to crash.log
void pc_announce();                             // one-time "index active/disabled" line at startup
std::string host_path(const std::string& guest);   // guest path -> resolved host path

// FindFirstFileA/FindNextFileA support.
bool wild_match(const char* pat, const char* s);
void fill_finddata(d2rt::Cpu& c, uint32_t p, const std::string& name, uint32_t sz);
uint32_t host_filesize(const std::string& p);
