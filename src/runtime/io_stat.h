// io_stat.h -- D2_IOSTAT / D2_READAHEAD: file-read accounting and per-handle
// read-ahead buffering for the KERNEL32 file shims that remain in
// tools/rt_boot.cpp. See io_stat.cpp for the full rationale.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#define RA_WINMAX 32
struct RaWin { uint8_t* buf=nullptr; uint32_t cap=0; long off=0; uint32_t len=0; uint64_t use=0; };
struct IoH {                        // per-handle state
    std::string nm;                 // short name (d2exp.mpq...)
    long lastEnd=-1;                // end of the last read: sequential or seek
    int fd=-1;                      // POSIX descriptor (D2_READAHEAD); -1 = fread path
    RaWin w[RA_WINMAX];             // read-ahead windows (LRU among them)
    uint32_t next=0;                // size of the next fill (adaptive)
    uint64_t lastUse=0;             // logical clock of the last read (LRU)
    int err=0;                      // errno of the last REAL read failure, 0 = none.
                                    // A short read that simply hit the end of the
                                    // file leaves this at 0: end of file is not an
                                    // error, and ReadFile must keep reporting it as
                                    // success. Only a failed read sets it, and only
                                    // then does ReadFile answer FALSE -- which is
                                    // what lets Storm's own retry run.
};

extern bool g_ioStat;

void io_init();                       // D2_IOSTAT / D2_READAHEAD: parse env once
bool ra_on();                         // D2_READAHEAD armed?
IoH* io_get(uint32_t h);              // per-handle state (created on demand)
void io_close(uint32_t h);            // release a handle's read-ahead buffers
// Read at offset `off` served from the handle's buffer (D2_READAHEAD). Returns
// the number of bytes copied into dst; `fromRam` = everything came from the buffer.
uint32_t ra_read(IoH& f, long off, void* dst, uint32_t n, bool& fromRam);
void io_account(IoH* f, long at, uint32_t n, uint32_t got, uint64_t us, bool hit);
void io_tick(uint64_t now);           // called on every ReadFile under D2_IOSTAT
void io_line(const char* quand);      // report line ("10s" / "final")
