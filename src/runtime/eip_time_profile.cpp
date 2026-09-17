// eip_time_profile.cpp -- see eip_time_profile.h.
//
// --- EIP profile: report extracted from main() so it can ALSO be emitted at
// the end of a window (D2_EIPPROF_FROM/TO), not only at program exit.
#include "eip_time_profile.h"
#include "runtime/rt_host.h"
#include "runtime/prof_map.h"
#include "platform/vita_present.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace d2rt;

extern "C" { extern uint32_t d2rt_eipprof_on, d2rt_eipprof_base; extern uint64_t d2rt_eipprof[8];
             extern uint64_t d2rt_eipprof_sub[32], d2rt_eipprof_sub2[32], d2rt_eipprof_fn[16], d2rt_eipprof_sub3[32], d2rt_eipprof_all[96];
             extern uint32_t d2rt_eipprof_key[8192]; extern uint64_t d2rt_eipprof_hit[8192]; }
extern "C" { extern uint32_t d2rt_timeprof_on, d2rt_timeprof_base;
             extern uint64_t d2rt_tp_key[8192], d2rt_tp_hit[8192], d2rt_tp_bucket[8];
             extern uint64_t d2rt_tp_samples, d2rt_tp_susp, d2rt_tp_us, d2rt_tp_blocks; }

static bool g_epWasOn=false;
static uint32_t g_tpTarget=0;
namespace {
struct EpLine {
    char b[200]; int n = 0;
    void reset(const char* head){ n = std::snprintf(b, sizeof b, "%s", head); }
    void add(const char* fmt, ...) {
        char t[120]; va_list ap; va_start(ap, fmt);
        std::vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
        int l = (int)std::strlen(t);
        if (n + l >= (int)sizeof b - 1) { flush(); n = std::snprintf(b, sizeof b, "  ..."); }
        n += std::snprintf(b + n, sizeof b - n, "%s", t);
    }
    void flush(){ if (n > 0) { jpline("%s", b); n = 0; } }
};
}
// The FAMILY MAP for the address profile (winx86 src/runtime/prof_map.h).
// These six ranges are Diablo II 1.14d subsystems inside Game.exe. They used
// to be hardcoded in the engine's profiler, which therefore served them to
// any port even if it had no Codec, DCC, or DRLG. ORDER matters: the first
// range containing the address wins, like the original else-if chain. The
// nm[] labels below name them.
void d2_prof_map(){
    Wx86ProfMap m;
    m.fam[0] = {0x20b200,0x20d040};   // Codec.cpp (DCC)
    m.fam[1] = {0x1fe000,0x204000};   // SpriteCache.cpp
    m.fam[2] = {0x2094b0,0x20ab00};   // Tilecmp.cpp
    m.fam[3] = {0x242000,0x280000};   // DRLG (level gen)
    m.fam[4] = {0x0f0000,0x110000};   // Gfx / blit family
    m.fam[5] = {0x000000,0x030000};   // Storm (alloc/MPQ/IO)
    m.nfam   = 6;
    m.zoom_4k_a = 0x0f0000;           // detail Gfx       -> d2rt_eipprof_sub
    m.zoom_4k_b = 0x0d0000;           // detail voisinage -> d2rt_eipprof_sub2
    m.zoom_256  = 0x0fa000;           // zoom fonction    -> d2rt_eipprof_fn
    wx86_prof_set_map(m);
}

