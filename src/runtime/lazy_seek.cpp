// lazy_seek.cpp -- see lazy_seek.h.
//
// ====== D2_LAZYSEEK: LAZY file positioning ==================================
// KERNEL32!SetFilePointer is the only I/O cost that holds the GIL end-to-end.
// The old shim did std::fseek THEN std::ftell on EVERY call against the memory
// card — about 4.8 ms for the seek itself, versus ~30 us for the read that follows.
//
// Fix: track a LOGICAL position per descriptor in memory, and only issue a
// real fseek when a read or write actually needs it and the stream's REAL
// position differs.
//
// Four invariants that lazy positioning must preserve:
//
//  1. OVERLAPPED is position-NEUTRAL: Storm's async I/O worker seeks via the
//     OVERLAPPED offset, NOT via SetFilePointer (a seek that moved the shared
//     pointer here produces corrupted sprite bytes). The async path therefore
//     moves the REAL position and leaves the LOGICAL one untouched — nothing
//     needs restoring, since the next sync operation sees real != logical and
//     repositions itself.
//
//  2. A stream opened for UPDATE ("r+b" / "w+b", save files) requires, per the
//     C standard, a positioning operation between a write and a read (and vice
//     versa). A real fseek is forced whenever the direction changes, even when
//     the positions already agree.
//
//  3. The fop diagnostic ring and the race_seek detector keep seeing the
//     LOGICAL position, unchanged from before.
//
//  4. fseek() to a negative offset FAILS and leaves the position unchanged, so
//     ftell() then returns the OLD position; the lazy arithmetic reproduces
//     this case explicitly.
//
// Default = old behavior (immediate fseek+ftell on every call).
#include "lazy_seek.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "platform/vita_present.h"

std::map<uint32_t,FPos>& g_fpos = wx86_file_pos();
uint64_t g_lsLazy=0;   // SetFilePointer served WITHOUT a syscall
static uint64_t g_lsSync=0;   // real fseeks actually issued, when needed
uint64_t g_lsEnd=0;    // FILE_END: falls back to a real fseek+ftell
uint64_t g_lsSize=0;   // GetFileSize: 2 operations instead of 4
// Report line. On console, printf reaches no log at all — everything must go
// through d2vita_progress. Paced to every 10 s, checked once per 1024 lazy
// calls so the clock-read cost stays under a thousandth of the gain it measures.
void ls_line(const char* quand){
    char m[192];
    std::snprintf(m,sizeof m,
        "lazyseek(%s): evites=%llu (paresseux=%llu vrais-fseek=%llu) fin-de-fichier=%llu taille=%llu",
        quand,
        (unsigned long long)(g_lsLazy>g_lsSync? g_lsLazy-g_lsSync : 0),
        (unsigned long long)g_lsLazy,(unsigned long long)g_lsSync,
        (unsigned long long)g_lsEnd,(unsigned long long)g_lsSize);
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
}
void ls_tick(){                      // called once per 1024
    static uint64_t t0=0; uint64_t now=rt_now_us();
    if(!t0){ t0=now; return; }
    if(now-t0 >= 10000000ull){ t0=now; ls_line("10s"); }
}
// Position the stream for a SYNCHRONOUS operation (dir 1=read, 2=write). The
// EOF flag is part of the contract: always issuing an fseek (as the naive
// version does) clears it; leaving it set would make fread return 0 even though
// the file has grown since — exactly the two-descriptor VITA.d2s scenario that
// flush_same_path (above) exists to cover.
void lazy_sync(uint32_t h, FILE* fp, int dir){
    FPos& s=g_fpos[h];
    if(s.real!=s.logical || (s.dir && s.dir!=dir) || std::feof(fp)){
        std::fseek(fp,s.logical,SEEK_SET); s.real=s.logical; ++g_lsSync; }
    s.dir=dir;
}
// Position for an OVERLAPPED operation: the LOGICAL position is untouched
// (invariant 1), only the REAL one moves.
void lazy_sync_ov(uint32_t h, FILE* fp, long off, int dir){
    FPos& s=g_fpos[h];
    if(s.real!=off || std::feof(fp)){ std::fseek(fp,off,SEEK_SET); s.real=off; ++g_lsSync; }
    s.dir=dir;
}
