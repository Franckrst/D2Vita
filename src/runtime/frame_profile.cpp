// frame_profile.cpp -- see frame_profile.h.
#include "frame_profile.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/prof.h"
#include "runtime/guest_sync.h"
#include "runtime/sched_cooperative.h"
#include "glide_ring/gx_host.h"
#include "platform/vita_present.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace d2rt;
extern "C" uint32_t d2rt_timeprof_base;   // also declared in tools/rt_boot.cpp

// ====== D2_LOOPWATCH: what the RENDER THREAD does between two frames =======
// The 25 fps ceiling isn't proven with an average frames-per-second figure: it
// is proven by showing that the thread that presents (the one calling
// StretchBlt) spends the rest of the frame WAITING. So, per frame and for that
// one thread only, this measures:
//   * wait = t(last empty PeekMessage of the frame) - t(first). Fog's main
//     loop (0x4fa590) spins empty between frames: empty pump, clock read,
//     Sleep(0), repeat. This interval IS the time the rate limiter makes
//     unusable. Measured in WALL time, so it's valid under either scheduler
//     (under coop, wait() returns immediately: timing inside the shim itself
//     would say nothing).
//   * spins  = number of empty loop iterations per frame.
//   * sleep / wfso / tick = calls to Sleep, WaitForSingleObject and
//     GetTickCount/timeGetTime from the render thread, per frame.
//   * site   = most frequent return address of the empty pumps, in
//     Game+0x...: it NAMES the loop that's waiting.
// Without the knob, all of this is dead code (one boolean check per hot shim).
static bool     g_lwOn=false;
static bool     g_lwArmed=false;
static uint32_t g_lwTid=0xFFFFFFFFu;   // render thread (the one that presented the last frame)
static uint64_t g_lwFirstUs=0,g_lwLastUs=0;
static uint32_t g_lwSpins=0;
static uint64_t g_lwWaitUs=0,g_lwSpinN=0,g_lwSleepN=0; uint64_t g_lwWfsoN=0,g_lwTickN=0; static uint64_t g_lwFrames=0;
// g_lwWaitUs is RESET TO ZERO every 10 s window — unusable for attributing
// cost to a SINGLE frame. A second, MONOTONE cumulative counter is kept for
// the per-frame profile. Motivating case: a 2264 ms frame showed io=137 gx=0
// jit=2 sync=0 and, crucially, run=0 — the dynarec had executed NO guest
// instructions at all. The game thread wasn't slow, it was STOPPED. What
// remained was to prove where it was waiting.
// FINE HISTOGRAM PER WINDOW, 10 ms buckets. The game targets a 40 ms render
// period (its own "render-period=40ms" variable). Measured slow frame rates
// cluster around 12.5 fps — EXACTLY half of 25. Hypothesis to test: a frame
// that overshoots 40 ms by a hair makes the NEXT period wait too, so 80 ms,
// halving the rate in one step. If true, frame durations cluster at 40 and
// 80 ms rather than spreading continuously — and then a tiny per-frame time
// saving would flip 12.5 back to 25.
// Window averages CANNOT settle this: the distribution is needed.
static uint64_t g_fpWinHist[13] = {0};   // 0-9,10-19,...,110-119, >=120 ms
// TIME profile restricted to slow frames (ring fed by d2_timesamp,
// vita_present.cpp). One sample = a fixed duration, so the shares are shares
// of TIME — and this time, only the time of the frames THAT ARE A PROBLEM.
#ifdef __vita__
extern "C" { extern volatile uint32_t d2rt_ts_w; extern uint64_t d2rt_ts_t[]; extern uint32_t d2rt_ts_ip[]; }
#endif
#ifndef __vita__
// No D2_TIMESAMP ring outside console (vita_present.cpp is entirely
// #ifdef __vita__, see dyn86_sync_us above in this file for the same
// pattern): w stays 0, so slow_collect() exits on its first iteration without
// reading d2rt_ts_t/d2rt_ts_ip — an honest no-op, not a fabricated value.
static const uint32_t d2rt_ts_w = 0;
static const uint64_t d2rt_ts_t[4096] = {0};
static const uint32_t d2rt_ts_ip[4096] = {0};
#endif
struct SlowEnt { uint32_t eip, n; };
static SlowEnt g_slowTab[128]{};
static uint64_t g_slowSamp=0, g_slowFrames=0;
static void slow_collect(uint64_t t0, uint64_t t1){
    ++g_slowFrames;
    const uint32_t w = d2rt_ts_w;
    for(uint32_t k=0;k<4096;k++){
        const uint32_t i=(w+4095u-k)&4095u;
        const uint64_t t=d2rt_ts_t[i];
        if(!t || t<t0) break;
        if(t>t1) continue;
        const uint32_t ip=d2rt_ts_ip[i]; if(!ip) continue;
        ++g_slowSamp;
        unsigned h=(ip*2654435761u)>>25;
        for(unsigned j=0;j<128;j++){ SlowEnt& e=g_slowTab[(h+j)&127];
            if(e.eip==ip){ ++e.n; break; }
            if(!e.eip){ e.eip=ip; e.n=1; break; } }
    }
}
static uint64_t g_lwWaitTot=0;
extern "C" { extern int d2rt_wakeprof; extern uint64_t d2rt_wake_n, d2rt_wake_us, d2rt_wake_worst, d2rt_wake_sig, d2rt_poll0_n; }
// ⚠️ RAW counters. Without them, an "n=0" cannot be told apart from a dead
// instrument. SetEvent/ResetEvent are counted unconditionally as soon as the
// profile is armed.
AsyncProf g_apWork{}, g_apSee{};   // request->signaled ; signaled->seen
uint64_t g_apSetN=0, g_apResetN=0;
static int g_asyncprof=-1;
bool asyncprof_on(){ if(g_asyncprof<0) g_asyncprof=getenv("D2_ASYNCPROF")?atoi(getenv("D2_ASYNCPROF")):0; return g_asyncprof!=0; }
void ap_add(AsyncProf& a, uint64_t us){ ++a.n; a.us+=us; if(us>a.worst) a.worst=us; }
// D2_WAITPROF — REQUESTED vs ACTUALLY ELAPSED, per thread.
// Typical breakdown during a slow-frame window: WaitForSingleObject ~60%,
// Sleep ~33%, real work under 5%. Frame-rate drops are therefore neither
// compute nor I/O: the game is WAITING. What remained to determine is whether
// the wait is LEGITIMATE (the game has nothing to do) or whether it's our own
// wake-up that's late — a Sleep(10) that returns control at 200 ms produces
// exactly that same picture. Hence timing both ends of the wait.
WaitProf g_wpW{}, g_wpS{};        // WFSO with a FINITE timeout ; Sleep
// WHO polls, and from WHERE. When frame rate drops, WFSO call counts can jump
// by a factor of several with a worst case of 0ms->2ms timeouts — that is not
// a late wake-up, it's a REPEATED TEST (zero timeout), i.e. busy-polling in a
// loop. I/O volume stays essentially unchanged across such windows, so it
// explains nothing. What was missing was the calling site and the nature of
// the object: without them, it's known that something polls, not what it's
// waiting for.
WSite g_wpSites[8]{};
int wp_site(uint32_t eip, const char* kind){   // returns the index, to record the outcome
    for(int i=0;i<8;i++){
        if(g_wpSites[i].eip==eip){ ++g_wpSites[i].n; return i; }
        if(!g_wpSites[i].eip){ g_wpSites[i].eip=eip; g_wpSites[i].n=1; g_wpSites[i].sig=0;
            std::snprintf(g_wpSites[i].kind,sizeof g_wpSites[i].kind,"%s",kind?kind:"?"); return i; }
    }
    return -1;
}
static void wp_sites_fmt(char* out, size_t n){
    int idx[8], k=0; for(int i=0;i<8;i++) if(g_wpSites[i].eip) idx[k++]=i;
    for(int a=0;a<k;a++) for(int b=a+1;b<k;b++) if(g_wpSites[idx[b]].n>g_wpSites[idx[a]].n){ int t=idx[a]; idx[a]=idx[b]; idx[b]=t; }
    int w=0; out[0]=0;
    for(int a=0;a<k && a<3;a++){ const WSite& S=g_wpSites[idx[a]];
        // "x<calls> sig<signaled>": without the second number, there's no way to
        // tell if the subsystem is WORKING (signaled) or spinning EMPTY (timeouts only).
        w+=std::snprintf(out+w,(int)n-w>0?n-w:0,"%sGame+0x%x[%s]x%u sig%u",a?" ":"",
                         (unsigned)(S.eip>=g_d2base?S.eip-g_d2base:S.eip),S.kind,S.n,S.sig); }
    if(!k) std::snprintf(out,n,"-");
}
uint64_t g_wpWinf_n=0, g_wpWinf_us=0;   // WFSO INFINITE: no overshoot definable
static int g_waitprof=-1;
bool waitprof_on(){ if(g_waitprof<0) g_waitprof = getenv("D2_WAITPROF")?atoi(getenv("D2_WAITPROF")):0; return g_waitprof!=0; }
void wp_add(WaitProf& w, uint32_t req_ms, uint64_t act_us, uint32_t tid){
    const uint64_t req_us = (uint64_t)req_ms*1000ull;
    ++w.n; w.req_us += req_us; w.act_us += act_us;
    const uint64_t over = act_us > req_us ? act_us-req_us : 0;
    w.over_us += over;
    if(over > (uint64_t)w.wact - (uint64_t)w.wreq || !w.wact){ w.wreq=req_ms; w.wact=(uint32_t)(act_us/1000ull); w.wtid=tid; }
}
// No SIMULATION involved (Game+0x12fd90, see the hook placed below): the game
// has its OWN pace, one step every [0x883d60] = 1000/[0x731014] = 40 ms of
// real time, on the game thread. THAT is the game's speed — not the drawing
// rate. This counter exists to PROVE it: it must stay at ~25/s on both sides
// of the knob.
uint64_t g_lwSimN=0; static uint64_t g_lwSimWin=0;
static uint64_t g_lwWinT0=0;
struct LwSite { uint32_t eip; uint64_t n; };
static LwSite   g_lwSites[8]{};      // empty-pump sites
static LwSite   g_lwSlSites[8]{};    // Sleep call sites
// Game counters read from guest memory (game loop 0x44ef..): [0x7a0494]
// frames DRAWN, [0x7a04b4] frames SKIPPED, [0x70ef1c] render period. They say,
// with no hook or trap, whether the game itself decides not to draw — and
// therefore whether a ceiling is game-side or host-side.
static uint32_t g_lwDrawPrev=0, g_lwSkipPrev=0;
// ---- D2_NOCAP: state of the rate-limiter removal (the mechanism is
// documented at its installation site, below). D2_NOCAP_FROM=<frame> delays
// the removal to a given frame: both legs of an A/B are then STRICTLY
// identical up to that point (same scenario, same game state, same frame),
// and the only difference afterward is the cap. Without this, an in-game A/B
// is impossible: the scripted scenario is indexed by FRAME, so a leg that
// renders 8x more frames races through the menus 8x faster in real time and
// never reaches the level load.
Cpu*     g_nocapCpu=nullptr;
uint32_t g_nocapFog=0;       // VA of the Fog loop's immediate (Game+0xfa62d)
uint32_t g_nocapJeu=0;       // VA of the IN-GAME render period (Game+0x30ef1c)
uint32_t g_nocapGate=0;      // VA of the in-game draw-lock jump (Game+0x4f27e)
uint8_t  g_nocapVal=0;       // requested render ms
uint32_t g_nocapFrom=0;      // arming frame (0 = from load time)
bool     g_nocapDone=false;
void nocap_apply(){
    if(g_nocapDone||!g_nocapCpu) return;
    g_nocapDone=true;
    char m[200];
    // 1) menus, loading, cutscenes: the Fog loop's immediate operand.
    if(g_nocapFog){
        uint8_t cur[3]={0,0,0}; g_nocapCpu->read(g_nocapFog-2,cur,3);
        if(cur[0]==0x83&&cur[1]==0xC7&&cur[2]==0x28){
            g_nocapCpu->write(g_nocapFog,&g_nocapVal,1);   // write() lifts DB protection and dirties the block
            std::snprintf(m,sizeof m,"nocap: hors-jeu — Game+0xfa62b add edi,0x28 -> 0x%02x (%u ms/image)",
                          g_nocapVal,(unsigned)g_nocapVal);
        } else
            std::snprintf(m,sizeof m,"nocap: REFUS hors-jeu — octets inattendus a Game+0xfa62b (%02x %02x %02x)",
                          cur[0],cur[1],cur[2]);
        d2vita_progress(m); std::printf("%s\n",m);
    }
    // 2) IN-GAME, the render period in data. Secondary (it only governs the
    //    game loop's "catch-up" branch), but it's the only render constant on
    //    the in-game path, so it's kept consistent.
    if(g_nocapJeu){
        uint32_t cur=g_nocapCpu->read_u32(g_nocapJeu);
        if(cur==40u){
            uint32_t v=g_nocapVal; g_nocapCpu->write(g_nocapJeu,&v,4);
            std::snprintf(m,sizeof m,"nocap: en jeu — Game+0x30ef1c periode de rendu 40 -> %u ms",(unsigned)g_nocapVal);
        } else
            std::snprintf(m,sizeof m,"nocap: REFUS en jeu — Game+0x30ef1c vaut %u et non 40",(unsigned)cur);
        d2vita_progress(m); std::printf("%s\n",m);
    }
    // 3) IN-GAME, the REAL lock: drawing is only allowed once per SIMULATION
    //    STEP. [0x7a0704] is reset to zero only in block 0x44f1f2, reached only
    //    when gate 0x52fc20 has decided "a step is due", then set back to 1
    //    right after drawing (0x44f2ba). Hence the measured equality
    //    fps == sim/s, frame for frame. This neutralizes the JUMP that checks
    //    that flag (0x44f27e `jne 44f2a4` -> two NOPs): drawing is then
    //    attempted on every turn of the wait loop 0x451bb0, while SIMULATION
    //    keeps its own pace (gate 0x52fc20 governs that, not this flag).
    //    ⚠️ WHAT THIS MEANS. Diablo II does no interpolation between two
    //    steps: the extra frames redraw the SAME state. So this is not
    //    "smoother", it's a RENDER THROUGHPUT bench at unchanged game speed —
    //    exactly what's needed for a drawing-path optimization to show up in fps.
    // ⚠️ Gated on ms<40: at 40 ms the knob is requesting THE ORIGINAL RATE, and
    //    that case must be a perfect identity — which is what the pixel oracle
    //    checks. Neutralizing the lock at 40 ms would change the frame count
    //    for the same play duration and make that check impossible.
    if(g_nocapGate && g_nocapVal<40){
        uint8_t cur[2]={0,0}; g_nocapCpu->read(g_nocapGate,cur,2);
        if(cur[0]==0x75&&cur[1]==0x24){
            uint8_t nop[2]={0x90,0x90}; g_nocapCpu->write(g_nocapGate,nop,2);
            std::snprintf(m,sizeof m,"nocap: en jeu — Game+0x4f27e `jne` du verrou de dessin -> 2 NOP (un dessin par tour, simulation INTACTE)");
        } else
            std::snprintf(m,sizeof m,"nocap: REFUS verrou de dessin — octets inattendus a Game+0x4f27e (%02x %02x)",cur[0],cur[1]);
        d2vita_progress(m); std::printf("%s\n",m);
    }
    std::fflush(stdout);
}