void eipprof_report(const char* tag){
    jpline("[EIPPROF] === %s ===", tag?tag:"total");
    if(!d2rt_eipprof_on && !g_epWasOn) return;
    EpLine L;
    { static const char* nm[8]={"Codec/DCC","SpriteCache","Tilecmp","DRLG","Gfx/blit","Storm","autre",""};
        uint64_t tot=0; for(int i=0;i<7;i++) tot+=d2rt_eipprof[i]; if(!tot) tot=1;
        L.reset("[EIPPROF] familles:");
        L.add(" echantillons=%llu",(unsigned long long)tot);
        for(int i=0;i<7;i++) L.add(" %s=%llu%%",nm[i],(unsigned long long)(d2rt_eipprof[i]*100/tot));
        L.flush();
        for(int pass=0;pass<3;pass++){ const uint64_t* s = pass==0? d2rt_eipprof_sub : pass==1? d2rt_eipprof_sub2 : d2rt_eipprof_sub3;
            uint32_t base = pass==0? 0x0f0000u : pass==1? 0x0d0000u : 0x030000u;
            uint32_t step = pass==2? 0x8000u : 0x1000u;
            char head[48]; std::snprintf(head,sizeof head,"[EIPPROF/detail 0x%06x]",base);
            L.reset(head);
            for(int k=0;k<5;k++){ int bi=-1; uint64_t bv=0;
                for(int i=0;i<32;i++) if(s[i]>bv){ bv=s[i]; bi=i; }
                if(bi<0||!bv) break;
                L.add("  Game+0x%05x..%05x=%llu%%",base+bi*step,base+bi*step+step-1,
                    (unsigned long long)(bv*100/tot));
                const_cast<uint64_t*>(s)[bi]=0; }
            L.flush(); }
        { L.reset("[EIPPROF/TOP]");
          uint64_t cp[96]; for(int i=0;i<96;i++) cp[i]=d2rt_eipprof_all[i];
          for(int k=0;k<8;k++){ int bi=-1; uint64_t bv=0;
              for(int i=0;i<96;i++) if(cp[i]>bv){ bv=cp[i]; bi=i; }
              if(bi<0||!bv) break;
              L.add("  Game+0x%06x..%06x=%llu%%",bi*0x8000,bi*0x8000+0x7fff,
                  (unsigned long long)(bv*100/tot));
              cp[bi]=0; }
          L.flush(); }
        L.reset("[EIPPROF/fn]");
        for(int i=0;i<16;i++) if(d2rt_eipprof_fn[i])
            L.add(" +0x%05x=%llu%%",0xfa000+i*0x100,(unsigned long long)(d2rt_eipprof_fn[i]*100/tot));
        L.flush();
        // EXACT addresses: this list names the functions worth porting. The
        // percentage is per translation block, so several neighboring lines
        // often belong to the SAME function (one per loop body) — group them
        // when reading the disassembly. Four per line: 40 console log lines
        // for one report is too many; ten is not.
        { uint32_t k[8192]; uint64_t v[8192]; int n=0;
          for(int i=0;i<8192;i++) if(d2rt_eipprof_hit[i]){ k[n]=d2rt_eipprof_key[i]; v[n]=d2rt_eipprof_hit[i]; ++n; }
          jpline("[EIPPROF/EXACT] %d adresses distinctes, top 40 (par bloc de traduction) :",n);
          L.reset("[EIPPROF/EXACT]");
          for(int r=0;r<40;r++){ int bi=-1; uint64_t bv=0;
              for(int i=0;i<n;i++) if(v[i]>bv){ bv=v[i]; bi=i; }
              if(bi<0||!bv) break;
              L.add(" %06x=%llu.%02llu%%",k[bi]-d2rt_eipprof_base,
                  (unsigned long long)(bv*10000/tot)/100ull,(unsigned long long)(bv*10000/tot)%100ull);
              if((r%4)==3){ L.flush(); L.reset("[EIPPROF/EXACT]"); }
              v[bi]=0; }
          L.flush(); } }
}

// TIME profile report (D2_TIMEPROF). Same jpline channel as the rest: printf
// doesn't exist on console. Also publishes what it dropped (`suspended`) and
// the average budget the feedback loop achieved — without which there's no
// way to judge whether the instrument did what it claims.
static bool g_tpWasOn=false;   // same role as g_epWasOn: the report is
                               // called AFTER the window is disarmed.
void timeprof_report(const char* tag){
    if(!d2rt_timeprof_on && !g_tpWasOn) return;
    const uint64_t tot = d2rt_tp_samples ? d2rt_tp_samples : 1;
    jpline("[TIMEPROF] === %s === cible=%u us echantillons=%llu suspendus=%llu",
           tag?tag:"total",(unsigned)(d2rt_timeprof_on?d2rt_timeprof_on:g_tpTarget),
           (unsigned long long)d2rt_tp_samples,(unsigned long long)d2rt_tp_susp);
    jpline("[TIMEPROF] couvert=%llu ms | intervalle moyen=%llu us | budget moyen=%llu blocs",
           (unsigned long long)(d2rt_tp_us/1000ull),
           (unsigned long long)(d2rt_tp_us/tot),
           (unsigned long long)(d2rt_tp_blocks/tot));
    { static const char* nm[7]={"Codec/DCC","SpriteCache","Tilecmp","DRLG","Gfx/blit","Storm","autre"};
      EpLine L; L.reset("[TIMEPROF] familles:");
      for(int i=0;i<7;i++) L.add(" %s=%llu%%",nm[i],(unsigned long long)(d2rt_tp_bucket[i]*100/tot));
      L.flush(); }
    { uint32_t k[8192]; uint64_t v[8192]; int n=0;
      for(int i=0;i<8192;i++) if(d2rt_tp_hit[i]){ k[n]=(uint32_t)d2rt_tp_key[i]; v[n]=d2rt_tp_hit[i]; ++n; }
      jpline("[TIMEPROF/EXACT] %d adresses distinctes, top 40 (part du TEMPS) :",n);
      EpLine L; L.reset("[TIMEPROF/EXACT]");
      for(int r=0;r<40;r++){ int bi=-1; uint64_t bv=0;
          for(int i=0;i<n;i++) if(v[i]>bv){ bv=v[i]; bi=i; }
          if(bi<0||!bv) break;
          L.add(" %06x=%llu.%02llu%%",k[bi]-d2rt_timeprof_base,
                (unsigned long long)(bv*10000/tot)/100ull,(unsigned long long)(bv*10000/tot)%100ull);
          if((r%4)==3){ L.flush(); L.reset("[TIMEPROF/EXACT]"); }
          v[bi]=0; }
      L.flush(); }
}

