// src/platform/d2_boot_config.cpp — Vita-only. Diablo II boot-time
// configuration: compact memory layout, baked environment variables (Vita has
// no shell/env), the frame-scripted input that drives the boot to Rogue
// Encampment, and diagnostic probes.
//
// Kept separate from vita_present.cpp (generic presentation/platform glue):
// this file is entirely game-specific and will never move into the engine.
// d2vita_platform_init() runs once at boot, so splitting it into its own
// translation unit costs no inlining on any hot path.
#ifdef __vita__
#include "runtime/gil.h"
#include <malloc.h>
#include "platform/vita_present.h"
#include "platform/vita_net.h"
#include "platform/vita_kb.h"
#include "crashreport/cr_boot.h"        // boot-time crash-report hooks
#include "platform/vita_host.h"        // engine: log, cores, sleep (CONSOLE-specific)
#include "runtime/host_clock.h"        // engine: monotonic host clock

#include <cstdlib>
#include <cstring>
#include <cctype>              // toupper: used to reject secrets in env.txt
#include <cstdio>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <cmath>
#include <strings.h>
#include <pthread.h>

extern "C" long long d2vita_wall_unix(void);
const char* d2vita_platform_init() {
    // No clock request = the app runs at the default 333 MHz ARM (power
    // management may drop it further). Clocks are requested (and logged)
    // after the boot_progress fresh-run wipe further below, otherwise that
    // wipe would erase the log line.
    // Baked runtime config: the Vita build has no shell/env. Drives the boot
    // to the game world via the compact single-arena memory model and the
    // frame-scheduled input script that reaches Rogue Encampment (matches
    // tools/rt_gameplay_arm_check.sh). Saves and data live under
    // ux0:data/d2vita.
// Measurement patrol: 8-point loop, one move every 120 frames, frame 3600 to
// 8400. Walking scrolls the whole screen, exercising the RLE blit, light grid
// and collision at full load — an idle character would show a misleadingly
// higher frame rate than a moving one. About 20 watchdog beats are enough for
// a stable average. Generated — do not hand-edit.
// no (200,200) entry: on this save that click opens a camp chest instead of
// moving the character, which stalls the script.
#define D2VITA_PATROL \
    ",3720:move:600:200,3730:ldown:600:200,3740:lup:600:200" \
    ",3840:move:660:330,3850:ldown:660:330,3860:lup:660:330" \
    ",3960:move:600:430,3970:ldown:600:430,3980:lup:600:430" \
    ",4080:move:200:430,4090:ldown:200:430,4100:lup:200:430" \
    ",4200:move:140:330,4210:ldown:140:330,4220:lup:140:330" \
    ",4320:move:400:180,4330:ldown:400:180,4340:lup:400:180" \
    ",4440:move:400:420,4450:ldown:400:420,4460:lup:400:420" \
    ",4560:move:200:200,4570:ldown:200:200,4580:lup:200:200" \
    ",4680:move:600:200,4690:ldown:600:200,4700:lup:600:200" \
    ",4800:move:660:330,4810:ldown:660:330,4820:lup:660:330" \
    ",4920:move:600:430,4930:ldown:600:430,4940:lup:600:430" \
    ",5040:move:200:430,5050:ldown:200:430,5060:lup:200:430" \
    ",5160:move:140:330,5170:ldown:140:330,5180:lup:140:330" \
    ",5280:move:400:180,5290:ldown:400:180,5300:lup:400:180" \
    ",5400:move:400:420,5410:ldown:400:420,5420:lup:400:420" \
    ",5520:move:200:200,5530:ldown:200:200,5540:lup:200:200" \
    ",5640:move:600:200,5650:ldown:600:200,5660:lup:600:200" \
    ",5760:move:660:330,5770:ldown:660:330,5780:lup:660:330" \
    ",5880:move:600:430,5890:ldown:600:430,5900:lup:600:430" \
    ",6000:move:200:430,6010:ldown:200:430,6020:lup:200:430" \
    ",6120:move:140:330,6130:ldown:140:330,6140:lup:140:330" \
    ",6240:move:400:180,6250:ldown:400:180,6260:lup:400:180" \
    ",6360:move:400:420,6370:ldown:400:420,6380:lup:400:420" \
    ",6480:move:200:200,6490:ldown:200:200,6500:lup:200:200" \
    ",6600:move:600:200,6610:ldown:600:200,6620:lup:600:200" \
    ",6720:move:660:330,6730:ldown:660:330,6740:lup:660:330" \
    ",6840:move:600:430,6850:ldown:600:430,6860:lup:600:430" \
    ",6960:move:200:430,6970:ldown:200:430,6980:lup:200:430" \
    ",7080:move:140:330,7090:ldown:140:330,7100:lup:140:330" \
    ",7200:move:400:180,7210:ldown:400:180,7220:lup:400:180" \
    ",7320:move:400:420,7330:ldown:400:420,7340:lup:400:420" \
    ",7440:move:200:200,7450:ldown:200:200,7460:lup:200:200" \
    ",7560:move:600:200,7570:ldown:600:200,7580:lup:600:200" \
    ",7680:move:660:330,7690:ldown:660:330,7700:lup:660:330" \
    ",7800:move:600:430,7810:ldown:600:430,7820:lup:600:430" \
    ",7920:move:200:430,7930:ldown:200:430,7940:lup:200:430" \
    ",8040:move:140:330,8050:ldown:140:330,8060:lup:140:330" \
    ",8160:move:400:180,8170:ldown:400:180,8180:lup:400:180" \
    ",8280:move:400:420,8290:ldown:400:420,8300:lup:400:420"

    // Gated to measurement flavors (D2VPK_TAG) only: a normal play build must
    // never drive its own input. Covers the whole script — menu navigation
    // and patrol alike — since a partial guard would leave the menu-clicking
    // active in every build, including the one shipped to play.
#ifdef D2VPK_TAG
    static const char* SCRIPT =
        "300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308,"
        "1500:move:400:300,1550:ldown:400:300,1560:lup:400:300,"
        "2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65,"
        // Several spaced-out Enter presses: a single one could land on the
        // wrong screen and exit D2 cleanly from the menu. Repeating is
        // harmless when the expected screen isn't there.
        "2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13,"
        "3000:keydown:13,3020:keyup:13,3200:keydown:13,3220:keyup:13,"
        "3400:keydown:13,3420:keyup:13"
        // --- Patrol: this is what actually exercises rendering cost. ---
        // Walking scrolls the whole screen, exercising the RLE blit, light
        // grid and collision; an idle character measures almost nothing.
        // 8-point loop, one move every 120 frames, frame 3600 to 12000 (~70
        // moves back to back).
        D2VITA_PATROL
        ;
#else
    static const char* SCRIPT = "";
#endif
    setenv("GAMEEXE", "1", 1);
    setenv("D2ARGS", "game.exe -3dfx", 1);   // Glide is the default renderer; see the perf block below
    setenv("D2LAYOUT", "compact", 1);
    setenv("D2ARENA", "12700000", 1);   // 295 MiB single block: covers the packed
                                          // 1.14d compact span up to the 0x11700000
                                          // trap ceiling + 16 MiB membase-rounding
                                          // slack (~330 MiB real-Vita user budget).
                                          // Grew with the 8 MiB heap-ceiling raise in
                                          // apply_compact_layout (rt_boot.cpp) — the two
                                          // MUST move together or the span overflows the
                                          // arena and H(va)=va+membase leaves the block.
    // MAXFRAMES / MAXSW deliberately NOT baked: unset = unlimited. A real play
    // session must never self-terminate (the old baked MAXFRAMES=40000 ended
    // every session after ~30-45 min, losing unsaved progress). The test
    // harness sets its own caps via env.txt / qemu env when it needs them.
#ifdef D2VPK_TAG
    setenv("D2WRITE", "ux0:data/d2vita/save_" D2VPK_TAG, 1);
    {   // D2VPK_OFF = comma list e.g. "NATIVEBLIT,NATIVELIGHT" -> each forced to 0.
        static const char off[] = D2VPK_OFF;
        char buf[192]; snprintf(buf, sizeof buf, "%s", off);
        for (char* t = strtok(buf, ","); t; t = strtok(nullptr, ",")) {
            if (*t) { setenv(t, "0", 1); char m[96];
                      snprintf(m, sizeof m, "saveur %s: %s=0", D2VPK_TAG, t);
                      d2vita_progress(m); } }
    }
#else
    setenv("D2WRITE", "ux0:data/d2vita/save", 1);
#endif
    // Sprite-corruption crash family (Gfx.cpp:1632 / CelDataHash.cpp:1420):
    // triggered by periodic preemption. The dynarec interrupts a thread at a
    // translated block entry ~500 times/second; something doesn't survive
    // that context switch and comes back as corrupt sprite data.
    //
    // This is a mitigation, not a root-cause fix: it removes the window, not
    // the underlying defect.
    //
    // Known risk: without a periodic quantum, threads only hand off at a
    // blocking wait. A guest loop that never calls an import would hang
    // forever. D2's own code yields via its imports (PeekMessage, Sleep,
    // critical sections); if an untested code path ever freezes, fall back
    // to the old behavior without a rebuild via env.txt `QUANTUM=300000`.
    setenv("QUANTUM", "2000000000", 1);
    setenv("D2_ROOMGUARD", "1", 1);       // halt-316 family fix: repair-at-first-contact
                                          // of un-rebased room2 nodes. env.txt
                                          // D2_ROOMGUARD=0 disables.
    setenv("D2SCRIPT", SCRIPT, 1);
    setenv("D2VITA_PRESENT", "1", 1);     // rt_boot: present each frame to SceDisplay
    // (1.13c-era D2_CMPHOOK / D2_TOLERATE knobs dropped: both act on
    // D2CMP.dll, absent from the 1.14d monolith — the hooks would no-op.)

    // These are the default settings. Where the getenv() call site lives in
    // d2vita, the dead branch is removed directly at the site instead (see
    // native_hooks_cellengine.cpp, phase_hooks.cpp, vita_gxm.cpp, rt_boot.cpp).
    // Only the two variables whose getenv() lives INSIDE winx86 (WX86_B5:
    // cpu_box86.cpp, WX86_YIELD: sched_native.cpp) are set here: winx86 is the
    // generic engine shared with other ports and must not carry game-specific
    // branches, so setenv from d2vita is the only way to apply the default
    // without touching it. WX86_FRAMEPROF / D2_FRAMEUS / D2_IOSTAT stay absent
    // here: they're diagnostics, never a game default.
    setenv("WX86_B5",    "1", 1);  // dynarec step 5 (index B5) — read in winx86/cpu_box86.cpp
    setenv("WX86_YIELD", "2", 1);  // yield every 2 rounds — read in winx86/sched_native.cpp
    // Crash-report collector: reads the PREVIOUS run's
    // reports/session.txt and D2VITA_PROGRESS_PATH, copies any evidence
    // (crash type, addresses, a matching psp2core dump if one exists) into
    // the outbox, then starts this run's own session record — all of it
    // BEFORE the rotation immediately below can destroy that evidence.
    // This must stay the LAST thing before that rotation: anything inserted
    // between this call and the sceIoRemove right after it that itself
    // touches D2VITA_PROGRESS_PATH or reports/session.txt would reopen
    // exactly the gap this call exists to close.
    d2cr::d2cr_boot_collect();

    // Optional overrides without a rebuild: ux0:data/d2vita/env.txt, one
    // KEY=VALUE per line (e.g. D2_EIPTRAP=01d894fb, D2_VIRTCLOCK=1). Read
    // AFTER the baked defaults so the file wins.
    // Fresh-run remove happens BEFORE these logs: doing it after would erase
    // the "env.txt: KEY=VAL" lines just written, making an applied override
    // indistinguishable from an ignored one.
    // Rotate instead of erase: a crash's log would otherwise be lost as soon
    // as the next run starts (a core dump doesn't include the guest stack).
    // The previous run's log is kept as _prev.txt — just a rename at boot,
    // and it removes any need to wait before relaunching after a crash.
    sceIoRemove(D2VITA_PROGRESS_PATH "_prev");
    sceIoRename(D2VITA_PROGRESS_PATH, D2VITA_PROGRESS_PATH "_prev");
    sceIoRemove(D2VITA_PROGRESS_PATH);                  // fresh run
    if (FILE* ef = fopen("ux0:data/d2vita/env.txt", "r")) {
        // 8 KiB per line, not 128: D2SCRIPT is the one large value, and
        // being able to inject it via env.txt allows driving an input script
        // remotely without a rebuild. A short buffer would silently truncate
        // long lines — the worst possible failure mode for a config file.
        static char line[8192];

        while (fgets(line, sizeof line, ef)) {
            char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            char* eq = strchr(line, '=');
            if (!eq || eq == line || line[0] == '#') continue;
            *eq = 0;
            // No secrets in env.txt: every line is echoed verbatim into
            // boot_progress*.txt, which tools/vita_drive.sh pulls off the
            // device and which ends up pasted into reports. Any CD key — or
            // any name containing KEY/SECRET/PASS — is REFUSED outright, not
            // just masked: masking wouldn't help since the game would still
            // read it into the process environment, visible elsewhere. The
            // refusal message never echoes the value.
            {   char up[128]; size_t li = 0;
                for (; line[li] && li + 1 < sizeof up; li++)
                    up[li] = (char)toupper((unsigned char)line[li]);
                up[li] = 0;
                if (strstr(up, "KEY") || strstr(up, "SECRET") || strstr(up, "PASS")) {
                    char m2[160]; snprintf(m2, sizeof m2,
                        "env.txt: %s REFUSE (secret interdit dans env.txt)", line);
                    d2vita_progress(m2);
                    continue; } }
            setenv(line, eq + 1, 1);
            char m[160]; snprintf(m, sizeof m, "env.txt: %s=%s", line, eq + 1);
            d2vita_progress(m);
        }
        fclose(ef);
    }
    // D2NET defaults to ON, matching the original PC game: the network
    // stack is simply available, the player still has to pick Battle.net
    // from the in-game menu to use it. D2NET=0 in env.txt opts back out to
    // solo-only. Every call site tests only for the variable's PRESENCE, so
    // the falsy values below are normalized to a real absence, right after
    // env.txt and before any reader.
    if (!getenv("D2NET")) {
        setenv("D2NET", "1", 1);
        d2vita_progress("reseau: D2NET actif par defaut (D2NET=0 dans env.txt pour repasser en solo uniquement)");
    }
    if (const char* n = getenv("D2NET")) {
        if (!*n || !strcmp(n, "0") || !strcasecmp(n, "non") || !strcasecmp(n, "off") || !strcasecmp(n, "false")) {
            unsetenv("D2NET");
            d2vita_progress("reseau: D2NET=0 -> reseau COUPE (desactive explicitement)");
        }
    }
    // D2_SCHEDPROBE=1: does the Vita kernel round-robin same-priority threads
    // on one core? Must be known before trusting the native backend: under
    // run-to-block, a guest thread that loops without ever calling an import
    // starves its siblings. Two threads pinned to USER_0 at the same priority
    // as the NativeScheduler runners (sched_native.cpp runner()): A spins, B
    // increments; B progressing means time-sliced preemption.
    //
    // Verdict: RUN-TO-BLOCK. The Vita kernel does not time-slice equal-
    // priority threads sharing a core, so anti-starvation nets are mandatory
    // before any native online play (solo remains fine: D2 yields via its own
    // imports). Vita3K's own scheduler answers ROUND-ROBIN instead — its host
    // thread scheduler is not the real kernel, so it's an indication only;
    // only real hardware settles the verdict.
    //
    // Later phases probe related kernel hypotheses: v3 whether a
    // sceKernelDelayThread nap yields the core to a ready same-priority
    // thread, v4 whether an affinity-0 thread is schedulable on a free core
    // while a spinner holds USER_0, v5 which cores are actually schedulable
    // for this app. See each phase's own comment below.
    //
    // Activates on VALUE '1' (same convention as D2_ROOMGUARD): the natural
    // way to disable remotely is D2_SCHEDPROBE=0 in env.txt, and it must
    // work — presence alone used to be enough to arm the probe.
    if (getenv("D2_SCHEDPROBE") && *getenv("D2_SCHEDPROBE") == '1') {
        // Named durations — the two-core threshold is DERIVED from them:
        // duel/calib = 2, so a full core during the duel counts ~2x calib,
        // two cores ~4x; INVALID sits halfway at 3x. Changing a duration
        // recalculates the threshold.
        enum : uint32_t { PROBE_CALIB_US = 1000 * 1000,      // phase 1: 1 s
                          PROBE_DUEL_US  = 2 * 1000 * 1000,  // phase 2: 2 s
                          PROBE_V3_US    = 2 * 1000 * 1000,  // phase 3 (nap): 2 s
                          PROBE_V4_US    = 2 * 1000 * 1000,  // phase 4 (affinity 0): 2 s
                          PROBE_V5_US    = 2 * 1000 * 1000,  // phase 5 (core map): 2 s
                          PROBE_V4_SETTLE_US = 10 * 1000,    // v4/v5: A settles on USER_0 before the others
                          PROBE_NAP_US   = 1000,             // v3 nap: 1 ms = pte sched_yield floor
                          PROBE_FLOOR    = 1000 };           // minimum credible progress
        const uint32_t PROBE_INVALID_X = PROBE_DUEL_US / PROBE_CALIB_US + 1;  // 3x calib
        static volatile uint32_t probe_a = 0, probe_b = 0, probe_c = 0;   // probe_c: phase 4 (v4) only
        static volatile int probe_stop = 0;
        // Self-terminating design: under run-to-block, a spinner pinned
        // alongside the main thread never lets the main thread run again to
        // set probe_stop, which would wedge the probe on its own judge. Each
        // counter instead bounds its loop against a deadline the CREATOR
        // posts (absolute process time) before starting each phase: a
        // running counter exits on its own at the deadline, freeing the
        // core so the main thread gets rescheduled and can collect verdicts
        // and joins.
        //
        // The deadline is a WALL-CLOCK anchor, not "time since this thread
        // started": a witness released late (e.g. behind another spinner)
        // sees the deadline already past and exits immediately, so the
        // measurement window doesn't stretch and its join still succeeds
        // (the 500 ms join timeout remains the safety net for threads that
        // never get scheduled at all). The deadline clock is read every 4096
        // iterations (~40 us of a core): under 1% overhead, and the same
        // overhead applies to calibration and to the counters, so derived
        // floors stay consistent. probe_stop is still the nominal stop (a
        // healthy main thread sets it ~200 ms before the deadline), so
        // normal-case measurement semantics are unchanged.
        //
        // The 64-bit deadline write is non-atomic on ARM32: it's posted
        // before any thread of the phase starts and read only by that
        // phase's threads. Only an abandoned thread from a previous phase
        // could read during a concurrent post, and such a thread can only
        // exist after a join timeout — by which point its own deadline has
        // already elapsed (an accepted residual risk on error paths only).
        enum : uint32_t { PROBE_DEADLINE_SLACK_US = 200 * 1000 };
        static SceUInt64 probe_deadline = 0;
        // probe_nap_every: A's nap period in spin iterations, derived from
        // phase 1's calibration to target ~1 nap per ms of spin (calib =
        // iterations/s of a full core, so calib/1000 iterations ~ 1 ms).
        // A runtime variable rather than a compile-time constant; static
        // because the Sce thread lambdas can't capture, and set only right
        // before phase 3 starts.
        static volatile uint32_t probe_nap_every = 0;
        // Sentinel value 1: never a valid Sce rc (0 = success, <0 = error)
        // nor a valid affinity mask (multiples of 0x10000, or 0). A "1"
        // surviving in the aff= log line means this thread never executed
        // its first instruction (start refused, or starved before it could
        // even pin itself). aff_c: phase 4 (v4) only.
        static volatile int aff_a = 1, aff_b = 1, aff_c = 1;
        auto cnt_a = [](SceSize, void*) -> int {
            aff_a = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                                         SCE_KERNEL_CPU_MASK_USER_0);
            uint32_t k = 0;   // never yields, never waits — exits via the deadline clock (self-termination)
            while (!probe_stop) { probe_a++;
                if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
            return 0; };
        auto cnt_b = [](SceSize, void*) -> int {
            aff_b = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                                         SCE_KERNEL_CPU_MASK_USER_0);
            uint32_t k = 0;
            while (!probe_stop) { probe_b++;
                if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
            return 0; };
        // v3: A spins but naps 1 ms every probe_nap_every iterations — the
        // exact pattern of the anti-starvation net's "nap at the seam".
        auto nap_a = [](SceSize, void*) -> int {
            aff_a = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                                         SCE_KERNEL_CPU_MASK_USER_0);
            uint32_t k = 0;
            while (!probe_stop) {
                probe_a++;
                if (++k >= probe_nap_every) { k = 0;   // deadline checked only at the nap seam (~1 ms): hot spin path untouched
                    if (sceKernelGetProcessTimeWide() >= probe_deadline) break;
                    sceKernelDelayThread(PROBE_NAP_US); }
            }
            return 0; };
        // v4 counters: no ChangeThreadCpuAffinityMask call for B — it stays
        // at the exact affinity-0 default a pthread gets (pte_osThreadCreate
        // passes cpuAffinityMask=0); C is pinned to USER_1 by its creator
        // before start (the same pattern the presentation thread uses).
        // Their first instruction reads back the effective mask: it doubles
        // as a "thread ran" marker (overwrites the sentinel value 1) and as
        // a log of what the kernel actually reports for a thread created
        // with affinity 0 (literal 0? or the creator's mask resolved at
        // creation time?).
        auto cnt_b0 = [](SceSize, void*) -> int {
            aff_b = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
            uint32_t k = 0;
            while (!probe_stop) { probe_b++;
                if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
            return 0; };
        auto cnt_c = [](SceSize, void*) -> int {
            aff_c = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
            uint32_t k = 0;
            while (!probe_stop) { probe_c++;
                if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
            return 0; };
        // Ordering invariant: the main thread is NOT pinned to USER_0 yet at
        // this point — that only happens later, in NativeScheduler::run().
        // Under run-to-block, an already-pinned main thread would freeze its
        // own DelayThread behind the spinning probe threads, so this probe
        // must run before the main thread takes any affinity. The startup
        // log line makes such a freeze self-identifying (last line = start).
        d2vita_progress("SCHEDPROBE: start (3 s)");
        // Phase 1 — calibration: B alone pinned to USER_0 for 1 s gives
        // the throughput of one full core. Without this baseline,
        // "B progresses" can't distinguish real round-robin from
        // silently-refused affinity, where A and B would each simply run
        // on their own separate core at full speed.
        // Every Create/StartThread rc is checked (a failed start during
        // the duel would look like a false RUN-TO-BLOCK; one during
        // calibration would zero out the baseline), and every finished
        // thread gets DeleteThread: WaitThreadEnd frees neither the TCB
        // nor the stack on Vita, so skipping it leaks memory every probe
        // boot.
        probe_deadline = sceKernelGetProcessTimeWide() + PROBE_CALIB_US + PROBE_DEADLINE_SLACK_US;
        SceUID b = sceKernelCreateThread("probe_cal", cnt_b, 0x10000100, 0x4000, 0, 0, nullptr);
        if (b < 0) {
            char m[96]; snprintf(m, sizeof m,
                "SCHEDPROBE: CreateThread KO (0x%08x) — pas de verdict", b);
            d2vita_progress(m);
        } else if (int rs = sceKernelStartThread(b, 0, nullptr); rs < 0) {
            char m[96]; snprintf(m, sizeof m,
                "SCHEDPROBE: StartThread calib KO (0x%08x) — pas de verdict", rs);
            d2vita_progress(m);
            sceKernelDeleteThread(b);
        } else {
            uint64_t wcal0 = sceKernelGetProcessTimeWide();
            sceKernelDelayThread(PROBE_CALIB_US);
            uint32_t calib_wall_ms = (uint32_t)((sceKernelGetProcessTimeWide() - wcal0) / 1000ull);
            uint32_t calib = probe_b; probe_stop = 1;
            sceKernelWaitThreadEnd(b, nullptr, nullptr);
            sceKernelDeleteThread(b);
            // Phase 2 — the duel: A and B, same priority as the
            // NativeScheduler runners, both pinned to USER_0, for 2 s. The
            // duel's result is hoisted out of its branches: phase 3 may only
            // speak if THIS boot's pinning was validated, i.e. the duel
            // produced a verdict (ROUND-ROBIN or RUN-TO-BLOCK). If pinning
            // silently failed, B would run at full speed on its own core,
            // producing a false SIESTE-CEDE on the very line that decides
            // whether to build the anti-starvation net.
            enum { DUEL_NONE, DUEL_INVALIDE, DUEL_OK };
            int duel = DUEL_NONE;
            probe_b = 0; probe_stop = 0;
            SceUID a2 = sceKernelCreateThread("probe_a", cnt_a, 0x10000100, 0x4000, 0, 0, nullptr);
            SceUID b2 = sceKernelCreateThread("probe_b", cnt_b, 0x10000100, 0x4000, 0, 0, nullptr);
            if (a2 < 0 || b2 < 0) {
                char m[96]; snprintf(m, sizeof m,
                    "SCHEDPROBE: CreateThread KO (0x%08x/0x%08x) — pas de verdict", a2, b2);
                d2vita_progress(m);
                if (a2 >= 0) sceKernelDeleteThread(a2);
                if (b2 >= 0) sceKernelDeleteThread(b2);
            } else {
                probe_deadline = sceKernelGetProcessTimeWide() + PROBE_DUEL_US + PROBE_DEADLINE_SLACK_US;
                int ra = sceKernelStartThread(a2, 0, nullptr);
                int rb = sceKernelStartThread(b2, 0, nullptr);
                if (ra < 0 || rb < 0) {
                    probe_stop = 1;
                    if (ra >= 0) sceKernelWaitThreadEnd(a2, nullptr, nullptr);
                    if (rb >= 0) sceKernelWaitThreadEnd(b2, nullptr, nullptr);
                    char m[96]; snprintf(m, sizeof m,
                        "SCHEDPROBE: StartThread duel KO (0x%08x/0x%08x) — pas de verdict", ra, rb);
                    d2vita_progress(m);
                    sceKernelDeleteThread(a2); sceKernelDeleteThread(b2);
                } else {
                    uint64_t w2 = sceKernelGetProcessTimeWide();
                    sceKernelDelayThread(PROBE_DUEL_US);
                    uint32_t duel_wall_ms = (uint32_t)((sceKernelGetProcessTimeWide() - w2) / 1000ull);
                    uint32_t ga = probe_a, gb = probe_b; probe_stop = 1;
                    sceKernelWaitThreadEnd(a2, nullptr, nullptr); sceKernelWaitThreadEnd(b2, nullptr, nullptr);
                    sceKernelDeleteThread(a2); sceKernelDeleteThread(b2);
                    // wallcal/wall: measured wall time of each sleep — must
                    // stay visible anywhere the main thread could be starved
                    // enough to stretch a nominal wait far longer.
                    char d[192]; snprintf(d, sizeof d,
                        "SCHEDPROBE: calib(1 coeur)=%u/s a=%u b=%u aff=0x%08x/0x%08x wallcal=%ums wall=%ums", calib, ga, gb,
                        (unsigned)aff_a, (unsigned)aff_b, calib_wall_ms, duel_wall_ms);
                    d2vita_progress(d);
                    // sum >> one core's worth = each thread got its own
                    // core, pinning didn't hold: INVALID, not ROUND-ROBIN. A
                    // below-floor calibration makes this test blind, so it
                    // reports INVALID explicitly rather than silently
                    // defaulting to ROUND-ROBIN — the one false verdict that
                    // would greenlight native online play without any
                    // anti-starvation net.
                    uint64_t sum = (uint64_t)ga + gb; const char* v;
                    if (calib < PROBE_FLOOR)
                        v = "INVALIDE (calibration nulle — sonde non auto-validee)";
                    else if (sum > (uint64_t)PROBE_INVALID_X * calib) {
                        v = "INVALIDE (somme=2 coeurs — affinite non appliquee ?)";
                        duel = DUEL_INVALIDE;
                    } else if (ga > PROBE_FLOOR && gb > PROBE_FLOOR) {
                        v = "ROUND-ROBIN (natif viable tel quel)";
                        duel = DUEL_OK;
                    } else {
                        v = "RUN-TO-BLOCK (filets anti-famine obligatoires)";
                        duel = DUEL_OK;
                    }
                    char m[96]; snprintf(m, sizeof m, "SCHEDPROBE: verdict => %s", v);
                    d2vita_progress(m);
                }
            }
            // Phase 3 — nap probe (v3): the last unverified kernel
            // hypothesis for the anti-starvation net: while a thread pinned
            // to USER_0 sleeps in sceKernelDelayThread, does a ready
            // same-priority thread on the same core get scheduled? This is
            // exactly the "nap at the seam" mechanism the anti-starvation
            // net relies on (pte sched_yield = DelayThread(1 ms), the only
            // reliable yield point). A naps 1 ms every probe_nap_every
            // iterations while B counts behind it. Under run-to-block: if
            // the nap yields, B gets scheduled on the first nap and (never
            // blocking) keeps the core, so a ~ probe_nap_every and b ~ a
            // full core's worth; if it doesn't yield, b stays 0. Runs only
            // after the duel produced a valid verdict — otherwise nothing
            // proves pinning held, and a false "nap yields" verdict would be
            // structural rather than real.
            if (calib < PROBE_FLOOR) {
                d2vita_progress("SCHEDPROBE: v3 saute (calibration invalide — pas de verdict sieste)");
            } else if (duel != DUEL_OK) {
                d2vita_progress("SCHEDPROBE: v3 saute (duel sans verdict valide — pas de verdict sieste)");
            } else {
                probe_nap_every = calib / 1000;   // ~1 nap/ms of spin (>=1 since calib >= PROBE_FLOOR)
                probe_a = 0; probe_b = 0; probe_stop = 0;   // reset between phases
                aff_a = 1; aff_b = 1;   // sentinels re-armed: a 0 inherited from the duel would lie
                d2vita_progress("SCHEDPROBE: v3 start (2 s)");
                SceUID a3 = sceKernelCreateThread("probe_v3a", nap_a, 0x10000100, 0x4000, 0, 0, nullptr);
                SceUID b3 = sceKernelCreateThread("probe_v3b", cnt_b, 0x10000100, 0x4000, 0, 0, nullptr);
                if (a3 < 0 || b3 < 0) {
                    char m[96]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v3 CreateThread KO (0x%08x/0x%08x) — pas de verdict", a3, b3);
                    d2vita_progress(m);
                    if (a3 >= 0) sceKernelDeleteThread(a3);
                    if (b3 >= 0) sceKernelDeleteThread(b3);
                } else {
                    // A starts FIRST: it must own the core before B is
                    // ready (a B started first, a pure counter, would never
                    // yield => full b = a guaranteed false "nap yields").
                    probe_deadline = sceKernelGetProcessTimeWide() + PROBE_V3_US + PROBE_DEADLINE_SLACK_US;
                    int ra = sceKernelStartThread(a3, 0, nullptr);
                    int rb = sceKernelStartThread(b3, 0, nullptr);
                    if (ra < 0 || rb < 0) {
                        probe_stop = 1;
                        if (ra >= 0) sceKernelWaitThreadEnd(a3, nullptr, nullptr);
                        if (rb >= 0) sceKernelWaitThreadEnd(b3, nullptr, nullptr);
                        char m[96]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v3 StartThread KO (0x%08x/0x%08x) — pas de verdict", ra, rb);
                        d2vita_progress(m);
                        sceKernelDeleteThread(a3); sceKernelDeleteThread(b3);
                    } else {
                        uint64_t w3 = sceKernelGetProcessTimeWide();
                        sceKernelDelayThread(PROBE_V3_US);
                        uint32_t v3_wall_ms = (uint32_t)((sceKernelGetProcessTimeWide() - w3) / 1000ull);
                        uint32_t ga = probe_a, gb = probe_b; probe_stop = 1;
                        sceKernelWaitThreadEnd(a3, nullptr, nullptr); sceKernelWaitThreadEnd(b3, nullptr, nullptr);
                        sceKernelDeleteThread(a3); sceKernelDeleteThread(b3);
                        const uint32_t v3_floor = calib / 1000;   // floor from calibration: ~1 ms of a core
                        // Start-race guard: in any legitimate scenario where
                        // B progresses, A (started first) has looped through
                        // at least one whole spin period before its first
                        // nap (the nap only comes after probe_nap_every
                        // increments, so ga >= probe_nap_every). ga below
                        // that with b past the floor means A was never
                        // scheduled: the measurement reflects a lost start
                        // race, not the nap — reported as INVALID rather
                        // than a false "nap yields".
                        const char* v3;
                        if (gb > v3_floor && ga < probe_nap_every)
                            v3 = "INVALIDE (A jamais elu — course de depart perdue ?)";
                        else if (gb > v3_floor)
                            v3 = "SIESTE-CEDE (DelayThread elit un pret de meme priorite — filet C fonde)";
                        else
                            v3 = "SIESTE-NE-CEDE-PAS (STOP filet C — repli sonde P4/approche A)";
                        char m[192]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v3 sieste a=%u b=%u aff=0x%08x/0x%08x wall=%ums => %s",
                            ga, gb, (unsigned)aff_a, (unsigned)aff_b, v3_wall_ms, v3);
                        d2vita_progress(m);
                    }
                }
            }
            // Phase 4 — affinity-0 probe (v4): on real hardware, is a thread
            // created with affinity 0 (the pthread default) schedulable on a
            // free core while a spinner holds USER_0? Three threads, 2 s:
            //   A (cnt_a)  : pins to USER_0 and spins — the starvation source.
            //   B (cnt_b0) : created with affinity 0, no explicit pin (the
            //                exact pthread default) — counter.
            //   C (cnt_c)  : pinned to USER_1 by its creator before start —
            //                counter for the proposed fix, measured in the
            //                same run.
            // Verdicts (floors from phase 1's calibration):
            //   b > floor          => AFF0-ELIGIBLE (hypothesis refuted);
            //   b ~ 0, c > floor   => AFF0-CAPTIVE (hypothesis confirmed,
            //                         explicit pinning works as a fix);
            //   c ~ 0 too         => INVALID (deeper problem);
            //   a ~ 0             => INVALID (lost the startup race).
            //
            // Caveat: the main thread isn't pinned yet at this point in boot,
            // so if "affinity 0" means "inherit the creator's mask", B
            // inherits the main thread's default mask here and would read as
            // AFF0-ELIGIBLE without ruling out captivity for a thread created
            // *after* the main thread is pinned to USER_0 (as the real
            // heartbeat is). The fix under test — explicit pinning, and
            // creating the heartbeat before the main thread gets pinned —
            // covers both possible semantics either way. Vita3K's own
            // (round-robin) scheduler always reads AFF0-ELIGIBLE here — an
            // indication only; real hardware is the only authority.
            if (calib < PROBE_FLOOR) {
                d2vita_progress("SCHEDPROBE: v4 saute (calibration invalide — pas de verdict aff0)");
            } else if (duel != DUEL_OK) {
                d2vita_progress("SCHEDPROBE: v4 saute (duel sans verdict valide — pas de verdict aff0)");
            } else {
                probe_a = 0; probe_b = 0; probe_c = 0; probe_stop = 0;   // reset between phases
                aff_a = 1; aff_b = 1; aff_c = 1;   // sentinels re-armed: a v2/v3 leftover would lie
                d2vita_progress("SCHEDPROBE: v4 start (2 s)");
                SceUID a4 = sceKernelCreateThread("probe_v4a", cnt_a, 0x10000100, 0x4000, 0, 0, nullptr);
                SceUID b4 = sceKernelCreateThread("probe_v4b", cnt_b0, 0x10000100, 0x4000, 0, 0, nullptr);
                SceUID c4 = sceKernelCreateThread("probe_v4c", cnt_c, 0x10000100, 0x4000, 0, 0, nullptr);
                if (a4 < 0 || b4 < 0 || c4 < 0) {
                    char m[112]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v4 CreateThread KO (0x%08x/0x%08x/0x%08x) — pas de verdict", a4, b4, c4);
                    d2vita_progress(m);
                    if (a4 >= 0) sceKernelDeleteThread(a4);
                    if (b4 >= 0) sceKernelDeleteThread(b4);
                    if (c4 >= 0) sceKernelDeleteThread(c4);
                } else if (int rc_c = sceKernelChangeThreadCpuAffinityMask(c4, SCE_KERNEL_CPU_MASK_USER_1); rc_c < 0) {
                    // An unpinned C loses the AFF0-CAPTIVE witness: b~0 and
                    // c~0 would become indistinguishable from INVALID. No
                    // verdict is issued on a compromised witness.
                    char m[112]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v4 epinglage C USER_1 KO (0x%08x) — pas de verdict", (unsigned)rc_c);
                    d2vita_progress(m);
                    sceKernelDeleteThread(a4); sceKernelDeleteThread(b4); sceKernelDeleteThread(c4);
                } else {
                    // A starts first and gets PROBE_V4_SETTLE_US to own
                    // USER_0 before B: if affinity-0 threads were captive to
                    // USER_0, a B started first would win the core and count
                    // at full speed, guaranteeing a false AFF0-ELIGIBLE. The
                    // floor guard below makes a lost start race
                    // self-identifying.
                    probe_deadline = sceKernelGetProcessTimeWide() + PROBE_V4_SETTLE_US + PROBE_V4_US + PROBE_DEADLINE_SLACK_US;
                    int ra = sceKernelStartThread(a4, 0, nullptr);
                    if (ra >= 0) sceKernelDelayThread(PROBE_V4_SETTLE_US);
                    int rb = sceKernelStartThread(b4, 0, nullptr);
                    int rc2 = sceKernelStartThread(c4, 0, nullptr);
                    if (ra < 0 || rb < 0 || rc2 < 0) {
                        probe_stop = 1;
                        if (ra >= 0) sceKernelWaitThreadEnd(a4, nullptr, nullptr);
                        if (rb >= 0) sceKernelWaitThreadEnd(b4, nullptr, nullptr);
                        if (rc2 >= 0) sceKernelWaitThreadEnd(c4, nullptr, nullptr);
                        char m[112]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v4 StartThread KO (0x%08x/0x%08x/0x%08x) — pas de verdict", ra, rb, rc2);
                        d2vita_progress(m);
                        sceKernelDeleteThread(a4); sceKernelDeleteThread(b4); sceKernelDeleteThread(c4);
                    } else {
                        uint64_t w4 = sceKernelGetProcessTimeWide();
                        sceKernelDelayThread(PROBE_V4_US);
                        uint32_t v4_wall_ms = (uint32_t)((sceKernelGetProcessTimeWide() - w4) / 1000ull);
                        uint32_t ga = probe_a, gb = probe_b, gc = probe_c; probe_stop = 1;
                        // Join order: A first — while A spins on USER_0, a
                        // captive B can't even read probe_stop; A exiting
                        // frees the core and unblocks B.
                        sceKernelWaitThreadEnd(a4, nullptr, nullptr);
                        sceKernelWaitThreadEnd(b4, nullptr, nullptr);
                        sceKernelWaitThreadEnd(c4, nullptr, nullptr);
                        sceKernelDeleteThread(a4); sceKernelDeleteThread(b4); sceKernelDeleteThread(c4);
                        const uint32_t v4_floor = calib / 1000;   // floor from calibration: ~1 ms of a core
                        // Start-race guard: A is a pure spinner pinned to
                        // USER_0, so it clears the floor under any
                        // legitimate scenario. a below floor means A never
                        // got installed — the measurement reflects a lost
                        // race, not affinity — reported as INVALID rather
                        // than a false AFF0-ELIGIBLE.
                        const char* v4;
                        if (ga < v4_floor)
                            v4 = "INVALIDE (A jamais installe — course de depart perdue ?)";
                        else if (gb > v4_floor)
                            v4 = "AFF0-ELIGIBLE (aff 0 court sur coeur libre — hypothese §9.4 refutee)";
                        else if (gc > v4_floor)
                            v4 = "AFF0-CAPTIF (aff 0 captif sous spinner USER_0 — epinglage explicite valide)";
                        else
                            v4 = "INVALIDE (C epingle USER_1 ne court pas non plus — probleme plus profond)";
                        // rcA/maskB/maskC are different KINDS of values:
                        // aff_a is a Change...AffinityMask return code
                        // (0x0 = success), aff_b/aff_c are masks read back
                        // via Get (0x0 there would be a null mask). Keeping
                        // them distinct avoids misreading a uniform
                        // 0x0/0x0/0x0 on console; sentinel value 1 ("thread
                        // never ran") stays legible in all three.
                        char m[224]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v4 aff0 a=%u b=%u c=%u rcA=0x%08x maskB=0x%08x maskC=0x%08x wall=%ums => %s",
                            ga, gb, gc, (unsigned)aff_a, (unsigned)aff_b, (unsigned)aff_c, v4_wall_ms, v4);
                        d2vita_progress(m);
                    }
                }
            }
            // Phase 5 — core-map probe (v5). Two questions left open by v4:
            //   1. Is USER_1 an actually schedulable core, distinct from
            //      USER_0, for this app?
            //   2. Does the pte path (pthread_create) even start on
            //      hardware? (it's born at Sce priority 191 — the least
            //      urgent — and pte_osThreadStart discards the start rc)
            // Five threads, 2 s:
            //   A  (cnt_a)    : pinned to USER_0, spins — the starvation source.
            //   C0 (cnt_slot) : pinned to USER_0 by its creator — negative
            //                   control: under run-to-block, a counter
            //                   sharing A's core must never clear the floor.
            //                   c0 > floor means creator-pinning didn't hold
            //                   this boot, so C1/C2 would be untrustworthy
            //                   (flagged as C0-ANORMAL in the verdict).
            //   C1 (cnt_slot) : pinned to USER_1 by its creator — the actual
            //                   test: a live free core should run at full
            //                   speed; below floor means USER_1 isn't a live
            //                   core in this app's configuration.
            //   C2 (cnt_slot) : pinned to USER_2 by its creator — a core
            //                   already known live (used by presentation):
            //                   positive control.
            //   P  (pte_cnt)  : created via pthread_create with no affinity
            //                   call — the exact reproduction of how the
            //                   anti-starvation heartbeat is born. Starts
            //                   alone on otherwise-idle cores so p0 shows
            //                   whether it starts at all; its progress after
            //                   that, under three spinners, shows whether it
            //                   stays eligible at priority 191.
            // Info-only lines (never a verdict on their own — a bare mask
            // readback can be misleading, so these are printed without being
            // judged): active core mask, and each thread's actual running
            // core sampled while they're still spinning. P's kernel thread
            // ID is recovered via the pte handle layout (offset 0 = SceUID)
            // — probe-only, never used in production.
            //
            // Teardown: A joins first (frees USER_0 so C0 can read
            // probe_stop); the three C joins are bounded (500 ms) — if a
            // core is dead for this app (exactly the hypothesis under test),
            // the thread pinned to it can never exit, and an unbounded join
            // would wedge the boot and lose the verdict that was the point
            // of the probe. On timeout: the thread is abandoned and logged
            // (a bounded leak of its stack + TCB) rather than deleted (a
            // non-dormant thread refuses deletion), and the verdict still
            // prints normally afterward. P is joined only if it proved it's
            // running (gp>0); otherwise it's detached, since joining a
            // possibly-never-started thread would reproduce the same wedge.
            if (calib < PROBE_FLOOR) {
                d2vita_progress("SCHEDPROBE: v5 saute (calibration invalide — pas de verdict coremap)");
            } else if (duel != DUEL_OK) {
                d2vita_progress("SCHEDPROBE: v5 saute (duel sans verdict valide — pas de verdict coremap)");
            } else {
                static volatile uint32_t v5_c0 = 0, v5_c1 = 0, v5_c2 = 0, v5_p = 0;
                probe_a = 0; probe_stop = 0;
                aff_a = 1;   // sentinel re-armed (same discipline as v3/v4; not printed in v5)
                d2vita_progress("SCHEDPROBE: v5 start (2 s)");
                {   // system line: active cores + what the kernel reports for the main thread
                    SceKernelSystemInfo si; std::memset(&si, 0, sizeof si); si.size = sizeof si;
                    int rsys = sceKernelGetSystemInfo(&si);
                    SceKernelThreadInfo mi; std::memset(&mi, 0, sizeof mi); mi.size = sizeof mi;
                    int rmi = sceKernelGetThreadInfo(sceKernelGetThreadId(), &mi);
                    char m[224]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v5 sys rc=0x%08x activeCpuMask=0x%08x | main rc=0x%08x prio=%d aff=0x%08x cpu=%d (informatif)",
                        (unsigned)rsys, (unsigned)si.activeCpuMask, (unsigned)rmi,
                        (int)mi.currentPriority, (unsigned)mi.currentCpuAffinityMask, (int)mi.currentCpuId);
                    d2vita_progress(m);
                }
                // Generic counter: the creator passes the slot's ADDRESS via
                // StartThread's argp (copied by the kernel at start) — one
                // lambda serves C0/C1/C2, no extra globals needed.
                auto cnt_slot = [](SceSize, void* argp) -> int {
                    volatile uint32_t* c = *(volatile uint32_t* const*)argp;
                    uint32_t k = 0;
                    while (!probe_stop) { ++*c;
                        if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
                    return 0; };
                auto pte_cnt = [](void*) -> void* {   // P: pure counter, no affinity or priority call
                    uint32_t k = 0;
                    while (!probe_stop) { ++v5_p;
                        if (!(++k & 0xFFF) && sceKernelGetProcessTimeWide() >= probe_deadline) break; }
                    return (void*)0; };
                SceUID a5  = sceKernelCreateThread("probe_v5a",  cnt_a,    0x10000100, 0x4000, 0, 0, nullptr);
                SceUID c50 = sceKernelCreateThread("probe_v5c0", cnt_slot, 0x10000100, 0x4000, 0, 0, nullptr);
                SceUID c51 = sceKernelCreateThread("probe_v5c1", cnt_slot, 0x10000100, 0x4000, 0, 0, nullptr);
                SceUID c52 = sceKernelCreateThread("probe_v5c2", cnt_slot, 0x10000100, 0x4000, 0, 0, nullptr);
                if (a5 < 0 || c50 < 0 || c51 < 0 || c52 < 0) {
                    char m[144]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v5 CreateThread KO (0x%08x/0x%08x/0x%08x/0x%08x) — pas de verdict", a5, c50, c51, c52);
                    d2vita_progress(m);
                    if (a5  >= 0) sceKernelDeleteThread(a5);
                    if (c50 >= 0) sceKernelDeleteThread(c50);
                    if (c51 >= 0) sceKernelDeleteThread(c51);
                    if (c52 >= 0) sceKernelDeleteThread(c52);
                } else if (int r0 = sceKernelChangeThreadCpuAffinityMask(c50, SCE_KERNEL_CPU_MASK_USER_0),
                               r1 = sceKernelChangeThreadCpuAffinityMask(c51, SCE_KERNEL_CPU_MASK_USER_1),
                               r2 = sceKernelChangeThreadCpuAffinityMask(c52, SCE_KERNEL_CPU_MASK_USER_2);
                           r0 < 0 || r1 < 0 || r2 < 0) {
                    // an unpinned witness would make the map unreadable: no
                    // verdict is issued on a compromised witness
                    char m[144]; snprintf(m, sizeof m,
                        "SCHEDPROBE: v5 epinglage createur KO (0x%08x/0x%08x/0x%08x) — pas de verdict",
                        (unsigned)r0, (unsigned)r1, (unsigned)r2);
                    d2vita_progress(m);
                    sceKernelDeleteThread(a5); sceKernelDeleteThread(c50);
                    sceKernelDeleteThread(c51); sceKernelDeleteThread(c52);
                } else {
                    // P starts first, alone (the Sce threads exist but are
                    // dormant): p0 measures whether the pte path starts at
                    // all, with no contention — the only moment a
                    // priority-191 thread is guaranteed to run if its start
                    // actually worked.
                    probe_deadline = sceKernelGetProcessTimeWide()
                                     + 2 * PROBE_V4_SETTLE_US + PROBE_V5_US + PROBE_DEADLINE_SLACK_US;
                    pthread_t pth{};
                    int prc = pthread_create(&pth, nullptr, pte_cnt, nullptr);
                    if (prc != 0) {
                        char m[112]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v5 pthread_create KO (rc=%d) — jetons PTE sans verdict", prc);
                        d2vita_progress(m);
                    }
                    sceKernelDelayThread(PROBE_V4_SETTLE_US);
                    uint32_t p0 = v5_p;
                    int ra = sceKernelStartThread(a5, 0, nullptr);   // A owns USER_0 before C0 starts (negative control)
                    if (ra >= 0) sceKernelDelayThread(PROBE_V4_SETTLE_US);
                    volatile uint32_t* s0 = &v5_c0; volatile uint32_t* s1 = &v5_c1; volatile uint32_t* s2 = &v5_c2;
                    int rb = sceKernelStartThread(c50, sizeof s0, &s0);
                    int rc0 = sceKernelStartThread(c51, sizeof s1, &s1);
                    int rd = sceKernelStartThread(c52, sizeof s2, &s2);
                    if (ra < 0 || rb < 0 || rc0 < 0 || rd < 0) {
                        probe_stop = 1;
                        // Bounded cleanup, same discipline as the nominal
                        // path: a thread started on a dead core never exits
                        // (500 ms join, then abandon); a thread that never
                        // started (rc < 0) is still dormant, so it can be
                        // deleted directly.
                        if (ra >= 0) sceKernelWaitThreadEnd(a5, nullptr, nullptr);   // A: USER_0 is the boot core — always exits
                        sceKernelDeleteThread(a5);
                        { SceUID cth[3] = { c50, c51, c52 }; int crc[3] = { rb, rc0, rd };
                          for (int i = 0; i < 3; ++i) {
                              if (crc[i] < 0) { sceKernelDeleteThread(cth[i]); continue; }
                              SceUInt to = 500 * 1000;
                              if (sceKernelWaitThreadEnd(cth[i], nullptr, &to) < 0) {
                                  char w[112]; snprintf(w, sizeof w,
                                      "SCHEDPROBE: v5 C%d jamais ordonnance — fil abandonne (fuite bornee)", i);
                                  d2vita_progress(w);
                              } else sceKernelDeleteThread(cth[i]);
                          } }
                        char m[144]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v5 StartThread KO (0x%08x/0x%08x/0x%08x/0x%08x) — pas de verdict", ra, rb, rc0, rd);
                        d2vita_progress(m);
                        if (prc == 0) {   // same anti-join-wedge guard as the nominal path
                            if (v5_p > 0) pthread_join(pth, nullptr);
                            else { pthread_detach(pth); d2vita_progress("SCHEDPROBE: v5 P jamais ne — detach"); }
                        }
                    } else {
                        uint64_t w0 = sceKernelGetProcessTimeWide();
                        sceKernelDelayThread(PROBE_V5_US);
                        uint32_t ga = probe_a, g0 = v5_c0, g1 = v5_c1, g2 = v5_c2, gp = v5_p;
                        uint32_t wall_ms = (uint32_t)((sceKernelGetProcessTimeWide() - w0) / 1000ull);
                        {   // sampled live (spinners still running) — informational
                            SceKernelThreadInfo ti; int cpu[4]; SceUID ids[4] = { a5, c50, c51, c52 };
                            for (int i = 0; i < 4; ++i) {
                                std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
                                cpu[i] = (sceKernelGetThreadInfo(ids[i], &ti) == 0) ? (int)ti.currentCpuId : -99;
                            }
                            SceUID p_uid = (prc == 0 && pth) ? *(SceUID*)(void*)pth : -1;   // pte handle layout — probe-only
                            std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
                            int rpi = (p_uid > 0) ? sceKernelGetThreadInfo(p_uid, &ti) : -1;
                            char m[240]; snprintf(m, sizeof m,
                                "SCHEDPROBE: v5 cpus A=%d C0=%d C1=%d C2=%d | P rc=0x%08x status=0x%x prio=%d aff=0x%08x cpu=%d last=%d (informatif)",
                                cpu[0], cpu[1], cpu[2], cpu[3], (unsigned)rpi, (unsigned)ti.status,
                                (int)ti.currentPriority, (unsigned)ti.currentCpuAffinityMask,
                                (int)ti.currentCpuId, (int)ti.lastExecutedCpuId);
                            d2vita_progress(m);
                        }
                        probe_stop = 1;
                        sceKernelWaitThreadEnd(a5, nullptr, nullptr);   // A first: frees USER_0 for C0 (the boot core — A always exits)
                        sceKernelDeleteThread(a5);
                        // Bounded joins for the three C threads: with
                        // self-termination, a C thread that ran has already
                        // exited (or exits at the deadline), so these joins
                        // normally succeed; the timeout only fires for a
                        // thread that was never scheduled at all (a core
                        // dead for this app) — C1 is exactly the thread that
                        // would never exit if USER_1 is dead for this app,
                        // the case this probe measures. An unbounded join
                        // here would wedge the boot and lose the verdict.
                        // On timeout: abandon and log it, no DeleteThread
                        // (a non-dormant thread refuses it) — the verdict
                        // still prints normally afterward.
                        { SceUID cth[3] = { c50, c51, c52 };
                          for (int i = 0; i < 3; ++i) {
                              SceUInt to = 500 * 1000;
                              if (sceKernelWaitThreadEnd(cth[i], nullptr, &to) < 0) {
                                  char w[112]; snprintf(w, sizeof w,
                                      "SCHEDPROBE: v5 C%d jamais ordonnance — fil abandonne (fuite bornee)", i);
                                  d2vita_progress(w);
                              } else sceKernelDeleteThread(cth[i]);
                          } }
                        if (prc == 0) {
                            if (gp > 0 || v5_p > 0) pthread_join(pth, nullptr);
                            else { pthread_detach(pth); d2vita_progress("SCHEDPROBE: v5 P jamais ne — detach (join impossible)"); }
                        }
                        const uint32_t v5_floor = calib / 1000;   // floor from calibration (same discipline as v3/v4)
                        char verdict[160];
                        if (ga < v5_floor)
                            snprintf(verdict, sizeof verdict, "INVALIDE (A jamais installe — course de depart perdue ?)");
                        else {
                            const char* t1 = (g1 > v5_floor) ? "USER1-LIVE" : "USER1-CAPTIF";
                            const char* t2 = (g2 > v5_floor) ? "USER2-LIVE" : "USER2-CAPTIF";
                            const char* t3; const char* t4;
                            if (prc != 0) { t3 = "PTE-SANS-VERDICT"; t4 = "PTE-SANS-VERDICT"; }
                            else {
                                t3 = (gp > 0) ? "PTE-NAIT" : "PTE-MORT-NE";
                                t4 = ((gp - p0) > v5_floor) ? "PTE-ELIGIBLE" : "PTE-ETOUFFE";
                            }
                            snprintf(verdict, sizeof verdict, "%s %s %s %s%s", t1, t2, t3, t4,
                                     (g0 > v5_floor) ? " C0-ANORMAL(USER_0 partage ?)" : "");
                        }
                        char m[256]; snprintf(m, sizeof m,
                            "SCHEDPROBE: v5 coremap a=%u c0=%u c1=%u c2=%u p=%u p0=%u rcA=0x%08x wall=%ums => %s",
                            ga, g0, g1, g2, gp, p0, (unsigned)aff_a, wall_ms, verdict);
                        d2vita_progress(m);
                    }
                }
            }
        }
    }
    // ===================== D2_VMPROBE=1 — VM domain probe =====================
    // Context: per-thread VM-domain opening runs but the kernel refuses it —
    // the process's first sceKernelOpenVMDomain() returns 0, every later
    // caller gets 0x80010058 (ENOSYS) — and pte runners still fault on their
    // first `strb` into the JIT arena.
    //
    // What's established going in:
    //  - The vitasdk header comment for sceKernelOpenVMDomain/CloseVMDomain
    //    (psp2/kernel/sysmem.h) has the semantics BACKWARDS. It claims Open
    //    makes VM memblocks executable and Close makes them non-executable.
    //    The primary sources say the opposite: yifanlu's own reference
    //    dynarec example documents Open as "set domain to be writable by
    //    user" and Close as "set domain back to read-only", and vita-luajit
    //    (the only real published Vita JIT) maps RX -> Close and RWX ->
    //    Open. So Open is RWX (a superset), Close is RX. That matches the
    //    observed crash exactly: a read at an address succeeds, a write to
    //    the same byte faults — closed domain = R+X without W.
    //  - vita-luajit toggles Open/Close in pairs, thousands of times, around
    //    each write burst — that toggling is the intended usage. "Open once
    //    and leave it open" (this codebase's original approach) is the
    //    exception, not the rule, and vita-luajit's own author notes the
    //    protection is process-global and was never made to work across
    //    multiple threads.
    //  - The DACR register lives in a per-process kernel structure but also
    //    appears in the per-thread saved exception context, so the public
    //    headers don't settle whether the open/closed state is per-process
    //    or per-thread. Only testing on hardware can — which is what this
    //    probe does.
    //
    // What the probe establishes in one boot (every rc logged): whether a
    // second Open from the same thread succeeds (one-shot-per-process vs.
    // per-caller refusal); whether Close+Open re-arms it; the same from a
    // freshly created Sce thread and from a pthread; whether the opening
    // thread can still execute code while the domain is open (safety of a
    // "bracket" fix); whether another thread doing Close+Open can write
    // (the candidate fix); whether a thread pinned to the main thread's core
    // (no bracket of its own) can write, to tell "per-core" from "per-thread"
    // state; and finally a bare pthread write with no bracket at all, to
    // reproduce the runner crash directly.
    //
    // Discipline: phases run in increasing order of risk, so a fault only
    // ever loses information about later steps, never earlier ones; every
    // step logs its intent before performing it, so the last log line always
    // names the exact instruction that killed the boot; nothing spins, and
    // every join is bounded (500 ms), with a never-scheduled thread logged
    // as abandoned rather than waited on forever.
    //
    // Best run without D2SCHED=native (the cooperative boot continues
    // normally afterward): the probe closes the domain again on exit, so
    // mman_vita's normal open behaves exactly as it would without the probe.
    if (getenv("D2_VMPROBE") && *getenv("D2_VMPROBE") == '1') {
        enum : SceUInt   { VMP_JOIN_TO_US = 500 * 1000 };   // join bound (same discipline as v5)
        enum : SceUInt32 { VMP_BLK = 0x100000 };            // 1 MiB — hardware VM granularity
        static SceUID             vmp_uid  = -1;
        static volatile uint8_t*  vmp_base = nullptr;
        static volatile int       vmp_pte_done = 0;
        static int                vmp_main_cpu = -1;   // phase H: main thread's core at re-arm time

        d2vita_progress("VMPROBE: start — aucune ouverture de domaine VM avant ce point "
                        "(platform_init precede l'allocation de l'arene JIT)");

        // ---- phase 0: witness block + what the kernel reports about its permissions ----
        vmp_uid = sceKernelAllocMemBlockForVM("d2_vmprobe", VMP_BLK);
        void* b0 = nullptr;
        int rbase = (vmp_uid < 0) ? (int)vmp_uid : sceKernelGetMemBlockBase(vmp_uid, &b0);
        { char m[176]; snprintf(m, sizeof m, "VMPROBE: 0 alloc uid=0x%08x base=%p rcbase=0x%08x",
                                (unsigned)vmp_uid, b0, (unsigned)rbase); d2vita_progress(m); }
        if (vmp_uid < 0 || rbase < 0 || !b0) {
            d2vita_progress("VMPROBE: 0 KO — pas de bloc VM, sonde ABANDONNEE (aucun verdict)");
            if (vmp_uid >= 0) sceKernelFreeMemBlock(vmp_uid);
        } else {
            vmp_base = (volatile uint8_t*)b0;
            {   SceKernelMemBlockInfo bi; std::memset(&bi, 0, sizeof bi); bi.size = sizeof bi;
                int ri = sceKernelGetMemBlockInfoByAddr(b0, &bi);
                char m[224]; snprintf(m, sizeof m,
                    "VMPROBE: 0 info AVANT ouverture rc=0x%08x access=0x%08x (X=1 W=2 R=4) "
                    "mtype=0x%08x type=0x%08x mappedSize=%u",
                    (unsigned)ri, (unsigned)bi.access, (unsigned)bi.memoryType,
                    (unsigned)bi.type, (unsigned)bi.mappedSize);
                d2vita_progress(m); }

            // ---- phase A: the two-line probe ---------------------------------
            // 4 calls in a row from the SAME thread, every rc logged:
            //   open#2 rc=0   => the runners' refusal is NOT "one-shot per
            //                    process": it's tied to the calling thread;
            //   open#2 rc<0   => it's stateful; close+open#3 then shows
            //                    whether it's re-armable (the "bracket" fix).
            int a1 = sceKernelOpenVMDomain();
            int a2 = sceKernelOpenVMDomain();
            int a3 = sceKernelCloseVMDomain();
            int a4 = sceKernelOpenVMDomain();
            {   SceKernelThreadInfo mi; std::memset(&mi, 0, sizeof mi); mi.size = sizeof mi;
                int rmi = sceKernelGetThreadInfo(sceKernelGetThreadId(), &mi);
                vmp_main_cpu = (rmi < 0) ? -1 : (int)mi.currentCpuId;
                char m[240]; snprintf(m, sizeof m,
                    "VMPROBE: A main(MEME fil) open#1=0x%08x open#2=0x%08x close=0x%08x open#3=0x%08x "
                    "| cpu=%d aff=0x%08x prio=%d",
                    (unsigned)a1, (unsigned)a2, (unsigned)a3, (unsigned)a4, vmp_main_cpu,
                    (unsigned)mi.currentCpuAffinityMask, (int)mi.currentPriority);
                d2vita_progress(m); }
            {   SceKernelMemBlockInfo bi; std::memset(&bi, 0, sizeof bi); bi.size = sizeof bi;
                int ri = sceKernelGetMemBlockInfoByAddr((void*)vmp_base, &bi);
                char m[176]; snprintf(m, sizeof m,
                    "VMPROBE: A info APRES ouverture rc=0x%08x access=0x%08x (identique a la phase 0 "
                    "=> access est de la comptabilite noyau, pas l'etat DACR)",
                    (unsigned)ri, (unsigned)bi.access);
                d2vita_progress(m); }

            // ---- thread helpers: bounded and self-terminating -------------------
            auto run_sce = [](const char* tag, int (*ent)(SceSize, void*), int affmask) -> void {
                SceUID t = sceKernelCreateThread(tag, ent, 0x10000100, 0x4000, 0, affmask, nullptr);
                if (t < 0) { char m[144]; snprintf(m, sizeof m,
                        "VMPROBE: %s CreateThread KO (0x%08x) — etape sans verdict", tag, (unsigned)t);
                    d2vita_progress(m); return; }
                if (affmask) {
                    int ra = sceKernelChangeThreadCpuAffinityMask(t, affmask);
                    if (ra < 0) { char m[176]; snprintf(m, sizeof m,
                            "VMPROBE: %s epinglage KO (0x%08x) — etape ABANDONNEE (un temoin non epingle "
                            "ne prouve rien sur le coeur)", tag, (unsigned)ra);
                        d2vita_progress(m); sceKernelDeleteThread(t); return; }
                }
                int rs = sceKernelStartThread(t, 0, nullptr);
                if (rs < 0) { char m[144]; snprintf(m, sizeof m,
                        "VMPROBE: %s StartThread KO (0x%08x) — etape sans verdict", tag, (unsigned)rs);
                    d2vita_progress(m); sceKernelDeleteThread(t); return; }
                SceUInt to = VMP_JOIN_TO_US;
                if (sceKernelWaitThreadEnd(t, nullptr, &to) < 0) {
                    char m[176]; snprintf(m, sizeof m,
                        "VMPROBE: %s jamais termine en %u ms — fil ABANDONNE (fuite bornee : TCB + 16 Kio)",
                        tag, (unsigned)(VMP_JOIN_TO_US / 1000));
                    d2vita_progress(m);            // pas de DeleteThread : un fil non dormant le refuse
                } else sceKernelDeleteThread(t);
            };
            auto run_pte = [](const char* tag, void* (*ent)(void*)) -> void {
                vmp_pte_done = 0;
                pthread_t th{};
                int rc = pthread_create(&th, nullptr, ent, nullptr);
                if (rc != 0) { char m[144]; snprintf(m, sizeof m,
                        "VMPROBE: %s pthread_create KO (rc=%d) — etape sans verdict", tag, rc);
                    d2vita_progress(m); return; }
                for (int i = 0; i < 100 && !vmp_pte_done; ++i) sceKernelDelayThread(5000);   // <= 500 ms
                if (vmp_pte_done) pthread_join(th, nullptr);
                else { pthread_detach(th); char m[176]; snprintf(m, sizeof m,
                        "VMPROBE: %s jamais termine en %u ms — detach (join impossible, meme garde que v5)",
                        tag, (unsigned)(VMP_JOIN_TO_US / 1000));
                    d2vita_progress(m); }
            };

            // ---- phase B: fresh Sce thread, RCs only (no memory access) ------------
            run_sce("B(sce-rc)", [](SceSize, void*) -> int {
                int o1 = sceKernelOpenVMDomain();
                int cl = sceKernelCloseVMDomain();
                int o2 = sceKernelOpenVMDomain();
                SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
                int rt = sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
                char m[224]; snprintf(m, sizeof m,
                    "VMPROBE: B fil Sce NEUF open=0x%08x close=0x%08x open2=0x%08x | cpu=%d prio=%d aff=0x%08x",
                    (unsigned)o1, (unsigned)cl, (unsigned)o2,
                    rt < 0 ? -1 : (int)ti.currentCpuId, rt < 0 ? -1 : (int)ti.currentPriority,
                    rt < 0 ? 0u : (unsigned)ti.currentCpuAffinityMask);
                d2vita_progress(m);
                return 0; }, 0);

            // ---- phase C: pthread (pte), RCs only ---------------------------
            run_pte("C(pte-rc)", [](void*) -> void* {
                int o1 = sceKernelOpenVMDomain();
                int cl = sceKernelCloseVMDomain();
                int o2 = sceKernelOpenVMDomain();
                SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
                int rt = sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
                char m[224]; snprintf(m, sizeof m,
                    "VMPROBE: C fil pte NEUF open=0x%08x close=0x%08x open2=0x%08x | cpu=%d prio=%d aff=0x%08x",
                    (unsigned)o1, (unsigned)cl, (unsigned)o2,
                    rt < 0 ? -1 : (int)ti.currentCpuId, rt < 0 ? -1 : (int)ti.currentPriority,
                    rt < 0 ? 0u : (unsigned)ti.currentCpuAffinityMask);
                d2vita_progress(m);
                vmp_pte_done = 1;
                return nullptr; });

            // ---- phase D: the main thread writes (positive control) ----------
            // Expected to succeed; if this is the last log line instead, the
            // whole model is wrong and that needs to be known.
            d2vita_progress("VMPROBE: D main — ECRITURE imminente dans le bloc VM "
                            "(si la ligne suivante manque : meme le fil ouvreur ne peut pas ecrire)");
            vmp_base[0] = 0x5A;
            { char m[112]; snprintf(m, sizeof m, "VMPROBE: D main ecriture OK relu=0x%02x",
                                    (unsigned)vmp_base[0]); d2vita_progress(m); }

            // ---- phase E: execute code with the domain OPEN (bracket safety) -
            // If Open actually removed exec permission (the vitasdk header's
            // literal claim), a bracket open while another runner executes
            // JIT code would crash it. Expected result: 5.
            d2vita_progress("VMPROBE: E stub thumb pose + Sync, APPEL imminent (domaine OUVERT) "
                            "— si la ligne suivante manque : Open retire l'execution, tout bracket est mortel");
            *(volatile uint32_t*)vmp_base = 0x47702005u;          // movs r0,#5 ; bx lr
            int rsy = sceKernelSyncVMDomain(vmp_uid, (void*)vmp_base, 0x1000);
            int (*vmp_fn)(void) = (int (*)(void))((uintptr_t)vmp_base | 1u);
            int fr = vmp_fn();
            { char m[144]; snprintf(m, sizeof m,
                "VMPROBE: E appel stub domaine OUVERT sync=0x%08x => %d (attendu 5) "
                "=> Open ne retire PAS l'execution", (unsigned)rsy, fr); d2vita_progress(m); }

            // ---- phase F: Sce thread with a close+open bracket, then write ---
            // The leading candidate fix (vita-luajit's own pattern).
            run_sce("F(sce-bracket)", [](SceSize, void*) -> int {
                int cl = sceKernelCloseVMDomain();
                int op = sceKernelOpenVMDomain();
                char m[208]; snprintf(m, sizeof m,
                    "VMPROBE: F fil Sce bracket close=0x%08x open=0x%08x — ECRITURE imminente "
                    "(si la ligne suivante manque : le bracket NE MARCHE PAS)", (unsigned)cl, (unsigned)op);
                d2vita_progress(m);
                vmp_base[0x100] = 0xA5;
                char n[144]; snprintf(n, sizeof n,
                    "VMPROBE: F fil Sce ECRITURE OK relu=0x%02x => BRACKET EFFICACE sur un fil Sce",
                    (unsigned)vmp_base[0x100]);
                d2vita_progress(n);
                return 0; }, 0);

            // ---- phase G: pthread with a bracket, then write ------------------
            // The NativeScheduler runners are pthreads, so this is the thread
            // type the fix actually needs to work for.
            run_pte("G(pte-bracket)", [](void*) -> void* {
                int cl = sceKernelCloseVMDomain();
                int op = sceKernelOpenVMDomain();
                char m[208]; snprintf(m, sizeof m,
                    "VMPROBE: G fil pte bracket close=0x%08x open=0x%08x — ECRITURE imminente "
                    "(si la ligne suivante manque : le bracket NE MARCHE PAS sur pte)",
                    (unsigned)cl, (unsigned)op);
                d2vita_progress(m);
                vmp_base[0x104] = 0x5B;
                char n[144]; snprintf(n, sizeof n,
                    "VMPROBE: G fil pte ECRITURE OK relu=0x%02x => BRACKET EFFICACE sur un pte",
                    (unsigned)vmp_base[0x104]);
                d2vita_progress(n);
                vmp_pte_done = 1;
                return nullptr; });

            // ---- phase G2: main thread re-writes without re-arming itself ----
            // The bracket fix's real safety question. Two other threads just
            // did Close+Open (F and G). If Close disarms the whole process
            // (not just a per-thread flag), the main thread has just been
            // disarmed by them, and every bracket would then need the same
            // lock as any JIT write — otherwise a thread translating code
            // during another thread's Close->Open window would fault. If the
            // main thread can still write, Close only disarms its own caller
            // and the bracket is safe as-is.
            d2vita_progress("VMPROBE: G2 main RE-ECRIT sans se re-armer, apres les brackets de F et G — "
                            "ECRITURE imminente (si la ligne suivante manque : Close est GLOBAL en effet, "
                            "un bracket non serialise avec les ecritures JIT est MORTEL)");
            vmp_base[0x110] = 0x6C;
            { char m[176]; snprintf(m, sizeof m,
                "VMPROBE: G2 main ecriture OK relu=0x%02x => Close ne desarme QUE son appelant, "
                "le bracket n'ouvre PAS de fenetre pour les autres fils", (unsigned)vmp_base[0x110]);
              d2vita_progress(m); }

            // ---- phase H: Sce thread pinned to the MAIN THREAD's core, no
            // bracket of its own. Discriminates "state is per-core" from
            // "state is per-thread": the main thread re-arms its own context
            // (close+open) and reads back its core; the witness thread is
            // pinned to that same core and writes without opening anything.
            //   write succeeds => per-core (conclusive);
            //   fault          => per-thread (strong evidence, though not
            //                     airtight: an intervening process context
            //                     switch on that core could also explain it).
            {   int hc = sceKernelCloseVMDomain();
                int ho = sceKernelOpenVMDomain();
                SceKernelThreadInfo mi; std::memset(&mi, 0, sizeof mi); mi.size = sizeof mi;
                int rmi = sceKernelGetThreadInfo(sceKernelGetThreadId(), &mi);
                vmp_main_cpu = (rmi < 0) ? -1 : (int)mi.currentCpuId;
                char m[208]; snprintf(m, sizeof m,
                    "VMPROBE: H re-armement main close=0x%08x open=0x%08x cpu=%d — temoin epingle sur ce coeur",
                    (unsigned)hc, (unsigned)ho, vmp_main_cpu);
                d2vita_progress(m); }
            if (vmp_main_cpu < 0 || vmp_main_cpu > 2) {
                d2vita_progress("VMPROBE: H saute (cpu du main illisible ou hors USER_0..2) — pas de verdict coeur");
            } else {
                run_sce("H(sce-brut-meme-coeur)", [](SceSize, void*) -> int {
                    SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
                    int rt = sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
                    char m[224]; snprintf(m, sizeof m,
                        "VMPROBE: H fil Sce cpu=%d (coeur du main) SANS ouverture — ECRITURE imminente "
                        "(si la ligne suivante manque : DACR PAR FIL ; si elle vient : DACR PAR COEUR)",
                        rt < 0 ? -1 : (int)ti.currentCpuId);
                    d2vita_progress(m);
                    vmp_base[0x108] = 0x33;
                    char n[176]; snprintf(n, sizeof n,
                        "VMPROBE: H ECRITURE OK SANS ouverture sur le coeur du main relu=0x%02x "
                        "=> le domaine VM suit le COEUR, pas le fil", (unsigned)vmp_base[0x108]);
                    d2vita_progress(n);
                    return 0; }, 0x00010000 << vmp_main_cpu);
            }

            // ---- phase I: pthread with no bracket — reproduces the crash -----
            // Last step: expected to kill the boot, the same way
            // allocBlock (custommem.c:190) does a few seconds into a normal run.
            run_pte("I(pte-brut)", [](void*) -> void* {
                d2vita_progress("VMPROBE: I fil pte SANS ouverture — ECRITURE imminente "
                                "(reproduction du crash runner ; si la ligne suivante manque, "
                                "c'est LE crash de allocBlock, en miniature et sous controle)");
                vmp_base[0x10c] = 0x77;
                char n[192]; snprintf(n, sizeof n,
                    "VMPROBE: I ECRITURE OK SANS ouverture depuis un pte relu=0x%02x "
                    "=> le domaine est GLOBAL au processus, la cause du crash est AILLEURS",
                    (unsigned)vmp_base[0x10c]);
                d2vita_progress(n);
                vmp_pte_done = 1;
                return nullptr; });

            // ---- end: close the domain and free the block --------------------
            // Leaves boot in the same state as without the probe: mman_vita's
            // first Open still returns 0 as usual.
            int fz = sceKernelCloseVMDomain();
            int ff = sceKernelFreeMemBlock(vmp_uid);
            vmp_base = nullptr; vmp_uid = -1;
            char m[176]; snprintf(m, sizeof m,
                "VMPROBE: fin — toutes les phases terminees, close final=0x%08x free=0x%08x "
                "(le boot continue dans l'etat d'un run SANS sonde)", (unsigned)fz, (unsigned)ff);
            d2vita_progress(m);
        }
    }

    // The giant quantum above is a solo-only safety net: it disables
    // preemption entirely. The online realm-login path hits exactly the
    // risk that was meant to guard against — after realm logon, D2 spawns a
    // thread that loops without ever calling an import, so without
    // preemption it holds the core forever and the whole game freezes, not
    // just networking.
    // Preemption is restored as soon as networking is requested, and only
    // then — solo keeps the safety net. An explicit QUANTUM in env.txt still
    // wins (it's read just before this point).
    if (getenv("D2NET")) {
        const char* q = getenv("QUANTUM");
        if (q && strcmp(q, "2000000000") == 0) {
            setenv("QUANTUM", "300000", 1);
            d2vita_progress("reseau: preemption retablie (QUANTUM 300000) — le chemin royaume se fige sans elle");
        }
    }

    // Full game-mode clocks: ARM 444, bus 222, GPU 222, xbar 166 — the
    // standard maxima of demanding Vita titles. The bus raise also speeds the
    // dynarec (memory-bound). Done after the fresh-run remove, so the result
    // line survives in boot_progress (a refused request must be visible on
    // device).
    // Memory budget: every arena/heap size in this project came from an
    // estimate. sceKernelGetFreeMemorySize reports the three pools the
    // device actually grants: size_user (where the arena + JIT + newlib heap
    // live), size_cdram (128 MiB of video RAM, only framebuffers so far),
    // size_phycont (physically-contiguous pool, usually budgeted separately
    // — untouched today). Sampled before the arena allocation (what we can
    // still claim) and again after the runtime is up (what's left on the
    // table).
    {
        SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
        int r = sceKernelGetFreeMemorySize(&fi);
        char m[176];
        snprintf(m, sizeof m, "mem-budget(pre-arena) rc=%d user=%d KB cdram=%d KB phycont=%d KB",
                 r, fi.size_user >> 10, fi.size_cdram >> 10, fi.size_phycont >> 10);
        d2vita_progress(m);
    }
    // D2_MEMPROBE=1: where the kernel places user memory blocks. This
    // matters because the game only survives at addresses below 2 GiB (bit
    // 31 is the discriminant bit for complemented pointers inside Blizzard's
    // containers). If no block type ever lands below 0x80000000, that
    // avenue is closed by the kernel. Allocates one block per type, logs its
    // base, frees it immediately. Diagnostic only, never used in play.
    if (getenv("D2_MEMPROBE")) {
        struct { const char* nom; SceKernelMemBlockType t; SceSize sz; } types[] = {
            { "USER_RW",            SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,                 0x100000 },
            { "USER_RW_UNCACHE",    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,         0x100000 },
            { "USER_CDRAM_RW",      SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,           0x100000 },
            { "USER_MAIN_PHYCONT_RW",    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,    0x100000 },
            { "USER_MAIN_PHYCONT_NC_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 0x100000 },
        };
        for (auto& ty : types) {
            SceUID u = sceKernelAllocMemBlock("d2_memprobe", ty.t, ty.sz, nullptr);
            void* b = nullptr; int rb = (u < 0) ? -1 : sceKernelGetMemBlockBase(u, &b);
            char m[128];
            snprintf(m, sizeof m, "memprobe: %-24s uid=0x%08x base=%p %s", ty.nom, (unsigned)u, b,
                     (u < 0) ? "REFUSE" : (((uintptr_t)b < 0x80000000u) ? "SOUS 2 Gio" : "au-dessus de 2 Gio"));
            d2vita_progress(m); (void)rb;
            if (u >= 0) sceKernelFreeMemBlock(u);
        }
        { SceUID u = sceKernelAllocMemBlockForVM("d2_memprobe_vm", 0x100000);
          void* b = nullptr; if (u >= 0) sceKernelGetMemBlockBase(u, &b);
          char m[128]; snprintf(m, sizeof m, "memprobe: %-24s uid=0x%08x base=%p %s", "ForVM", (unsigned)u, b,
                     (u < 0) ? "REFUSE" : (((uintptr_t)b < 0x80000000u) ? "SOUS 2 Gio" : "au-dessus de 2 Gio"));
          d2vita_progress(m); if (u >= 0) sceKernelFreeMemBlock(u); }
    }
    {
        // D2_ARMCLOCK / D2_BUSCLOCK: ARM and bus clocks (default 444 / 222,
        // the maximum). Not a speed lever — a diagnostic: the slope of
        // fps(333)/fps(444) discriminates "compute-bound" (~0.75) from
        // "memory-bound" (close to 1). Kernel-accepted range: ARM 41-444,
        // bus 41-222.
        int arm = 444, bus = 222;
        if (const char* e = getenv("D2_ARMCLOCK")) { int v = atoi(e); if (v >= 41 && v <= 444) arm = v; }
        if (const char* e = getenv("D2_BUSCLOCK")) { int v = atoi(e); if (v >= 41 && v <= 222) bus = v; }
        // D2_GPUCLOCK / D2_GPUXBAR: the two GPU clocks. Defaults 222 and 166
        // match what was always requested, so behavior doesn't change
        // without the knob. Made configurable to help diagnose GPU frame
        // cost: the "clocks:" line below reads back both values, which it
        // didn't used to.
        int gpu = 222, xbar = 166;
        if (const char* e = getenv("D2_GPUCLOCK")) { int v = atoi(e); if (v >= 41 && v <= 222) gpu = v; }
        if (const char* e = getenv("D2_GPUXBAR"))  { int v = atoi(e); if (v >= 41 && v <= 166) xbar = v; }
        int ra = scePowerSetArmClockFrequency(arm);
        int rb = scePowerSetBusClockFrequency(bus);
        int rg = scePowerSetGpuClockFrequency(gpu);
        int rx = scePowerSetGpuXbarClockFrequency(xbar);
        char m[208];
        snprintf(m, sizeof m, "clocks: arm%d=%d bus%d=%d gpu%d=%d xbar%d=%d (0=ok)"
                              " relu arm=%d bus=%d gpu=%d xbar=%d",
                 arm, ra, bus, rb, gpu, rg, xbar, rx,
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                 scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
        d2vita_progress(m);
    }
#ifdef D2VPK_TAG
    sceIoMkdir("ux0:data/d2vita/save_" D2VPK_TAG, 0777);
    sceIoMkdir("ux0:data/d2vita/save_" D2VPK_TAG "/Save", 0777);
#else
    sceIoMkdir("ux0:data/d2vita/save", 0777);
    sceIoMkdir("ux0:data/d2vita/save/Save", 0777);
#endif
#ifdef D2VPK_TAG
    d2vita_progress("flavour: " D2VPK_TAG " (A/B par shim)");
#else
    d2vita_progress("flavour: PORTAGES — lumiere/blend/RLE/collision/grille/SCompExplode actifs");
#endif
    { SceRtcTick t; t.tick=0; int rc=sceRtcGetCurrentTick(&t);
      long long u=d2vita_wall_unix();
      char m[120]; snprintf(m,sizeof m,"rtc: rc=0x%08x tick=%llu unix=%lld",rc,(unsigned long long)t.tick,u);
      d2vita_progress(m); }
    // Network bring-up happens BEFORE main() claims the arena: Sony's
    // network stack needs its own memory block, and the user budget is
    // already almost entirely consumed by the arena (297 MB out of ~330).
    // Doing it the other way around fails silently. make_cpu_box86() runs
    // after this point, so the ordering holds as long as this call stays
    // here. The CONNECTION WAIT itself no longer blocks here though: it used
    // to run serially before any of arena/DllMain/Authenticode/GXM setup,
    // adding up to its full timeout on top of a screen that was already
    // black for none of its own reasons. It now continues on a background
    // thread (d2vita_net_start) and is only joined later, in rt_boot's
    // main(), right before D2NET_FAILED is actually consulted -- by then
    // several seconds of that other setup have usually already covered it.
    if (getenv("D2NET")) {
        if (getenv("D2NETTEST")) {
            // Diagnostic knob: wants an immediate, synchronous verdict to
            // test against, not the overlapped background wait -- keep it
            // simple and blocking, like before.
            int nr = d2vita_net_init(5000);
            d2vita_progress(d2vita_net_status());
            if (nr != 0) {
                char m[64]; snprintf(m, sizeof m, "reseau: init KO (code %d) -> hors-ligne", nr);
                d2vita_progress(m);
                setenv("D2NET_FAILED", "1", 1);
            } else {
                char hp[96]; snprintf(hp, sizeof hp, "%s", getenv("D2NETTEST"));
                char* col = strchr(hp, ':');
                int port = col ? atoi(col + 1) : 6112;
                if (col) *col = 0;
                int f = d2vita_net_selftest(hp, port);
                char m[64]; snprintf(m, sizeof m, "nettest: %d echec(s)", f);
                d2vita_progress(m);
            }
        } else {
            int nr = d2vita_net_start(5000);
            if (nr != 0) {
                // Only the synchronous bring-up (module/pool/NetInit/
                // CtlInit) can fail here -- same clean-failure handling as
                // before. D2NET_FAILED is read by rt_boot right before
                // arming g_netOn: stack down means sockets stay offline,
                // the game shows its own connection-failed message, and
                // solo play is unaffected.
                d2vita_progress(d2vita_net_status());
                char m[64]; snprintf(m, sizeof m, "reseau: init KO (code %d) -> hors-ligne", nr);
                d2vita_progress(m);
                setenv("D2NET_FAILED", "1", 1);
            }
            // else: bring-up ok, connection wait now running in the
            // background -- joined later in rt_boot.cpp's main().
        }
    }
    d2vita_progress("platform_init: config baked, present ready");
    d2vita_present_init();
    // Crash-report consent + network start (spec §4.3: "juste
    // apres d2vita_present_init()" — the first point in boot where a screen
    // exists to draw a dialog on, and still before the arena claim that
    // follows once this function returns to tools/rt_boot.cpp).
    d2cr::d2cr_after_present();
    return "ux0:data/d2vita/1.14d";       // the genuine Blizzard 1.14d install
}

#endif // __vita__
