// src/crashreport/cr_consent_vita.cpp — boot-time consent dialog (spec
// §4.3).
//
// ---- sceMsgDialog vs. the native fallback screen (read this before
// touching the D2_CRASHREPORT_MSGDIALOG knob below) -------------------------
//
// sceMsgDialog is a SceCommonDialog: every frame it is up, the app must call
// sceCommonDialogUpdate() with a SceCommonDialogUpdateParam whose
// renderTarget names a real sceGxm color surface (width/height/stride,
// pixel format, colorSurfaceData) AND a live SceGxmSyncObject*
// (psp2/common_dialog.h). This is not an implementation detail this file
// works around — it is the documented contract of the whole SceCommonDialog
// family, and this codebase hits the identical wall elsewhere:
// third_party/winx86/src/platform/vita_kb.h's own top comment explains, in
// these exact terms, why the SYSTEM keyboard (SceIme, another
// SceCommonDialog) was rejected in favor of the hand-drawn virtual keyboard —
// "requires an initialized sceGxm, a color surface, and a sync object
// advanced via sceGxmDisplayQueueAddEntry. The default display path
// (vita_present.cpp) has neither — it sets a CDRAM memblock via
// sceDisplaySetFrameBuf".
//
// d2cr_after_present() (cr_boot.cpp) runs this dialog immediately after
// d2vita_present_init(), i.e. before Game.exe is even loaded — before
// anything could have called vita_gxm.cpp's d2gxm_init(). At that point in
// boot there is no sceGxm context, no color surface, and — critically — no
// way to synthesize a SceGxmSyncObject* without first calling
// sceGxmInitialize(), which this file does NOT do: initializing sceGxm here,
// on a throwaway basis, purely to satisfy one dialog call, would compete
// with the game's OWN later (and real) sceGxm init in vita_gxm.cpp — Vita
// only tolerates one sceGxm context per process.
//
// run_msg_dialog() below is still built, since trying it costs nothing but a
// knob, but it is analytically expected to fail the same way the system
// keyboard already does, for the same documented reason, and has never been
// run on real hardware or Vita3K — only compiled and analyzed. Confusingly,
// "does not render" is not even the worst case: SceCommonDialogErrorCode
// even NAMES this exact scenario (SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED
// = 0x80020436), so if this ever runs on real hardware, watch the
// "consent: msgdialog update rc=" line in boot_progress for that code — it
// would confirm the analysis instead of merely being consistent with it. Do
// not read a future "it compiles and links" as this analysis being wrong: no
// dialog blocking here has ever been observed to render.
//
// Consequence: D2_CRASHREPORT_MSGDIALOG defaults to UNSET (native fallback
// only). Setting it to 1 tries sceMsgDialog first, with a short grace window
// (kMsgDialogGraceMs) — if it has not reached FINISHED by then, this file
// aborts it and falls back to the native screen for the REST of the 60 s
// budget, so a maintainer curious enough to flip the knob on real hardware
// never loses the dialog entirely if the analysis above turns out right.
//
// ---- Native fallback screen ------------------------------------------------
//
// Drawn directly into an independently-allocated CDRAM framebuffer via
// sceDisplaySetFrameBuf (the same primitive vita_present.cpp uses, same
// pixel format A8B8G8R8) — never vita_present.cpp's own g_fb[] buffers: at
// this point in boot d2vita_present_init() has already started its
// presentation thread (present_thread / present_pipe_thread), parked on a
// semaphore with nothing posted to it yet (no frame exists before Game.exe
// runs), so it never touches the display while this dialog owns it, and this
// file never touches g_fb[]/g_sema in return. Ownership of the physical
// display does not pass back on its own: without an explicit last frame,
// whatever this screen last drew (the countdown panel, or the Send/
// AlwaysSend acknowledgment) stays on screen, frozen, for as long as boot
// takes to reach Game.exe's own first present() — indistinguishable from a
// hang. run_native() therefore draws one last plain frame right before it
// returns, precisely so "passes back" is something this file does, not
// something it merely leaves to the next owner — see the comment at the end
// of that function.
//
// third_party/winx86/src/platform/vita_kb.h's font covers ASCII 32..126
// only, so the French text below is a plain-ASCII transliteration (accents
// stripped: "ferme" not "fermé") for THIS path specifically — the sceMsgDialog
// path above, when/if it ever renders, gets the properly accented UTF-8
// string, since Sce's own dialog font supports it. This is a deliberate
// consequence of reusing the existing bitmap font, not an oversight; the two
// strings are kept side by side below so the difference is visible in one
// place.
//
// ---- Native screen behavior: language, double buffering, hand-off ---------
//
// The native screen shows English text only (kMsgEn/kBodyEn/kButtonsEn) — a
// deliberate, permanent departure from spec §4.3's original locale-following
// design for this one screen specifically (see draw_dialog() below).
// kMsgFr/kBodyFr/kButtonsFr stay in the source as working, tested code,
// simply not selected. run_msg_dialog() below stays FR/EN-aware on purpose
// (see its own comment), so the two paths are intentionally allowed to
// disagree.
//
// The screen uses real double buffering — two independent CDRAM blocks, so
// the buffer being redrawn is never the one currently on screen — see the
// comment above run_native() for the full invariant and why no
// sceDisplayWaitVblankStart() is needed inside the redraw loops themselves.
// Send/AlwaysSend get a brief, bounded "Sending..." acknowledgment
// (draw_sending()) of the BUTTON PRESS only; it never watches the real
// (background, spec §4.9) upload. Whichever way the dialog ends (Send,
// DontSend, AlwaysSend, or the budget running out), one last plain blank
// frame is drawn and presented before control returns, so the hand-off to
// Game.exe's own first present() is explicit rather than left to chance —
// see the comment at the end of run_native(), and
// clear_screen_before_handoff() for the two show_consent() early-return
// paths that need the same treatment.
#include "crashreport/cr_consent.h"

