// lazy_seek.h -- D2_LAZYSEEK: lazy (in-memory) file-position tracking used by
// the KERNEL32 file shims (SetFilePointer/ReadFile/WriteFile/GetFileSize/...)
// that remain in tools/rt_boot.cpp. See lazy_seek.cpp for the full rationale
// (SetFilePointer used to be the only I/O cost holding the GIL end-to-end).
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include "runtime/guest_files.h"

using FPos = WxFPos;                       // engine (guest_files.h)
extern std::map<uint32_t,FPos>& g_fpos;
extern uint64_t g_lsLazy;   // SetFilePointer served WITHOUT a syscall
extern uint64_t g_lsEnd;    // FILE_END: falls back to a real fseek+ftell
extern uint64_t g_lsSize;   // GetFileSize: 2 operations instead of 4

void ls_tick();                 // called once per 1024 lazy calls (paced 10s report)
void ls_line(const char* quand);   // report line ("10s" / "final")
// Position the stream for a SYNCHRONOUS operation (dir 1=read, 2=write).
void lazy_sync(uint32_t h, FILE* fp, int dir);
// Position for an OVERLAPPED operation: the LOGICAL position stays untouched.
void lazy_sync_ov(uint32_t h, FILE* fp, long off, int dir);
