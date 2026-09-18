// win32_shims_user32_d2.cpp -- see win32_shims_user32_d2.h. Helpers
// g_hwnd/g_wndProc/g_msgQ/g_wndCallEcx/g_locp/g_dump_fring/guest_call_stub/
// gread_mb are defined in rt_boot.cpp and exposed via runtime/rt_host.h.
// The real-window-semantics tail (RegisterClass*/CreateWindowExA/the message
// pump/GetKeyState family) used to sit inline in tools/rt_boot.cpp's main(),
// well after this function's original call site; folded in here since it
// shares the exact same USER32 local `U` registration helper and the exact
// same g_hwnd/g_wndProc/g_msgQ state, and no other registration site for any
// of its names sits between the old and new call points (checked: neither
// win32_shims_window_install() nor win32_shims_user32_install() register any
// of them) -- so moving the registration earlier changes nothing observable.
#include "win32_shims_user32_d2.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/scripted_input.h"   // win_activate_once/cmd_poll/g_keyState/d2vita_input_tick
#include "runtime/frame_profile.h"    // lw_peek_empty
#include "runtime/win32_shims_window.h"  // wx86_get_cursor
#include "runtime/guest_sync.h"       // WxEvent (manual-reset "never signaled" wait)
#include "platform/vita_present.h"    // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
using namespace d2rt;

using KEvent = WxEvent;   // engine (guest_sync.h); same alias as tools/rt_boot.cpp's own

