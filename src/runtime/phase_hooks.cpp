// phase_hooks.cpp — PHASE and RING-TAGGING hooks installed on the 1.14d
// monolith: D2_PHASEPROF / D2_ONEDRAW / D2_PHASEPROF_X (per-phase timing, one
// draw per simulation step), the shared hook on the simulation step
// Game+0x12fd90 (also counted by D2_LOOPWATCH), and D2_REPLAY60 / D2_RINGTAG /
// D2_RINGPHASE_X (unit Game+0xdc7b0, camera Game+0x5b440, phases). Pattern:
// `alternate` at function ENTRY, faithful replay of the first instruction,
// exit captured via a return trap, EAX left unchanged.
// D2_LOOPWATCH (counters, shims) and D2_NOCAP stay in rt_boot.cpp; what
// rt_boot.cpp provides is declared in runtime/rt_host.h.
#include "runtime/phase_hooks.h"
#include "glide_ring/gx_host.h"
#include "glide_ring/replay60.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"
#include "runtime/sched_cooperative.h"
#include "runtime/rt_host.h"
#include "runtime/pad_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
using namespace d2rt;

// ====== D2_PHASEPROF: where a frame's ~18ms go, PER PHASE ==========
// An uncapped bench (D2_NOCAP=1) measures throughput but not the split
// between one simulation step and one draw. Host clock (rt_now_us:
// sceKernelGetProcessTimeWide on console, CLOCK_MONOTONIC under qemu)
// accumulates per phase, on the game thread:
//   sim   = Game+0x12fd90 (VA 0x52fd90), the client simulation step (called
//           by the gate at 0x52fc20 once per period [0x883d60]);
//   draw  = the function pointed to by [0x7a0484] (table 0x70ef0c: 0x44c990,
//           0x45fde0, 0x460190 — all three are hooked), called from 0x44f28b
//           / 0x44f017 inside the frame function 0x44efa0;
//   flush = the host side of presentation (GDI StretchBlt; or the Glide ring
//           flush, gr_replay + GXM submit). Nested inside draw, so "draw" is
//           reported net of flush;
//   tour  = the frame function 0x44efa0 itself (ret 4), called on each turn
//           of the wait loop 0x451bb0 (`push esi ; call ebx`);
//   other = frame − sim − draw − flush (message pump, Fog, waits, per-frame
//           host overhead).
// MECHANISM: `alternate` hook at the ENTRY of each function (never a jump
// target — block fusion would make the hook silent), faithful fallback that
// replays the first instruction; exit is captured by overwriting the return
// address on the stack with a dedicated trap slot, which hands control back
// to the original address with EAX UNCHANGED. The dynarec exits on any EIP
// inside the trap window (same mechanism as the thread-exit sentinel), so a
// `ret` to the slot is seen. A slot already busy (reentrancy, another
// thread) is not timed and is counted under "refus=".
// COST: each entry and exit is a trap (console baseline: 1.29us per
// cross-module call) plus a clock read (measured at arm time, reported as
// "horloge=...ns"). Hook count per frame is also reported.
// Without D2_PHASEPROF: nothing is installed, just a boolean check at the boundaries.
//
// D2_ONEDRAW=1|2: ONE DRAW PER SIMULATION STEP. D2 does no interpolation:
// under D2_NOCAP, extra draws just redraw the same state. The draw entry
// hook short-circuits (emulates `ret`) any draw whose simulation step
// already got its draw; mode 2 additionally sleeps the thread for
// D2_ONEDRAW_US (default 1000us) on each skip, so the freed time becomes
// real idle time instead of a busy-loop turn. Input pump (0x451bb0),
// networking, and the simulation gate are untouched. Outside gameplay (no
// step in the last 200ms) every draw goes through, since menus have no
// simulation step.
static bool     g_ppOn=false, g_ppArmed=false;
static int      g_odMode=0;            // D2_ONEDRAW: 0 off, 1 skip-draw, 2 skip-draw + sleep
static uint32_t g_odSleepUs=1000;      // D2_ONEDRAW_US
static bool     g_odArmed=false;
bool pp_on(){ if(!g_ppArmed){ g_ppArmed=true; g_ppOn=getenv("D2_PHASEPROF")!=nullptr; } return g_ppOn; }
int  od_mode(){
    if(!g_odArmed){ g_odArmed=true;
        // Default is mode 1; D2_ONEDRAW still selects mode 2 (skip-draw + sleep).
        g_odMode = 1;
        if(const char* e=getenv("D2_ONEDRAW")){ g_odMode=atoi(e); if(g_odMode<0||g_odMode>2) g_odMode=1; }
        if(const char* e=getenv("D2_ONEDRAW_US")){ long v=atol(e); if(v>=0&&v<=40000) g_odSleepUs=(uint32_t)v; } }
    return g_odMode; }
