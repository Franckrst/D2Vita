// kernel32_files.cpp -- see kernel32_files.h.
//
// CreateFileA and CreateFileW both funnel through one local helper,
// do_create_file() -- the automap-ENOENT special case, the write-dir
// disposition handling, and the Crash.txt-triggered diagnostics are all
// identical between the two, differing only in how the path string is
// decoded (CP1252/MBCS vs UTF-16). Splitting them into two files would mean
// either duplicating do_create_file() or awkwardly extern-ing it one way
// across a file boundary for no benefit -- they stay together here as one
// "KERNEL32 file-open shims" module, along with the small family of other
// file-handle shims (SetFilePointer/ReadFile/WriteFile/...) that share its
// local state: the file-op ring (FOp/g_fring), the seek/read race detector
// (g_lastSeekTid), and the OVERLAPPED-completion helper (ov_done).
//
// wnarrow() (UTF-16 -> ASCII narrowing) stays defined in tools/rt_boot.cpp:
// the remaining KERNEL32 W-variant shims there need it too. Likewise
// crash_txt_diag_dump() stays in rt_boot.cpp -- it reaches into allocator/
// codec/room-guard/VA counters that other not-yet-extracted code also reads,
// so only the (unconditional) call moves here, not that state itself. Both
// are declared in runtime/rt_host.h.
#include "kernel32_files.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_thread.h"    // full d2rt::ThreadScheduler (g_sched->current())
#include "runtime/guest_sync.h"      // d2rt::Waitable/WxEvent, wx86_handle_find, wx86_sync_notify
#include "runtime/guest_files.h"     // wx86_files()/wx86_file_maps() (engine-owned handle tables)
#include "runtime/lazy_seek.h"       // D2_LAZYSEEK: FPos/g_fpos, lazy_sync[_ov], ls_tick, g_ls*
#include "runtime/io_stat.h"         // D2_IOSTAT/D2_READAHEAD: IoH, g_ioStat, ra_on/io_get/ra_read/...
#include "runtime/path_cache.h"      // host_path()
#include "platform/vita_present.h"   // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
using namespace d2rt;

using FMap = WxFMap;    // engine (guest_files.h); same alias as tools/rt_boot.cpp's own
using KEvent = WxEvent; // engine (guest_sync.h); same alias as tools/rt_boot.cpp's own

// Ownership lives in the engine (guest_files.h). This is a SEPARATE reference
// to the SAME underlying table as tools/rt_boot.cpp's own g_files/g_fmaps
// (still needed there too, e.g. CreateFileMappingA/MapViewOfFile, a report
// dump) -- wx86_files()/wx86_file_maps() always return the one true table.
static std::map<uint32_t,FILE*>& g_files = wx86_files();
static std::map<uint32_t,FMap>&  g_fmaps = wx86_file_maps();

// Host path per open handle + writer flush: D2 re-reads a file (VITA.d2s at
// game entry) through a SECOND handle while the writer handle is still open —
// unflushed stdio buffers make the reader see a short/empty file. Won or lost
// on timing (newlib vs glibc, realclock interleavings): the intermittent
// hardware crash/hang entering a game. Flush every open handle on the same
// host path before opening it again.
static void flush_same_path(const std::string& hp){
    for(auto& kv:g_files){ auto it=g_filePathByH.find(kv.first);
        if(it!=g_filePathByH.end()&&it->second==hp) std::fflush(kv.second); } }

// Asset accounting: bytes READ per file handle, O(1) on the hot ReadFile path
// (direct-indexed slots, collision = overwrite, fine for the ~15 simultaneous
// handles). Dumped as [FILES] at exit + Crash.txt (tools/rt_boot.cpp; g_frb
// itself stays there too, since that dump reads it directly).
static void frb_add(uint32_t h, uint32_t n){
    FRead& e=g_frb[h&0xFF]; if(e.h!=h){ e.h=h; e.bytes=0; e.reads=0; } e.bytes+=n; ++e.reads; }

// WLOG: trace short, printable WriteFile payloads. Only WriteFile uses this.
static bool env_wlog(){ static const bool v=getenv("WLOG")!=nullptr; return v; }