#include "crashreport/cr_boot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>

#include "platform/vita_kb.h"
#include "platform/vita_present.h"

// Defined in src/platform/vita_present.cpp, always linked into the eboot;
// reused here rather than re-initializing SceAppUtil a second time (spec
// §4.3: language follows SCE_SYSTEM_PARAM_ID_LANG, French vs. everything
// else).
extern "C" uint32_t d2vita_sys_lcid(void);

namespace d2cr {

namespace {

constexpr int kScrW = 960, kScrH = 544;
constexpr uint32_t kMsgDialogGraceMs = 3000;   // see the sceMsgDialog comment above
constexpr uint32_t kPollMs = 33;               // ~30 Hz input/redraw tick

// ---- text (English is already plain ASCII; French is the accent-stripped
// transliteration the native font can render — see the file comment) -------
//
// kMsgFr/kBodyFr/kButtonsFr: not selected by draw_dialog() (see the comment
// there and the one at the top of this file) — kept in the source as
// working, tested code, simply not read by anything any more.
// [[maybe_unused]] says so explicitly instead of letting
// this silently regress into an unused-variable build error under leg 7's
// -Werror (tools/tests/run_crashreport_tests.sh, the Vita .text-size leg).
[[maybe_unused]] const char* kMsgFr[2] = {
    "Diablo II s'est ferme de facon inattendue.",   // [0] crash
    "Diablo II semble s'etre bloque.",              // [1] hang
};
[[maybe_unused]] const char* kBodyFr =
    "Envoyer un rapport anonyme pour aider a corriger le probleme ? Il contient "
    "la version du jeu, le type d'erreur, les adresses du plantage et les "
    "journaux ; les cles CD et le nom de compte sont effaces.";
[[maybe_unused]] const char* kButtonsFr = "X Envoyer     O Ne pas envoyer     Triangle Toujours envoyer sans demander";

const char* kMsgEn[2] = {
    "Diablo II closed unexpectedly.",
    "Diablo II seems to be stuck.",
};
const char* kBodyEn =
    "Send an anonymous report to help fix the problem? It contains the game "
    "version, the error type, the crash addresses and the logs; CD keys and "
    "the account name are removed.";
const char* kButtonsEn = "X Send     O Don't send     Triangle Always send, don't ask again";

// UTF-8 accented text, for the sceMsgDialog path only (SCE_MSG_DIALOG_USER_MSG_SIZE = 512).
const char* kMsgDialogFr[2] = {
    "Diablo II s'est ferm\xC3\xA9 de fa\xC3\xA7on inattendue.",
    "Diablo II semble s'\xC3\xAAtre bloqu\xC3\xA9.",
};
const char* kMsgDialogBodyFr =
    "Envoyer un rapport anonyme pour aider \xC3\xA0 corriger le probl\xC3\xA8me ? "
    "Il contient la version du jeu, le type d'erreur, les adresses du plantage "
    "et les journaux ; les cl\xC3\xA9s CD et le nom de compte sont effac\xC3\xA9s.";

bool is_french() { return d2vita_sys_lcid() == 0x040C; }

// ---- native fallback: framebuffer, drawing, input --------------------------

void* alloc_dialog_fb(SceUID* uid) {
    uint32_t sz = (kScrW * kScrH * 4 + 0x3FFFF) & ~0x3FFFFu;
    *uid = sceKernelAllocMemBlock("d2cr_consent_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, nullptr);
    if (*uid < 0) {
        uint32_t sz2 = (kScrW * kScrH * 4 + 0xFFFFF) & ~0xFFFFFu;
        *uid = sceKernelAllocMemBlock("d2cr_consent_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz2, nullptr);
    }
    if (*uid < 0) return nullptr;
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0) return nullptr;
    return base;
}

// Splits `text` into lines of at most max_cols characters, breaking on
// spaces (a single word longer than max_cols is cut hard rather than
// overrunning the margin — none of the fixed strings above are that long).
std::vector<std::string> wrap(const char* text, int max_cols) {
    std::vector<std::string> lines;
    std::string cur;
    const char* p = text;
    while (*p) {
        const char* start = p;
        while (*p && *p != ' ') ++p;
        std::string word(start, p - start);
        while (*p == ' ') ++p;
        if (word.size() > (size_t)max_cols) {
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
            while (word.size() > (size_t)max_cols) { lines.push_back(word.substr(0, max_cols)); word = word.substr(max_cols); }
        }
        if (cur.empty()) cur = word;
        else if (cur.size() + 1 + word.size() <= (size_t)max_cols) cur += " " + word;
        else { lines.push_back(cur); cur = word; }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

void draw_dialog(uint32_t* fb, bool hang, const std::string& install_id, uint32_t remaining_s) {
    using namespace d2kb::draw_detail;
    rect(fb, kScrW, kScrH, 0, 0, kScrW, kScrH, rgb(0x10, 0x10, 0x10));
    const int panelX = 60, panelY = 90, panelW = kScrW - 120, panelH = kScrH - 180;
    rect(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x20, 0x20, 0x20));
    frame(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x60, 0x60, 0x60));

    // English only, a deliberate, permanent override of spec §4.3's original
    // "follows SCE_SYSTEM_PARAM_ID_LANG" design for THIS specific screen —
    // not a statement that FR/EN is wrong in general (run_msg_dialog() below
    // is left FR/EN-aware on purpose — see its own comment). is_french()
    // stays defined and used there; it is simply not consulted here.
    const char* head = kMsgEn[hang ? 1 : 0];
    const char* body = kBodyEn;
    const char* buttons = kButtonsEn;

    int y = panelY + 24, x = panelX + 24;
    const int maxCols2 = (panelW - 48) / (8 * 2);
    for (const std::string& l : wrap(head, maxCols2)) { text(fb, kScrW, kScrH, l.c_str(), x, y, 2, rgb(0xFF, 0xC0, 0x40)); y += 36; }
    y += 12;
    const int maxCols1 = (panelW - 48) / 8;
    for (const std::string& l : wrap(body, maxCols1)) { text(fb, kScrW, kScrH, l.c_str(), x, y, 1, rgb(0xE0, 0xE0, 0xE0)); y += 20; }

    y = panelY + panelH - 80;
    text(fb, kScrW, kScrH, buttons, x, y, 1, rgb(0xA0, 0xD0, 0xFF));
    y += 24;
    char small[192];
    std::snprintf(small, sizeof small, "install: %s   (auto: later in %u s)",
                  install_id.empty() ? "?" : install_id.c_str(), remaining_s);
    text(fb, kScrW, kScrH, small, x, y, 1, rgb(0x80, 0x80, 0x80));
}

// Send/AlwaysSend need a visible acknowledgment that the button press
// registered, since nothing else on screen shows it. The real upload thread
// does not even exist yet at this point: cr_boot.cpp (d2cr_after_present())
// only creates d2cr_upload AFTER show_consent() returns, so there is no live
// status this file could show even if it reached across to it — doing so
// would need bigger, riskier cross-thread plumbing this dialog does not
// otherwise need. This draws confirmation of the BUTTON PRESS only, for a
// short fixed window (see run_native()); the real HTTP upload proceeds
// unobserved in the background (spec §4.9: boot must not block on it).
//
// English-only, same rule as draw_dialog() above — this is the same screen,
// just its final state before returning.
void draw_sending(uint32_t* fb, unsigned dot_phase) {
    using namespace d2kb::draw_detail;
    rect(fb, kScrW, kScrH, 0, 0, kScrW, kScrH, rgb(0x10, 0x10, 0x10));
    const int panelX = 60, panelY = 90, panelW = kScrW - 120, panelH = kScrH - 180;
    rect(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x20, 0x20, 0x20));
    frame(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x60, 0x60, 0x60));

