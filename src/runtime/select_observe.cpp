// select_observe.cpp -- see select_observe.h.
//
// Game.exe 1.14d calls select from five sites (IAT 0x6cc584, thunk 0x6b0186).
// The log prints the guest return address, i.e. site + 5:
//   site        return   sets            timeval               caller
//   0x0040de52  40de57   read + except   {0,100000}            Battle.net chat and realm connections
//   0x0052ab67  52ab6c   read            {0,100000}            game server client thread
//   0x006c3766  6c376b   read + except   {0,100000/(n/64+1)}   TCP/IP game host (hosting only)
//   0x0051ccfe  51cd03   write + except  {1,0}                 gateway connection race
//   0x006c2912  6c2917   read            {0,0}                 listener poll
#include "select_observe.h"
#include "runtime/rt_host.h"
#include "runtime/cpu.h"
#include "runtime/win32_shims_wsock32.h"
#include <cstdio>

static constexpr unsigned kSelReportMs = 5000;   // at most one summary line every 5 s
SelStats g_sel{};

static void sel_note_site(uint32_t va, uint8_t cls){
    for(unsigned i=0;i<kSelSites;i++){
        if(!g_sel.sites[i].n){ g_sel.sites[i].va=va; g_sel.sites[i].n=1; g_sel.sites[i].cls=cls; return; }
        if(g_sel.sites[i].va==va){ ++g_sel.sites[i].n; g_sel.sites[i].cls|=cls; return; }
    }
    ++g_sel.sitesOver;   // table full: counted, never dropped silently
}

void sel_observe(const WsockEvent& e){
    ++g_sel.calls;
    const uint8_t cls = e.timeout_ms==-1 ? 1 : e.timeout_ms==-2 ? 8 : e.timeout_ms==0 ? 2 : 4;
    switch(cls){ case 1: ++g_sel.tvNull; break; case 2: ++g_sel.tvZero; break;
                 case 4: ++g_sel.tvPos; break; default: ++g_sel.tvBad; break; }
    if(e.result>0) ++g_sel.ready;
    else if(e.result==0) ++g_sel.timedOut;
    else { ++g_sel.errors; g_sel.lastErr=e.wsa_err; }
    g_sel.waitedMs += e.waited_ms;
    // The shim is still executing: ESP points at the guest return address.
    if(e.cpu) sel_note_site(e.cpu->read_u32(e.cpu->reg(d2rt::R_ESP)), cls);
    sel_report(false);
}

// Bounded by TIME, not by call count: select throughput depends entirely on
// the game. A line is written only when something changed since the last one.
void sel_report(bool force){
    if(!force && !env_netlog()) return;
    static uint32_t lastCalls=0;
    const uint64_t now=rt_now_ms();
    if(!force){
        if(g_sel.calls==lastCalls) return;
        if(g_sel.lastReport && now-g_sel.lastReport < kSelReportMs) return;
    }
    g_sel.lastReport=now; lastCalls=g_sel.calls;
    char sites[192]; int w=0; sites[0]=0;
    for(unsigned i=0;i<kSelSites && g_sel.sites[i].n && w<(int)sizeof sites-24;i++){
        char cls[5]; int k=0;
        if(g_sel.sites[i].cls&1) cls[k++]='N';   // NULL timeval
        if(g_sel.sites[i].cls&2) cls[k++]='0';   // {0,0}: poll
        if(g_sel.sites[i].cls&4) cls[k++]='+';   // positive timeval
        if(g_sel.sites[i].cls&8) cls[k++]='X';   // invalid timeval
        cls[k]=0;
        w+=std::snprintf(sites+w,sizeof sites-(size_t)w,"%s%06x:%u/%s",i?",":"",g_sel.sites[i].va,g_sel.sites[i].n,cls);
    }
    d2_crashlog("select: appels=%u sites=%s%s | tv nul=%u zero=%u pos=%u invalide=%u | pretes=%u expirees=%u erreurs=%u (wsa=%u) | dans-select=%llums",
                g_sel.calls, sites[0]?sites:"-", g_sel.sitesOver?"(+autres)":"",
                g_sel.tvNull,g_sel.tvZero,g_sel.tvPos,g_sel.tvBad,
                g_sel.ready,g_sel.timedOut,g_sel.errors,g_sel.lastErr,
                (unsigned long long)g_sel.waitedMs);
}
