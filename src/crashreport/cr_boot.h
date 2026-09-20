// src/crashreport/cr_boot.h — orchestration entry points called from the
// real boot sites (tools/rt_boot.cpp, src/platform/d2_boot_config.cpp).
// Implemented by cr_boot.cpp (Vita-only: it owns the sceKernel thread that
// runs the uploader, so it is never built for the host tests or the
// arm-linux-gnueabihf qemu-arm gate — see tools/crashreport_srcs.sh). The
// four entry points also called from tools/rt_boot.cpp (a file built for
// EVERY target: Vita, the qemu-arm ARM-Linux gate, and the host) get a
// no-op fallback below so those builds keep compiling unchanged, exactly
// the pattern src/platform/vita_present.h already uses for d2vita_progress
// et al.
//
// d2cr_boot_collect() and d2cr_after_present() are called only from
// src/platform/d2_boot_config.cpp, which is entirely #ifdef __vita__ (it
// never exists as a translation unit off-Vita), so they need no such
// fallback.
#pragma once
#include <cstdint>
#include <cstring>

namespace d2cr {

// ---- pure gating rules (no Vita dependency: shared by cr_boot.cpp,
// cr_consent_vita.cpp, and tools/tests/*_test.cpp) --------------------------

// The ABSOLUTE RULE: this project's own unattended deterministic
// bench/regression scripts drive the console via D2SCRIPT (frame-scheduled
// input) or D2CMDFILE (polled runtime injection, see tools/rt_boot.cpp and
// src/runtime/scripted_input.cpp) — see the file for what each becomes NULL
// or "" under. A dialog blocking one of those silently hangs every future
// automated run in this repo, so this checks NON-EMPTY, not merely set:
// d2_boot_config.cpp always setenv("D2SCRIPT", "", 1) for a normal build
// (SCRIPT defaults to the empty string outside D2VPK_TAG measurement
// flavors), so getenv("D2SCRIPT") is never NULL — only ever empty or not.
inline bool should_suppress_dialog(const char* d2script, const char* d2cmdfile) {
  return (d2script && *d2script) || (d2cmdfile && *d2cmdfile);
}

// D2_CRASHREPORT=0: the reporter is off entirely — no collection AND no
// sending, not just no dialog (spec §4.3/§4.9).
inline bool crashreport_disabled(const char* d2_crashreport) {
  return d2_crashreport && std::strcmp(d2_crashreport, "0") == 0;
}

// D2_CRASHREPORT=ask: forget any stored "always send" choice (consent.txt)
// and ask again at the next opportunity.
inline bool crashreport_ask_again(const char* d2_crashreport) {
  return d2_crashreport && std::strcmp(d2_crashreport, "ask") == 0;
}

// ---- boot orchestration (Vita only; cr_boot.cpp) ---------------------------

// Called from d2vita_platform_init() (src/platform/d2_boot_config.cpp)
// BEFORE the sceIoRemove/sceIoRename pair that rotates D2VITA_PROGRESS_PATH
// to "_prev" (spec §4.1/§4.2): reads the previous run's
// reports/session.txt, builds Evidence from it (boot_progress, Crash.txt,
// a matching psp2core dump), copies whatever pieces exist into the outbox —
// all of this BEFORE that rotation can destroy the evidence — then starts
// the new session (fresh session.txt, install_id.txt created on first boot,
// sceCoredumpWriteUserData stamped for THIS run's own future dump). A
// D2_CRASHREPORT=0 skips collection entirely (still writes session.txt: the
// stop/clean-exit hooks need somewhere to record state, but build_evidence
// is never called and the outbox is never touched).
void d2cr_boot_collect();

// Called right after d2vita_present_init(), inside d2vita_platform_init()
// (spec §4.3: "juste après d2vita_present_init()") — the first point in
// boot where a screen exists to draw a dialog on. Runs the consent flow
// only if the outbox holds at least one report still pending an answer,
// subject to the ABSOLUTE RULE (should_suppress_dialog) and
// D2_CRASHREPORT. When a report ends up consented (Send/AlwaysSend, or a
// report already carrying consent=granted from an earlier boot), starts the
// network stack usage (see cr_boot.cpp for how it reuses or brings up
// vita_net.h's sceNet stack) and the d2cr_upload background thread — never
// otherwise: per spec §4.9, the network is touched only when there is
// something consented to send.
void d2cr_after_present();

#ifdef __vita__
// Called once Game.exe's load base is known (tools/rt_boot.cpp, right after
// `g_d2base = br.module_base(exeLogical);`): recorded into session.txt so a
// FUTURE boot's evidence collector can format this run's crash addresses as
// Game+0x<rva> even when the fault itself does not carry a base (spec
// §4.4: "la base vient de game_base (session) ou de la liste de modules du
// Crash.txt").
void d2cr_session_game_loaded(uint32_t game_base);

// Called from d2vita_platform_init() once the EFFECTIVE write root is known —
// after env.txt has been read and after the root has been created and probed.
// Until then the session record holds the compile-time default, which is wrong
// for anyone who set D2WRITE (docs-site/gains.md recommends doing exactly
// that). The record is what the NEXT boot's evidence collector uses to find
// crash.log and Crash.txt, so a stale value there does not just lose evidence:
// it uploads another session's crash.log in its place.
void d2cr_session_write_root(const char* root);

// Called once, right after the scheduler-stopped diagnostic block (both the
// coop and the native branches — tools/rt_boot.cpp, "scheduler stopped:"):
// records stop_reason/main_exit into session.txt. This can fire on an
// otherwise-normal run that still falls through to d2cr_session_clean_exit()
// right after (spec §2: "CLEAN EXIT ne prouve rien" — the same
// rt_boot.cpp function logs both on a genuine abnormal exit); the evidence
// rules of cr_evidence.cpp look at stop_reason itself, not at whether
// clean-exit was ALSO logged, so this ordering does not misclassify a run.
void d2cr_session_stopped(const char* stop_reason, uint32_t main_exit);

// Called right where d2_crashlog("CLEAN EXIT (game path, frame=%d)", ...)
// is logged (tools/rt_boot.cpp), BEFORE the ordered teardown block that
// follows it: records state=exited into session.txt.
void d2cr_session_clean_exit();

// Called inside that same ordered, bounded teardown block (tools/rt_boot.cpp,
// same rule as d2vita_watchdog_stop: "a thread still alive at that point is
// not defensible in any mode"). Stops the d2cr_upload thread and WAITS,
// bounded, for it to actually reach DORMANT — aborting any in-flight HTTP
// request rather than letting it run past this call — before returning; the
// teardown block calls this before it reaches sceKernelExitProcess(). A
// no-op if no upload thread was ever started (D2_CRASHREPORT=0, or nothing
// was ever consented this run).
void d2cr_shutdown();
#else
static inline void d2cr_session_game_loaded(uint32_t) {}
static inline void d2cr_session_stopped(const char*, uint32_t) {}
static inline void d2cr_session_clean_exit() {}
static inline void d2cr_shutdown() {}
#endif

}  // namespace d2cr
