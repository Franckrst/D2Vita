// src/platform/install_screen_vita.cpp -- see install_screen_vita.h.
//
// Drawn the exact same way as the crash-report consent dialog
// (src/crashreport/cr_consent_vita.cpp): a hand-drawn CDRAM framebuffer via
// sceDisplaySetFrameBuf, no sceGxm context. This runs even earlier in boot
// than that dialog (right after d2vita_platform_init() returns, before
// Game.exe is even opened), for the same reason that one avoids sceGxm --
// there is no color surface or sync object to hand a SceCommonDialog yet,
// and initializing sceGxm here on a throwaway basis would compete with the
// game's own later, real init in vita_gxm.cpp.
//
// This does NOT read platform/vita_present.h (no D2VITA_PROGRESS_PATH
// dependency), so unlike its caller it needs no D2VPK_TAG-flavored rebuild;
// tools/build_rt_boot_vpk.sh's NEEDS_TAG list is correctly left untouched.
#include "platform/install_screen_vita.h"

#ifdef __vita__
#include "platform/vita_kb.h"   // d2kb::draw_detail::{rect,frame,text,rgb}
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr int kScrW = 960, kScrH = 544;

void* alloc_fb(SceUID* uid) {
    uint32_t sz = (kScrW * kScrH * 4 + 0x3FFFF) & ~0x3FFFFu;
    *uid = sceKernelAllocMemBlock("d2vita_installcheck_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, nullptr);
    if (*uid < 0) {
        uint32_t sz2 = (kScrW * kScrH * 4 + 0xFFFFF) & ~0xFFFFFu;
        *uid = sceKernelAllocMemBlock("d2vita_installcheck_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz2, nullptr);
    }
    if (*uid < 0) return nullptr;
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0) return nullptr;
    return base;
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
} // namespace

void d2vita_show_missing_files_screen(const std::string& dir, const std::vector<std::string>& missing) {
    if (missing.empty()) return;
    using namespace d2kb::draw_detail;
    SceUID uid;
    void* base = alloc_fb(&uid);
    if (!base) return;   // can't draw -- boot_progress.txt still has the detail
    uint32_t* fb = (uint32_t*)base;

    rect(fb, kScrW, kScrH, 0, 0, kScrW, kScrH, rgb(0x10, 0x10, 0x10));
    const int panelX = 60, panelY = 60, panelW = kScrW - 120, panelH = kScrH - 120;
    rect(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x20, 0x20, 0x20));
    frame(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0xC0, 0x40, 0x40));

    int y = panelY + 20, x = panelX + 24;
    text(fb, kScrW, kScrH, "FICHIER(S) MANQUANT(S) OU INVALIDE(S) / MISSING OR INVALID FILE(S)", x, y, 1, rgb(0xFF, 0xC0, 0x40)); y += 32;
    char hdr[160]; std::snprintf(hdr, sizeof hdr, "dans / in: %s", dir.c_str());
    text(fb, kScrW, kScrH, hdr, x, y, 1, rgb(0xA0, 0xD0, 0xFF)); y += 28;

    for (size_t i = 0; i < missing.size() && y < panelY + panelH - 60; ++i) {
        std::string l = "- " + missing[i];
        text(fb, kScrW, kScrH, l.c_str(), x, y, 1, rgb(0xE0, 0xE0, 0xE0)); y += 20;
    }

    y = panelY + panelH - 36;
    text(fb, kScrW, kScrH, "detail: ux0:data/d2vita/boot_progress.txt", x, y, 1, rgb(0x80, 0x80, 0x80));
    // No "press a button to continue" caption: a required file is missing,
    // so there is nothing to continue TO -- the wait below is only to give
    // the player time to read this before the process exits on its own,
    // and a button just skips that wait, it doesn't fix anything.

    present(fb);
    sceDisplayWaitVblankStart();   // let the SetFrameBuf actually land before waiting

    // Seed with whatever is ALREADY held -- typically Cross, still
    // physically down from launching the app in VitaShell a moment
    // earlier -- so the first poll doesn't edge-detect that residual press
    // as a fresh one and instantly skip a screen the player never saw.
    // Same fix, same reason, as cr_consent_vita.cpp's run_native().
    uint32_t prevButtons = 0;
    { SceCtrlData seed; std::memset(&seed, 0, sizeof seed);
      if (sceCtrlPeekBufferPositive(0, &seed, 1) >= 1) prevButtons = seed.buttons; }

    for (int i = 0; i < 300; ++i) {   // ~10 s at ~30 polls/s, then continue anyway
        SceCtrlData pad; std::memset(&pad, 0, sizeof pad);
        if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 1) {
            const uint32_t pressed = pad.buttons & ~prevButtons;
            prevButtons = pad.buttons;
            if (pressed) break;
        }
        sceKernelDelayThread(33 * 1000);
    }
}