enum PpKind { PP_SIM=0, PP_DRAW=1, PP_TOUR=2, PP_X0=3, PP_XN=8, PP_N=PP_X0+PP_XN };   // PP_X0..: D2_PHASEPROF_X extra slots
struct PpSlot { bool busy=false; uint32_t tid=0; uint32_t ret=0; uint64_t t0=0; };
static PpSlot   g_ppSlot[PP_N];
static uint64_t g_ppUs[PP_N]={0}, g_ppCnt[PP_N]={0};
static uint32_t g_ppXRva[PP_XN]={0}; static int g_ppXN=0;   // D2_PHASEPROF_X: extra functions being timed
static uint64_t g_ppIdlePrev=0;
static uint64_t g_ppFlushUs=0, g_ppFlushN=0, g_ppFlushHors=0, g_ppFlushT0=0;
static uint64_t g_ppHooks=0, g_ppRefus=0, g_ppPerdus=0;   // hooks fired; entries not timed; slots abandoned (exit never seen)
static uint64_t g_ppFrames=0, g_ppWinT0=0, g_ppClockNs=0;
static uint64_t g_ppSimN=0;                 // simulation steps, all sources (LOOPWATCH keeps its own g_lwSimN)
static uint64_t g_odSimAtDraw=~0ull;        // g_ppSimN at the last EXECUTED draw
static uint64_t g_odLastSimUs=0;
static uint64_t g_odDrawn=0, g_odSkipped=0, g_odSleptUs=0, g_odSleeps=0;
// Entry: opens the slot and redirects the return address to the exit trap.
static void pp_enter(Cpu& c, PpKind k, uint32_t exitTrap){
    ++g_ppHooks;
    PpSlot& s=g_ppSlot[k];
    const uint64_t now=rt_now_us();
    if(s.busy){
        // Exit never observed (SEH exception, longjmp): reclaim the slot
        // after 5s, otherwise the phase would stay silent forever.
        if(now-s.t0>5000000ull){ s.busy=false; ++g_ppPerdus; }
        else { ++g_ppRefus; return; }
    }
    const uint32_t E=c.reg(R_ESP);
    s.busy=true; s.tid=lw_tid(); s.ret=c.read_u32(E); s.t0=now;
    c.write_u32(E,exitTrap);
}
// Exit: the guest's `ret` already popped the address; undo the handler's +4
// and redirect to the original address. EAX unchanged.
static uint32_t pp_exit(Cpu& c, PpKind k, Bridge& br){
    ++g_ppHooks;
    PpSlot& s=g_ppSlot[k];
    const uint64_t now=rt_now_us();
    if(now>=s.t0) g_ppUs[k]+=now-s.t0; ++g_ppCnt[k];
    const uint32_t ret=s.ret; s.busy=false;
    c.set_reg(R_ESP,c.reg(R_ESP)-4);
    br.redirect_next(ret);
    return c.reg(R_EAX);
}
void pp_flush_begin(){ if(!pp_on()) return; g_ppFlushT0=rt_now_us(); }
void pp_flush_end(){
    if(!pp_on()||!g_ppFlushT0) return;
    const uint64_t now=rt_now_us();
    if(now>=g_ppFlushT0) g_ppFlushUs+=now-g_ppFlushT0; ++g_ppFlushN;
    if(!g_ppSlot[PP_DRAW].busy) ++g_ppFlushHors;     // a flush outside a draw would make "draw net of flush" wrong
    g_ppFlushT0=0;
}
static void pp_frac(char* o,size_t n,uint64_t num,uint64_t den){
    if(!den){ std::snprintf(o,n,"n/a"); return; }
    const unsigned long long h=(unsigned long long)((num*100ull)/den);
    std::snprintf(o,n,"%llu.%02llu",h/100ull,h%100ull);
}
// Frame boundary (same three call sites as lw_frame): counts the frame,
// reports every 10s alongside "frames: fps=".
void pp_frame(){
    if(!pp_on()) return;
    const uint64_t now=rt_now_us();
    if(!g_ppWinT0){ g_ppWinT0=now; return; }
    ++g_ppFrames;
    if(now-g_ppWinT0<10000000ull) return;
    const uint64_t win=now-g_ppWinT0, F=g_ppFrames?g_ppFrames:1, T=g_ppCnt[PP_SIM];
    const uint64_t sim=g_ppUs[PP_SIM], drawT=g_ppUs[PP_DRAW], fl=g_ppFlushUs, tour=g_ppUs[PP_TOUR];
    const uint64_t drawNet = drawT>fl ? drawT-fl : 0;
    const uint64_t img=win/F;
    const uint64_t used=sim/F+drawNet/F+fl/F;
    const uint64_t autre = img>used ? img-used : 0;
    jpline("phase: img=%llu (%llu.%llu/s) ticks=%llu | par image: sim=%llu dessin=%llu flush=%llu autre=%llu (image=%llu us, tour=%llu) | dessins=%llu flushs=%llu tours=%llu",
           (unsigned long long)g_ppFrames,
           (unsigned long long)(g_ppFrames*10000000ull/win)/10ull,
           (unsigned long long)(g_ppFrames*10000000ull/win)%10ull,
           (unsigned long long)T,
           (unsigned long long)(sim/F),(unsigned long long)(drawNet/F),(unsigned long long)(fl/F),
           (unsigned long long)autre,(unsigned long long)img,(unsigned long long)(tour/F),
           (unsigned long long)g_ppCnt[PP_DRAW],(unsigned long long)g_ppFlushN,(unsigned long long)g_ppCnt[PP_TOUR]);
    if(T){
        const uint64_t per=win/T, usedT=(sim+drawT)/T;
        const uint64_t libre = per>usedT ? per-usedT : 0;
        const uint64_t horsTour = per>tour/T ? per-tour/T : 0;
        char dpt[24],spt[24]; pp_frac(dpt,sizeof dpt,g_ppCnt[PP_DRAW],T); pp_frac(spt,sizeof spt,g_odSkipped,T);
        // "libre-hote": actual host-thread idle time when every guest thread
        // is blocked (cooperative scheduler, real clock). Under cooperative
        // scheduling, "dormi" (measured in the shim) is ~0: wait() doesn't
        // block, it just requests a switch after the shim returns. Native
        // scheduling is the opposite: "dormi" is real and "libre-hote" is 0.
        // Both are reported for this reason.
        uint64_t idleUs=0;
        if(g_schedCoop){ const uint64_t cur=g_schedCoop->idle_ms_; idleUs=(cur>g_ppIdlePrev?cur-g_ppIdlePrev:0)*1000ull; g_ppIdlePrev=cur; }
        jpline("phase/tick: periode=%llu sim=%llu dessin=%llu flush=%llu libre=%llu hors-tour=%llu us | dessins/tick=%s sautes/tick=%s dormi=%llu us/tick (%llu siestes) libre-hote=%llu us/tick",
               (unsigned long long)per,(unsigned long long)(sim/T),(unsigned long long)(drawNet/T),
               (unsigned long long)(fl/T),(unsigned long long)libre,(unsigned long long)horsTour,
               dpt,spt,(unsigned long long)(g_odSleptUs/T),(unsigned long long)g_odSleeps,(unsigned long long)(idleUs/T));
    }
    for(int i0=0;i0<g_ppXN;i0+=3){          // 3 per line: jpline truncates at 220 characters
        char m[240]; int off=std::snprintf(m,sizeof m,"phase/x:");
        for(int i=i0;i<g_ppXN && i<i0+3 && off>0 && off<(int)sizeof m-48;i++)
            off+=std::snprintf(m+off,sizeof m-(size_t)off," Game+0x%x=%llu us/img (n=%llu/img)",
                               (unsigned)g_ppXRva[i],(unsigned long long)(g_ppUs[PP_X0+i]/F),(unsigned long long)(g_ppCnt[PP_X0+i]/F));
        jpline("%s",m);
    }
    jpline("phase/cout: crochets=%llu/img refus=%llu perdus=%llu flush-hors-dessin=%llu horloge=%lluns onedraw=%d",
           (unsigned long long)(g_ppHooks/F),(unsigned long long)g_ppRefus,(unsigned long long)g_ppPerdus,
           (unsigned long long)g_ppFlushHors,(unsigned long long)g_ppClockNs,g_odMode);
    g_ppWinT0=now; g_ppFrames=0; g_ppHooks=0; g_ppRefus=0; g_ppFlushUs=0; g_ppFlushN=0; g_ppFlushHors=0;
    for(int k=0;k<PP_N;k++){ g_ppUs[k]=0; g_ppCnt[k]=0; }
    g_odSkipped=0; g_odDrawn=0; g_odSleptUs=0; g_odSleeps=0;
}