bool lw_on(){ if(!g_lwArmed){ g_lwArmed=true; g_lwOn=getenv("D2_LOOPWATCH")!=nullptr; } return g_lwOn; }
uint32_t lw_tid(){ return (g_sched&&g_sched->current())?g_sched->current()->id:0xFFFFFFFEu; }
static void lw_site(LwSite* tab, uint32_t eip){
    for(int i=0;i<8;i++){ if(tab[i].eip==eip){ ++tab[i].n; return; } if(!tab[i].eip){ tab[i].eip=eip; tab[i].n=1; return; } }
}
// "Game+0x... (n)" for a table's two most frequent sites.
static void lw_top2(const LwSite* tab, char* out, size_t n){
    int a=-1,b=-1;
    for(int i=0;i<8;i++){ if(!tab[i].eip) continue;
        if(a<0||tab[i].n>tab[a].n){ b=a; a=i; } else if(b<0||tab[i].n>tab[b].n) b=i; }
    if(a<0){ std::snprintf(out,n,"-"); return; }
    auto rel=[](uint32_t e){ return (unsigned)(e>=g_d2base?e-g_d2base:e); };
    if(b<0) std::snprintf(out,n,"Game+0x%x(%llu)",rel(tab[a].eip),(unsigned long long)tab[a].n);
    else    std::snprintf(out,n,"Game+0x%x(%llu) Game+0x%x(%llu)",rel(tab[a].eip),(unsigned long long)tab[a].n,
                          rel(tab[b].eip),(unsigned long long)tab[b].n);
}
// Empty pump from the render thread: bounds the frame's wait interval.
void lw_peek_empty(Cpu& c){
    if(!lw_on() || lw_tid()!=g_lwTid) return;
    const uint64_t now=rt_now_us();
    if(!g_lwFirstUs) g_lwFirstUs=now;
    g_lwLastUs=now; ++g_lwSpins;
    lw_site(g_lwSites,c.read_u32(c.reg(R_ESP)));
}
void lw_count(uint64_t& ctr){ if(lw_on() && lw_tid()==g_lwTid) ++ctr; }
// D2_NETSLEEP=<N>: number of FREE Sleep(0) calls per 20 ms slice inside the
// packet loop (0 = knob off). Beyond N, Sleep(0) takes the real path.
// Why a BOUND instead of "always free": making every Sleep(0) free made the
// game loop — for which this is the ONLY rest point in-game — spin many times
// more per frame, and time attributed to "other" work rose sharply: removing
// the cost also removed the yield point that kept the loop honest. N covers
// genuine packet processing; the (N+1)th call yields control for real.
int netsleep_n(){ static int v=-1; if(v<0){ const char* e=getenv("D2_NETSLEEP"); v=(e&&*e)?atoi(e):0; if(v<0) v=0; } return v; }
bool netsleep_on(){ return netsleep_n()>0; }
bool gamesleep_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GAMESLEEP"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
uint64_t g_gameSleepN=0, g_gameSleepMs=0;
uint64_t g_netSleepN=0, g_netSleepFull=0;   // packet-loop Sleep(0) calls served without a kernel call
bool fogsleep_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_FOGSLEEP"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
uint64_t g_fogSleepN=0;   // Fog Sleep(0) calls converted into a credit-based sleep (windowed)
void lw_sleep(Cpu& c){ if(!lw_on()||lw_tid()!=g_lwTid) return; ++g_lwSleepN; lw_site(g_lwSlSites,c.read_u32(c.reg(R_ESP))); }
// D2_ONLINE_CAP: ONLINE-only target frame rate, engine-side (no Game.exe byte
// touched, like everything else on the shipped path). Over the network D2 draws AND simulates at ~35/s instead of the
// exact 25/s seen solo. Solo is already correct: in-game D2 draws EXACTLY one
// frame per simulation step, gated by lock 0x52fc20 read from REAL elapsed
// time. Online, that same lock doesn't throttle the way it does solo — this
// is original Battle.net/TCP-IP behavior, not something introduced by this
// port — so this recovers the solo pace rather than letting it run free.
// MECHANISM: injecting the missing REAL time BEFORE D2 rereads its clock (at
// the frame boundary, grBufferSwap) makes the game's OWN lock do the work —
// no guest memory is written. Default: active ONLY if D2NET is armed (solo is
// never touched). D2_ONLINE_CAP=0 disarms it (bench/diagnostic);
// D2_ONLINE_CAP=<hz> (1..120) picks a different rate.
uint32_t online_cap_period_us(){
    static int v=-1;
    if(v<0){ const char* e=getenv("D2_ONLINE_CAP");
        if(e && *e){ long hz=strtol(e,nullptr,10);
            v = (hz<=0) ? 0 : (int)(1000000L/((hz>120)?120:(int)hz)); }
        else v = getenv("D2NET") ? (1000000/25) : 0;
        if(v) d2vita_progress("online-cap: arme (cadence ciblee 25 Hz, cote moteur, aucun octet du jeu touche)"); }
    return (uint32_t)v;
}
// Waits, with the GIL released, until `per` microseconds have elapsed since
// the last call (first call: arms silently, does not sleep). Same pattern as
// D2_GAMESLEEP (an event that's never signaled, bounded g_sched->wait).
// WxEvent (not yet the KEvent alias, defined further below in this file): this
// site comes before `using KEvent = WxEvent;`.
void online_cap_wait(uint32_t per){
    if(!per) return;
    static uint64_t lastUs=0;
    const uint64_t now=rt_now_us();
    if(lastUs){
        const uint64_t elapsed=now-lastUs;
        if(elapsed<per){
            const uint32_t waitMs=(uint32_t)((per-elapsed+999)/1000);
            if(waitMs && g_sched){ static WxEvent* capEv=new WxEvent(); capEv->manual=true; capEv->signaled=false;
                g_sched->wait(capEv,waitMs); }
        }
    }
    lastUs=rt_now_us();
}
// Frame boundary: closes the wait interval and publishes a line every 10 s,
// at the same cadence as "frames: fps=" (same window, so the two lines read
// together).
void lw_frame(Cpu& c){
    if(!lw_on()) return;
    const uint64_t now=rt_now_us();
    const uint32_t me=lw_tid();
    if(g_lwTid==0xFFFFFFFFu){ g_lwTid=me; g_lwWinT0=now;
        jpline("rendu: fil de presentation = tid %u", me); }
    if(me==g_lwTid){
        ++g_lwFrames;
        if(g_lwFirstUs && g_lwLastUs>g_lwFirstUs){ g_lwWaitUs += g_lwLastUs-g_lwFirstUs;
                                                    g_lwWaitTot += g_lwLastUs-g_lwFirstUs; }
        g_lwSpinN += g_lwSpins;
    }
    g_lwFirstUs=0; g_lwLastUs=0; g_lwSpins=0;
    if(now-g_lwWinT0 >= 10000000ull){
        const uint64_t F = g_lwFrames?g_lwFrames:1;
        uint32_t best=0; uint64_t bn=0;
        for(auto& st:g_lwSites) if(st.n>bn){ bn=st.n; best=st.eip; }
        char pmp[128], slp[128]; lw_top2(g_lwSites,pmp,sizeof pmp); lw_top2(g_lwSlSites,slp,sizeof slp);
        const uint64_t winUs=now-g_lwWinT0;
        const uint64_t dSim=g_lwSimN-g_lwSimWin; g_lwSimWin=g_lwSimN;
        if(waitprof_on()){
            // Averages alone lie (one legitimate INFINITE wait drowns
            // everything): finite waits, the infinite ones, and the WORST case
            // with its thread are published SEPARATELY. An average overshoot
            // near zero clears the wake-up path; a systematic overshoot indicts it.
            const WaitProf W=g_wpW, S=g_wpS;
            jpline("attentes(10s): WFSO fini n=%llu demande=%llums ecoule=%llums DEPASSEMENT=%llums (pire %ums->%ums fil %u)"
                   " | WFSO infini n=%llu ecoule=%llums | Sleep n=%llu demande=%llums ecoule=%llums DEPASSEMENT=%llums (pire %ums->%ums fil %u)",
                   (unsigned long long)W.n,(unsigned long long)(W.req_us/1000),(unsigned long long)(W.act_us/1000),
                   (unsigned long long)(W.over_us/1000),W.wreq,W.wact,W.wtid,
                   (unsigned long long)g_wpWinf_n,(unsigned long long)(g_wpWinf_us/1000),
                   (unsigned long long)S.n,(unsigned long long)(S.req_us/1000),(unsigned long long)(S.act_us/1000),
                   (unsigned long long)(S.over_us/1000),S.wreq,S.wact,S.wtid);
            if(g_slowSamp){
                int idx[6]; uint32_t best[6]={0};
                for(int k=0;k<6;k++){ idx[k]=-1;
                    for(int i=0;i<128;i++){ bool used=false;
                        for(int j=0;j<k;j++) if(idx[j]==i) used=true;
                        if(!used && g_slowTab[i].eip && g_slowTab[i].n>best[k]){ best[k]=g_slowTab[i].n; idx[k]=i; } } }
                char sb2[240]; int w2=0;
                w2+=std::snprintf(sb2+w2,sizeof sb2-w2,"temps/images-lentes(10s): images=%llu echantillons=%llu ",
                                  (unsigned long long)g_slowFrames,(unsigned long long)g_slowSamp);
                for(int k=0;k<6 && idx[k]>=0;k++)
                    w2+=std::snprintf(sb2+w2,(size_t)w2<sizeof sb2?sizeof sb2-w2:0,"%s%x=%llu%%",k?" ":"",
                                      (unsigned)(g_slowTab[idx[k]].eip-d2rt_timeprof_base),
                                      (unsigned long long)(g_slowTab[idx[k]].n*100ull/g_slowSamp));
                jpline("%s",sb2);
                for(int i=0;i<128;i++) g_slowTab[i]=SlowEnt{};
                g_slowSamp=0; g_slowFrames=0;
            }
            { char hb[220]; int w=0; uint64_t tot=0;
              for(int i=0;i<13;i++) tot+=g_fpWinHist[i];
              w+=std::snprintf(hb+w,sizeof hb-w,"images/duree(10s): ");
              for(int i=0;i<13;i++) if(g_fpWinHist[i])
                  w+=std::snprintf(hb+w,(size_t)w<sizeof hb?sizeof hb-w:0,
                                   "%s%s=%llu", i?" ":"",
                                   i==12?">=120ms":({ static char lb[12]; std::snprintf(lb,sizeof lb,"%d-%dms",i*10,i*10+9); lb; }),
                                   (unsigned long long)g_fpWinHist[i]);
              if(tot) jpline("%s", hb);
              for(int i=0;i<13;i++) g_fpWinHist[i]=0; }
            if(d2rt_wakeprof){
                // PURE scheduling latency: from pthread_cond_signal to the
                // thread actually resuming (which must reacquire the GIL). Mixes
                // in NEITHER the game's own polling rate NOR the worker's work —
                // unlike "signaled->seen".
                jpline("reveil(10s): mesures=%llu moy=%lluus pire=%llums | brut: signaux emis=%llu | sondages-delai-0 servis sans dormir=%llu",
                       (unsigned long long)d2rt_wake_n,
                       (unsigned long long)(d2rt_wake_n?d2rt_wake_us/d2rt_wake_n:0),
                       (unsigned long long)(d2rt_wake_worst/1000),(unsigned long long)d2rt_wake_sig,(unsigned long long)d2rt_poll0_n);
                d2rt_wake_n=0; d2rt_wake_us=0; d2rt_wake_worst=0; d2rt_wake_sig=0; d2rt_poll0_n=0;
            }
            if(asyncprof_on()){ const AsyncProf W=g_apWork, S=g_apSee;
                jpline("async(10s): demande->signalement n=%llu moy=%lluus pire=%llums"
                       " | signalement->vu n=%llu moy=%lluus pire=%llums | brut: SetEvent=%llu ResetEvent=%llu",
                       (unsigned long long)W.n,(unsigned long long)(W.n?W.us/W.n:0),(unsigned long long)(W.worst/1000),
                       (unsigned long long)S.n,(unsigned long long)(S.n?S.us/S.n:0),(unsigned long long)(S.worst/1000),
                       (unsigned long long)g_apSetN,(unsigned long long)g_apResetN);
                g_apWork=AsyncProf{}; g_apSee=AsyncProf{}; g_apSetN=0; g_apResetN=0; }
            { char sb[200]; wp_sites_fmt(sb,sizeof sb);
              jpline("attentes/sites(10s): WFSO appele depuis %s", sb); }
            g_wpW=WaitProf{}; g_wpS=WaitProf{}; g_wpWinf_n=0; g_wpWinf_us=0;
            for(int i=0;i<8;i++) g_wpSites[i]=WSite{};
        }
        jpline("rendu: img=%llu (%llu.%llu/s) attente=%lluus/img spins=%llu/img sleep=%llu/img wfso=%llu/img tick=%llu/img pompe=Game+0x%x(%llu) sim=%llu (%llu.%llu/s)",
               (unsigned long long)g_lwFrames,
               (unsigned long long)(g_lwFrames*10000000ull/winUs)/10ull,
               (unsigned long long)(g_lwFrames*10000000ull/winUs)%10ull,
               (unsigned long long)(g_lwWaitUs/F), (unsigned long long)(g_lwSpinN/F),
               (unsigned long long)(g_lwSleepN/F), (unsigned long long)(g_lwWfsoN/F),
               (unsigned long long)(g_lwTickN/F),
               (unsigned)(best>=g_d2base?best-g_d2base:best), (unsigned long long)bn,
               (unsigned long long)dSim,
               (unsigned long long)(dSim*10000000ull/winUs)/10ull,
               (unsigned long long)(dSim*10000000ull/winUs)%10ull);
        // Game counters (loop 0x44ef..) read directly from guest memory.
        if(g_d2base){
            const uint32_t drw=c.read_u32(g_d2base+0x003a0494u);   // 0x7a0494 frames drawn
            const uint32_t skp=c.read_u32(g_d2base+0x003a04b4u);   // 0x7a04b4 frames skipped
            const uint32_t per=c.read_u32(g_d2base+0x0030ef1cu);   // 0x70ef1c render period
            const uint32_t sper=c.read_u32(g_d2base+0x00483d60u);  // 0x883d60 simulation period
            // Both game counters are reset to zero by certain screen changes: a
            // bare subtraction would give a negative delta read as %u (4
            // billion). Clamped to zero.
        if(gamesleep_on()){ char gb[112]; std::snprintf(gb,sizeof gb,"gamesleep: credits dormis=%llu (%llu ms au total)",(unsigned long long)g_gameSleepN,(unsigned long long)g_gameSleepMs); jpline(gb); }
        if(netsleep_on()){ char nb[96]; std::snprintf(nb,sizeof nb,"netsleep: gratuits=%llu (budget %d/img) vrais=%llu",(unsigned long long)g_netSleepN,netsleep_n(),(unsigned long long)g_netSleepFull); jpline(nb); }
        if(fogsleep_on()){ char fb[96]; std::snprintf(fb,sizeof fb,"fogsleep: Sleep(0) de Fog dormis=%llu (credit borne 20 ms)",(unsigned long long)g_fogSleepN); jpline(fb); }
            jpline("jeu: dessinees=%u/fen sautees=%u/fen periode-rendu=%ums periode-sim=%ums | pompe:%s | sleep:%s",
                   drw>g_lwDrawPrev?drw-g_lwDrawPrev:0u, skp>g_lwSkipPrev?skp-g_lwSkipPrev:0u,
                   per, sper, pmp, slp);
            g_lwDrawPrev=drw; g_lwSkipPrev=skp;
        }
        g_lwWinT0=now; g_lwFrames=0; g_lwWaitUs=0; g_lwSpinN=0;
        g_lwSleepN=0; g_lwWfsoN=0; g_lwTickN=0;
        for(auto& st:g_lwSites){ st.eip=0; st.n=0; }
        for(auto& st:g_lwSlSites){ st.eip=0; st.n=0; }
    }
}