    // 0..3 rotating dots (dot_phase ticks once per kAckDotStepMs — see
    // run_native()). x below is centered on the WIDEST phase ("Sending..."),
    // not on this call's actual (shorter, some phases) string, so the visible
    // text always starts at the same point and only grows rightward — it
    // never re-centers/shifts as the dot count changes.
    char msg[16];
    std::snprintf(msg, sizeof msg, "Sending%.*s", (int)(dot_phase % 4), "...");
    const int scale = 2;
    const int tw = (int)std::strlen("Sending...") * 8 * scale;
    const int x = panelX + (panelW - tw) / 2;
    const int y = panelY + panelH / 2 - 8 * scale;
    text(fb, kScrW, kScrH, msg, x, y, scale, rgb(0xFF, 0xC0, 0x40));
}

// Once Send/DontSend/AlwaysSend is confirmed (or the budget times out), the
// screen would otherwise stay displayed while Diablo is loading, since
// nothing downstream repaints it — indistinguishable from a hang. This is
// the explicit clear: a plain full-screen fill, no text — a transition
// frame, not a message, so it draws nothing else. See the comment at the
// end of run_native() for where and why this is called.
void draw_blank(uint32_t* fb) {
    using namespace d2kb::draw_detail;
    rect(fb, kScrW, kScrH, 0, 0, kScrW, kScrH, rgb(0x10, 0x10, 0x10));
}

void present(uint32_t* fb) {
    SceDisplayFrameBuf sfb; std::memset(&sfb, 0, sizeof sfb);
    sfb.size = sizeof sfb;
    sfb.base = fb;
    sfb.pitch = kScrW;
    sfb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    sfb.width = kScrW;
    sfb.height = kScrH;
    sceDisplaySetFrameBuf(&sfb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

// One sample, edge-detected against *prev (so a HELD button fires once, not
// on every poll — this is a one-shot decision, not a repeatable key).
ConsentAnswer poll_buttons(uint32_t* prev) {
    SceCtrlData cd; std::memset(&cd, 0, sizeof cd);
    if (sceCtrlPeekBufferPositive(0, &cd, 1) < 1) { *prev = 0; return ConsentAnswer::Later; }
    const uint32_t pressed = cd.buttons & ~*prev;
    *prev = cd.buttons;
    if (pressed & SCE_CTRL_CROSS) return ConsentAnswer::Send;
    if (pressed & SCE_CTRL_CIRCLE) return ConsentAnswer::DontSend;
    if (pressed & SCE_CTRL_TRIANGLE) return ConsentAnswer::AlwaysSend;
    return ConsentAnswer::Later;
}

// FLICKER: a single shared buffer that is BOTH the thing being redrawn AND
// the thing sceDisplaySetFrameBuf most recently handed to the display
// hardware is unsafe here — draw_dialog() opens with a full-screen clear to
// black, so every redraw would visibly blank whatever the scanout is
// reading, mid-scan, with no synchronization back to this thread: a flicker
// on every redraw.
//
// Fix: real double buffering. Two independent CDRAM blocks, each sized for
// one surface (the same per-surface allocator as vita_present.cpp's own
// g_fb[2]/g_fb_uid[2], mirrored here with alloc_dialog_fb() called twice
// rather than inventing a single double-size block — alloc_dialog_fb()
// already matches that file's alloc_fb() one-surface-at-a-time shape).
// `back` names the buffer NOT currently referenced by the last
// sceDisplaySetFrameBuf call; every redraw draws into base[back], presents
// it, and ONLY THEN flips `back` to the other index — see the loop and the
// acknowledgment block below, both follow this same draw-then-present-
// then-flip order. So the buffer the display hardware is scanning is never
// the one being mutated: there is no instant at which a viewer could catch a
// partially-drawn frame, because the code never writes to that buffer while
// it is the presented one — `back` is defined, at every point in this
// function, to name the other one.
//
// No sceDisplayWaitVblankStart() call was added INSIDE either redraw loop,
// for two independent reasons:
//   - SCE_DISPLAY_SETBUF_NEXTFRAME (already used by present(), unchanged)
//     already defers the actual scanout-source swap to the next vblank at
//     the driver level — that is the documented difference from IMMEDIATE,
//     and it is what makes the SWAP ITSELF glitch-free without any extra
//     wait from this code.
//   - The remaining risk a wait would guard against is reusing a buffer
//     before the hardware has finished handing it off, and each loop only
//     redraws once the countdown SECOND changes (~1 Hz) — the acknowledgment
//     block further down redraws at most once per kAckDotStepMs (200 ms).
//     Both are more than an order of magnitude looser than one vblank
//     (~16.6 ms at 60 Hz): by the time either buffer is drawn into again
//     WITHIN THE SAME LOOP it has not been the presented one for many
//     vblanks already. This matches vita_present.cpp's own default present
//     path (do_scale_and_flip: draw into g_fb[g_cur],
//     sceDisplaySetFrameBuf(..., NEXTFRAME), g_cur ^= 1 — no vblank wait
//     either), which redraws every game frame (far tighter than this
//     dialog's ~1 Hz/5 Hz) and relies on exactly this same
//     NEXTFRAME-plus-flip-index pattern. Compare vita_gxm.cpp's
//     D2_GXMVSYNC>=2 sceDisplayWaitVblankStart(), which exists for a
//     genuinely tighter case this file does not have: reusing a buffer
//     within the SAME rendered frame, at up to ~50 fps.
//
// ONE seam the above does NOT cover: the BOUNDARY out of the countdown loop,
// whichever way it is left. The countdown loop can `break` — on Send,
// DontSend, OR AlwaysSend alike, all three the same way (poll_buttons()
// called unconditionally right after that iteration's conditional draw) —
// in the exact iteration it just redrew, with zero delay before whatever
// runs next reuses base[back]: the acknowledgment loop's first pass for
// Send/AlwaysSend (which draws immediately, by design, see its lastPhase
// sentinel below) or draw_blank() at the very end of this function for
// DontSend. At that boundary the "redraws only every ~1 Hz/5 Hz" argument
// above does not apply, because it is loop-to-loop, not within a loop — so
// this one spot DOES use an explicit sceDisplayWaitVblankStart(),
// unconditionally, right after the countdown loop and before branching on
// `answer`, covering Send/DontSend/AlwaysSend alike; see the comment at that
// call.
ConsentAnswer run_native(const ConsentPrompt& prompt, uint32_t budget_ms) {
    SceUID uid[2] = {-1, -1};
    void* base[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        base[i] = alloc_dialog_fb(&uid[i]);
        if (!base[i]) {
            d2vita_progress("consent: fallback fb alloc KO — rapport(s) restent en attente");
            for (int j = 0; j < 2; ++j) if (uid[j] >= 0) sceKernelFreeMemBlock(uid[j]);
            return ConsentAnswer::Later;
        }
        std::memset(base[i], 0, (size_t)kScrW * kScrH * 4);
    }
    int back = 0;   // base[back] is always the buffer safe to draw into next

    const uint64_t t0 = sceKernelGetProcessTimeWide();
    uint32_t prevButtons = 0;
    uint32_t lastDrawnRemaining = 0xFFFFFFFFu;
    ConsentAnswer answer = ConsentAnswer::Later;
    for (;;) {
        const uint64_t elapsed_ms = (sceKernelGetProcessTimeWide() - t0) / 1000ull;
        if (elapsed_ms >= budget_ms) break;
        const uint32_t remaining_s = (uint32_t)((budget_ms - elapsed_ms) / 1000ull);
        if (remaining_s != lastDrawnRemaining) {
            draw_dialog((uint32_t*)base[back], prompt.hang, prompt.install_id, remaining_s);
            present((uint32_t*)base[back]);
            back ^= 1;
            lastDrawnRemaining = remaining_s;
        }
        answer = poll_buttons(&prevButtons);
        if (answer != ConsentAnswer::Later) break;
        sceKernelDelayThread(kPollMs * 1000u);
    }

    // This call must sit HERE — unconditionally, right after the countdown
    // loop and before branching on `answer` — not inside the `if` just below
    // that guards only the Send/AlwaysSend transition into the acknowledgment
    // loop. The countdown loop above can `break` in the exact iteration it
    // just called present()+back^=1, with zero delay before whatever runs
    // next reuses base[back] — roughly a 1-in-~30 chance per press, since a
    // redraw only happens once per ~30 polls (kPollMs=33ms vs. the ~1 Hz
    // countdown) — and poll_buttons() is called unconditionally right after
    // that iteration's conditional draw, so this is exactly as true for
    // DontSend as for Send/AlwaysSend: all three break the same way, in the
    // same spot. DontSend skips the `if` below entirely and falls straight
    // through to the draw_blank()+present() call at the end of this
    // function — guarding only inside the `if` would leave that path writing
    // its first frame into the buffer the display is still actively
    // scanning, the same mid-scanout-write hazard the double buffering above
    // exists to prevent. The Later/timeout exit does NOT share this hazard:
    // its `elapsed_ms >= budget_ms` check runs at the TOP of the loop, before
    // that iteration's draw, so a full kPollMs sleep from the previous
    // iteration already separates the last redraw from a timeout break.
    // Placing the wait here — once, unconditionally, right after the loop and
    // before branching on `answer` — covers Send/DontSend/AlwaysSend with a
    // single choke point instead of guarding only some exits, and it costs
    // the already-safe Later/timeout path nothing worse than one redundant
    // vblank (~16.6 ms), once, during boot.
    sceDisplayWaitVblankStart();

    // Brief, bounded acknowledgment that the button press was received —
    // see draw_sending() above for what this does and, just as deliberately,
    // does not do. DontSend gets none of this: nothing happens afterward for
    // the player to wonder about, so there is nothing to acknowledge.
    if (answer == ConsentAnswer::Send || answer == ConsentAnswer::AlwaysSend) {
        // ~1.2 s: long enough that the dot animation (a full "Sending" ->
        // "Sending..." cycle is kAckDotStepMs * 4 = 800 ms) is seen
        // completing at least once, so it reads as motion/acknowledgment
        // rather than a single static redraw; short enough to stay a brief
        // confirmation rather than any kind of stand-in for the real
        // (background, unrelated) upload, which this must NOT become —
        // spec §4.9 forbids blocking boot on the network, and this window is
        // fixed-length precisely so it can never accidentally grow into
        // that. lastPhase's 0xFFFFFFFFu sentinel (right below) forces this
        // loop's very first pass to draw_sending()+present() immediately,
        // into base[back] — safe here because the vblank wait just above
        // already proved a full vblank has elapsed since base[back] was last
        // the active scanout target.
        constexpr uint32_t kAckMs = 1200;
        constexpr uint32_t kAckDotStepMs = 200;
        const uint64_t ackT0 = sceKernelGetProcessTimeWide();
        uint32_t lastPhase = 0xFFFFFFFFu;
        for (;;) {
            const uint64_t ack_elapsed_ms = (sceKernelGetProcessTimeWide() - ackT0) / 1000ull;
            if (ack_elapsed_ms >= kAckMs) break;
            const uint32_t phase = (uint32_t)(ack_elapsed_ms / kAckDotStepMs);
            if (phase != lastPhase) {
                draw_sending((uint32_t*)base[back], phase);
                present((uint32_t*)base[back]);
                back ^= 1;
                lastPhase = phase;
            }
            sceKernelDelayThread(kPollMs * 1000u);
        }
    }

    // Whichever way run_native() is left — the countdown timing out
    // (Later), DontSend, or falling out of the acknowledgment block above
    // (Send/AlwaysSend) — control reaches this same spot with whatever was
    // last drawn still the active scanout buffer, and nothing downstream of
    // run_native() ever repaints it: cr_boot.cpp goes straight on to loading
    // Game.exe, and an un-cleared screen here would be indistinguishable
    // from a hang rather than a loading game. One last plain frame — a
    // clear, no text, see draw_blank() — using the exact same
    // draw-into-back/present/flip discipline as every redraw above:
    // base[back] is, as always, the buffer NOT currently on screen, so
    // drawing into it here is safe by the same argument as the rest of this
    // function.
    draw_blank((uint32_t*)base[back]);
    present((uint32_t*)base[back]);
    back ^= 1;

    // Which buffer is safe to free, and when, matters here: freeing both
    // CDRAM blocks unconditionally — including whichever one is the active
    // scanout target — would be a use-after-free of live display memory. The
    // sceDisplayWaitVblankStart() below only proves the swap to the blank
    // frame has landed (so the OLD dialog/ack buffer is genuinely retired);
    // it says nothing about the blank frame itself, which is now the
    // display's live, continuous scanout source from this instant until
    // Game.exe's own first present() — per cr_boot.cpp's own
    // d2vita_net_init(15000) on the granted-consent path alone, that can be
    // many seconds away. Freeing it while the display hardware is still
    // reading it on every refresh would be a use-after-free of scanout
    // memory, just narrowed to the newer of the two buffers instead of
    // either.
    //
    // Closing that gap, short of reaching into vita_present.cpp's
    // presentation thread to learn precisely when Game.exe's first frame
    // lands (bigger, riskier cross-file plumbing this dialog does not
    // otherwise need — the same tradeoff draw_sending() above makes about
    // the upload thread), means never freeing that buffer at all:
    //   - Once the vblank wait below returns, the swap scheduled by the
    //     present() call three lines up is proven to have landed — so
    //     base[back] (named AFTER the `back ^= 1` above, i.e. the buffer
    //     that held the LAST dialog/ack frame, NOT the blank one) is now
    //     definitely off screen, and freeing it is genuinely safe rather
    //     than merely likely-safe.
    //   - base[1 - back] — the blank frame just presented — is the opposite:
    //     definitely ON screen, for an unknown and potentially long window.
    //     It is deliberately left allocated. One CDRAM block (~2 MiB, see
    //     alloc_dialog_fb) leaked for the remaining life of the process is a
    //     small, bounded, one-time cost against a hard guarantee that
    //     nothing ever hands that exact memory to a later allocation
    //     (Game.exe's own sceGxm/CDRAM setup, most plausibly) while the
    //     display is still reading it — which is what would turn into
    //     visible memory-garbage on the physical screen, strictly worse than
    //     a small permanent leak.
    sceDisplayWaitVblankStart();
    if (uid[back] >= 0) sceKernelFreeMemBlock(uid[back]);
    // uid[1 - back] (the blank frame now on screen) is intentionally left
    // allocated — see above; freeing it here would race the display
    // hardware for as long as Game.exe takes to present its own first frame.
    return answer;
}

// show_consent()'s two early-return paths — sceMsgDialog reaching FINISHED,
// and the msg-dialog grace period exhausting the whole budget — skip
// run_native() entirely, so without this function, whatever was last on
// screen would stay frozen through the rest of boot: the same
// indistinguishable-from-a-hang hazard run_native()'s own trailing
// draw_blank()+present() exists to prevent (see the comment at the end of
// that function), just reached from a sibling path. Low practical risk (the
// sceMsgDialog analysis above expects sceMsgDialogInit()/Update() to fail
// before ever rendering, and kMsgDialogGraceMs=3000 vs.
// kConsentTimeoutMs=60000 makes the budget_ms==0 branch essentially
// unreachable) — but the same bug class, so fixed the same way: one plain
// blank frame, presented and given a vblank to land, right before
// show_consent() returns on either path.
//
// Unlike run_native()'s trailing blank, there is no pre-existing
// double-buffer pair to reuse here: this is a fresh, single CDRAM
// allocation, presented for the first time. That means no REUSE-of-a-live-
// buffer hazard on the draw side (the flicker hazard described above run_
// native() is specifically about redrawing into a buffer already on screen;
// this buffer has never been shown before, so there is nothing to race
// there) — but the FREE side has the exact hazard "Which buffer is safe to
// free, and when" (above, in run_native()) fixes, and gets the same fix:
// never free it. One leaked CDRAM block on this rare path is the same
// small, bounded price paid above, for the same reason.
void clear_screen_before_handoff() {
    SceUID uid = -1;
    void* base = alloc_dialog_fb(&uid);
    if (!base) return;   // best-effort only — same spirit as run_msg_dialog()'s own render attempt
    draw_blank((uint32_t*)base);
    present((uint32_t*)base);
    sceDisplayWaitVblankStart();
    // Intentionally never freed — see the comment above and the identical
    // choice at the end of run_native().
}

// Best-effort sceMsgDialog attempt (see the sceMsgDialog comment above).
// Returns false without ever reaching FINISHED — the caller then falls back
// to the native screen for the rest of the time budget.
//
// Left FR/EN-aware on purpose, NOT switched to English-only like
// draw_dialog() above: draw_dialog()'s English-only choice is about the
// native fallback screen specifically, the only path that has ever actually
// rendered anything. Per the analysis above, this sceMsgDialog path is
// expected to fail before ever rendering
// (SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED), is off by default
// (D2_CRASHREPORT_MSGDIALOG=1 only), and even when tried, a failure here
// falls straight through to the English-only fallback for the rest of the
// budget. Known, flagged inconsistency if that analysis ever turns out
// wrong: a maintainer who both sets that knob AND is on a French system
// would see French here, then English if/when this aborts to run_native()
// — judged lower-risk than rewriting the one path that still honors spec
// §4.3's original FR/EN design, for a code path with no confirmed hardware
// behavior at all yet.
bool run_msg_dialog(const ConsentPrompt& prompt, ConsentAnswer* out) {
    const bool fr = is_french();
    const char* head = fr ? kMsgDialogFr[prompt.hang ? 1 : 0] : kMsgEn[prompt.hang ? 1 : 0];
    const char* body = fr ? kMsgDialogBodyFr : kBodyEn;
    char msg[SCE_MSG_DIALOG_USER_MSG_SIZE];
    std::snprintf(msg, sizeof msg, "%s %s", head, body);

    SceMsgDialogButtonsParam bp; std::memset(&bp, 0, sizeof bp);
    bp.msg1 = fr ? "Envoyer" : "Send";
    bp.msg2 = fr ? "Ne pas envoyer" : "Don't send";
    bp.msg3 = fr ? "Toujours envoyer" : "Always send";

    SceMsgDialogUserMessageParam ump; std::memset(&ump, 0, sizeof ump);
    ump.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS;
    ump.msg = (const SceChar8*)msg;
    ump.buttonParam = &bp;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &ump;

    int irc = sceMsgDialogInit(&param);
    if (irc < 0) {
        char m[80]; std::snprintf(m, sizeof m, "consent: sceMsgDialogInit rc=0x%08x — repli natif", (unsigned)irc);
        d2vita_progress(m);
        return false;
    }
    d2vita_progress("consent: sceMsgDialog tente (D2_CRASHREPORT_MSGDIALOG=1) — voir le commentaire S1 de cr_consent_vita.cpp");

    // Own CDRAM surface as the best-effort render target: no sceGxm context
    // exists at this point (see the file comment), so this is offered on a
    // "try it and log the rc" basis, not because it is expected to satisfy
    // the contract. displaySyncObject stays null: fabricating one needs
    // sceGxmInitialize, out of scope here (see the comment above).
    SceUID uid = -1;
    void* base = alloc_dialog_fb(&uid);

    const uint64_t t0 = sceKernelGetProcessTimeWide();
    bool finished = false;
    int lastUpdateRc = 0;
    for (;;) {
        const uint64_t elapsed_ms = (sceKernelGetProcessTimeWide() - t0) / 1000ull;
        if (elapsed_ms >= kMsgDialogGraceMs) break;
        if (base) {
            SceCommonDialogUpdateParam up; std::memset(&up, 0, sizeof up);
            up.renderTarget.colorSurfaceData = base;
            up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
            up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
            up.renderTarget.width = kScrW;
            up.renderTarget.height = kScrH;
            up.renderTarget.strideInPixels = kScrW;
            up.displaySyncObject = nullptr;
            lastUpdateRc = sceCommonDialogUpdate(&up);
            present((uint32_t*)base);
        }
        if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) { finished = true; break; }
        sceKernelDelayThread(kPollMs * 1000u);
    }
    { char m[96]; std::snprintf(m, sizeof m, "consent: msgdialog update rc=0x%08x finished=%d (grace %ums)",
                                (unsigned)lastUpdateRc, finished ? 1 : 0, (unsigned)kMsgDialogGraceMs);
      d2vita_progress(m); }

    ConsentAnswer answer = ConsentAnswer::Later;
    if (finished) {
        SceMsgDialogResult res; std::memset(&res, 0, sizeof res);
        if (sceMsgDialogGetResult(&res) >= 0) {
            switch (res.buttonId) {
                case SCE_MSG_DIALOG_BUTTON_ID_BUTTON1: answer = ConsentAnswer::Send; break;
                case SCE_MSG_DIALOG_BUTTON_ID_BUTTON2: answer = ConsentAnswer::DontSend; break;
                case SCE_MSG_DIALOG_BUTTON_ID_BUTTON3: answer = ConsentAnswer::AlwaysSend; break;
                default: answer = ConsentAnswer::Later; break;
            }
        }
    } else {
        sceMsgDialogAbort();
    }
    sceMsgDialogTerm();
    if (uid >= 0) sceKernelFreeMemBlock(uid);
    if (!finished) return false;
    *out = answer;
    return true;
}

}  // namespace

ConsentAnswer show_consent(const ConsentPrompt& prompt, bool try_msg_dialog) {
    // Belt and suspenders: cr_boot.cpp already refuses to call this function
    // at all under the ABSOLUTE RULE, but a dialog silently blocking one of
    // this project's own unattended benches (D2SCRIPT/D2CMDFILE) is exactly
    // the kind of regression that must not depend on every future call site
    // remembering the check.
    if (should_suppress_dialog(getenv("D2SCRIPT"), getenv("D2CMDFILE"))) return ConsentAnswer::Later;

    uint32_t budget_ms = kConsentTimeoutMs;
    if (try_msg_dialog) {
        ConsentAnswer msgAnswer;
        const uint64_t before = sceKernelGetProcessTimeWide();
        // clear_screen_before_handoff() on this return, and on the
        // budget_ms==0 one below, so this path also never leaves the screen
        // frozen — see the comment above that function.
        if (run_msg_dialog(prompt, &msgAnswer)) { clear_screen_before_handoff(); return msgAnswer; }
        const uint64_t spent_ms = (sceKernelGetProcessTimeWide() - before) / 1000ull;
        budget_ms = spent_ms >= budget_ms ? 0 : budget_ms - (uint32_t)spent_ms;
    }
    if (budget_ms == 0) { clear_screen_before_handoff(); return ConsentAnswer::Later; }
    return run_native(prompt, budget_ms);
}

}  // namespace d2cr