// ====== HOOK INSTALLATION (called from main, after [inlinehot], before D2_NOCAP) ======
// `cpu` and `br` are main's; lambdas capture `&br` since main's Bridge lives
// for the lifetime of the program.
void phase_hooks_install(Cpu* cpu, Bridge& br){
    // ---- D2_LOOPWATCH: SIMULATION STEP counter (fidelity check) ------------
    // Game+0x12fd90 (VA 0x52fd90) is the game step: it only runs when the
    // gate at 0x52fc20 says a period has elapsed. That gate reads timeGetTime
    // and compares it to [0x883d60] = 1000/[0x731014]; [0x731014] is 0x19 =
    // 25 in the original binary, so ONE STEP EVERY 40ms OF REAL TIME on the
    // game thread — a clock entirely separate from the 40ms draw-loop credit
    // that D2_NOCAP adjusts. This counter is the proof that the knob doesn't
    // touch game speed: it must read ~25/s with or without it.
    // Hooked at function ENTRY (int3 padding before it, `push ebp` prologue),
    // faithful fallback: EAX unchanged. D2_PHASEPROF / D2_ONEDRAW share this
    // hook (one alternate per address): it counts for LOOPWATCH, times for
    // PHASEPROF, and timestamps the last step for ONEDRAW. Nothing is
    // installed unless one of the three knobs is set.
    // od_mode() must be evaluated BEFORE the `||`: short-circuiting on
    // pp_on() would leave it at zero.
    const int odMode=od_mode(); const bool ppOn=pp_on(); const bool r60Tag=d2gr::Replay60::tagOn();
    if((lw_on()||ppOn||odMode||r60Tag) && g_114 && g_d2base){
        static uint32_t s_simEntry=0; s_simEntry=g_d2base+0x0012fd90u;
        static uint32_t s_simExit=0;
        if(pp_on()){
            Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!pp_sim_exit";
            x.fn=[&br](Cpu&c)->uint32_t{ return pp_exit(c,PP_SIM,br); };
            br.register_shim("native.hook","pp_sim_exit",x);
            s_simExit=br.shim_trap("native.hook","pp_sim_exit");
        }
        Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!sim_step";
        s.fn=[&br](Cpu&c)->uint32_t{
            ++g_lwSimN; ++g_ppSimN;
            if(g_odMode) g_odLastSimUs=rt_now_us();
            if(g_r60TagOn){ const uint32_t t=(uint32_t)g_ppSimN; gr_emit(c,D2GR_OP_TICK,&t,1); }   // D2_REPLAY60: timestamps the step in the ring
            if(g_ppOn) pp_enter(c,PP_SIM,s_simExit);
            const uint32_t E=c.reg(R_ESP);
            c.write_u32(E-4,c.reg(R_EBP));      // faithful fallback: `push ebp`
            c.set_reg(R_ESP,E-8);               // the handler's +4 lands back on E-4
            br.redirect_next(s_simEntry+1);     // resumes at 'mov ebp,esp'
            return c.reg(R_EAX); };             // faithful fallback: EAX UNCHANGED
        br.register_shim("native.hook","sim_step",s);
        cpu->set_alternate(s_simEntry,br.shim_trap("native.hook","sim_step"));
        std::printf("loopwatch: pas de simulation comptes (alternate sur Game+0x12fd90, cadence [0x731014]=%u Hz)\n",
                    cpu->read_u32(g_d2base+0x00331014u));
    }
    // ---- D2_PHASEPROF / D2_ONEDRAW: the DRAW and the loop TURN -------------
    // [0x7a0484] is set by 0x44c930 from table 0x70ef0c (index = mode, written
    // to [0x7a0480]) and by 0x44e300 (default value 0x44c990). All three
    // targets are hooked at ENTRY: 0x44c990 starts with `push ebp`, 0x45fde0
    // and 0x460190 with `call 0x4f6070` (E8 rel32, 5 bytes) — the fallback
    // replays the instruction (pushes return address entry+5 and jumps to
    // 0x4f6070). Called as `xor ecx,ecx ; call [0x7a0484]`, bare `ret`: no
    // stack argument, return value ignored by both callers (0x44f291
    // `mov eax,1`, 0x44f01d `call`).
    if((ppOn||odMode) && g_114 && g_d2base){
        static const uint32_t kDrawRva[3]={0x0004c990u,0x0005fde0u,0x00060190u};
        static uint32_t s_drawEntry[3]={0,0,0}, s_drawExit=0, s_callee=0;
        for(int i=0;i<3;i++) s_drawEntry[i]=g_d2base+kDrawRva[i];
        s_callee=g_d2base+0x000f6070u;
        if(pp_on()){
            Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!pp_draw_exit";
            x.fn=[&br](Cpu&c)->uint32_t{ return pp_exit(c,PP_DRAW,br); };
            br.register_shim("native.hook","pp_draw_exit",x);
            s_drawExit=br.shim_trap("native.hook","pp_draw_exit");
        }
        for(int i=0;i<3;i++){
            Shim s; s.argc=0; s.stdcall_cleanup=false;
            static const char* tags[3]={"native!pp_draw0","native!pp_draw1","native!pp_draw2"};
            static const char* names[3]={"pp_draw0","pp_draw1","pp_draw2"};
            s.tag=tags[i];
            s.fn=[&br,i](Cpu&c)->uint32_t{
                const uint32_t entry=s_drawEntry[i];
                if(g_odMode){
                    const uint64_t now=rt_now_us();
                    // in-game = a simulation step happened less than 200ms ago
                    const bool enJeu = g_odLastSimUs && (now-g_odLastSimUs)<200000ull;
                    if(enJeu && g_odSimAtDraw==g_ppSimN){
                        ++g_odSkipped;
                        if(g_odMode==2 && g_sched){
                            Waitable* never=rt_never_event();   // manual KEvent that is NEVER signaled, created once on first use (rt_boot.cpp)
                            const uint32_t ms=g_odSleepUs/1000u;
                            ++g_odSleeps;
                            if(ms) g_sched->wait(never,ms); else g_sched->yield();
                            g_odSleptUs+=rt_now_us()-now;
                        }
                        return 0u;              // `ret`: the handler pops the return address
                    }
                    g_odSimAtDraw=g_ppSimN; ++g_odDrawn;
                }
                if(g_ppOn) pp_enter(c,PP_DRAW,s_drawExit);
                const uint32_t E=c.reg(R_ESP);
                if(i==0){ c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); }  // `push ebp`
                else    { c.write_u32(E-4,entry+5);      c.set_reg(R_ESP,E-8); br.redirect_next(s_callee); } // `call 0x4f6070`
                return c.reg(R_EAX); };
            br.register_shim("native.hook",names[i],s);
            cpu->set_alternate(s_drawEntry[i],br.shim_trap("native.hook",names[i]));
        }
        jpline("phaseprof: dessin crochete (alternate sur Game+0x4c990, +0x5fde0, +0x60190 ; sortie par trap de retour) onedraw=%d sieste=%uus",
               g_odMode,(unsigned)g_odSleepUs);
    }
    if(ppOn && g_114 && g_d2base){
        // The frame function 0x44efa0 (ret 4, `push ebp`), called on each turn
        // of the wait loop 0x451bb0. It contains the simulation gate, the
        // step, and the draw; whatever it doesn't contain (pump, Fog, host)
        // falls under "hors-tour".
        static uint32_t s_tourEntry=0, s_tourExit=0; s_tourEntry=g_d2base+0x0004efa0u;
        Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!pp_tour_exit";
        x.fn=[&br](Cpu&c)->uint32_t{ return pp_exit(c,PP_TOUR,br); };
        br.register_shim("native.hook","pp_tour_exit",x);
        s_tourExit=br.shim_trap("native.hook","pp_tour_exit");
        Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!pp_tour";
        s.fn=[&br](Cpu&c)->uint32_t{
            pp_enter(c,PP_TOUR,s_tourExit);
            const uint32_t E=c.reg(R_ESP);
            c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(s_tourEntry+1);
            return c.reg(R_EAX); };
        br.register_shim("native.hook","pp_tour",s);
        cpu->set_alternate(s_tourEntry,br.shim_trap("native.hook","pp_tour"));
        // Cost of one clock read, measured once here (reported as "horloge=").
        { const uint64_t a=rt_now_us(); uint64_t x2=0; for(int k=0;k<1000;k++) x2+=rt_now_us(); const uint64_t b=rt_now_us();
          g_ppClockNs=(b-a); (void)x2; }   // 1000 reads in us == ns per read
        jpline("phaseprof: ARME — sim Game+0x12fd90, dessin [0x7a0484], tour Game+0x4efa0, flush = corps hote ; horloge=%lluns/lecture",
               (unsigned long long)g_ppClockNs);
        // ---- D2_PHASEPROF_X=<rva>[,<rva>...]: up to 8 additional functions --
        // Lets you bisect "the rest of the turn" (frame − sim − draw) without
        // rebuilding: each RVA is hooked at ENTRY with the same mechanism
        // (alternate + return trap). The fallback replays the FIRST
        // INSTRUCTION, which must be one of: push ebp/ebx/esi/edi
        // (55/53/56/57), call rel32 (E8), mov eax,[imm32] (A1). Any other form
        // is REFUSED and logged (a blind hook that corrupts the guest would
        // be worse than no hook). Reported as "phase/x: Game+0x...=... us/img".
        if(const char* xs=getenv("D2_PHASEPROF_X")){
            static uint32_t s_xEntry[PP_XN]={0}, s_xExit[PP_XN]={0};
            static uint8_t  s_xForm[PP_XN]={0};     // function's first byte
            static uint32_t s_xCallee[PP_XN]={0};   // call E8 target / mov A1 imm32
            char buf[256]; std::snprintf(buf,sizeof buf,"%s",xs);
            for(char* tok=std::strtok(buf,", ;"); tok && g_ppXN<PP_XN; tok=std::strtok(nullptr,", ;")){
                const uint32_t rva=(uint32_t)strtoul(tok,nullptr,16);
                if(!rva) continue;
                const uint32_t entry=g_d2base+rva;
                uint8_t b[5]={0,0,0,0,0}; cpu->read(entry,b,5);
                const bool okPush=(b[0]==0x55||b[0]==0x53||b[0]==0x56||b[0]==0x57), okCall=(b[0]==0xE8), okMov=(b[0]==0xA1);
                if(!(okPush||okCall||okMov)){
                    jpline("phaseprof: REFUS Game+0x%x — premiere instruction %02x %02x %02x %02x %02x non rejouable (55/53/56/57/E8/A1 seulement)",
                           (unsigned)rva,b[0],b[1],b[2],b[3],b[4]);
                    continue; }
                const int i=g_ppXN++;
                g_ppXRva[i]=rva; s_xEntry[i]=entry; s_xForm[i]=b[0];
                uint32_t imm; std::memcpy(&imm,b+1,4);
                s_xCallee[i]= okCall ? entry+5+imm : imm;
                const PpKind k=(PpKind)(PP_X0+i);
                char nx[32]; std::snprintf(nx,sizeof nx,"pp_x%d_exit",i);
                // Tag must be UNIQUE: Bridge::alloc_trap dedupes by tag, a
                // shared tag would make every X point at the first one's slot.
                Shim xe; xe.argc=0; xe.stdcall_cleanup=false; xe.tag=std::string("native!")+nx;
                xe.fn=[&br,k](Cpu&c)->uint32_t{ return pp_exit(c,k,br); };
                br.register_shim("native.hook",nx,xe);
                s_xExit[i]=br.shim_trap("native.hook",nx);
                char ne[32]; std::snprintf(ne,sizeof ne,"pp_x%d",i);
                Shim se; se.argc=0; se.stdcall_cleanup=false; se.tag=std::string("native!")+ne;
                se.fn=[&br,k,i](Cpu&c)->uint32_t{
                    pp_enter(c,k,s_xExit[i]);
                    const uint32_t E=c.reg(R_ESP), entry=s_xEntry[i];
                    switch(s_xForm[i]){
                    case 0x55: c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x53: c.write_u32(E-4,c.reg(R_EBX)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x56: c.write_u32(E-4,c.reg(R_ESI)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x57: c.write_u32(E-4,c.reg(R_EDI)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0xE8: c.write_u32(E-4,entry+5); c.set_reg(R_ESP,E-8); br.redirect_next(s_xCallee[i]); break;   // call rel32
                    case 0xA1: c.set_reg(R_ESP,E-4); br.redirect_next(entry+5); return c.read_u32(s_xCallee[i]);      // mov eax,[imm32]: EAX = the value read
                    }
                    return c.reg(R_EAX); };
                br.register_shim("native.hook",ne,se);
                cpu->set_alternate(entry,br.shim_trap("native.hook",ne));
                jpline("phaseprof: X Game+0x%x crochete (forme %02x)",(unsigned)rva,b[0]);
            }
        }
    }
}
void ringtag_hooks_install(Cpu* cpu, Bridge& br){
    const bool r60Tag=d2gr::Replay60::tagOn();
    // ---- D2_REPLAY60 / D2_RINGTAG / D2_RINGPHASE_X: RING TAGGING -----------
    // Four `alternate` hooks at function ENTRY (never a jump target),
    // faithful fallback (push ebp / call rel32 replayed, EAX unchanged), exit
    // via return trap — the same pattern as D2_PHASEPROF. Each hook WRITES a
    // record into the Glide ring (gr_emit), in stream order:
    //   (a) Game+0xdc7b0 (VA 0x4dc7b0): UNIT DRAW. ECX = UnitAny* (type +0,
    //       id +0xC, mode +0x10, pPath +0x2C). This is the function that
    //       contains the GetUnitX/GetUnitY calls at 0x4dc8df/0x4dc8e8
    //       (0x620650/0x6206b0: [pPath+8]/[pPath+0xC] for a dynamic path,
    //       [pPath+4]/[pPath+8] for a static one). FINE 16.16 position read
    //       at pPath+0 / pPath+4 (wOffset | wPos<<16) — checked against the
    //       engine's conv-err counter. 3 callers: 0x4712bb, 0x4df43a (draw
    //       list loop, esi=[esi+0x10]), 0x4df596.
    //       Entry -> UNITTAG {id,type,x,y,mode,[pPath+8],[pPath+0xC]},
    //       exit -> UNITEND {id}.
    //   (b) Game+0x5b440 (VA 0x45b440): sets SCREEN OFFSETS [0x7a520c]/
    //       [0x7a5208] from the player ([0x7a6a70]) — called once per frame
    //       at 0x44cae3, right before the world. Exit -> CAMERA {offX,offY,
    //       playerId, playerX, playerY (fine)}.
    //   (c) Game+0x12fd90 (simulation step, shared hook) -> TICK {n}.
    //   (d) D2_RINGPHASE_X=<rva,...> (8 max) and D2_REPLAY60_UI=<rva>: PHASE
    //       {rva, 1|0} on entry/exit — the per-function draw/vertex inventory
    //       (the "ringtag:" line) used to locate the UI boundary.
    const bool padOn = padst::on();
    if((r60Tag || padOn) && g_114 && g_d2base){
        g_r60TagOn = r60Tag;      // the ring consumer; padOn is the second consumer of the same hooks
        // D2_GLIDERING is now a permanent default (rt_boot.cpp) — no env
        // lookup here; only D2_GLIDEGXM (grgx_on) is still a real condition.
        g_r60Active = d2gr::Replay60::on() && grgx_on();
        if(d2gr::Replay60::on() && !g_r60Active)
            jpline("replay60: INACTIF — exige D2_GLIDEGXM=1 (defaut ; D2_GLIDERING est desormais permanent) ; le marquage seul est arme");
        if(const char* e=getenv("D2_REPLAY60_UI")) g_r60UiRva=(uint32_t)strtoul(e,nullptr,16);
        struct R60Slot{ bool busy; uint32_t ret; uint32_t id; };
        static R60Slot s_unit={false,0,0}, s_cam={false,0,0};
        static uint32_t s_unitEntry=0, s_unitExit=0, s_camEntry=0, s_camExit=0;
        s_unitEntry=g_d2base+0x000dc7b0u; s_camEntry=g_d2base+0x0005b440u;
        {   // (a) unit draw
            uint8_t b0=0; cpu->read(s_unitEntry,&b0,1);
            uint8_t b1=0; cpu->read(s_camEntry,&b1,1);
            if(b0!=0x55||b1!=0x55) jpline("ringtag: REFUS — Game+0xdc7b0=%02x Game+0x5b440=%02x (push ebp attendu) : crochets unite/camera NON poses",b0,b1);
            else {
            Shim x; x.argc=0; x.stdcall_cleanup=false; x.tag="native!r60_unit_exit";
            x.fn=[&br](Cpu&c)->uint32_t{
                uint32_t w[1]={s_unit.id}; gr_emit(c,D2GR_OP_UNITEND,w,1);
                const uint32_t ret=s_unit.ret; s_unit.busy=false;
                c.set_reg(R_ESP,c.reg(R_ESP)-4); br.redirect_next(ret); return c.reg(R_EAX); };
            br.register_shim("native.hook","r60_unit_exit",x);
            s_unitExit=br.shim_trap("native.hook","r60_unit_exit");
            Shim e; e.argc=0; e.stdcall_cleanup=false; e.tag="native!r60_unit";
            e.fn=[&br,padOn](Cpu&c)->uint32_t{
                const uint32_t u=c.reg(R_ECX);
                uint32_t w[7]={0,0,0,0,0,0,0};
                if(u){
                    const uint32_t type=c.read_u32(u), id=c.read_u32(u+0xc), mode=c.read_u32(u+0x10), path=c.read_u32(u+0x2c);
                    int32_t x=0,y=0; uint32_t r8=0,rc=0;
                    if(path){
                        // STATIC path (objects, items, tiles): the draw code picks between
                        // TWO complete position sources each frame, gated by the live flag
                        // the game computes as (Game+0x32da4c!=0) ? Game+0x32da48 : 0
                        // (that's the entirety of Game+0xf51d0 — confirmed by disassembly to
                        // be nothing but these two global reads, no other state). Our own
                        // ground-item position fix AND the independently-disassembled Alt
                        // ground-item name-label renderer (Game+0x71620/0x71450) both branch
                        // on this exact same flag. We previously hardcoded branch A
                        // (interpolated) unconditionally, which is wrong whenever the live
                        // flag is 0 that frame — this is what produced the reported
                        // "sometimes off, no consistent direction" symptom (confirmed by a
                        // dispatched subagent's independent disassembly, not yet verified
                        // live). Branch A (flag!=0): integer subtile at [+0xC]/[+0x10],
                        // shifted to 16.16 (0x6203b0/0x620410) — the two values branch A
                        // adds afterward (0x45afc0/0x45afd0) read global camera-scroll state,
                        // not per-unit fields, so they must NOT be added here (our own
                        // camera-relative world_to_screen already accounts for scroll).
                        // Branch B (flag==0): [+4]/[+8] used DIRECTLY as the complete
                        // position (0x620650/0x6206b0, "GetUnitX/Y") — not a near-zero
                        // fractional offset as an earlier single-frame sample suggested; that
                        // sample simply landed on a frame where the flag was nonzero, so
                        // branch B's own fields were stale/near-zero at that instant.
                        // DYNAMIC: [+8]/[+0xC] (0x6489c0/0x6489d0). Both 16.16.
                        if(type==2||type==4||type==5){
                            const uint32_t flagGate=c.read_u32(g_d2base+0x0032da4cu);
                            const uint32_t flag=flagGate?c.read_u32(g_d2base+0x0032da48u):0u;
                            if(flag){
                                x=(int32_t)(((uint32_t)c.read_u32(path+0xc))<<16);
                                y=(int32_t)(((uint32_t)c.read_u32(path+0x10))<<16);
                            } else {
                                x=(int32_t)c.read_u32(path+4);
                                y=(int32_t)c.read_u32(path+8);
                            }
                        }
                        else { x=(int32_t)c.read_u32(path); y=(int32_t)c.read_u32(path+4); r8=c.read_u32(path+8); rc=c.read_u32(path+0xc); } }
                    w[0]=id; w[1]=type; w[2]=(uint32_t)x; w[3]=(uint32_t)y; w[4]=mode; w[5]=r8; w[6]=rc;
                    if(padOn){
                        padst::Unit pu; std::memset(&pu,0,sizeof pu);
                        pu.id=id; pu.type=type; pu.mode=mode; pu.cls=c.read_u32(u+4);
                        // DYNAMIC units (player, monsters): pPath+0/+4, the same
                        // two words the camera hook reads for the player -- a
                        // dynamic Path packs xOffset/xPos and yOffset/yPos as the
                        // low and high halves of those dwords, so each one IS the
                        // 16.16 fine coordinate. +8/+0xC are NOT: console log
                        // 20/09 projected every monster to ~(4201,-90266) while
                        // the player sat at (400,284), which left pick_hostile
                        // with no candidate in range -- L could not attack
                        // anything outside town. The ringtag payload below has
                        // always used x/y here; only this consumer drifted.
                        pu.fx=x; pu.fy=y;
                        if(type==1){
                            pu.ownerType=c.read_u32(u+0x94); pu.ownerId=c.read_u32(u+0x98);
                        }
                        padst::add_unit(pu);
                    }
                }
                if(g_r60TagOn) gr_emit(c,D2GR_OP_UNITTAG,w,7);
                const uint32_t E=c.reg(R_ESP);
                if(g_r60TagOn){
                    if(!s_unit.busy){ s_unit.busy=true; s_unit.ret=c.read_u32(E); s_unit.id=w[0]; c.write_u32(E,s_unitExit); }
                    else ++g_r60Reent;
                }
                c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(s_unitEntry+1);   // faithful fallback: push ebp
                return c.reg(R_EAX); };
            br.register_shim("native.hook","r60_unit",e);
            cpu->set_alternate(s_unitEntry,br.shim_trap("native.hook","r60_unit"));
            // (b) camera: read on EXIT (globals have just been set)
            Shim cx; cx.argc=0; cx.stdcall_cleanup=false; cx.tag="native!r60_cam_exit";
            cx.fn=[&br,padOn](Cpu&c)->uint32_t{
                uint32_t w[5]={0,0,0,0,0};
                w[0]=c.read_u32(g_d2base+0x003a520cu); w[1]=c.read_u32(g_d2base+0x003a5208u);
                const uint32_t pl=c.read_u32(g_d2base+0x003a6a70u);
                uint32_t path=0;
                if(pl){ w[2]=c.read_u32(pl+0xc); path=c.read_u32(pl+0x2c);
                        if(path){ w[3]=c.read_u32(path); w[4]=c.read_u32(path+4); } }
                if(g_r60TagOn) gr_emit(c,D2GR_OP_CAMERA,w,5);
                if(padOn){
                    uint32_t ui[38]; for(int i=0;i<38;i++) ui[i]=c.read_u32(g_d2base+0x003a27c0u+4u*(uint32_t)i);
                    uint32_t lvl=0; int32_t pfx=0,pfy=0;
                    if(pl && path){
                        // Fine position at camera-hook time: pPath+0/+4, NOT +8/+0xC.
                        // Proven by the existing D2GR_OP_CAMERA consumer (replay60.cpp
                        // worldToScreen on frame-to-frame deltas of exactly these two
                        // words; replay60.h calls them "camera = player, 16.16") — the
                        // spec table's +8/+0xC is what GetUnitX/Y return, which is only
                        // populated once this frame's unit-draw pass reaches the player;
                        // read that early (camera fires "right before the world"), it is
                        // still last frame's value or zero. Confirmed by a qemu diagnostic
                        // dump: +0/+4 gave plausible large 16.16 values (~4874,~4228
                        // subtiles) while +8/+0xC gave near-zero garbage, at the same tick.
                        pfx=(int32_t)w[3]; pfy=(int32_t)w[4];
                        // Level+0x1F8 (spec table, sourced from struct D2BS, never
                        // disassembly-confirmed for 1.14d) does not exist in this
                        // binary: a scan of every mov/cmp/lea/movzx/test in .text found
                        // ZERO instructions touching [reg+0x1F8] anywhere. r1/r2 (Room1/
                        // Room2) DO resolve correctly — proven dynamically: 3 different
                        // units (different Room1 *and* Room2 pointers, i.e. different
                        // room tiles) all converged on the exact same r2+0x58 pointer,
                        // the fan-in you expect from "many rooms, one level". +0x1C0
                        // resolves and tracks transitions, but it holds dwLevelTYPE, not
                        // dwLevelNo: console, 21/09, an out-and-back from Lut Gholein
                        // read 12 <-> 16 = Act 2 Town <-> Act 2 Desert, while Lut
                        // Gholein's level NUMBER is 40. The earlier check that seemed to
                        // confirm a level number — 1 at the Rogue camp, 2 in Blood Moor —
                        // passed by coincidence: Act 1 Town and Act 1 Wilderness have
                        // type ids equal to their level numbers. Consumers must compare
                        // against TYPES (see pad_is_town).
                        uint32_t lvptr=0;
                        const uint32_t r1=c.read_u32(path+0x1c);
                        if(r1){ const uint32_t r2=c.read_u32(r1+0x10);
                            if(r2){ const uint32_t lv=c.read_u32(r2+0x58); lvptr=lv; if(lv) lvl=c.read_u32(lv+0x1c0); } }
                        padst::set_level_ptr(lvptr);
                    }
                    // Ground-item name labels: the game's OWN array (see
                    // padst::Label). Read whole, in one pass, no hook and no
                    // geometry -- these are the exact rects it hit-tests the
                    // mouse against. It holds the PREVIOUS frame's labels at
                    // this point (Game+0xc0810 runs later, with the UI), the
                    // same one-frame lag the unit list already has.
                    padst::Label lb[padst::MAX_LABELS]; int nlb=0;
                    uint32_t lcount=c.read_u32(g_d2base+0x003c54a0u);
                    if(lcount>(uint32_t)padst::MAX_LABELS) lcount=padst::MAX_LABELS;
                    for(uint32_t i=0;i<lcount;i++){
                        const uint32_t e=g_d2base+0x003c54a8u+i*0x120u;
                        const uint32_t pu=c.read_u32(e+0x10);
                        if(!pu) continue;
                        padst::Label& L=lb[nlb];
                        L.x1=(int32_t)c.read_u32(e);      L.y1=(int32_t)c.read_u32(e+4);
                        L.x2=(int32_t)c.read_u32(e+8);    L.y2=(int32_t)c.read_u32(e+0xc);
                        L.unitId=c.read_u32(pu+0xc);      // UnitAny+0x0c = dwUnitId
                        if(L.x2>L.x1 && L.y2>L.y1) ++nlb;
                    }
                    padst::set_labels(lb,nlb);
                    padst::frame_begin(w[2], pfx, pfy, (int32_t)w[0], (int32_t)w[1], lvl,
                                       c.read_u32(g_d2base+0x003a6a94u), c.read_u32(g_d2base+0x003a6a78u),
                                       c.read_u32(g_d2base+0x003a6a8cu), ui);
                }
                const uint32_t ret=s_cam.ret; s_cam.busy=false;
                c.set_reg(R_ESP,c.reg(R_ESP)-4); br.redirect_next(ret); return c.reg(R_EAX); };
            br.register_shim("native.hook","r60_cam_exit",cx);
            s_camExit=br.shim_trap("native.hook","r60_cam_exit");
            Shim ce; ce.argc=0; ce.stdcall_cleanup=false; ce.tag="native!r60_cam";
            ce.fn=[&br](Cpu&c)->uint32_t{
                const uint32_t E=c.reg(R_ESP);
                if(!s_cam.busy){ s_cam.busy=true; s_cam.ret=c.read_u32(E); c.write_u32(E,s_camExit); }
                c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(s_camEntry+1);
                return c.reg(R_EAX); };
            br.register_shim("native.hook","r60_cam",ce);
            cpu->set_alternate(s_camEntry,br.shim_trap("native.hook","r60_cam"));
            jpline("ringtag: crochets poses — unite Game+0xdc7b0 (ECX=UnitAny*), camera Game+0x5b440, pas Game+0x12fd90 ; rejeu=%s ui=Game+0x%x ; pad=%s",
                   g_r60Active?"ACTIF":"non",(unsigned)g_r60UiRva, padOn?"oui":"non");
            }
        }
        // (d) phases: D2_RINGPHASE_X + the UI boundary
        {
            static const int RP_N=9;
            static uint32_t s_pEntry[RP_N]={0}, s_pExit[RP_N]={0}, s_pCallee[RP_N]={0}, s_pRva[RP_N]={0};
            static uint8_t  s_pForm[RP_N]={0};
            static R60Slot  s_pSlot[RP_N];
            uint32_t rvas[RP_N]; int nr=0;
            if(const char* xs=getenv("D2_RINGPHASE_X")){
                char buf[256]; std::snprintf(buf,sizeof buf,"%s",xs);
                for(char* tok=std::strtok(buf,", ;"); tok && nr<RP_N-1; tok=std::strtok(nullptr,", ;")){
                    const uint32_t rva=(uint32_t)strtoul(tok,nullptr,16); if(rva) rvas[nr++]=rva; } }
            if(g_r60UiRva){ bool dup=false; for(int i=0;i<nr;i++) if(rvas[i]==g_r60UiRva) dup=true; if(!dup) rvas[nr++]=g_r60UiRva; }
            for(int k=0;k<nr;k++){
                const uint32_t rva=rvas[k], entry=g_d2base+rva;
                if(rva==0x000dc7b0u||rva==0x0005b440u){   // only one alternate per address: unit and camera already have theirs
                    jpline("ringtag: Game+0x%x IGNORE dans D2_RINGPHASE_X (adresse deja crochetee : unite/camera)",(unsigned)rva); continue; }
                uint8_t b[5]={0,0,0,0,0}; cpu->read(entry,b,5);
                const bool okPush=(b[0]==0x55||b[0]==0x53||b[0]==0x56||b[0]==0x57), okCall=(b[0]==0xE8), okMov=(b[0]==0xA1);
                if(!(okPush||okCall||okMov)){
                    jpline("ringtag: REFUS Game+0x%x — premiere instruction %02x %02x %02x %02x %02x non rejouable",(unsigned)rva,b[0],b[1],b[2],b[3],b[4]);
                    continue; }
                const int i=g_rphN; if(i>=8) break;
                g_rph[i].rva=rva; g_rph[i].draws=g_rph[i].verts=g_rph[i].enters=g_rph[i].nested=0; g_rph[i].open=false;
                g_rph[i].hA=g_rph[i].hR=g_rph[i].pA=g_rph[i].pR=1469598103934665603ull;
                g_rph[i].hA=g_rph[i].hR=g_rph[i].pA=g_rph[i].pR=1469598103934665603ull;
                g_rph[i].frames=g_rph[i].sameA=g_rph[i].sameR=g_rph[i].dr=g_rph[i].vt=0;
                g_rph[i].fr0=g_rph[i].sm0=g_rph[i].fr1=g_rph[i].sm1=0; ++g_rphN;
                s_pRva[i]=rva; s_pEntry[i]=entry; s_pForm[i]=b[0];
                uint32_t imm; std::memcpy(&imm,b+1,4); s_pCallee[i]= okCall ? entry+5+imm : imm;
                s_pSlot[i].busy=false;
                char nx[32]; std::snprintf(nx,sizeof nx,"r60_ph%d_exit",i);
                Shim xe; xe.argc=0; xe.stdcall_cleanup=false; xe.tag=std::string("native!")+nx;
                xe.fn=[&br,i](Cpu&c)->uint32_t{
                    uint32_t w[2]={s_pRva[i],0u}; gr_emit(c,D2GR_OP_PHASE,w,2);
                    const uint32_t ret=s_pSlot[i].ret; s_pSlot[i].busy=false;
                    c.set_reg(R_ESP,c.reg(R_ESP)-4); br.redirect_next(ret); return c.reg(R_EAX); };
                br.register_shim("native.hook",nx,xe);
                s_pExit[i]=br.shim_trap("native.hook",nx);
                char ne[32]; std::snprintf(ne,sizeof ne,"r60_ph%d",i);
                Shim se; se.argc=0; se.stdcall_cleanup=false; se.tag=std::string("native!")+ne;
                se.fn=[&br,i](Cpu&c)->uint32_t{
                    uint32_t w[2]={s_pRva[i],1u}; gr_emit(c,D2GR_OP_PHASE,w,2);
                    const uint32_t E=c.reg(R_ESP), entry=s_pEntry[i];
                    if(!s_pSlot[i].busy && s_pForm[i]!=0xA1){ s_pSlot[i].busy=true; s_pSlot[i].ret=c.read_u32(E); c.write_u32(E,s_pExit[i]); }
                    switch(s_pForm[i]){
                    case 0x55: c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x53: c.write_u32(E-4,c.reg(R_EBX)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x56: c.write_u32(E-4,c.reg(R_ESI)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0x57: c.write_u32(E-4,c.reg(R_EDI)); c.set_reg(R_ESP,E-8); br.redirect_next(entry+1); break;
                    case 0xE8: c.write_u32(E-4,entry+5); c.set_reg(R_ESP,E-8); br.redirect_next(s_pCallee[i]); break;
                    case 0xA1: c.set_reg(R_ESP,E-4); br.redirect_next(entry+5); return c.read_u32(s_pCallee[i]);   // no exit captured (EAX carries the value read)
                    }
                    return c.reg(R_EAX); };
                br.register_shim("native.hook",ne,se);
                cpu->set_alternate(entry,br.shim_trap("native.hook",ne));
                jpline("ringtag: phase Game+0x%x crochetee (forme %02x)%s",(unsigned)rva,b[0],rva==g_r60UiRva?" = borne UI":"");
            }
        }
        // (e) pad only: ground-item name labels for the loot D-pad cursor
        //
        // NO HOOK. The labels are read straight out of the game's own array
        // in the camera hook above (see padst::Label) -- Game+0xc0810 fills
        // it every frame with the very rects it hit-tests the mouse against.
        //
        // Two earlier candidates are ruled out, both console-confirmed 20/09,
        // so neither gets tried again:
        //   Game+0x71620  -- the monster/object/player NAMEPLATE renderer. It
        //     IS entered for items, but always bails first thing: bit 5 of
        //     [unit+0xc4] is set for every item, every time.
        //   Game+0x50b690 -- reached only through the active gfx backend's
        //     vtable ([Game+0x3c8cc0]+0x90), which made it look like a shared
        //     text renderer. It is slot 0x90 of the GLIDE backend table =
        //     the sprite SHADOW blit (its GDI twin, 0x6c87e0, applies the
        //     isometric shadow shear). It was hooked, and it captured the
        //     shadows of every sprite on screen -- which is why its positions
        //     kept landing near things but never on a word. The real text
        //     path is D2WIN_DrawRectangledText (Game+0x1023b0), and the label
        //     array makes hooking it unnecessary.
    }
}
