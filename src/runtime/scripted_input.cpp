// scripted_input.cpp -- see scripted_input.h.
//
// ---- scripted input injection (drive the menu without a real mouse) --------
// D2SCRIPT env: comma-separated "frame:action[:a[:b]]" events, frame-ordered.
//   actions: move:x:y  ldown:x:y  lup:x:y  rdown:x:y  rup:x:y  click:x:y
//            keydown:vk  keyup:vk  key:vk (down+up)  chr:ascii  activate
// Events fire when the blit counter reaches `frame`; messages are queued and
// consumed by the game's own PeekMessageA/DispatchMessageA pump (real path).
#include "scripted_input.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/prof.h"
#include "runtime/win32_shims_window.h"
#include "platform/vita_present.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
using namespace d2rt;

bool g_snap=false;                         // one-shot frame dump request
extern "C" { __attribute__((weak)) void d2gxm_shot_request(void); }   // vita_gxm.cpp (Vita only)
uint8_t g_keyState[256]={0};               // VK states driven by injected input

struct InjEv { int frame; std::string act; int a,b; };
static std::vector<InjEv> g_inj; static size_t g_injIx=0;

// WINDOW ACTIVATION (see the comment on ShowWindow). Idempotent: called by
// ShowWindow (the faithful path) AND by the first message pump (a safety net
// in case the game never calls ShowWindow). The first call wins.
void win_activate_once(){
    static bool done=false; if(done) return; done=true;
    g_msgQ.push_back({0x001C,1,0});   // WM_ACTIVATEAPP, fActive=TRUE
    g_msgQ.push_back({0x0006,1,0});   // WM_ACTIVATE,    WA_ACTIVE
    g_msgQ.push_back({0x0007,0,0});   // WM_SETFOCUS
    g_injN+=3;
    d2vita_progress("fenetre: activee (WM_ACTIVATEAPP/ACTIVATE/SETFOCUS)");
}
void inj_parse(const char* s){
    std::string str(s); size_t p=0;
    while(p<str.size()){ size_t e=str.find(',',p); if(e==std::string::npos) e=str.size();
        std::string ev=str.substr(p,e-p); p=e+1;
        InjEv iv; iv.a=iv.b=0; char act[16]={0};
        if(std::sscanf(ev.c_str(),"%d:%15[a-z]:%d:%d",&iv.frame,act,&iv.a,&iv.b)>=2){ iv.act=act; g_inj.push_back(iv); } }
}
// Plausible PC/AT set-1 scancode for a VK/ASCII code (letters, digits, Enter):
// real Windows always fills lParam bits 16-23; handlers may check them.
static uint32_t inj_scan(int v){
    static const uint8_t L[26]={0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if(v>='A'&&v<='Z') return L[v-'A'];
    if(v>='a'&&v<='z') return L[v-'a'];
    if(v>='1'&&v<='9') return 0x02+(v-'1');
    if(v=='0') return 0x0B;
    if(v==0x0D) return 0x1C;   // Enter
    if(v==0x08) return 0x0E;   // Backspace
    if(v==0x1B) return 0x01;   // Escape
    if(v==' ')  return 0x39;
    return 0x1E; }
// Bornes du curseur injecte : la taille du jeu, pas 800x600 en dur. Avec la
// resolution native (D2_RES) le jeu fait 960x544 et un clic borne a 799
// n'atteindrait jamais les 160 px de droite — la ou vit la grille d'inventaire.
static int g_injMaxX=799, g_injMaxY=599;
void inj_set_bounds(int w,int h){ if(w>0&&h>0){ g_injMaxX=w-1; g_injMaxY=h-1; } }
void inj_queue(const std::string& act,int a,int b){
    auto clampxy=[&](int&x,int&y){ if(x<0)x=0; if(x>g_injMaxX)x=g_injMaxX; if(y<0)y=0; if(y>g_injMaxY)y=g_injMaxY; };
    auto mouse=[&](uint32_t m,uint32_t wp,int x,int y){ clampxy(x,y); wx86_set_cursor((uint32_t)x,(uint32_t)y);
        uint32_t lp=(uint32_t)((y<<16)|(x&0xffff));
        // Coalesce WM_MOUSEMOVE ONLY: if an unconsumed move sits at the tail,
        // overwrite it instead of queuing another — this is genuine Win32
        // semantics (the OS keeps only the latest pending mouse position) and
        // spares the wndproc a flood of stale moves during continuous motion.
        // Buttons/keys/char are never coalesced (order + each event preserved).
        if(m==0x200 && !g_msgQ.empty() && g_msgQ.back().msg==0x200){
            g_msgQ.back().wp=wp; g_msgQ.back().lp=lp; g_coalN++; return; }
        g_msgQ.push_back({m,wp,lp}); g_injN++; };
    if(act=="move")      mouse(0x200,0,a,b);
    else if(act=="ldown"){ g_keyState[0x01]=0x80; mouse(0x201,1,a,b); }
    else if(act=="lup"){   g_keyState[0x01]=0;    mouse(0x202,0,a,b); }
    else if(act=="rdown"){ g_keyState[0x02]=0x80; mouse(0x204,2,a,b); }
    else if(act=="rup"){   g_keyState[0x02]=0;    mouse(0x205,0,a,b); }
    else if(act=="click"){ mouse(0x200,0,a,b); g_keyState[0x01]=0x80; mouse(0x201,1,a,b); g_keyState[0x01]=0; mouse(0x202,0,a,b); }
    else if(act=="keydown"){ g_keyState[a&0xff]=0x80; g_msgQ.push_back({0x100,(uint32_t)a,1u|(inj_scan(a)<<16)}); g_injN++; }
    else if(act=="keyup"){   g_keyState[a&0xff]=0;    g_msgQ.push_back({0x101,(uint32_t)a,0xC0000001u|(inj_scan(a)<<16)}); g_injN++; }
    else if(act=="key"){     g_msgQ.push_back({0x100,(uint32_t)a,1u|(inj_scan(a)<<16)}); g_msgQ.push_back({0x101,(uint32_t)a,0xC0000001u|(inj_scan(a)<<16)}); g_injN+=2; }
    else if(act=="chr"){     g_msgQ.push_back({0x102,(uint32_t)a,1u|(inj_scan(a)<<16)}); g_injN++; }   // WM_CHAR (a = ASCII)
    else if(act=="eipdump"){  // dump the preempt-slice EIP histogram (needs -DPROF_COUNTERS)
#ifdef PROF_COUNTERS
        {   char m[96]; std::snprintf(m,sizeof m,"[eipdump] %llu echantillons, top %d",
                (unsigned long long)prof::eip_samples(), a>0?a:24);
            std::printf("  %s\n",m); d2vita_progress(m); }
        // Result also goes to the boot log: on console there is no stdout, and
        // that is precisely where this profiling is useful.
        prof::top_eips(a>0?a:24,[](uint32_t eip,uint64_t hits,void*){
            char m[64]; std::snprintf(m,sizeof m,"  eip 0x%08x  %llu",eip,(unsigned long long)hits);
            std::printf("  %s\n",m); d2vita_progress(m); },nullptr);
        // PER THREAD: exact blocks + samples per guest thread, top exact EIPs per
        // thread; D2_EIPDUMP=<file> dumps the WHOLE histogram ("tid eip hits")
        // for offline analysis (tools/eip_fils.py: functions, blocks/s, cost).
        {   static FILE* df=nullptr; static bool dfTried=false;
            if(!dfTried){ dfTried=true; if(const char* dp=getenv("D2_EIPDUMP")) df=std::fopen(dp,"w"); }
            { static int dumpN=0; if(df) std::fprintf(df,"# eipdump n=%d\n",++dumpN); }
            struct Ctx{ FILE* f; int n; } cx{df, a>0?a:24};
            prof::threads([](uint32_t tid,uint32_t entry,uint64_t samples,uint64_t blocks,void* u){
                Ctx* c=(Ctx*)u;
                char m[128]; std::snprintf(m,sizeof m,"[eipdump] fil %u entree=0x%08x echantillons=%llu blocs=%llu",
                    tid,entry,(unsigned long long)samples,(unsigned long long)blocks);
                std::printf("  %s\n",m); d2vita_progress(m);
                if(c->f) std::fprintf(c->f,"T %u %08x %llu %llu\n",tid,entry,(unsigned long long)samples,(unsigned long long)blocks);
                struct C2{ FILE* f; uint32_t tid; int shown; } c2{c->f,tid,0};
                prof::top_eips_tid(tid,-1,[](uint32_t eip,uint64_t hits,void* u2){
                    C2* k=(C2*)u2;
                    if(k->f) std::fprintf(k->f,"%u %08x %llu\n",k->tid,eip,(unsigned long long)hits);
                    if(k->shown<8){ ++k->shown; std::printf("      eip 0x%08x  %llu\n",eip,(unsigned long long)hits); } },&c2);
                },&cx);
            if(df) std::fflush(df);
        }
        std::fflush(stdout);
#else
        std::printf("  [eipdump] rebuild with -DPROF_COUNTERS to enable\n"); std::fflush(stdout);
#endif
    }
    else if(act=="profdump"){  // full PROF_COUNTERS snapshot (wall/dynarec/shims, top shims, hot EIPs)
        if(g_profDump) g_profDump("cmd");
        else { std::printf("  [profdump] rebuild with -DPROF_COUNTERS to enable\n"); std::fflush(stdout); }
    }
    // force-dump the next frame: the DIB (GDI path), AND the GPU color buffer
    // when the Glide/GXM path is the one drawing (d2gxm_shot_request is weak:
    // absent from the qemu build, present on the Vita).
    else if(act=="snap"){    g_snap=true; if(d2gxm_shot_request) d2gxm_shot_request(); }
    else if(act=="activate"){ g_msgQ.push_back({0x1C,1,0}); g_msgQ.push_back({6,1,0}); g_msgQ.push_back({7,0,0}); g_injN+=3; }  // WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS
    else if(act=="scriptoff"){ g_injIx=g_inj.size(); }   // autopilot: drop the remaining D2SCRIPT events (hand control back to physical input)
    // WM_CLOSE — the Windows close box. D2 handles it through its own CLEAN
    // shutdown path, the one that writes the character save. This exists
    // because the pause menu's "Save and exit" button does not respond to
    // clicks in this runtime (same defect as the character-creation OK
    // button, worked around there with the Enter key): without it the player
    // can never exit cleanly, so D2 never replaces the 335-byte stub written
    // at character creation with a real save — and it's that stub whose
    // loading breaks. This action provides a clean exit until the click
    // hit-test is fixed. Injectable remotely via D2CMDFILE (the "close" line).
    else if(act=="close"){ g_msgQ.push_back({0x0010,0,0}); g_injN++; }
}
// Physical-controls bridge (Vita): vita_present's input layer synthesizes the
// same mouse/keyboard events as D2SCRIPT through this C export; the per-frame
// tick is weak so the qemu build (no vita_present) links unchanged.
extern "C" void d2vita_inject(const char* act, int a, int b){ if(act) inj_queue(act, a, b); }
unsigned long inj_count(){ return (unsigned long)g_inj.size(); }
void inj_tick(int frame){
    while(g_injIx<g_inj.size() && g_inj[g_injIx].frame<=frame){
        InjEv& e=g_inj[g_injIx++];
        std::printf("  [inj] frame %d: %s %d %d\n",frame,e.act.c_str(),e.a,e.b); std::fflush(stdout);
        inj_queue(e.act,e.a,e.b); }
}
// Runtime command injection for the vision-driven driver: poll $D2CMDFILE and
// inject any newly-appended "act:a:b" lines (same grammar as the D2SCRIPT
// actions, minus the leading frame number). An external loop that watches the
// rendered frames can append clicks/keys while the game runs — no relaunch, no
// frame timing, so it never stalls on a static screen the way frame-scheduled
// injection does. Byte-offset cursor (g_cmdPos) means only new lines fire.
//
// Cost: a naive version issuing getenv + fopen + fseek(END) + ftell +
// fclose on EVERY call, with the "nothing new" check only happening AFTER
// the open, costs several percent of frame rate — it's called once per
// frame and ~3 more times by the message pump, i.e. four file opens on the
// memory card per frame for a hand-driven channel.
// Current version: getenv once, descriptor opened once and KEPT, size
// reread via fseek/ftell on that same descriptor, paced to 500 ms
// (D2_CMDMS) — 2 Hz is enough for a human-driven channel.
// Safety net (file REPLACED rather than appended to): an FTP upload that
// recreates the file can change the inode, so a kept descriptor would then
// see nothing. After D2_CMDREOPEN polls with no new data (8 by default,
// ~4 s) the descriptor is closed and reopened. A size SMALLER than the
// cursor (truncation or a shorter file) resets the cursor to zero and
// rereads everything, as a fresh start would.
static long g_cmdPos = 0;
void cmd_poll(){
    static const char* f = getenv("D2CMDFILE");   // resolved once, for good
    if(!f) return;
    static const uint64_t periodUs =
        (uint64_t)(getenv("D2_CMDMS")?strtoul(getenv("D2_CMDMS"),nullptr,10):500u)*1000ull;
    static uint64_t lastUs = 0;
    const uint64_t nowUs = rt_now_us();
    if(lastUs && nowUs-lastUs < periodUs) return;
    lastUs = nowUs;
    static FILE* fp = nullptr;
    static int stale = 0;
    static const int reopenAfter =
        getenv("D2_CMDREOPEN")?atoi(getenv("D2_CMDREOPEN")):8;
    if(!fp){ fp = std::fopen(f,"rb"); if(!fp) return; stale = 0; }
    if(std::fseek(fp,0,SEEK_END)!=0){ std::fclose(fp); fp=nullptr; return; }
    long sz=std::ftell(fp);
    if(sz<g_cmdPos) g_cmdPos = 0;               // file truncated/replaced: reread everything
    if(sz<=g_cmdPos){
        if(reopenAfter>0 && ++stale>=reopenAfter){ std::fclose(fp); fp=nullptr; stale=0; }
        return; }
    stale = 0;
    std::fseek(fp,g_cmdPos,SEEK_SET);
    char line[160];
    while(std::fgets(line,sizeof line,fp)){
        std::string s(line);
        while(!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
        if(s.empty()||s[0]=='#') continue;
        char act[16]={0}; int a=0,b=0;
        if(std::sscanf(s.c_str(),"%15[a-z]:%d:%d",act,&a,&b)>=1){
            std::printf("  [cmd] %s %d %d\n",act,a,b); std::fflush(stdout);
            inj_queue(act,a,b); }
    }
    g_cmdPos = std::ftell(fp);
    // The descriptor stays OPEN: that is the whole point of this design.
    std::clearerr(fp);   // fgets set EOF; without this the next fgets refuses to read further
}