void kernel32_files_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    static auto do_create_file=[](Cpu&c, std::string n)->uint32_t{
        uint32_t access=c.arg(1), disp=c.arg(4);
        bool wantWrite=(access & 0x40000000u)||disp==1||disp==2; // GENERIC_WRITE, CREATE_NEW, CREATE_ALWAYS
        // Automap overlay files (<char>.map / .ma0 / .ma1 / .ma2): D2 opens them
        // read+write with OPEN_ALWAYS on level-enter to LOAD the explored-map
        // overlay. A fresh character has none, and honoring OPEN_ALWAYS by creating
        // it empty hands D2 a 0-byte file whose fixed-size header read returns EOF —
        // which spins the act-load (the client never finishes loading the level, so
        // it never sends LOADCOMPLETE and the GS drops it). Report ENOENT for a
        // MISSING-or-EMPTY automap so D2 takes its "no saved automap -> generate a
        // fresh one" path; a real (non-empty) automap still opens normally, and the
        // later CREATE_ALWAYS save path still creates the file.
        { size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
          size_t dot=base.find_last_of('.'); std::string ext=dot==std::string::npos?"":base.substr(dot);
          for(char& ch:ext) ch=(char)std::tolower((unsigned char)ch);
          if((disp==3||disp==4) && (ext==".map"||ext==".ma0"||ext==".ma1"||ext==".ma2")){
              std::string wp=g_writeRoot+"/"+base; struct stat st;
              if(::stat(wp.c_str(),&st)!=0 || st.st_size==0){ set_lasterr(c,2);
                  std::printf("    [automap] %s missing/empty -> ENOENT (D2 regenerates)\n",base.c_str());
                  return 0xFFFFFFFFu; } } }
        if(wantWrite){                                          // game-created files (saves, logs) live in the write dir
            size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
            std::string wp=g_writeRoot+"/"+base;
            // Real dispositions: CREATE_NEW/CREATE_ALWAYS truncate; OPEN_EXISTING/
            // OPEN_ALWAYS update in place ("a+b" would force append-only writes
            // and corrupt rewritten saves).
            FILE* fp=nullptr;
            flush_same_path(wp);
            if(disp==3||disp==4){ fp=std::fopen(wp.c_str(),"r+b"); if(!fp&&disp==4) fp=std::fopen(wp.c_str(),"w+b");
                if(!fp&&disp==3){ set_lasterr(c,2); return 0xFFFFFFFFu; } }
            else fp=std::fopen(wp.c_str(),"w+b");
            if(!fp){ set_lasterr(c,5);
                char m[160]; std::snprintf(m,sizeof m,"file W FAIL %s disp=%u errno=%d",base.c_str(),disp,errno);
                d2vita_progress(m); std::printf("    [%s]\n",m);
                return 0xFFFFFFFFu; }
            pc_dirty(g_writeRoot);          // a directory entry has just appeared
            uint32_t hd=g_nextH++; g_files[hd]=fp; g_filePathByH[hd]=wp;
            g_fpos[hd]=FPos{};                      // D2_LAZYSEEK : position logique = reelle = 0
            { char m[160]; std::snprintf(m,sizeof m,"file W %s disp=%u -> h=%08x",base.c_str(),disp,hd);
              d2vita_progress(m); }
            if(env_filelog()) std::fprintf(stderr,"    [file W] %s disp=%u -> h=0x%08x\n",n.c_str(),disp,hd);
            // ALWAYS at a Fog Halt (D2 opens Crash.txt): dump the last sprite
            // decompressions, allocator/codec/room-guard/VA state, etc. (stays
            // in tools/rt_boot.cpp -- see the comment at its definition there).
            crash_txt_diag_dump(c, base);
            return hd; }
        std::string h=host_path(n);
        flush_same_path(h);
        uint64_t io0=rt_now_us();
        FILE* fp=std::fopen(h.c_str(),"rb");
        g_ioUs+=rt_now_us()-io0; if(!fp){ set_lasterr(c,2); if(env_filelog()) std::fprintf(stderr,"    [file MISS] %s\n",n.c_str()); return 0xFFFFFFFFu; }
        // Big stdio buffer: Storm reads MPQs in many small chunks — on a Vita
        // SD card the per-IO latency dominates without this.
        // D2_READAHEAD: UNBUFFERED stream, reads via lseek+read on the
        // descriptor, read-ahead buffer allocated on first read.
        if(ra_on()) setvbuf(fp,nullptr,_IONBF,0); else
        setvbuf(fp,nullptr,_IOFBF,256*1024);
        uint32_t hd=g_nextH++; g_files[hd]=fp; g_filePathByH[hd]=h;
        g_fpos[hd]=FPos{};                          // D2_LAZYSEEK : position logique = reelle = 0
        if(ra_on()){ IoH* f=io_get(hd); if(f) f->fd=fileno(fp); }
        std::printf("    [file] open %s -> h=0x%08x\n",n.c_str(),hd); return hd; };
    K("CreateFileA",7,[](Cpu&c){ return do_create_file(c,gread_mb(c,c.arg(0),-1)); });
    K("CreateFileW",7,[](Cpu&c){ return do_create_file(c,wnarrow(c,c.arg(0))); });
    K("GetFileAttributesA",1,[](Cpu&c){ std::string n=gread_mb(c,c.arg(0),-1);
        struct stat st;
        if(stat(host_path(n).c_str(),&st)==0)
            return S_ISDIR(st.st_mode)?0x10u:0x80u;   // FILE_ATTRIBUTE_DIRECTORY / _NORMAL
        // Well-known game dirs live under the write root (created at startup):
        // "C:\Diablo II\Save" etc. must LOOK present or char creation aborts.
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        if(stat((g_writeRoot+"/"+base).c_str(),&st)==0)
            return S_ISDIR(st.st_mode)?0x10u:0x80u;
        set_lasterr(c,2); if(env_filelog()) std::fprintf(stderr,"    [attr MISS] %s\n",n.c_str());
        return 0xFFFFFFFFu; });
    // File-op ring: last 48 seek/read ops (handle, offset, len) — dumped when
    // Storm's error dialog fires, to spot a wrong-offset/garbled sector read.
    struct FOp { char op; uint32_t h,off,len,tid; };
    static FOp g_fring[48]; static uint32_t g_fri=0;
    auto fop=[](char op,uint32_t h,uint32_t off,uint32_t len){
        uint32_t tid=(g_sched&&g_sched->current())?g_sched->current()->id:0;
        g_fring[g_fri++%48]={op,h,off,len,tid}; };
    g_dump_fring=[](){ std::printf("  [file-op ring, oldest first]\n");
        for(uint32_t k=0;k<48;k++){ FOp&f=g_fring[(g_fri+k)%48]; if(!f.op) continue;
            std::printf("    %c h=0x%08x off=0x%08x len=0x%x tid=%u\n",f.op,f.h,f.off,f.len,f.tid); } };
    // Seek/read handle race detector: a non-OVERLAPPED ReadFile whose file
    // position was last set by ANOTHER thread means two guest threads
    // interleaved SetFilePointer+ReadFile on one handle — the read returns
    // the wrong sector (corrupt sprite data under the real clock).
    static std::map<uint32_t,uint32_t> g_lastSeekTid;
    auto race_seek=[](uint32_t h){ uint32_t tid=(g_sched&&g_sched->current())?g_sched->current()->id:0;
        g_lastSeekTid[h]=tid; };
    auto race_read=[](uint32_t h){ uint32_t tid=(g_sched&&g_sched->current())?g_sched->current()->id:0;
        auto it=g_lastSeekTid.find(h);
        if(it!=g_lastSeekTid.end()&&it->second!=tid){ static int n=0;
            if(n<40){ n++; std::printf("  [race] ReadFile h=0x%08x tid=%u after seek by tid=%u\n",h,tid,it->second); } }
        g_lastSeekTid[h]=tid; };
    K("SetFilePointer",4,[fop,race_seek](Cpu&c){ auto it=g_files.find(c.arg(0)); if(it==g_files.end()) return 0xFFFFFFFFu;
        long dist=(long)(int32_t)c.arg(1); int meth=(int)c.arg(3);
        uint32_t pos;
        // D2_LAZYSEEK (default): in-memory arithmetic, NO syscall. The value
        // returned to the game, the one fop sees, and the moment race_seek
        // arms are UNCHANGED — only the real fseek is deferred.
        {
            FPos& s=g_fpos[c.arg(0)];
            long np;
            if(meth==1)      np=s.logical+dist;                 // FILE_CURRENT
            else if(meth==0) np=dist;                           // FILE_BEGIN
            else {                                              // FILE_END: need the size
                std::fseek(it->second,0,SEEK_END); long endp=std::ftell(it->second);
                s.real=endp; s.dir=0; np=endp+dist; ++g_lsEnd; }
            if(np<0) np=s.logical;      // fseek() would have failed: position unchanged
            s.logical=np; pos=(uint32_t)np;
            if(meth!=2){ if(((++g_lsLazy)&1023u)==0) ls_tick(); }
        }
        race_seek(c.arg(0));
        fop('S',c.arg(0),pos,0); return pos; });

    // OVERLAPPED (+8 Offset, +12 OffsetHigh, +16 hEvent): Storm's async-I/O
    // worker positions reads via the OVERLAPPED offset, NOT SetFilePointer —
    // ignoring it feeds wrong sector bytes to SCOMP (decompress "error 8").
    // OVERLAPPED ops are position-NEUTRAL on Windows (they carry their own
    // offset and leave the shared file pointer alone). D2_LAZYSEEK handles
    // this via pure arithmetic (lazy_sync/lazy_sync_ov): no more real
    // fseek/ftell on this path, so no need to save/restore a stream position
    // (ov_seek/ov_pos_restore no longer exist).
    auto ov_done=[](Cpu& c, uint32_t ov, uint32_t bytes){ if(!ov) return;
        c.write_u32(ov,0); c.write_u32(ov+4,bytes);                       // Internal=STATUS_SUCCESS, InternalHigh=bytes
        uint32_t ev=c.read_u32(ov+16);
        if(ev){ Waitable* itw=wx86_handle_find(ev); if(itw&&is_kind(itw,"event")){
            auto* e=static_cast<KEvent*>(itw); e->signaled=true; wx86_sync_notify({WX86_SYNC_EVENT_SET,&c,e,ev});
            if(g_sched) g_sched->notify(itw); } } };
    K("ReadFile",5,[ov_done,fop,race_read](Cpu&c){ g_readN++; auto it=g_files.find(c.arg(0)); if(it==g_files.end()) return 0u;
        uint32_t buf=c.arg(1),n=c.arg(2),pRead=c.arg(3),ov=c.arg(4);
        { int b=0; uint32_t v=n; while(v>>=1) b++; if(b>23) b=23; g_rdHist[b]++; g_rdBytes+=n;
          // D2_IOHIST>=2: census PER GUEST CALL SITE. [ESP] at the stub's entry
          // = the return address, i.e. the `call [IAT]` site — this names
          // WHO reads, and therefore which block size really governs the
          // stream (the answer wasn't the one assumed).
          if(g_ioHist>=2){ RdSite& r=g_rdSite[c.read_u32(c.reg(R_ESP))]; r.n++; r.bytes+=n; } }
        if(!ov) race_read(c.arg(0));
        uint32_t at;      // 'at' = LOGICAL position of the read (fop ring)
        {
            const long o = ov ? (long)c.read_u32(ov+8) : 0;
            if(ov) lazy_sync_ov(c.arg(0),it->second,o,1);   // position NEUTRE : logique intacte
            else   lazy_sync   (c.arg(0),it->second,1);
            at = (uint32_t)(ov? o : g_fpos[c.arg(0)].logical);
        }
        fop(ov?'O':'R',c.arg(0),at,n);
        uint64_t io0=rt_now_us();
        // Perf: Storm streams MPQ sectors 4 KiB at a time — the old path paid
        // one malloc'd vector + fread + c.write memcpy PER SECTOR. fread
        // straight into the host view; invalidate_code() is exactly
        // c.write()'s barrier (cpu_box86.cpp), so semantics are equal.
        void* hp = n? c.hostptr(buf,n) : nullptr;
        size_t r; bool ramHit=false;
        IoH* ioh = (g_ioStat||ra_on()) ? io_get(c.arg(0)) : nullptr;
        if(ioh && ioh->fd>=0){
            // D2_READAHEAD: served from the handle's buffer, or a direct
            // lseek+read. The (unbuffered) stdio stream is NOT read here; its
            // position is reset to `at+r` below when not lazy.
            if(hp){ c.invalidate_code(buf,n); r=ra_read(*ioh,(long)at,hp,n,ramHit); }
            else { static std::vector<uint8_t> tmp; if(tmp.size()<n) tmp.resize(n);
                   r=ra_read(*ioh,(long)at,tmp.data(),n,ramHit);
                   if(r){ c.write(buf,tmp.data(),(uint32_t)r); } }
        } else
        if(hp){ c.invalidate_code(buf,n); r=std::fread(hp,1,n,it->second); }
        else { static std::vector<uint8_t> tmp; if(tmp.size()<n) tmp.resize(n);
               r=std::fread(tmp.data(),1,n,it->second);
               if(r){ c.write(buf,tmp.data(),(uint32_t)r); } }
        frb_add(c.arg(0),(uint32_t)r);
        { const uint64_t io1=rt_now_us(); g_ioUs+=io1-io0;
          if(ioh){ io_account(ioh,(long)at,n,(uint32_t)r,io1-io0,ramHit); if(g_ioStat) io_tick(io1); } }
        // Short/failed reads never happen under qemu but can on a real SD
        // card (or a newlib stdio bug) — and Storm feeds whatever it got to
        // the decoder. Surface the first ones in boot_progress.
        if(r!=n){ static int sr=0; if(sr<24){ sr++;
            auto pit=g_filePathByH.find(c.arg(0));
            char m[256]; std::snprintf(m,sizeof m,"io: SHORT read h=%08x want=%u got=%u eof=%d err=%d file=%s",
                c.arg(0),n,(unsigned)r,feof(it->second)?1:0,ferror(it->second)?1:0,
                pit!=g_filePathByH.end()?pit->second.c_str():"?");
            std::printf("[%s]\n",m); d2vita_progress(m); } }
        { FPos& s=g_fpos[c.arg(0)];
            s.real=(long)at+(long)r;                 // where the stream REALLY is
            if(!ov) s.logical=s.real;                // OVERLAPPED: logical unchanged
        }
        if(pRead) c.write_u32(pRead,(uint32_t)r); ov_done(c,ov,(uint32_t)r); return 1u; });
    K("WriteFile",5,[ov_done](Cpu&c){ uint32_t h=c.arg(0),buf=c.arg(1),n=c.arg(2),pW=c.arg(3),ov=c.arg(4);
        auto it=g_files.find(h);
        if(it!=g_files.end()){ long at=0; size_t wn=0;
            {
                const long o = ov ? (long)c.read_u32(ov+8) : 0;
                if(ov) lazy_sync_ov(h,it->second,o,2);       // position NEUTRE : logique intacte
                else   lazy_sync   (h,it->second,2);
                at = ov? o : g_fpos[h].logical;
            }
            if(n){ const uint8_t* src; static std::vector<uint8_t> b;
                if(const void* hp=c.hostptr(buf,n)) src=(const uint8_t*)hp;    // zero-copy read of guest data
                else { if(b.size()<n) b.resize(n); c.read(buf,b.data(),n); src=b.data(); }
                wn=std::fwrite(src,1,n,it->second);
                // D2_SAVELOG: trace character-file writes. The produced .d2s is
                // 335 bytes — a header alone, with NONE of a character's six
                // sections — and it's this file whose reload breaks. Knowing
                // whether D2 issues a single 335-byte write (it stops by
                // itself) or several that we then lose (our fault) tells the
                // two causes apart.
                { static const bool sl = getenv("D2_SAVELOG")!=nullptr;
                  if(sl){ auto pit=g_filePathByH.find(h);
                    if(pit!=g_filePathByH.end() && pit->second.find(".d2s")!=std::string::npos){
                        static int k=0; if(k<24){ ++k;
                            char m[160]; std::snprintf(m,sizeof m,
                                "save: ecriture #%d taille=%u pos=%ld fichier=%s",
                                k,n,std::ftell(it->second),pit->second.c_str());
                            d2vita_progress(m); std::fprintf(stderr,"  [%s]\n",m); } } } }
                if(env_wlog() && n<512){ bool txt=true; for(uint32_t i=0;i<n&&i<64;i++) if(src[i]&&(src[i]<9||src[i]>126)){txt=false;break;}
                    if(txt){ std::string s((const char*)src,n>160?160:n);
                        for(auto&ch:s) if(ch=='\r'||ch=='\n') ch='|';
                        std::fprintf(stderr,"[W h=%08x] %s\n",h,s.c_str()); } } }
            { FPos& s=g_fpos[h];
                s.real=at+(long)wn;                  // where the stream REALLY is
                if(!ov) s.logical=s.real;            // OVERLAPPED: logical unchanged
            } }
        if(pW) c.write_u32(pW,n); ov_done(c,ov,n); return 1u; });
    K("GetOverlappedResult",4,[](Cpu&c){ uint32_t ov=c.arg(1),pN=c.arg(2);
        if(pN&&ov) c.write_u32(pN,c.read_u32(ov+4)); return 1u; });
    K("FlushFileBuffers",1,[](Cpu&c){ auto it=g_files.find(c.arg(0)); if(it!=g_files.end()) std::fflush(it->second); return 1u; });
    // SetEndOfFile is already a lie (no truncation happens). If it ever
    // became real, it should truncate to the LOGICAL position, not the
    // stream's.

    K("GetFileSize",2,[](Cpu&c){ auto it=g_files.find(c.arg(0)); if(it==g_files.end()) return 0xFFFFFFFFu;
        long sz;
        // Neutral on the LOGICAL position: the stream is left at end-of-file
        // and this is NOTED — the next synchronous operation repositions itself.
        std::fseek(it->second,0,SEEK_END); sz=std::ftell(it->second);
        { FPos& s=g_fpos[c.arg(0)]; s.real=sz; s.dir=0; ++g_lsSize; }
        if(c.arg(1)) c.write_u32(c.arg(1),0); return (uint32_t)sz; });
    K("CloseHandle",1,[](Cpu&c){ auto it=g_files.find(c.arg(0)); if(it!=g_files.end()){ std::fclose(it->second); g_files.erase(it); g_filePathByH.erase(c.arg(0)); g_fpos.erase(c.arg(0)); io_close(c.arg(0)); return 1u; }
        { auto fm=g_fmaps.find(c.arg(0)); if(fm!=g_fmaps.end()){ g_fmaps.erase(fm); return 1u; } }   // file mapping
        Waitable* how=wx86_handle_find(c.arg(0)); if(how){ /* keep the object; a thread handle may still be waited on */ } return 1u; });
    K("CreateDirectoryA",2,[](Cpu&c){ std::string n=gread_mb(c,c.arg(0),-1);
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        mkdir((g_writeRoot+"/"+base).c_str(),0755); pc_dirty(g_writeRoot); return 1u; });


    K("DeleteFileA",1,[](Cpu&c){ std::string n=gread_mb(c,c.arg(0),-1);
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        std::remove((g_writeRoot+"/"+base).c_str()); pc_dirty(g_writeRoot); return 1u; });
    // MoveFileA: COPY then DELETE, deliberately never std::rename. On the
    // Vita's C library, rename() is newlib's generic rename, which calls
    // link(); link() there is just a stub that writes ENOSYS (88) to errno
    // through the reentrant state pointer. From a shim, that pointer isn't 1
    // by chance: it's invalid, and the write lands at address 0x1 — deleting
    // a character (D2 renames the .d2s) crashes the process on "Invalid
    // write of uint32_t at addr: 0x1" inside _link_r. The files involved are
    // kilobyte-sized saves, so copying costs nothing and only uses
    // fopen/fread/fwrite, already proven elsewhere here.
    K("MoveFileA",2,[](Cpu&c)->uint32_t{ std::string a=gread_mb(c,c.arg(0),-1),b=gread_mb(c,c.arg(1),-1);
        auto bn=[](const std::string&p){ size_t s=p.find_last_of("\\/"); return s==std::string::npos?p:p.substr(s+1); };
        std::string src=g_writeRoot+"/"+bn(a), dst=g_writeRoot+"/"+bn(b);
        FILE* fi=std::fopen(src.c_str(),"rb"); if(!fi) return 0u;
        FILE* fo=std::fopen(dst.c_str(),"wb"); if(!fo){ std::fclose(fi); return 0u; }
        char buf[4096]; size_t n; bool ok=true;
        while((n=std::fread(buf,1,sizeof buf,fi))>0) if(std::fwrite(buf,1,n,fo)!=n){ ok=false; break; }
        std::fclose(fo); std::fclose(fi);
        if(!ok){ std::remove(dst.c_str()); pc_dirty(g_writeRoot); return 0u; }
        std::remove(src.c_str());
        pc_dirty(g_writeRoot);            // dst created AND src removed
        { char m[160]; std::snprintf(m,sizeof m,"fichier: deplace %s -> %s",bn(a).c_str(),bn(b).c_str());
          d2vita_progress(m); }
        return 1u; });
}