void win32_shims_user32_d2_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    // wsprintfA/wvsprintfA are NOT registered here: rt_boot.cpp registers them
    // under the same key, so any body added here would be dead (shadowed).
    // The live implementation is rt_boot.cpp's local `fmtA` formatter, which is
    // NOT equivalent to a full do_wsprintf: it bounds output to 1024 bytes and
    // guards the buffer and null %s, but does not handle %p.

    // USER32 stdcall (added as the boot reaches them).
    auto U=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("USER32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))
            s.fn=[fn,tag,trace](Cpu&c){ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("USER32.dll",name,s);
    };
    U("MessageBoxA",4,[](Cpu&c){ uint32_t txt=c.arg(1),cap=c.arg(2),ty=c.arg(3);
        uint32_t caller=c.read_u32(c.reg(R_ESP));   // return address at trap entry
        std::string t=txt?gread_mb(c,txt,-1):"(null)";
        std::string capS=cap?gread_mb(c,cap,-1):"(null)";
        std::printf("  [MessageBoxA] type=0x%x caption=\"%s\" caller=%s\n  text: %s\n",
            ty, capS.c_str(), g_locp?(*g_locp)(caller).c_str():"?", t.c_str());
        // The only place D2's OWN error dialogs surface (missing/corrupt data
        // file, CD check, etc.) -- printf alone is invisible on console, so
        // without this line these never reach boot_progress.txt either.
        { char m[220]; std::snprintf(m,sizeof m,"dialogue: \"%s\" -- %s",capS.c_str(),t.c_str());
          d2vita_progress(m); }
        static int mb=0; if(t.find("Expansion")!=std::string::npos && mb++==0){
            if(g_dump_fring) g_dump_fring(); }
        return 1u; });
    // These must not return 0 ("no active/foreground window"): our window IS
    // the only one and has focus. A game that checks foreground status before
    // handling keyboard input would otherwise discard every keystroke while
    // still letting mouse input through (it arrives via a different path).
    U("GetForegroundWindow",0,[](Cpu&){ return g_hwnd; });
    U("GetActiveWindow",0,[](Cpu&){ return g_hwnd; });
    // GWL_WNDPROC subclassing (D2Win hooks the window for its UI): route future
    // dispatches to the NEW proc and hand back the old one for chaining.
    // Window long storage: index -4 is GWL_WNDPROC (subclassing); all other
    // indices (GWL_USERDATA=-21, and the window's extra bytes 0,4,8... that D2
    // allocates via cbWndExtra) are per-(hwnd,index) slots the game writes and
    // reads back — commonly its C++ window-object pointer, which the wndproc
    // dereferences. Returning 0 there made the wndproc deref a bad pointer.
    static std::map<uint64_t,uint32_t> g_winLong;   // (hwnd<<32|index) -> value
    U("SetWindowLongA",3,[](Cpu&c)->uint32_t{ uint32_t h=c.arg(0); int32_t idx=(int32_t)c.arg(1);
        if(idx==-4 && c.arg(2)){ uint32_t old=g_wndProc; g_wndProc=c.arg(2);
            std::printf("    [win] SetWindowLongA GWL_WNDPROC -> 0x%08x (was 0x%08x)\n",g_wndProc,old);
            return old; }
        uint64_t k=((uint64_t)h<<32)|(uint32_t)idx; uint32_t old=g_winLong.count(k)?g_winLong[k]:0;
        g_winLong[k]=c.arg(2); return old; });
    U("GetWindowLongA",2,[](Cpu&c)->uint32_t{ uint32_t h=c.arg(0); int32_t idx=(int32_t)c.arg(1);
        if(idx==-4) return g_wndProc;
        uint64_t k=((uint64_t)h<<32)|(uint32_t)idx; return g_winLong.count(k)?g_winLong[k]:0u; });
    // CallWindowProcA(prevProc,hwnd,msg,wp,lp): invoke the given proc via a
    // guest stub (same mechanism as DispatchMessageA).
    U("CallWindowProcA",5,[&br](Cpu&c)->uint32_t{
        uint32_t proc=c.arg(0); if(!proc) return 0u;
        uint32_t retaddr=c.read_u32(c.reg(R_ESP));
        // Data-driven stub slots shared with DispatchMessageA (SMC-safe).
        br.redirect_next(guest_call_stub(c,proc,c.arg(1),c.arg(2),c.arg(3),c.arg(4),retaddr));
        return 0u; });
    // PostMessageA / SendMessageA must actually deliver the message: D2 uses
    // SendMessageA to notify its own window of chosen menu actions (e.g. save
    // and exit), so silently discarding it while reporting success breaks
    // that action.
    // Post: enqueue, the game's pump will dispatch it like any other message.
    U("PostMessageA",4,[](Cpu&c)->uint32_t{
        g_msgQ.push_back({c.arg(1),c.arg(2),c.arg(3)}); g_injN++; return 1u; });
    // Send is SYNCHRONOUS by definition -- call the window right away and
    // return ITS result. Same mechanism as CallWindowProcA/DispatchMessageA
    // above (guest stub, round-robin slots, ecx = end-of-hook-chain marker);
    // the 0 returned here gets overwritten by the stub.
    U("SendMessageA",4,[&br](Cpu&c)->uint32_t{
        if(!g_wndProc) return 0u;
        uint32_t hwnd=c.arg(0); if(!hwnd) hwnd=g_hwnd;
        if(g_d2base) g_wndCallEcx=c.read_u32(g_d2base+0x3d55d8);
        uint32_t retaddr=c.read_u32(c.reg(R_ESP));
        br.redirect_next(guest_call_stub(c,g_wndProc,hwnd,c.arg(1),c.arg(2),c.arg(3),retaddr));
        return 0u; });

    // ---- real window semantics ------------------------------------------------
    // RegisterClass keeps the game's wndproc; CreateWindowExA dispatches
    // WM_NCCREATE + WM_CREATE to it synchronously (as Windows does) via a guest
    // stub (we can't nest the emulator inside a trap hook).
    // U() previously lived higher up before USER32 was extracted into winx86;
    // redefined HERE identically. What was purely generic (stateless window,
    // rect, cursor, paint, screen keys...) moved to winx86 via
    // win32_shims_window_install(); what stays here is genuinely tied to
    // shared window/wndproc state (g_hwnd/g_wndProc/g_msgQ/g_wndClass) or to
    // d2vita's real-time message pump (RegisterClass*/CreateWindowExA via
    // misc(), ShowWindow/PeekMessageA/GetMessageA/DispatchMessageA — the
    // latter hardcodes g_wndCallEcx=*(g_d2base+0x3d55d8), a Game.exe RVA), as
    // well as GetKeyState/GetAsyncKeyState/GetKeyboardState (g_keyState is
    // written by d2vita's scripted injection at 6+ sites — an input surface
    // already known to regress).
    static std::map<std::string,uint32_t> g_wndClass;   // class name -> wndproc VA
    U("RegisterClassA",1,[](Cpu&c){ uint32_t wc=c.arg(0);
        uint32_t proc=c.read_u32(wc+4); std::string name=gread_mb(c,c.read_u32(wc+36),-1);
        g_wndClass[name]=proc; std::printf("    [win] RegisterClassA '%s' wndproc=0x%08x\n",name.c_str(),proc);
        return 0xC001u; });
    U("RegisterClassExA",1,[](Cpu&c){ uint32_t wc=c.arg(0);
        uint32_t proc=c.read_u32(wc+8); std::string name=gread_mb(c,c.read_u32(wc+40),-1);
        g_wndClass[name]=proc; return 0xC002u; });
    U("CreateWindowExA",12,[&br](Cpu&c)->uint32_t{
        std::string cls=gread_mb(c,c.arg(1),-1);
        uint32_t proc = g_wndClass.count(cls)?g_wndClass[cls]:0;
        // HWND must be a VALID guest pointer: 1.14d's window/message code
        // dereferences it (D2 stores window state at the HWND). A fake handle
        // (0x85000001, unmapped) faulted on the first mouse message. Back it
        // with a zeroed guest struct so derefs read zeros. Allocated once.
        static uint32_t hwnd = 0; if(!hwnd){ hwnd = misc(0x400);
            std::vector<uint8_t> z(0x400,0); c.write(hwnd,z.data(),0x400); }
        g_hwnd=hwnd; g_wndProc=proc;
        std::printf("    [win] CreateWindowExA class='%s' title='%s' wndproc=0x%08x -> hwnd=0x%08x\n",
            cls.c_str(), gread_mb(c,c.arg(2),-1).c_str(), proc, hwnd);
        uint32_t retaddr=c.read_u32(c.reg(R_ESP));   // call-site return (before cleanup)
        if(proc){
            // CREATESTRUCT for WM_CREATE
            uint32_t cs=misc(0x30);
            c.write_u32(cs+0x00,c.arg(11));           // lpCreateParams
            c.write_u32(cs+0x04,c.arg(10));           // hInstance
            c.write_u32(cs+0x0c,c.arg(8));            // hwndParent
            c.write_u32(cs+0x10,c.arg(7)); c.write_u32(cs+0x14,c.arg(6)); // cy,cx
            c.write_u32(cs+0x18,c.arg(5)); c.write_u32(cs+0x1c,c.arg(4)); // y,x
            c.write_u32(cs+0x20,c.arg(3));            // style
            c.write_u32(cs+0x24,c.arg(2)); c.write_u32(cs+0x28,c.arg(1)); // name, class
            // guest stub: wndproc(h,WM_NCCREATE,0,&cs); wndproc(h,WM_CREATE,0,&cs);
            // mov eax,hwnd; push retaddr; ret
            uint32_t st=misc(0x40); std::vector<uint8_t> b;
            auto P32=[&](uint8_t op,uint32_t v){ b.push_back(op); for(int i=0;i<4;i++) b.push_back((v>>(8*i))&0xff); };
            auto CALL=[&](){ b.push_back(0xB8); uint32_t v=proc; for(int i=0;i<4;i++) b.push_back((v>>(8*i))&0xff); b.push_back(0xFF); b.push_back(0xD0); };
            P32(0x68,cs); b.push_back(0x6A); b.push_back(0x00); P32(0x68,0x81); P32(0x68,hwnd); CALL();
            P32(0x68,cs); b.push_back(0x6A); b.push_back(0x00); b.push_back(0x6A); b.push_back(0x01); P32(0x68,hwnd); CALL();
            P32(0xB8,hwnd); P32(0x68,retaddr); b.push_back(0xC3);
            c.write(st,b.data(),(uint32_t)b.size());
            br.redirect_next(st);
        }
        return hwnd; });
    U("UnregisterClassA",2,[](Cpu&){ return 1u; });
    // WINDOW ACTIVATION — fixes "no key does anything". On Windows, showing a
    // top-level window ACTIVATES it, and the system then sends it
    // WM_ACTIVATEAPP, WM_ACTIVATE, then WM_SETFOCUS. This runtime created the
    // window but never sent those three messages: D2Win's internal "window
    // has focus" flag stayed 0, and its keyboard handler drops every key
    // while it is 0. The mouse doesn't consult that flag, which is why
    // keyboard input alone was dead. Only sent once, on first show: Windows
    // doesn't re-activate on every call.
    U("ShowWindow",2,[](Cpu&c){ if(c.arg(1)!=0 /* SW_HIDE */) win_activate_once(); return 1u; });
    U("SetFocus",1,[](Cpu&){ return g_hwnd; });
    // Real message pump: injected events queue in g_msgQ; the game's own
    // Peek/Get/Dispatch loop delivers them to the captured Blizzard wndproc.
    // MSG.time must be on the SAME clock the guest reads via GetTickCount
    // (0x10000+virt_ms+g_tick) — D2Win's keyboard handler gates on message
    // timestamps, and a raw g_tick stamp looks aeons in the past.
    auto fillMsg=[](Cpu&c,uint32_t pm,const GMsg&m){
        uint32_t now = tick_now(c);
        c.write_u32(pm+0x00,g_hwnd); c.write_u32(pm+0x04,m.msg);
        c.write_u32(pm+0x08,m.wp);   c.write_u32(pm+0x0c,m.lp);
        uint32_t cx,cy; wx86_get_cursor(&cx,&cy);
        c.write_u32(pm+0x10,now); c.write_u32(pm+0x14,cx); c.write_u32(pm+0x18,cy); };
    U("PeekMessageA",5,[fillMsg](Cpu&c){ g_pmN++; if(g_pmN==1){ std::printf("  [ref] first pump at reads=%llu\n",(unsigned long long)g_readN); win_activate_once(); }
        // PHYSICAL CONTROLS: hooked to the PUMP, not to blits. They used to be
        // read only from the per-frame body, itself triggered by
        // BitBlt/StretchBlt. On a screen where D2 stops redrawing, the
        // controller was therefore never sampled at all and no click was
        // injected — which made the pause menu's "Save and exit" impossible
        // to click, even though the same buttons work fine on character
        // creation or the video options screens, which the game keeps
        // redrawing. Chain effect: without a clean exit, D2 never replaces
        // the 335-byte stub written at character creation with a real save,
        // and it's that stub whose reload breaks half the time.
        // The pump always runs (~4000 calls per frame), hence the ~60 Hz cap:
        // same useful rate as before, independent of rendering.
        // The remote command channel (D2CMDFILE) suffered EXACTLY the same
        // defect: read from the per-frame body, so frozen on static screens —
        // precisely the ones where remote control is needed.
        { static uint64_t lastIn=0; uint64_t nowIn=rt_now_us();
          if(nowIn-lastIn>=16000ull){ lastIn=nowIn;
              if(d2vita_input_tick) d2vita_input_tick();
              cmd_poll(); } }
        if(g_msgQ.empty()){
            // Real-clock mode: D2 paces frames by spinning on an empty pump
            // (~4000 peeks/frame measured) — each spin lap costs a trap plus a
            // scheduler round, which is what starved rendering on console.
            // Turn the spin into a 1 ms timed wait: other guest threads run,
            // and an all-blocked idle really sleeps.
            lw_peek_empty(c);   // D2_LOOPWATCH: bounds the render thread's wait
            static bool nopw=getenv("D2_NOPUMPWAIT")!=nullptr;   // diag: isolate the peek-throttle
            if(g_realclock&&g_sched&&!nopw){ static KEvent* nv=new KEvent(); nv->manual=true; nv->signaled=false;
                g_sched->wait_noresult(nv,1); }
            return 0u; }
        uint32_t mn=c.arg(2),mx=c.arg(3); GMsg m=g_msgQ.front();
        if((mn||mx) && (m.msg<mn||m.msg>mx)) return 0u;   // filtered pump: not ours
        fillMsg(c,c.arg(0),m);
        if(c.arg(4)&1) g_msgQ.pop_front();               // PM_REMOVE
        return 1u; });
    U("GetMessageA",4,[fillMsg](Cpu&c){ g_gmN++;
        if(g_msgQ.empty()) return 0u;                    // (menu loop uses Peek; 0 here = WM_QUIT)
        GMsg m=g_msgQ.front(); g_msgQ.pop_front(); fillMsg(c,c.arg(0),m);
        return m.msg==0x12?0u:1u; });
    U("DispatchMessageA",1,[&br](Cpu&c)->uint32_t{ g_dispN++;
        uint32_t pm=c.arg(0); if(!g_wndProc||!pm) return 0u;
        uint32_t hwnd=c.read_u32(pm),msg=c.read_u32(pm+4),wp=c.read_u32(pm+8),lp=c.read_u32(pm+12);
        // Hand the wndproc ecx = D2's message-handler global [Game.exe+0x37d55d8]
        // so its `cmp ecx,[that]; je` terminates the hook chain (default proc)
        // instead of call'ing an uninitialized/garbage handler pointer.
        if(g_d2base) g_wndCallEcx=c.read_u32(g_d2base+0x3d55d8);
        uint32_t retaddr=c.read_u32(c.reg(R_ESP));
        // guest stub: wndproc(hwnd,msg,wp,lp); return to call site (eax = wndproc
        // result). Data-driven round-robin slots — see guest_call_stub (SMC-safe
        // for the dynarec backend; a preemption mid-stub must not see its data
        // rewritten by the next dispatch, hence 8 slots).
        br.redirect_next(guest_call_stub(c,g_wndProc,hwnd,msg,wp,lp,retaddr));
        return 0u; });
    // Key state mirrors the injected input: D2Win widgets confirm mouse-button
    // presses via GetKeyState(VK_LBUTTON), not just the window message.
    U("GetKeyState",1,[](Cpu&c)->uint32_t{ return (g_keyState[c.arg(0)&0xff]&0x80)?0xFFFF8000u:0u; });
    U("GetAsyncKeyState",1,[](Cpu&c)->uint32_t{ return (g_keyState[c.arg(0)&0xff]&0x80)?0xFFFF8000u:0u; });
    U("GetKeyboardState",1,[](Cpu&c){ c.write(c.arg(0),g_keyState,256); return 1u; });
}
