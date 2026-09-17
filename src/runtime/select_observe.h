// select_observe.h -- census of the guest's select() calls, fed by the network
// observer (WX86_NET_SELECT, emitted by winx86's select). Names the calling
// sites, the timeval each one passes and how the calls ended, so a network
// freeze can be read from the log. Silent unless D2NETLOG is set.
#pragma once
#include <cstdint>
struct WsockEvent;

struct SelSite { uint32_t va; uint32_t n; uint8_t cls; };   // cls: bit0 NULL, bit1 {0,0}, bit2 >0, bit3 invalid
static constexpr unsigned kSelSites = 8;         // Game.exe 1.14d has 5 call sites

struct SelStats {
    uint32_t calls, tvNull, tvZero, tvPos, tvBad;   // timeval classes seen
    uint32_t ready, timedOut, errors;               // how the calls ended
    uint32_t lastErr;                               // last Winsock error returned
    uint64_t waitedMs;                              // total time spent inside select
    uint32_t sitesOver;                             // calls from sites beyond kSelSites
    SelSite  sites[kSelSites];
    uint64_t lastReport;
};
extern SelStats g_sel;

void sel_observe(const WsockEvent& e);   // accounts one WX86_NET_SELECT event
void sel_report(bool force);             // summary line: D2NETLOG only, unless forced