// D2_PHASEPROF / D2_ONEDRAW: state, pp_enter/pp_exit, pp_flush_begin/end,
// pp_frame -> src/runtime/phase_hooks.cpp.

// ====== D2_FRAMEPROF: PER-FRAME attribution of fps drops ====================
// A 10 s average cannot see a one-second drop: 20 fps falling to 5 fps for
// 1.5 s reads as "18.5 fps" to the watchdog. This measures EVERY presented
// frame and keeps the slowest ones together with what happened during them —
// translated blocks, JIT us, cache-sync us, file reads, decompressions,
// scheduling slices.
//
// Cost: one clock read and about ten counter reads per frame, ~20 times a
// second. Nothing in the emitted ARM code, nothing per block. One log line
// every 10 s (same cadence as the watchdog), plus the full table at exit.
// Without D2_FRAMEPROF, all of this is dead code.
extern "C" uint64_t d2rt_gx_flush_us;   // timer for d2vGlideFlush (gx_host.cpp)
// Dynarec counters also read by runtime/jit_profile.cpp (D2_JITPROFILE) --
// duplicate extern declarations, same underlying dynarec globals.
extern "C" { uint64_t dyn86_jp_now_us(void);
             extern uint64_t dyn86_jp_created, dyn86_jp_run_us, dyn86_jp_lookup_calls;
#ifdef __vita__
             extern uint64_t dyn86_sync_us;
#endif
}
#ifndef __vita__
static const uint64_t dyn86_sync_us = 0;
#endif
extern "C" uint64_t dyn86_fill_ns;   // also declared in tools/rt_boot.cpp
extern "C" uint64_t d2rt_eipprof[8]; // guest-code family breakdown (D2_EIPPROF); defined in tools/rt_boot.cpp
struct FpCnt { uint64_t t,blocks,jit_us,sync_us,run_us,reads,scomp,sw,look;  uint64_t io_us, gx_us, att_us; };
struct FpRec { uint32_t frame; uint32_t dt_us,blocks,jit_us,sync_us,run_us,reads,scomp,sw,look,io_us,gx_us,att_us;
    // D2_LAGWATCH: GUEST-side attribution of the slow frame. `ech` = EIP
    // samples that fell inside this frame; ip1/n1..ip3/n3 = the three dominant
    // addresses. ech==0 reads as "no samples" — either the frame was too
    // short, or the freeze happened INSIDE a single dynablock (an inner loop
    // is never sampled).
    uint32_t ech, ip1,n1, ip2,n2, ip3,n3; };
