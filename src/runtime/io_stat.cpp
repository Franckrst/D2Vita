// io_stat.cpp -- see io_stat.h.
//
// ====== D2_IOSTAT / D2_READAHEAD: file reads ================================
// ReadFile calls run tens of ms each, while Storm's sector reader (sub_4154b0)
// only asks for 4-8 KiB per call — the read-ahead below exists to close that
// gap. Guest-visible counts and sizes match between qemu and console (same
// guest bytes); only host-side timing differs.
//
//  D2_IOSTAT=1          one `io:` + `io/fichiers:` line per 10 s window: calls,
//                       bytes, host time, sizes, sequential/seek, served from
//                       RAM (hit) or card, per file.
//  D2_READAHEAD=<KiB>   per-handle read-ahead (read-only, data folder): the
//                       stdio stream goes unbuffered, reads go through
//                       lseek+read on the descriptor, and a <KiB> buffer serves
//                       reads that land inside it. A read >= <KiB> goes
//                       straight into the guest buffer. Default 0 = unchanged
//                       behavior (fread + 256 KiB stdio buffer per handle).
//  D2_READAHEAD_MIN=<KiB>  adaptive read-ahead: a SEEK read only fills <MIN>
//                       KiB; each sequential continuation doubles the size up
//                       to <READAHEAD>. Default = READAHEAD (non-adaptive).
//                       Storm's access pattern is "sector table (seek) then
//                       sectors in sequence": the large block is only paid for
//                       once the sequence is proven.
//  D2_READAHEAD_MAX=<KiB>  memory ceiling for ALL buffers (default 2048);
//                       beyond it, the least-recently-read handle's buffer is
//                       RECLAIMED (`steals` counter); if none is available, the
//                       handle reads unbuffered (`refusals` counter).
//
// Why lseek+read instead of fread: the only thing that matters on the memory
// card is the SIZE we request, which we control directly here. newlib's 256 KiB
// stdio buffer, by contrast, fills completely on every read that falls outside
// its window — request 4 KiB, read 256 KiB.
#include "io_stat.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "platform/vita_present.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
using namespace d2rt;

static std::map<uint32_t,IoH> g_ioh;
struct IoCnt { uint64_t n=0,bytes=0,us=0,seq=0,jump=0,h[4]={0,0,0,0},
                        hit=0,hitB=0,card=0,cardN=0,cardB=0,cardUs=0,direct=0; };