void d2vita_show_version_error_screen(const std::string& dir, const std::string& found) {
    using namespace d2kb::draw_detail;
    SceUID uid;
    void* base = alloc_fb(&uid);
    if (!base) return;   // can't draw -- boot_progress.txt still has the detail
    uint32_t* fb = (uint32_t*)base;

    rect(fb, kScrW, kScrH, 0, 0, kScrW, kScrH, rgb(0x10, 0x10, 0x10));
    const int panelX = 60, panelY = 60, panelW = kScrW - 120, panelH = kScrH - 120;
    rect(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0x20, 0x20, 0x20));
    frame(fb, kScrW, kScrH, panelX, panelY, panelW, panelH, rgb(0xC0, 0x40, 0x40));

    // 8 px per glyph at scale 1: ~100 characters fit on a panel line.
    int y = panelY + 20, x = panelX + 24;
    text(fb, kScrW, kScrH, "FICHIERS DU JEU INVALIDES / INVALID GAME FILES", x, y, 1, rgb(0xFF, 0xC0, 0x40)); y += 34;
    text(fb, kScrW, kScrH, "D2Vita only runs the OFFICIAL Diablo II: Lord of Destruction 1.14d files:", x, y, 1, rgb(0xE0, 0xE0, 0xE0)); y += 22;
    text(fb, kScrW, kScrH, "one monolithic Game.exe (3618792 bytes, built 2016-05-31) + the MPQs.", x, y, 1, rgb(0xE0, 0xE0, 0xE0)); y += 30;
    char fnd[200]; std::snprintf(fnd, sizeof fnd, "Found: %s", found.c_str());
    text(fb, kScrW, kScrH, fnd, x, y, 1, rgb(0xFF, 0x90, 0x90)); y += 30;
    text(fb, kScrW, kScrH, "-> Install Blizzard's official 1.14d Game.exe + MPQs and copy them to:", x, y, 1, rgb(0xA0, 0xD0, 0xFF)); y += 22;
    char hdr[160]; std::snprintf(hdr, sizeof hdr, "   %s", dir.c_str());
    text(fb, kScrW, kScrH, hdr, x, y, 1, rgb(0x90, 0xB0, 0xE0)); y += 30;
    text(fb, kScrW, kScrH, "Le port ne fonctionne qu'avec les fichiers officiels Diablo II LoD 1.14d.", x, y, 1, rgb(0xC0, 0xC0, 0xC0));

    y = panelY + panelH - 36;
    text(fb, kScrW, kScrH, "detail: ux0:data/d2vita/boot_progress.txt", x, y, 1, rgb(0x80, 0x80, 0x80));

    present(fb);
    sceDisplayWaitVblankStart();

    // Same residual-press seeding as the missing-files screen: don't let the
    // button still held from launching in VitaShell edge-detect as a fresh
    // press and skip a screen the player never saw.
    uint32_t prevButtons = 0;
    { SceCtrlData seed; std::memset(&seed, 0, sizeof seed);
      if (sceCtrlPeekBufferPositive(0, &seed, 1) >= 1) prevButtons = seed.buttons; }
    for (int i = 0; i < 300; ++i) {   // ~10 s, then continue (the process exits anyway)
        SceCtrlData pad; std::memset(&pad, 0, sizeof pad);
        if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 1) {
            const uint32_t pressed = pad.buttons & ~prevButtons;
            prevButtons = pad.buttons;
            if (pressed) break;
        }
        sceKernelDelayThread(33 * 1000);
    }
}
#else
void d2vita_show_missing_files_screen(const std::string&, const std::vector<std::string>&) {}
void d2vita_show_version_error_screen(const std::string&, const std::string&) {}
#endif