// This report goes entirely through jpline(), which does printf AND
// d2vita_progress — a bare printf never reaches any log on console.
// Accumulator: jpline takes 220 bytes, so this report's lines are built
// piece by piece and flushed as they approach capacity.
// D2_EIPPROF_FROM / D2_EIPPROF_TO: profile ONLY the desired frame window (an
// in-game movement phase is unreadable in a profile diluted by level
// loading, which is an order of magnitude heavier and only happens once).
static uint32_t g_epFrom=0,g_epTo=0; static int g_epArmed=0;
static void tp_zero(){ std::memset((void*)d2rt_tp_key,0,sizeof d2rt_tp_key);
    std::memset((void*)d2rt_tp_hit,0,sizeof d2rt_tp_hit);
    std::memset((void*)d2rt_tp_bucket,0,sizeof d2rt_tp_bucket);
    d2rt_tp_samples=0; d2rt_tp_susp=0; d2rt_tp_us=0; d2rt_tp_blocks=0; }
static void ep_zero(){ std::memset(d2rt_eipprof,0,sizeof d2rt_eipprof);
    std::memset(d2rt_eipprof_sub,0,sizeof d2rt_eipprof_sub); std::memset(d2rt_eipprof_sub2,0,sizeof d2rt_eipprof_sub2);
    std::memset(d2rt_eipprof_sub3,0,sizeof d2rt_eipprof_sub3); std::memset(d2rt_eipprof_fn,0,sizeof d2rt_eipprof_fn);
    std::memset(d2rt_eipprof_all,0,sizeof d2rt_eipprof_all);
    std::memset(d2rt_eipprof_key,0,sizeof d2rt_eipprof_key); std::memset(d2rt_eipprof_hit,0,sizeof d2rt_eipprof_hit); }
void ep_tick(int frame){
    if(!g_epArmed){ g_epArmed=1;
        if(const char* s=getenv("D2_EIPPROF_FROM")) g_epFrom=(uint32_t)strtoul(s,nullptr,10);
        if(const char* s=getenv("D2_EIPPROF_TO"))   g_epTo=(uint32_t)strtoul(s,nullptr,10);
        if(g_epFrom && d2rt_eipprof_on){ d2rt_eipprof_on=0; g_epWasOn=true; }
        // D2_TIMEPROF follows the SAME window: otherwise it would never
        // report on console, where main() never returns (the pass is
        // destroyed). g_tpTarget keeps the target while the instrument sleeps
        // outside the window.
        if(g_epFrom && d2rt_timeprof_on){ g_tpTarget=d2rt_timeprof_on; d2rt_timeprof_on=0; }
        else if(d2rt_timeprof_on) g_tpWasOn=true; }
    if(!g_epFrom && !g_epTo) return;
    if(g_epFrom && (uint32_t)frame==g_epFrom){ ep_zero(); tp_zero(); d2rt_eipprof_on=1; g_epWasOn=true;
        if(g_tpTarget){ d2rt_timeprof_on=g_tpTarget; g_tpWasOn=true; }
        char m[80]; std::snprintf(m,sizeof m,"EIPPROF: fenetre OUVERTE image %u",g_epFrom); d2vita_progress(m); std::printf("  %s\n",m); }
    if(g_epTo && (uint32_t)frame==g_epTo && (d2rt_eipprof_on||d2rt_timeprof_on)){
        d2rt_eipprof_on=0; d2rt_timeprof_on=0;
        char m[80]; std::snprintf(m,sizeof m,"EIPPROF: fenetre FERMEE image %u",g_epTo); d2vita_progress(m); std::printf("  %s\n",m);
        char t[64]; std::snprintf(t,sizeof t,"fenetre %u..%u",g_epFrom,g_epTo); eipprof_report(t); timeprof_report(t); std::fflush(stdout); } }
