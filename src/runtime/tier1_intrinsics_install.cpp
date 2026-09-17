// tier1_intrinsics_install.cpp -- see tier1_intrinsics_install.h.
#include "tier1_intrinsics_install.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/tick_intrinsic.h"
#include "runtime/cs_intrinsic.h"
#include "platform/vita_present.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace d2rt;

extern "C" { extern unsigned int d2rt_b5_index_on, d2rt_b5_direct_n, d2rt_b5_direct_lo; }

// ---- Tier-1 Warden-safe intrinsics --------------------------------------
// Point the dynarec trap dispatch at inline handlers for the ultra-hot
// trivial clock imports. Registration is unconditional (the D2_DISABLE_
// INTRINSICS env gates DISPATCH inside the CPU, so a self-test can flip it
// per call). Captures IAT- and GetProcAddress-resolved sites alike — both
// resolve to this one trap VA; the IAT is untouched.
//
// ---- B6: critical sections as intrinsics (D2_CSINTRIN=1) ----------
// DEFAULT = OLD PATH. Without the knob, set_intrinsic is never called for
// these three slots: today's split stays exactly as it is, without even
// an extra test. A single binary carries both legs.
// ARMING IS REFUSED if any slow-path instrument is active — each of those
// instruments lives in Bridge::trap_handler, which the intrinsic skips:
// arming would make them SILENTLY BLIND, which is worse than zero gain.
void tier1_clock_cs_intrinsics_install(Cpu* cpu, Bridge& br){
    {   uint32_t tv=br.shim_trap("KERNEL32.dll","GetTickCount");
        uint32_t wv=br.shim_trap("WINMM.dll","timeGetTime");
        if(tv) cpu->set_intrinsic(tv,&tick_intrinsic);
        if(wv) cpu->set_intrinsic(wv,&tick_intrinsic);
        // B5: both legs live in the SAME binary. Read once, here, after
        // env.txt is loaded. "0" means OFF.
        {   auto on=[](const char* n){ const char* v=getenv(n);
                                       return v&&v[0]&&!(v[0]=='0'&&!v[1]); };
            g_b5clock = on("D2_B5CLOCK") || on("D2_B5");
            if(const char* cl=getenv("D2_CLOCKLOG")) g_clockLogN=(uint32_t)strtoul(cl,nullptr,10); }
        std::printf("intrinsics: GetTickCount@0x%08x timeGetTime@0x%08x%s\n",
            tv,wv, getenv("D2_DISABLE_INTRINSICS")?" (DISABLED at dispatch)":"");
        // jpline: on Vita stdout goes nowhere — whether a leg is armed must
        // reach the console log, otherwise a dead leg would look like zero
        // gain (see oracle_cellloop.sh / inlinetrap).
        jpline("[b5] horloge groupee=%s | index direct=%s (fenetre=%u creneaux @0x%08x)",
               g_b5clock?"OUI":"non", d2rt_b5_index_on?"OUI":"non",
               d2rt_b5_direct_n, d2rt_b5_direct_lo); }

    {   // D2_CSINTRIN=1 is the default (validated in env.txt) — the arming
        // refusal below (incompatible instrumentation) stays active.
        const bool want = true;
        const char* veto=nullptr;
#ifdef PROF_COUNTERS
        veto="build PROF_COUNTERS (prof::cs_enter et le chrono par creneau vivent dans le pont)";
#endif
        if(!veto && getenv("CRITLOG"))  veto="CRITLOG (la trace enter/leave vit dans le corps de shim)";
        if(!veto && getenv("D2_WATCH")) veto="D2_WATCH (le controle d'integrite vit dans le pont)";
        if(!veto && getenv("TRAPTAG"))  veto="TRAPTAG (le journal des creneaux vit dans le pont)";
        if(want && veto){
            char m[200]; std::snprintf(m,sizeof m,
                "csintrin: D2_CSINTRIN=1 REFUSE — %s ; chemin de shim conserve",veto);
            std::printf("%s\n",m); d2vita_progress(m);
        } else if(want){
            // shim_trap_EXISTING: allocate nothing. 1.14d does NOT import
            // TryEnterCriticalSection; an allocating shim_trap() would have
            // opened a new slot and SHIFTED every slot allocated afterward
            // (the native blit, among others) by 16 bytes. The armed leg of
            // the A/B would then differ from the control by more than just
            // the knob. The D2_CSTEST bench, however, allocates then arms
            // explicitly.
            uint32_t ev=br.shim_trap_existing("KERNEL32.dll","EnterCriticalSection");
            uint32_t lv=br.shim_trap_existing("KERNEL32.dll","LeaveCriticalSection");
            uint32_t yv=br.shim_trap_existing("KERNEL32.dll","TryEnterCriticalSection");
            if(ev) cpu->set_intrinsic(ev,&cs_enter_intrinsic);
            if(lv) cpu->set_intrinsic(lv,&cs_leave_intrinsic);
            if(yv) cpu->set_intrinsic(yv,&cs_try_intrinsic);
            g_csIntrin = (ev||lv||yv);
            char m[200]; std::snprintf(m,sizeof m,
                "csintrin: Enter@0x%08x Leave@0x%08x TryEnter@0x%08x %s",
                ev,lv,yv, g_csIntrin?"ARME":"AUCUN CRENEAU (rien a armer)");
            std::printf("%s\n",m); d2vita_progress(m);
        }
    }
}
