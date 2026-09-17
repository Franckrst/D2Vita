// scheduler_backend.cpp -- see scheduler_backend.h.
//
// ---- Scheduling backend -----------------------------------------------------
// D2SCHED=native (DEFAULT) | coop. The mode is stamped into boot_progress AND
// crash.log at construction time — a native run must never be
// indistinguishable from a coop run (the "diagnostic that lies" family of
// bug). This is the ONLY assignment point for g_sched/g_schedCoop: the
// invariant "always assigned together, g_schedCoop null outside coop" is
// structural.
//
// This port targets NATIVE only. That is the only supported GAME
// configuration — the one that runs on console, online, and whose
// regressions are validated under real load.
//
// coop is not removed, and this isn't half-heartedness: it remains the
// ORACLE scheduler. Many scripts under tools/ depend on it — they set
// D2_VIRTCLOCK, which native refuses by construction (see below) — and among
// them the image-fidelity oracles, the only tools able to prove a change
// hasn't moved a single pixel. Removing them to "finish the cleanup" would
// cost the proof, not just code.
//
// D2SCHED=coop therefore stays accepted, but as a BENCH INSTRUMENT and never
// as a game configuration: the stamp says so explicitly, so an accidental
// coop run can never read as a nominal one.
#include "scheduler_backend.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_thread.h"
#include "runtime/sched_cooperative.h"
#include "runtime/sched_native.h"
#include "runtime/emutls_probe.h"
#include "runtime/guest_thread_ctx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
using namespace d2rt;
extern "C" void dyn86_set_native(int);   // also declared in tools/rt_boot.cpp

ThreadScheduler* make_scheduler(Cpu* cpu, Bridge* br,
                                       uint32_t stacks, uint32_t stack_each, uint32_t tibs){
    const char* m=getenv("D2SCHED");
    const bool wantCoop = m && std::strcmp(m,"coop")==0;
    if(m && !wantCoop && std::strcmp(m,"native")!=0){
        d2_crashlog("FATAL: D2SCHED=%s inconnu (native|coop)", m); _exit(2); }
    g_schedNative = !wantCoop;   // D2SCHED absent => natif
    if(g_schedNative){
        // Invariant: native = real-clock by construction.
        // frozen strings: bench assertions grep these — do not reword.
        //
        if(getenv("D2_VIRTCLOCK")){ d2_crashlog("FATAL: D2SCHED=native + D2_VIRTCLOCK incompatibles (le natif est real-clock par construction)"); _exit(2); }
        // Multi-emu probe: native requires CpuBox86. probe_emu stays in the
        // append-only registry (cpu.h: never freed, request_stop and the
        // watchdog iterate it) — a documented, ONE-TIME ~4 KiB leak; the first
        // CreateThread creates its own, simpler than reusing this one.
        void* probe_emu = cpu->thread_emu_create();
        if(!probe_emu){ d2_crashlog("FATAL: D2SCHED=native exige le backend CpuBox86 (multi-emu)"); _exit(2); }
        // TLS probe: emutls must be ACTIVE — two pthreads must see DIFFERENT
        // addresses for the thread_local sentinel (an inert emutls would make
        // t_cur/the emu binding SHARED = silent corruption).
        { int tlsrc=d2rt_tls_probe();
          if(tlsrc>0){ d2_crashlog("FATAL: D2SCHED=native — pthread_create de la sonde TLS a echoue (rc=%d)", tlsrc); _exit(2); }
          if(tlsrc){ d2_crashlog("FATAL: D2SCHED=native mais TLS par fil INERTE (emutls mono-thread — verifier -Wl,-u,pthread_cancel du build)"); _exit(2); } }
        dyn86_set_native(1);
        NativeScheduler* ns = new NativeScheduler(cpu,br,stacks,stack_each,tibs);
        g_native_sched = ns;                   // BEFORE any create_thread (runner_tramp backref)
        g_sched = ns; wx86_set_scheduler(ns); /* g_schedCoop stays null — structural */
        // Server-thread offload: the engine knows no module names, so WE name
        // Game.exe and resolve the server thread's RVA to an absolute guest
        // address. A null base (modules not loaded yet) => null entry => the
        // offload stays disarmed, exactly as before.
        // D2_COEUR_SERVEUR_RVA=<hex>: entry RVA of the server thread in
        // Game.exe; default below.
        // 0x1a550 = entry point of Storm's I/O thread (0x41a550 in Game.exe
        // 1.14d), "thread 4" in the "fils:" lines. This address belongs to a
        // Blizzard DLL: it has no business in a generic engine, which is why
        // it lives here.
        { uint32_t rva = 0x1a550u;
          const char* e = getenv("WX86_COEUR_SERVEUR_RVA"); if(!e) e = getenv("D2_COEUR_SERVEUR_RVA");
          if(e && *e) rva = (uint32_t)strtoul(e,nullptr,16);
          ns->set_server_thread_entry(rva && g_d2base ? g_d2base + rva : 0u); }
        g_rtwant = true;                       // natif = real-clock par construction
        // The PREFIX "sched: backend=NATIVE" is a bench contract:
        // tools/rt_boot_arm_check.sh (native mode) greps it verbatim. The
        // parenthetical part is free-form description — reword the
        // parenthetical, NEVER the prefix.
        d2_crashlog("sched: backend=NATIVE (GIL sur les shims, code traduit sans GIL ; mono-coeur epingle sur Vita)");
        return ns;
    }
    // Starvation self-test: NATIVE-ONLY — under coop the safety net doesn't
    // exist, so a coop "PASS" would be a bench lie. FATAL refusal, the
    // repo-consistent shape for incompatible env combos (see D2_VIRTCLOCK +
    // native above: a clean FATAL, rc=2) — never a silent no-op, so a
    // misconfigured bench can't mistake this refusal for a PASS (rc != 0).
    { const char* ft=getenv("D2_FAMINETEST");
      if(ft && *ft=='1'){
          d2_crashlog("FATAL: D2_FAMINETEST exige D2SCHED=native (auto-test famine natif-seulement)"); _exit(2); } }
    // The parenthetical says this is NOT the game config: a bench log reread
    // months later must not suggest the shipped game was being measured. (No
    // script greps this text — only the "sched: backend=NATIVE" prefix above
    // is a contract.)
    d2_crashlog("sched: backend=coop (INSTRUMENT DE BANC deterministe — PAS la config de jeu supportee, cf. D2SCHED)");
    CooperativeScheduler* cs = new CooperativeScheduler(cpu,br,stacks,stack_each,tibs);
    g_sched = cs; g_schedCoop = cs; wx86_set_scheduler(cs);
    return cs;
}