static bool     g_fpOn = false;
static int      g_fpArmed = 0;
static FpCnt    g_fpPrev{};
static FpRec    g_fpTop[24]{};
static int      g_fpTopN = 0;
// PER-WINDOW top (D2_LAGWATCH). The GLOBAL top is useless for finding a freeze
// mid-game: level loads put 1-3 second frames in it that never fall back out,
// and a 150 ms in-combat stutter never makes the cut. A second ranking is
// therefore kept, RESET on every publication: each window shows its own worst
// frames regardless of what happened before. The global top remains, but only
// for the final report.
static FpRec    g_fpTopW[8]{};
static int      g_fpTopWN = 0;
static uint64_t g_fpHist[6] = {0,0,0,0,0,0};   // <50 / <100 / <250 / <500 / <1000 / >=1000 ms
static uint64_t g_fpFrames = 0, g_fpSumUs = 0;
static uint64_t g_fpWinT0 = 0; static uint32_t g_fpWinFrames = 0;
static FpRec    g_fpWinWorst{};
static FpCnt    g_fpWinBase{};

static FpCnt fp_now(){
    FpCnt c;
    c.t       = dyn86_jp_now_us();
    c.blocks  = dyn86_jp_created;
    c.jit_us  = dyn86_fill_ns/1000ull;
    c.sync_us = dyn86_sync_us;
    c.run_us  = dyn86_jp_run_us;
    c.reads   = g_readN;
    c.scomp   = g_scompN + g_scomp2N;
    c.sw      = g_schedCoop ? g_schedCoop->switches() : 0ull;
    c.look    = dyn86_jp_lookup_calls;
    // TIME spent in I/O, not the NUMBER of reads. A frame with a handful of
    // reads can still be entirely explained by I/O if each read costs tens of
    // ms (a cold card access + a 128 KiB D2_READAHEAD fill). Counting reads
    // says nothing; timing them settles it.
    c.io_us   = g_ioUs;
    c.gx_us   = d2rt_gx_flush_us;
    c.att_us  = g_lwWaitTot;
    return c;
}
static FpRec fp_delta(uint32_t frame, const FpCnt& a, const FpCnt& b){
    // INITIALIZE EVERYTHING. The guest-attribution fields (ech, ip1..n3) are
    // only filled in by lag_attrib(), which is only called under D2_LAGWATCH.
    // Without this zeroing, a pass without the knob prints uninitialized
    // stack garbage instead, e.g. "guest ech=2169652880 81401a9c=32317389
    // 000000=2170870828" — HOST addresses and multi-billion counters in a
    // diagnostic log, enough to send someone chasing a bug that doesn't exist.
    FpRec r{}; r.frame = frame;
    r.dt_us  = (uint32_t)(b.t - a.t);           r.blocks = (uint32_t)(b.blocks - a.blocks);
    r.jit_us = (uint32_t)(b.jit_us - a.jit_us); r.sync_us= (uint32_t)(b.sync_us- a.sync_us);
    r.run_us = (uint32_t)(b.run_us - a.run_us); r.reads  = (uint32_t)(b.reads  - a.reads);
    r.scomp  = (uint32_t)(b.scomp  - a.scomp);  r.sw     = (uint32_t)(b.sw     - a.sw);
    r.look   = (uint32_t)(b.look   - a.look);
    r.io_us  = (uint32_t)(b.io_us  - a.io_us);
    r.gx_us  = (uint32_t)(b.gx_us  - a.gx_us);
    r.att_us = (uint32_t)(b.att_us - a.att_us);
    return r;
}
// ---- D2_LAGWATCH: attribute a slow frame to GUEST code ---------------------
// Rereads the ring fed by the sampler (cpu_box86.cpp) and keeps the three most
// frequent addresses seen in [t0, t1]. Called ONLY when a frame exceeds the
// threshold: a normal session pays nothing here.
extern "C" {
    extern uint32_t d2rt_lag_on, d2rt_lag_w;
    extern uint64_t d2rt_lag_t[]; extern uint32_t d2rt_lag_ip[];
    extern uint32_t d2rt_timeprof_base;
}
#define LAGRING 2048u
static void lag_attrib(FpRec& r, uint64_t t0, uint64_t t1){
    r.ech = r.ip1 = r.n1 = r.ip2 = r.n2 = r.ip3 = r.n3 = 0;
    if(!d2rt_lag_on) return;
    // Small counting table: at most 32 distinct addresses kept, which is
    // plenty (a freeze is rarely spread out) and bounds the cost.
    uint32_t k[32]; uint32_t c[32]; int n = 0;
    const uint32_t w = d2rt_lag_w;
    const uint32_t first = (w > LAGRING) ? (w - LAGRING) : 0u;
    for(uint32_t i = first; i < w; ++i){
        const uint32_t s = i % LAGRING;
        const uint64_t t = d2rt_lag_t[s];
        if(t < t0 || t > t1) continue;
        const uint32_t ip = d2rt_lag_ip[s];
        ++r.ech;
        int j = 0; for(; j < n; ++j) if(k[j] == ip) { ++c[j]; break; }
        if(j == n && n < 32){ k[n] = ip; c[n] = 1; ++n; }
    }
    for(int pass = 0; pass < 3; ++pass){
        int bi = -1; uint32_t bv = 0;
        for(int i = 0; i < n; ++i) if(c[i] > bv){ bv = c[i]; bi = i; }
        if(bi < 0 || !bv) break;
        const uint32_t rva = k[bi] - d2rt_timeprof_base;
        if(pass == 0){ r.ip1 = rva; r.n1 = bv; }
        else if(pass == 1){ r.ip2 = rva; r.n2 = bv; }
        else { r.ip3 = rva; r.n3 = bv; }
        c[bi] = 0;
    }
}
// Insert into a bounded descending ranking. Returns 1 if `r` was inserted.
static int fp_ins(FpRec* top, int cap, int& cnt, const FpRec& r){
    int n = cnt;
    if(n == cap){
        if(r.dt_us <= top[n-1].dt_us) return 0;
        n--;                                  // the fastest of the slow ones drops out
    }
    int i = n;
    while(i > 0 && top[i-1].dt_us < r.dt_us){ top[i] = top[i-1]; --i; }
    top[i] = r;
    if(cnt < cap) ++cnt;
    return 1;
}
static void fp_top_insert(const FpRec& r){
    fp_ins(g_fpTop,  (int)(sizeof g_fpTop /sizeof g_fpTop[0]),  g_fpTopN,  r);
    fp_ins(g_fpTopW, (int)(sizeof g_fpTopW/sizeof g_fpTopW[0]), g_fpTopWN, r);
}
// The N slowest frames, as log lines. Emitted periodically in addition to
// the final dump: a play session often ends with the app being killed
// outright, so the final table would never arrive.
static void fp_top_lines_of(const FpRec* top, int have, int n, const char* tag){
    char m[224];
    if(n > have) n = have;
    if(!n) return;
    std::snprintf(m,sizeof m,"--- images les plus lentes (%s, %llu images vues) ---",
                  tag,(unsigned long long)g_fpFrames);
    d2vita_progress(m); std::printf("%s\n",m);
    for(int i=0;i<n;i++){
        const FpRec& r = top[i];
        std::snprintf(m,sizeof m,
            "lente#%02d f%u %ums | io=%ums gx=%ums attente-invitee=%ums INEXPLIQUE=%dms | blocs=%u jit=%ums sync=%ums run=%ums reads=%u scomp=%u sw=%u look=%u"
            " | invite ech=%u %06x=%u %06x=%u %06x=%u",
            i+1, r.frame, r.dt_us/1000u, r.io_us/1000u, r.gx_us/1000u, r.att_us/1000u,
            (int)((long long)r.dt_us - r.io_us - r.gx_us - r.att_us - r.jit_us - r.sync_us)/1000,
            r.blocks, r.jit_us/1000u, r.sync_us/1000u,
            r.run_us/1000u, r.reads, r.scomp, r.sw, r.look,
            r.ech, r.ip1, r.n1, r.ip2, r.n2, r.ip3, r.n3);
        d2vita_progress(m); std::printf("%s\n",m);
    }
    std::fflush(stdout);
}
static void fp_top_lines(int n, const char* tag){ fp_top_lines_of(g_fpTop, g_fpTopN, n, tag); }
// Publishes the worst frames OF THE WINDOW, then clears it. This is the
// ranking to read when looking for a mid-game freeze: it holds no memory of
// loads from minutes earlier.
static void fp_top_window(){
    if(!g_fpTopWN) return;                    // no slow frame in the window: silent
    fp_top_lines_of(g_fpTopW, g_fpTopWN, g_fpTopWN, "fenetre");
    g_fpTopWN = 0;
    for(size_t i=0;i<sizeof g_fpTopW/sizeof g_fpTopW[0];++i) g_fpTopW[i] = FpRec{};
}
static uint64_t g_fpTopT0 = 0;
uint32_t g_lagMs = 0;      // D2_LAGWATCH=<ms> : seuil d'attribution
static uint64_t g_fpPrevT = 0;
// ---- STAGE 2 of the frame profile: where does time go WITHIN a frame? -----
// The "frames:" line above says HOW MUCH a frame costs. This says ON WHAT.
// Two sources, both windowed over the last 10 s (a cumulative total since
// boot would say nothing about the moving phase, which starts minutes after
// the menu):
//   * PROF_COUNTERS: run_ns covers cpu->run() (dynarec + shim bodies executed
//     inside it), trap_ns covers only the shim bodies. So pure dynarec =
//     run - trap, and the wall-clock remainder (present, scheduler, host) =
//     wall - run. The same instrument gives the top shims by time, where our
//     native ports show up NAMED.
//   * D2_EIPPROF: the breakdown of GUEST code by family.
// Everything goes through d2vita_progress: on console, printf reaches no log
// at all. The cost of the line itself is one fopen every 10 s.
d2rt::Bridge* g_profBr = nullptr;      // set in main, right after Bridge br(cpu)
static void pc_line(uint32_t frames, uint64_t wall_us){
#ifdef PROF_COUNTERS
    if(!frames || !wall_us) return;
    static uint64_t pr=0, pt=0;
    const uint64_t run=d2rt::prof::run_ns, trp=d2rt::prof::trap_ns;
    const uint64_t dRun=(run>pr?run-pr:0), dTrp=(trp>pt?trp-pt:0); pr=run; pt=trp;
    const uint64_t wall_ns=wall_us*1000ull;
    const uint64_t dyn = dRun>dTrp? dRun-dTrp : 0;
    const uint64_t hors = wall_ns>dRun? wall_ns-dRun : 0;
    // tenths of a millisecond per frame
    auto dm=[frames](uint64_t ns){ return (unsigned)(ns/frames/100000ull); };
    char m[224];
    std::snprintf(m,sizeof m,
        "budget: trame=%ums | dynarec=%u.%u shim=%u.%u hors-run=%u.%u (ms/img)",
        (unsigned)(wall_us/frames/1000u),
        dm(dyn)/10u,dm(dyn)%10u, dm(dTrp)/10u,dm(dTrp)%10u, dm(hors)/10u,dm(hors)%10u);
    d2vita_progress(m); std::printf("[%s]\n",m);
    if(g_profBr){ char top[256];
        if(g_profBr->prof_window(top,sizeof top,6)>0){
            std::snprintf(m,sizeof m,"budget/shims:%s",top);
            d2vita_progress(m); std::printf("[%s]\n",m); } }
    std::fflush(stdout);
#else
    (void)frames; (void)wall_us;
#endif
}
// gr_line() (at the call site, right after ep_line()) is the real diagnostic,
// always active.
// Breakdown of GUEST code over the window, if D2_EIPPROF is armed. Without
// it, silent — the counters stay at zero and tot is 0.
static void ep_line(){
    static uint64_t prev[7]={0,0,0,0,0,0,0};
    static const char* nm[7]={"Codec","Sprite","Tilecmp","DRLG","Gfx","Storm","autre"};
    uint64_t tot=0, d[7];
    for(int i=0;i<7;i++){ const uint64_t c=d2rt_eipprof[i];
        d[i] = c>prev[i]? c-prev[i] : 0; prev[i]=c; tot+=d[i]; }
    if(!tot) return;
    char m[224]; int off=std::snprintf(m,sizeof m,"invite: ech=%llu",(unsigned long long)tot);
    for(int i=0;i<7 && off>0 && off<(int)sizeof m-24;i++) if(d[i])
        off+=std::snprintf(m+off,sizeof m-(size_t)off," %s=%llu%%",nm[i],
                           (unsigned long long)(d[i]*100ull/tot));
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
}
void fp_tick(int frame){
    if(!g_fpArmed){ g_fpArmed = 1;
        g_fpOn = getenv("WX86_FRAMEPROF")!=nullptr || getenv("D2_FRAMEPROF")!=nullptr;
        if(g_fpOn){ g_fpPrev = fp_now(); g_fpWinBase = g_fpPrev; g_fpWinT0 = g_fpPrev.t;
                    g_fpPrevT = g_fpPrev.t; } }
    if(!g_fpOn) return;
    FpCnt cur = fp_now();
    FpRec r = fp_delta((uint32_t)frame, g_fpPrev, cur);
    const uint64_t tPrev = g_fpPrev.t;
    g_fpPrev = cur;
    (void)tPrev;
    ++g_fpFrames; g_fpSumUs += r.dt_us;
    uint32_t ms = r.dt_us/1000u;
    g_fpHist[ ms<50?0 : ms<100?1 : ms<250?2 : ms<500?3 : ms<1000?4 : 5 ]++;
    { const unsigned b = ms>=120 ? 12u : (unsigned)(ms/10); ++g_fpWinHist[b]; }
    if(g_lagMs && ms >= g_lagMs){ lag_attrib(r, g_fpPrevT, cur.t); slow_collect(g_fpPrevT, cur.t); }
    g_fpPrevT = cur.t;
    fp_top_insert(r);
    ++g_fpWinFrames;
    if(r.dt_us > g_fpWinWorst.dt_us) g_fpWinWorst = r;
    if(cur.t - g_fpWinT0 >= 10000000ull){       // one line every 10 s
        FpRec w = fp_delta((uint32_t)frame, g_fpWinBase, cur);
        char m[224];
        std::snprintf(m,sizeof m,
            "frames: n=%u fps=%u.%u pire=%ums@f%u (blocs=%u jit=%ums sync=%ums reads=%u scomp=%u sw=%u) | fen: blocs=%u jit=%ums reads=%u",
            g_fpWinFrames,
            (unsigned)((uint64_t)g_fpWinFrames*10000000ull/(cur.t-g_fpWinT0))/10u,
            (unsigned)((uint64_t)g_fpWinFrames*10000000ull/(cur.t-g_fpWinT0))%10u,
            g_fpWinWorst.dt_us/1000u, g_fpWinWorst.frame, g_fpWinWorst.blocks,
            g_fpWinWorst.jit_us/1000u, g_fpWinWorst.sync_us/1000u, g_fpWinWorst.reads,
            g_fpWinWorst.scomp, g_fpWinWorst.sw,
            w.blocks, w.jit_us/1000u, w.reads);
        d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
        pc_line(g_fpWinFrames, cur.t - g_fpWinT0);   // where the time goes (PROF_COUNTERS)
        ep_line();                                   // guest code breakdown
        gr_line(g_fpWinFrames);                      // ring buffer (silent without D2_GLIDERING)
        g_fpWinT0 = cur.t; g_fpWinFrames = 0; g_fpWinWorst = FpRec{}; g_fpWinBase = cur;
    }
    if(!g_fpTopT0) g_fpTopT0 = cur.t;
    else if(cur.t - g_fpTopT0 >= 30000000ull){   // 30 s sliding window
        g_fpTopT0 = cur.t;
        fp_top_window();                         // silent if nothing happened
    }
}
void fp_dump(){
    if(!g_fpOn || !g_fpFrames) return;
    char m[224];
    std::snprintf(m,sizeof m,"=== FRAME PROFILE : %llu images, moyenne %llums ===",
        (unsigned long long)g_fpFrames,(unsigned long long)(g_fpSumUs/g_fpFrames/1000ull));
    d2vita_progress(m); std::printf("%s\n",m);
    // blocks/jit/sync/reads/scomp are exact (counters incremented inline).
    // run= is APPROXIMATE: guest time is only added at the end of a
    // scheduling slice, and the frame tick happens INSIDE a slice — so
    // attribution can drift by one slice (run>dt is possible).
    std::snprintf(m,sizeof m,"note: blocs/jit/sync/reads/scomp exacts, run= approche (+/- une tranche)");
    d2vita_progress(m); std::printf("%s\n",m);
    std::snprintf(m,sizeof m,"repartition: <50ms=%llu 50-100=%llu 100-250=%llu 250-500=%llu 500-1000=%llu >=1s=%llu",
        (unsigned long long)g_fpHist[0],(unsigned long long)g_fpHist[1],(unsigned long long)g_fpHist[2],
        (unsigned long long)g_fpHist[3],(unsigned long long)g_fpHist[4],(unsigned long long)g_fpHist[5]);
    d2vita_progress(m); std::printf("%s\n",m);
    fp_top_lines(g_fpTopN, "final");
}