static IoCnt g_ioW, g_ioT;          // current window / cumulative total
struct IoFileCnt { uint64_t n=0,bytes=0,us=0,seq=0,hit=0,cardN=0,cardB=0; };
static std::map<std::string,IoFileCnt> g_ioF;     // per file, current window
bool g_ioStat=false;
static uint32_t g_raKB=0, g_raMinKB=0, g_raMaxKB=2048;
// D2_READAHEAD_JUMP=<KiB>: how much to fill on a SEEK. Default = MIN, i.e.
// unchanged behavior. Smaller means less over-reading when the access pattern
// is random; the sequential ramp (doubling) catches up from there.
// D2_READAHEAD_WIN=<n>: windows kept PER HANDLE (default 1 = old behavior).
// Multiple windows catch short backward seeks, which is what drops the
// service rate from 94% to 54% with a single window.
static uint32_t g_raJumpKB=0, g_raWins=1;
// D2_READAHEAD_WINKB=<KiB>: size of ONE window, decoupled from D2_READAHEAD.
// A window used to always be D2_READAHEAD (128 KiB) even when only 8 KiB of it
// was ever filled — 94% of the buffer wasted, and only 16 windows fit in the
// 2 MiB ceiling. At 32 KiB per window, the same ceiling holds 64 — four times
// the cache for the same memory. This matters because the Vita only has ~14 MiB
// of free user RAM; an 8 MiB ceiling here crashed game loading in a way qemu
// could not reproduce. Gains now come from GRANULARITY, not raw volume.
static uint32_t g_raWinKB=0;
static uint64_t g_raWinHit=0, g_raWinMiss=0;   // secondary-window service count
static uint32_t g_raUse=0, g_raPeak=0, g_raRefus=0, g_raHandles=0, g_raSteal=0;   // buffer memory accounting
static uint64_t g_raClock=0;        // logical clock of reads (LRU)
static uint64_t g_ioT0=0;
void io_init(){
    // Defaults match env.txt's tuned values (128/32/8/8/32); still overridable
    // via env vars without a rebuild.
    if(const char* e=getenv("D2_IOSTAT")) g_ioStat = *e && *e!='0';
    { long v=128; if(const char* e=getenv("D2_READAHEAD")) v=strtol(e,nullptr,0);
      g_raKB=(uint32_t)(v>0?v:0); if(g_raKB>16384) g_raKB=16384; }
    if(const char* e=getenv("D2_READAHEAD_MAX")){ long v=strtol(e,nullptr,0); if(v>0) g_raMaxKB=(uint32_t)v; }
    g_raMinKB = g_raKB<32?g_raKB:32;
    if(const char* e=getenv("D2_READAHEAD_MIN")){ long v=strtol(e,nullptr,0); if(v>0 && (uint32_t)v<g_raKB) g_raMinKB=(uint32_t)v; }
    g_raJumpKB = g_raMinKB<8?g_raMinKB:8;
    if(const char* e=getenv("D2_READAHEAD_JUMP")){ long v=strtol(e,nullptr,0); if(v>0 && (uint32_t)v<=g_raMinKB) g_raJumpKB=(uint32_t)v; }
    g_raWins=8>RA_WINMAX?RA_WINMAX:8;
    if(const char* e=getenv("D2_READAHEAD_WIN")){ long v=strtol(e,nullptr,0);
        if(v>=1 && v<=RA_WINMAX) g_raWins=(uint32_t)v; }
    g_raWinKB = g_raKB<32?g_raKB:32;
    if(const char* e=getenv("D2_READAHEAD_WINKB")){ long v=strtol(e,nullptr,0);
        if(v>0 && (uint32_t)v<=g_raKB) g_raWinKB=(uint32_t)v; }
    if(g_raKB){ char m[160]; std::snprintf(m,sizeof m,
        "readahead: ARME — max %u Kio, fenetre %u Kio x%u/poignee, saut %u Kio,"
        " plafond memoire %u Kio (stdio non tamponne)",
        g_raKB,g_raWinKB,g_raWins,g_raJumpKB,g_raMaxKB);
        d2vita_progress(m); std::printf("[%s]\n",m); }
}
bool ra_on(){ return g_raKB!=0; }
static const char* io_short(const std::string& p){ const char* b=std::strrchr(p.c_str(),'/'); return b?b+1:p.c_str(); }
IoH* io_get(uint32_t h){
    auto it=g_ioh.find(h); if(it!=g_ioh.end()) return &it->second;
    if(!g_ioStat && !ra_on()) return nullptr;
    IoH& f=g_ioh[h]; auto p=g_filePathByH.find(h); f.nm = p==g_filePathByH.end()? "?" : io_short(p->second);
    return &f;
}
void io_close(uint32_t h){
    auto it=g_ioh.find(h); if(it==g_ioh.end()) return;
    for(uint32_t i=0;i<RA_WINMAX;i++){ RaWin& w=it->second.w[i];
        if(w.buf){ std::free(w.buf); g_raUse-=w.cap; --g_raHandles; w.buf=nullptr; w.cap=0; w.len=0; } }
    g_ioh.erase(it);
}
// A RAW read from the card: lseek + read, looping until n bytes or EOF.
static uint32_t ra_raw(IoH& f, long off, void* dst, uint32_t n){
    uint64_t t0=rt_now_us();
    uint32_t got=0;
    if(::lseek(f.fd,off,SEEK_SET)==(off_t)off){
        while(got<n){ ssize_t r=::read(f.fd,(uint8_t*)dst+got,n-got); if(r<=0) break; got+=(uint32_t)r; } }
    uint64_t dt=rt_now_us()-t0;
    g_ioW.cardN++; g_ioW.cardB+=got; g_ioW.cardUs+=dt; g_ioT.cardN++; g_ioT.cardB+=got; g_ioT.cardUs+=dt;
    auto fc=g_ioF.find(f.nm); if(fc!=g_ioF.end()){ fc->second.cardN++; fc->second.cardB+=got; }
    return got;
}
// Read at offset `off` served from the handle's buffer (D2_READAHEAD). Returns
// the number of bytes copied into dst; `fromRam` = everything came from the buffer.
uint32_t ra_read(IoH& f, long off, void* dst, uint32_t n, bool& fromRam){
    fromRam=true; uint32_t done=0;
    while(done<n){
        long o=off+done;
        // --- 1. serve from ANY of the windows -----------------------------
        RaWin* hit=nullptr;
        for(uint32_t i=0;i<g_raWins;i++){ RaWin& w=f.w[i];
            if(w.buf && w.len && o>=w.off && o<w.off+(long)w.len){ hit=&w; break; } }
        if(hit){
            uint32_t k=(uint32_t)(hit->off+(long)hit->len-o); if(k>n-done) k=n-done;
            std::memcpy((uint8_t*)dst+done,hit->buf+(o-hit->off),k);
            hit->use=++g_raClock; done+=k; ++g_raWinHit; continue; }
        fromRam=false; ++g_raWinMiss;
        // --- 2. choose the window to (re)fill: a free one, else the oldest
        RaWin* tgt=nullptr;
        for(uint32_t i=0;i<g_raWins;i++) if(!f.w[i].buf){ tgt=&f.w[i]; break; }
        if(!tgt){ tgt=&f.w[0];
            for(uint32_t i=1;i<g_raWins;i++) if(f.w[i].use<tgt->use) tgt=&f.w[i]; }
        // SEQUENTIAL continuation: the read starts exactly where a window ends.
        // This is what allows doubling; a seek restarts at the seek step.
        // Storm's pattern is "sector table (seek) then sectors in sequence" —
        // the large block is only paid for once the sequence is proven.
        bool cont=false;
        for(uint32_t i=0;i<g_raWins;i++){ RaWin& w=f.w[i];
            if(w.buf && w.len && o==w.off+(long)w.len){ cont=true; break; } }
        if(!tgt->buf){
            const uint32_t cap=g_raWinKB<<10;
            if(g_raUse+cap>g_raMaxKB*1024u){   // ceiling: reclaim the least-recently-read window across ALL handles
                RaWin* victim=nullptr;
                for(auto& kv:g_ioh){ IoH& v=kv.second;
                    for(uint32_t i=0;i<RA_WINMAX;i++) if(v.w[i].buf && &v.w[i]!=tgt &&
                        (!victim || v.w[i].use<victim->use)) victim=&v.w[i]; }
                if(victim){ tgt->buf=victim->buf; tgt->cap=victim->cap;
                            victim->buf=nullptr; victim->cap=0; victim->len=0; ++g_raSteal; }
            } else { tgt->buf=(uint8_t*)std::malloc(cap);
                if(tgt->buf){ tgt->cap=cap; g_raUse+=cap; ++g_raHandles;
                              if(g_raUse>g_raPeak) g_raPeak=g_raUse; } }
            tgt->len=0;
            if(!tgt->buf) ++g_raRefus; }
        if(!tgt->buf || n-done>=tgt->cap){    // no buffer, or bigger than it: direct
            g_ioW.direct++; g_ioT.direct++;
            done+=ra_raw(f,o,(uint8_t*)dst+done,n-done); break; }
        uint32_t want = cont ? (f.next? f.next*2u : g_raJumpKB<<10) : (g_raJumpKB<<10);
        if(want>tgt->cap) want=tgt->cap;
        if(want<n-done) want=n-done;          // at least whatever remains to serve
        f.next=want;
        tgt->off=o; tgt->len=ra_raw(f,o,tgt->buf,want); tgt->use=++g_raClock;
        if(!tgt->len) break;                  // end of file
    }
    return done;
}
void io_account(IoH* f, long at, uint32_t n, uint32_t got, uint64_t us, bool hit){
    IoCnt* cs[2]={&g_ioW,&g_ioT};
    if(f) f->lastUse=++g_raClock;
    const int b = n<=4096u?0 : n<=65536u?1 : n<=524288u?2 : 3;
    const bool seq = f && f->lastEnd==at;
    for(IoCnt* c:cs){ c->n++; c->bytes+=got; c->us+=us; c->h[b]++; if(seq) c->seq++; else c->jump++;
                      if(hit){ c->hit++; c->hitB+=got; } else c->card++; }
    if(f){ f->lastEnd=at+(long)got;
        IoFileCnt& fc=g_ioF[f->nm]; fc.n++; fc.bytes+=got; fc.us+=us; if(seq) fc.seq++; if(hit) fc.hit++; }
}
static void io_fmt(char* m, size_t sz, const char* quand, const IoCnt& w){
    std::snprintf(m,sz,
        "io(%s): lectures=%llu octets=%lluK hote=%llums (%lluus/lect) tailles: <=4K=%llu <=64K=%llu <=512K=%llu >512K=%llu seq=%llu saut=%llu"
        " | ram=%llu/%lluK carte=%llu lect %llu/%lluK %llums direct=%llu | tampons=%uK pic=%uK poignees=%u vols=%u refus=%u",
        quand,(unsigned long long)w.n,(unsigned long long)(w.bytes>>10),(unsigned long long)(w.us/1000),
        (unsigned long long)(w.n?w.us/w.n:0),
        (unsigned long long)w.h[0],(unsigned long long)w.h[1],(unsigned long long)w.h[2],(unsigned long long)w.h[3],
        (unsigned long long)w.seq,(unsigned long long)w.jump,
        (unsigned long long)w.hit,(unsigned long long)(w.hitB>>10),
        (unsigned long long)w.card,(unsigned long long)w.cardN,(unsigned long long)(w.cardB>>10),(unsigned long long)(w.cardUs/1000),
        (unsigned long long)w.direct, g_raUse>>10,g_raPeak>>10,g_raHandles,g_raSteal,g_raRefus,
        (unsigned long long)g_raWinHit,(unsigned long long)g_raWinMiss);
}
void io_line(const char* quand){
    char m[320];
    io_fmt(m,sizeof m,quand,g_ioW); d2vita_progress(m); std::printf("[%s]\n",m);
    if(!std::strcmp(quand,"final")){ io_fmt(m,sizeof m,"cumul",g_ioT); d2vita_progress(m); std::printf("[%s]\n",m); }
    // per file: the 6 heaviest in host time
    std::vector<std::pair<std::string,IoFileCnt>> v(g_ioF.begin(),g_ioF.end());
    std::sort(v.begin(),v.end(),[](const std::pair<std::string,IoFileCnt>&a,const std::pair<std::string,IoFileCnt>&b){
        return a.second.us!=b.second.us? a.second.us>b.second.us : a.second.n>b.second.n; });
    int off=std::snprintf(m,sizeof m,"io/fichiers(%s):",quand);
    for(size_t i=0;i<v.size()&&i<6&&off<(int)sizeof m-64;i++){ const IoFileCnt& c=v[i].second;
        off+=std::snprintf(m+off,sizeof m-(size_t)off," %s n=%llu %lluK %llums seq=%llu ram=%llu carte=%llu/%lluK;",
            v[i].first.c_str(),(unsigned long long)c.n,(unsigned long long)(c.bytes>>10),(unsigned long long)(c.us/1000),
            (unsigned long long)c.seq,(unsigned long long)c.hit,(unsigned long long)c.cardN,(unsigned long long)(c.cardB>>10)); }
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
    g_ioW=IoCnt{}; g_ioF.clear();
}
void io_tick(uint64_t now){          // called on every ReadFile under D2_IOSTAT
    if(!g_ioT0){ g_ioT0=now; return; }
    if(now-g_ioT0>=10000000ull){ g_ioT0=now; io_line("10s"); }
}
