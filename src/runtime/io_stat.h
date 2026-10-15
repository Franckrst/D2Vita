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
    uint8_t busy=0;                 // D2_IOGIL: a card read is in flight on this
                                    // handle WITHOUT the GIL (1 = synchronous
                                    // read, 2 = OVERLAPPED). While set, the fd,
                                    // the windows and this struct belong to the
                                    // reading thread alone: every other shim that
                                    // touches the handle waits (io_wait_idle), and
                                    // the window steal skips it. Read/written
                                    // only under the GIL, except by its owner.
};

extern bool g_ioStat;

void io_init();                       // D2_IOSTAT / D2_READAHEAD: parse env once
bool ra_on();                         // D2_READAHEAD armed?
IoH* io_get(uint32_t h);              // per-handle state (created on demand)
void io_close(uint32_t h);            // release a handle's read-ahead buffers
// Read at offset `off` served from the handle's buffer (D2_READAHEAD). Returns
// the number of bytes copied into dst; `fromRam` = everything came from the buffer.
// `rel` (D2_IOGIL): 0 = everything under the GIL (historical); 1/2 = the card
// reads (lseek+read) run with the GIL RELEASED, the handle marked busy=rel
// meanwhile (1 = synchronous ReadFile, 2 = OVERLAPPED). Only for a destination that stays valid
// and private while the GIL is down (the guest buffer of the blocked caller —
// never a shared static scratch buffer).
uint32_t ra_read(IoH& f, long off, void* dst, uint32_t n, bool& fromRam, uint8_t rel=0);
// D2_IOGIL: release the GIL around card reads? (knob AND native scheduler)
bool io_gil_on();
// D2_IOGIL: block (GIL released, condition variable on the GIL mutex) until no
// card read is in flight on handle h. `seekOnly`: the caller only moves the
// LOGICAL position (SetFilePointer FILE_BEGIN/FILE_CURRENT), which commutes
// with an in-flight OVERLAPPED read, so only a synchronous one is waited for.
// Call FIRST in a shim, before looking the handle up: the handle may be
// closed by the time the wait returns. Free when nothing is in flight.
void io_wait_idle(uint32_t h, bool seekOnly=false);
bool io_busy(uint32_t h);             // a GIL-released read is in flight on h
void io_account(IoH* f, long at, uint32_t n, uint32_t got, uint64_t us, bool hit);
void io_tick(uint64_t now);           // called on every ReadFile under D2_IOSTAT
void io_line(const char* quand);      // report line ("10s" / "final")
