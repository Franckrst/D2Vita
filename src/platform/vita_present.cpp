// src/platform/vita_present.cpp — Vita-only. See vita_present.h.
#ifdef __vita__
#include "runtime/gil.h"
#include <malloc.h>
#include "runtime/trapcnt.h"   // trap counts per slot: the denominator for the "traversals" cost line
#ifdef D2_TLSWRAP
// Counter defined in rt_boot.cpp (measurement build D2VPK_TLSWRAP=1). Must be
// declared at FILE scope: an extern "C" linkage spec is not allowed inside a
// function body.
extern "C" unsigned d2_emutls_calls;
extern "C" int d2_tlswrap_dump(char*, unsigned);
#endif
#include "platform/vita_present.h"
#include "platform/vita_net.h"
#include "platform/vita_kb.h"   // full virtual keyboard (layouts, font, drawing)
#include "platform/radial_menu.h"   // 7-sector radial menu (hold Select + left stick)
#include "platform/pad_core.h"      // scheme v2 ("aim"): pure core
#include "runtime/pad_state.h"      // per-frame guest snapshot (hooks)
#include "runtime/scripted_input.h" // d2vita_vpad_get
#include "runtime/text_focus_probe.h"   // d2vita_text_focus : edit box focalisee (ouverture auto du clavier)
#include "platform/vita_gxm.h"      // d2gxm_ui_active: le menu part-il sur le GPU ?
#include "platform/vita_host.h"        // engine: log, cores, sleep (CONSOLE-specific)
#include "runtime/host_clock.h"           // engine: monotonic host clock
#include "runtime/scripted_input.h"       // inj_set_bounds : le curseur injecte suit la taille du jeu
#include "platform/present_scale.h"   // engine: generic scaling (D2_PRESENT_WX86)
#include "crashreport/build_id.h"     // d2cr::build_id() : etiquette de build a l'ecran

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>    // D2_SCHEDPROBE probe (kernel threads)
#include <psp2/kernel/cpu.h>          // SCE_KERNEL_CPU_MASK_USER_n (D2_COEURS)
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
#include <pthread.h>   // v5 probe: reproduces the heartbeat's pte code path

// VitaSDK runtime sizing. rt_boot's main() has a very large frame (hundreds of
// shim lambdas) and loads ~40 MiB of DLL images onto the C++ heap via slurp()
// + PeImage — the default main-thread stack and newlib heap are far too small,
// which crashes the boot silently at a fixed point. The dynarec arena is a
// separate memblock, so this heap is only for C++/STL + module bytes.
extern "C" {
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;    // 4 MiB host stack
// 38 MiB newlib heap. The heap is reserved as one fixed block at boot: this
// ceiling is hard regardless of how much system RAM is actually free, and
// std::bad_alloc hits it even with plenty of system memory left. It also has
// to leave room for the JIT code cache, which draws from the same reserved
// block and can grow to ~13 MiB mid-game. 38 MiB sits a few MiB above the
// observed peak heap usage (~30.6 MiB) while still leaving headroom for the
// JIT cache — a larger reservation starves that headroom instead. Total user
// budget: arena (285 MiB, see cpu_box86.cpp) + 38 MiB heap + 4 MiB stack =
// 327 MiB, within the extended-memory app mode budget (param.sfo ATTRIBUTE2,
// set in build_rt_boot_vpk.sh).
unsigned int _newlib_heap_size_user     = 38 * 1024 * 1024;
}

// D2VITA_PROGRESS_PATH is defined in platform/vita_present.h (shared by
// multiple translation units).

// ===========================================================================
//  CONSOLE SERVICES — now live in the engine
// ===========================================================================
// The durable progress log, the monotonic clock, and core assignment are
// CONSOLE-specific, not game-specific: the engine targets the Vita, so they
// belong to it and live in winx86 platform/vita_host.{h,cpp}.
//
// The only port-specific data left is the PATH above: each port writes to
// its own folder. It's published to the engine via a CONSTANT-initializer
// definition — not a setter called at startup — because a progress-log line
// can be emitted before any initialization point, and a lazy initializer
// would pull in __cxa_guard_acquire, exactly the trap family tools/build_vpk.sh's
// `nm` guard exists to catch.
extern "C" const char* const wx86_vita_progress_path = D2VITA_PROGRESS_PATH;

// Thin aliases: about fifty call sites in this repo use the d2vita_* names,
// so a mass rename has no place in an ownership move and would only get lost
// in the noise. A port only needs to supply the log PATH; everything else
// forwards straight to the engine's own wx86_vita_* symbols.
void d2vita_progress(const char* msg) { wx86_vita_progress(msg); }

// The monotonic clock lives in runtime/host_clock.h rather than vita_host:
// it's meaningful outside the console context too.
uint64_t d2vita_now_ms() { return wx86_now_ms(); }
uint64_t d2vita_now_us() { return wx86_now_us(); }
void d2vita_sleep_ms(uint32_t ms) { wx86_vita_sleep_ms(ms); }

int  d2vita_core_mask(int who) { return wx86_vita_core_mask(who); }
void d2vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc) {
    wx86_vita_core_register(nom, uid, wanted, pin_rc); }
extern "C" int  d2vita_pin_self(int mask, unsigned* relu) { return wx86_vita_pin_self(mask, relu); }

namespace {
constexpr int SCR_W = 960, SCR_H = 544;
int g_game_w = 800, g_game_h = 600;    // last presented GAME resolution (input layer maps to it)
// Called from the GLIDE path (d2gxm_set_window): without it, input stays
// stuck at 800x600 when the game changes resolution under Glide (touch taps
// land in the wrong place).
extern "C" void d2vita_set_game_size(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == g_game_w && h == g_game_h) return;
    g_game_w = w; g_game_h = h; inj_set_bounds(w, h);
    char m[72]; std::snprintf(m, sizeof m, "entree: resolution du jeu -> %dx%d (Glide)", w, h);
    d2vita_progress(m);
}
// Projection écran <- fenêtre de jeu, publiée par le chemin GXM (proj_update).
// Sans elle, l'entrée supposerait que l'image remplit l'écran : dès qu'il y a
// des bandes (D2_ASPECT=4:3), le curseur tactile atterrirait décalé.
// Tant qu'elle n'a pas été publiée (chemin GDI, GXM non armé), on garde
// EXACTEMENT l'ancienne formule.
bool  g_map_ok = false;
float g_map_ox = 0.f, g_map_oy = 0.f, g_map_ix = 1.f, g_map_iy = 1.f;
// Même knob que le chemin GXM (D2_ASPECT), relu ici : les deux chemins de
// présentation sont indépendants et mutuellement exclusifs.
bool aspect_iso() {
    static int v = -1;
    if (v < 0) { const char* a = getenv("D2_ASPECT");
        v = (a && (!std::strcmp(a, "etire") || !std::strcmp(a, "stretch") ||
                   !std::strcmp(a, "0"))) ? 0 : 1; }
    return v != 0;
}
extern "C" void d2vita_set_present_map(float ox, float oy, float ix, float iy) {
    if (ix <= 0.f || iy <= 0.f) return;
    g_map_ox = ox; g_map_oy = oy; g_map_ix = ix; g_map_iy = iy; g_map_ok = true;
}
// Pavé tactile avant (1920x1088) -> espace fenêtre de jeu.
// Sans carte publiée : l'ancienne formule entière, inchangée. Avec : on passe
// par l'écran pour pouvoir retirer les bandes (et on perd au passage la double
// troncature entière de l'ancienne, ce qui ne peut que rapprocher du bon
// pixel).
inline void pad_to_game(int px, int py, int* gx, int* gy) {
    if (!g_map_ok) { *gx = px * g_game_w / 1920; *gy = py * g_game_h / 1088; return; }
    const float sx = (float)px * (float)SCR_W / 1920.f;
    const float sy = (float)py * (float)SCR_H / 1088.f;
    *gx = (int)((sx - g_map_ox) * g_map_ix);
    *gy = (int)((sy - g_map_oy) * g_map_iy);
}
uint32_t* g_fb[2] = {nullptr, nullptr};
SceUID    g_fb_uid[2] = {0, 0};
int       g_cur = 0;
bool      g_inited = false;

// --- FPS overlay (top-left). Real GAME frames: d2vita_present runs once per
// guest frame, so a rolling average over the present calls IS the game fps.
// Drawn straight into the framebuffer with a tiny 8x8 digit font at 3x scale.
// Off by default; D2VITA_FPS=1 to show.
const uint8_t g_digits[10][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0},{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},{0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},
};
void draw_fps(uint32_t* fb, int fps10) {           // fps*10 (one decimal)
    if (fps10 < 0) fps10 = 0; if (fps10 > 999) fps10 = 999;   // "99.9" max => txt always fits
    char txt[8]; int n = std::snprintf(txt, sizeof txt, "%d.%d", fps10/10, fps10%10);
    const int S = 3, X0 = 8, Y0 = 8;               // 3x scale, top-left corner
    int wpx = n*8*S + 8, hpx = 8*S + 8;            // black backing box
    for (int y = 0; y < hpx; ++y)
        for (int x = 0; x < wpx; ++x)
            fb[(size_t)(Y0-4+y)*SCR_W + (X0-4+x)] = 0xFF000000u;
    for (int i = 0; i < n; ++i) {
        const uint8_t* g;
        if (txt[i] >= '0' && txt[i] <= '9') g = g_digits[txt[i]-'0'];
        else if (txt[i] == '.') { static const uint8_t dot[8]={0,0,0,0,0,0,0x18,0x18}; g = dot; }
        else continue;
        for (int gy = 0; gy < 8; ++gy)
            for (int gx = 0; gx < 8; ++gx)
                if (g[gy] & (0x80 >> gx))
                    for (int sy = 0; sy < S; ++sy)
                        for (int sx = 0; sx < S; ++sx)
                            fb[(size_t)(Y0+gy*S+sy)*SCR_W + (X0+i*8*S+gx*S+sx)] = 0xFFFFFFFFu;
    }
}
// --- Full virtual keyboard (drawn into the frame buffer) --------------------
// Opened/closed by R+Start at any time. The LAYOUT, font, drawing, and state
// machine live in src/platform/vita_kb.h — a self-contained header, no
// VitaSDK, so it's testable on the host (tools/tests/kb_test.cpp covers all
// 95 printable ASCII characters, drawing bounds, touch, and masking).
// Only the wiring lives here: buttons -> state, actions -> d2vita_inject.
// Written by the input tick (game thread), drawn by the presentation thread:
// volatile state, a benign race (display only).
// D2_KBSIMPLE=1 restores the earlier, simpler layout without a rebuild.
d2kb::State g_kb;
int g_kb_simple = -1;                 // read once, on first open
radial_menu::State g_rm{};            // radial menu: state written by the input tick, read by presentation
// Scheme v2 overlay (game coords): hostile target diamond, ground aim dot,
// loot cursor brackets (Phase 2). Published once per tick by the input
// thread; consumed by draw_reticle on the GXM DISPLAY thread (see
// display_cb in vita_gxm.cpp — runs on sceGxm's display thread, not the
// game thread). A seqlock avoids tearing the read across the writer's
// several stores (a fresh `has` paired with a stale x/y from the previous
// target/item showed up on console as an intermittent, sometimes-large
// jump of the loot cursor).
struct OverlayPub { int retHas, retVer, retX, retY, lootHas, lootX, lootY, lootW, lootH; };
static volatile unsigned g_ovSeq = 0;
static OverlayPub g_ovPub = {};
static void overlay_publish(const OverlayPub& p) {
    g_ovSeq = g_ovSeq + 1;                          // odd: write in progress
    __sync_synchronize();
    g_ovPub = p;
    __sync_synchronize();
    g_ovSeq = g_ovSeq + 1;                          // even: stable
}
static OverlayPub overlay_read() {
    OverlayPub p{}; unsigned s0, s1;
    do {
        s0 = g_ovSeq;
        __sync_synchronize();
        p = g_ovPub;
        __sync_synchronize();
        s1 = g_ovSeq;
    } while ((s0 & 1u) || s0 != s1);
    return p;
}
uint32_t g_kb_last_focus = 0;         // ouverture auto du clavier : dernier focus texte vu
// Translucent by default: the player can still see the character/menu behind
// the keys while typing. D2_KBALPHA=0-100 in env.txt overrides (100 = opaque,
// the old look); read once and clamped, like every other knob here.
//
// Below 100, drawing needs to READ whatever is already on screen to blend --
// on this console that means CDRAM, measured elsewhere in this project at
// ~814 ns PER PIXEL read (see d2gxm_kb_active below). So alpha<100 is only
// ever drawn this (CPU) way as a fallback: the sceGxm path composes it on
// the GPU instead (vita_gxm.cpp), which never touches CDRAM from the CPU at
// all. This function stays correct standalone (host tests, the older GDI
// presentation path, and the case where the GPU path isn't armed) but is
// SLOW below 100 -- never call it for that case when d2gxm_kb_active() is
// true, that CPU work would just be thrown away by the GPU draw underneath.
int g_kb_alpha = -1;
inline int kb_alpha(){
    if (g_kb_alpha < 0){
        const char* e = getenv("D2_KBALPHA");
        g_kb_alpha = e ? atoi(e) : 80;
        if (g_kb_alpha < 0) g_kb_alpha = 0; if (g_kb_alpha > 100) g_kb_alpha = 100;
    }
    return g_kb_alpha;
}
inline void draw_keyboard(uint32_t* fb){ d2kb::draw(g_kb, fb, SCR_W, SCR_H, kb_alpha()); }
// --- async present: the scale+flip runs on its OWN Vita core -----------------
// The guest emulation is single-core; the 960x544 palette scale (~2-5 ms of
// A9 time per frame) moves to a second CPU via a dedicated thread (the
// "CSMT" idea transposed). The producer memcpys the DIB (~0.2 ms) into a
// staging slot and signals; frames arriving while the thread is busy are
// dropped last-wins (display only — the game state is unaffected).
// D2VITA_ASYNC=0 falls back to the synchronous path.
//
// ---- D2_PRESENTZC: who does the 480 KB copy? ----------------------------
// Copying the DIB (up to 800x600 = 480 KB) is currently paid on USER_0, the
// core all guest threads are pinned to — i.e. the critical path. This knob
// moves that work to USER_2, the presentation thread.
//   0 (DEFAULT, historical behavior): the producer copies.
//   2: the producer PUBLISHES (pointer, w, h, bpp, palette) and WAITS for the
//      consumer to finish its own copy — the thread's first action. The work
//      moves cores; the producer only waits for the copy, not the scaling or
//      the flip.
//   1: same as 2 but WITHOUT waiting. The game overwrites the DIB as soon as
//      StretchBlt returns, so there's a real race between publish and copy,
//      and its symptom is torn frames. Mode 1 is for measurement only, never
//      a default.
// The palette (1 KB) is always copied on the producer side in every mode: it
// costs nothing, and pulling it out of the race removes a risk for no gain.
// Dimensionne sur l'ECRAN (960x544 = 522 240 o), pas sur 800x600 : avec
// D2_RES le jeu dessine desormais jusqu'a la taille de la dalle, et l'ancien
// tampon de 480 000 o faisait tomber l'image dans la garde SILENCIEUSE plus
// bas. +42 Ko par emplacement, deux emplacements.
constexpr size_t PRESENT_PX_MAX = 960u * 544u;
struct PresentSlot { uint8_t px[PRESENT_PX_MAX]; uint8_t pal[1024]; int w,h,bpp,fps10;
                     const uint8_t* src; };   // src==px => already copied by the producer
PresentSlot g_slot[2];
volatile int g_pub = 0;        // slot index published for the consumer
volatile int g_pending = 0;    // a frame is queued/being drawn
volatile int g_thread_run = 0;
SceUID g_sema = -1, g_pth = -1;
SceUID g_sema_copied = -1;     // D2_PRESENTZC=2: "copy done" handoff, producer -> consumer
int    g_zc = 0;               // 0 = producer copies (default)
void do_scale_and_flip(const PresentSlot* s);

int present_thread(SceSize, void*) {
    { unsigned relu = 0; const int m = d2vita_core_mask(0); const int rc = d2vita_pin_self(m, &relu);
      char s[112]; std::snprintf(s, sizeof s, "present: auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", (unsigned)m, (unsigned)rc, relu);
      d2vita_progress(s); }
    while (g_thread_run) {
        sceKernelWaitSema(g_sema, 1, nullptr);
        if (!g_thread_run) break;
        PresentSlot* s = &g_slot[g_pub];
        // FIRST ACTION: close the race. Until this is done, the game can
        // rewrite the DIB out from under us.
        if (s->src && s->src != s->px)
            std::memcpy(s->px, s->src, (size_t)s->w * s->h * (s->bpp / 8));
        if (g_sema_copied >= 0) sceKernelSignalSema(g_sema_copied, 1);
        do_scale_and_flip(s);
        g_pending = 0;
    }
    return 0;
}

// ===========================================================================
// D2_PRESENTTHREAD=<core> — pipelined presentation, on another core
// ===========================================================================
// Absent: nothing changes (the historical async path above, with its
// last-wins drops, is the only one active). Present (0, 1, or 2): a
// dedicated thread is created and pinned to the requested core, and the
// render thread (USER_0) only, on each StretchBlt:
//   (a) WAITS, and only if the presenter is two frames behind (two slots: one
//       frame being displayed, one queued). In steady state it never waits:
//       conversion takes 2-5 ms against a 40 ms frame;
//   (b) copies the 8-bit DIB into the free slot plus THIS frame's palette
//       (the conversion depends on it, and it changes mid-game — fades,
//       flashes);
//   (c) posts a memory barrier, then publishes the sequence number.
// The presentation thread does the palette->32-bit conversion, the 960x544
// scaling, and sceDisplaySetFrameBuf, on its own core.
//
// Deliberate difference from the historical path: here NO frame is ever
// dropped (the producer waits), so latency is bounded to one frame, and the
// counters report the cost ("retard=" frames where the previous one wasn't
// displayed yet, "attente-rendu=" microseconds actually paid by the render
// thread). Every presented frame is produced by the SAME function
// (do_scale_and_flip) from the SAME inputs (DIB bytes + that frame's
// palette), so the framebuffer is identical by construction.
//
// D2_PRESENTZC=1 on top: the producer publishes a pointer instead of copying
// (the copy becomes the consumer's first action). This is for measurement,
// not a default: between publish and the end of the copy, the game can
// rewrite the DIB. The "course=" counter counts frames where that happened
// (a 1 KB sample re-read after the copy).
PresentSlot* const g_pt_slot = g_slot;      // same slots: the two paths are mutually exclusive
int      g_pt_core   = -1;                  // -1 = knob absent => historical path
volatile int g_pt_run = 0;
SceUID   g_pt_th     = -1;
volatile uint32_t g_pt_pub  = 0;            // frames published by the producer
volatile uint32_t g_pt_done = 0;            // frames presented by the consumer
// Proof counters (10 s window).
volatile unsigned  g_pt_images = 0;         // frames presented
volatile unsigned  g_pt_copies = 0;         // 8-bit copies done by the PRODUCER
volatile unsigned  g_pt_late   = 0;         // frames where the producer had to wait
volatile unsigned  g_pt_waitn  = 0;         // cumulative polling rounds
volatile unsigned  g_pt_waitus = 0;         // cumulative µs the render thread waited
volatile unsigned  g_pt_expire = 0;         // bounded waits that expired (thread stuck)
volatile unsigned  g_pt_race   = 0;         // D2_PRESENTZC: frames where the DIB moved
volatile int       g_pt_cpu    = -1;        // core READ BACK by the thread itself
int      g_pt_aff_rc = 0;                   // affinity pin return code
// 1 KB sample (32 cache lines of 32 B) spread across the whole DIB: race
// detector for zero-copy mode. Covers only 0.2% of the DIB — it can prove a
// race, never its absence.
uint8_t  g_pt_sample[2][1024];   // one per slot: two frames can be in flight
inline void pt_sample(const uint8_t* p, uint32_t sz, uint8_t* out) {
    const uint32_t step = sz / 32 ? sz / 32 : 1;
    for (int i = 0; i < 32; ++i) {
        uint32_t off = (uint32_t)i * step;
        if (off + 32 > sz) off = sz > 32 ? sz - 32 : 0;
        std::memcpy(out + i * 32, p + off, 32);
    }
}

int present_pipe_thread(SceSize, void*) {
    { const int core = g_pt_core;
      const int m = core == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : core == 1 ? SCE_KERNEL_CPU_MASK_USER_1
                  : core == 3 ? (int)WX86_CPU_MASK_USER_3 : SCE_KERNEL_CPU_MASK_USER_2;
      unsigned relu = 0; const int rc = d2vita_pin_self(m, &relu);
      char s[112]; std::snprintf(s, sizeof s, "present-pipe: auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", (unsigned)m, (unsigned)rc, relu);
      d2vita_progress(s); }
    {   // self-reported liveness: the thread itself states which core it's running on
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        int rt = sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
        g_pt_cpu = (rt < 0) ? -1 : (int)ti.currentCpuId;
        char m[128];
        std::snprintf(m, sizeof m, "present: fil arme (demande=coeur %d, epinglage rc=0x%08x, coeur relu=%d, prio=%d)",
                      g_pt_core, (unsigned)g_pt_aff_rc, g_pt_cpu,
                      sceKernelGetThreadCurrentPriority());
        d2vita_progress(m); }
    unsigned spin = 0;
    while (g_pt_run) {
        if (g_pt_done == g_pt_pub) {         // nothing to do
            // Wake pattern: short poll (the producer publishes roughly every
            // ~40 ms, but we don't want to pay for a kernel object every
            // frame), then nap. No condition variable.
            if (++spin < 2000) continue;   // ~20-50 us of polling
            sceKernelDelayThread(500);      // then nap: ~2000 wakes/s, no kernel object
            continue;
        }
        spin = 0;
        __sync_synchronize();                // pair with the producer's barrier
        PresentSlot* s = &g_pt_slot[g_pt_done & 1u];
        if (s->src && s->src != s->px) {     // D2_PRESENTZC: the copy happens HERE
            const uint32_t sz = (uint32_t)s->w * s->h * (s->bpp / 8);
            std::memcpy(s->px, s->src, sz);
            uint8_t now[1024]; pt_sample(s->src, sz, now);
            if (std::memcmp(now, g_pt_sample[g_pt_done & 1u], sizeof now)) ++g_pt_race;
        }
        do_scale_and_flip(s);
        // The core is READ BACK periodically, not just once at startup: an
        // accepted pin (rc=0) doesn't prevent migration, which is exactly
        // what a boot-time-only reading could never reveal.
        if ((++g_pt_images & 255u) == 0u) {
            SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
            if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0)
                g_pt_cpu = (int)ti.currentCpuId;
        }
        __sync_synchronize();
        ++g_pt_done;                         // frees the slot for the producer
    }
    d2vita_progress("present: arret demande, fil termine");
    return 0;
}

// Producer. Returns true once the pipelined path has taken ownership of the frame.
bool pt_publish(const uint8_t* pixels, int w, int h, int bpp,
                const uint8_t* palette, int fps10) {
    // (a) LATENCY BOUNDED TO ONE FRAME. Two slots: one frame can be presenting
    //     while the next is staged. The render thread only waits if the
    //     presenter is two frames behind — never in steady state (2-5 ms
    //     conversion against a 40 ms frame). "retard=" counts frames where
    //     the previous one wasn't displayed yet; "attente-rendu=" is what the
    //     render thread actually paid.
    // In zero-copy mode the published pointer points at the LIVE DIB, so only
    // ONE frame is allowed in flight, otherwise the previous slot would
    // describe a frame the game has already fully repainted over.
    const uint32_t vol = g_zc ? 1u : 2u;
    if (g_pt_pub != g_pt_done) ++g_pt_late;
    if ((uint32_t)(g_pt_pub - g_pt_done) >= vol) {
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        unsigned tours = 0;
        while ((uint32_t)(g_pt_pub - g_pt_done) >= vol) {
            if (++tours < 400) continue;                  // short poll
            // BOUNDED wait: a stuck presenter must never freeze the game
            // (same doctrine as mode 2's handshake).
            if (sceKernelGetProcessTimeWide() - t0 > 100000ull) {  // 100 ms
                ++g_pt_expire;
                return false;   // fall back: caller presents synchronously
            }
            sceKernelDelayThread(100);
        }
        g_pt_waitn  += tours;
        g_pt_waitus += (unsigned)(sceKernelGetProcessTimeWide() - t0);
    }
    // (b) the free slot is the one the consumer isn't reading.
    PresentSlot* sl = &g_pt_slot[g_pt_pub & 1u];
    const uint32_t sz = (uint32_t)w * h * (bpp / 8);
    // The palette is ALWAYS captured together with the frame: the conversion
    // depends on it, and it changes between frames (fades, flashes).
    if (bpp == 8 && palette) std::memcpy(sl->pal, palette, 1024);
    if (g_zc) { sl->src = pixels; pt_sample(pixels, sz, g_pt_sample[g_pt_pub & 1u]); }
    else      { std::memcpy(sl->px, pixels, sz); sl->src = sl->px; ++g_pt_copies; }
    sl->w = w; sl->h = h; sl->bpp = bpp; sl->fps10 = fps10;
    // (c) barrier THEN publish: the consumer must never see the sequence
    //     number before the bytes it describes.
    __sync_synchronize();
    ++g_pt_pub;
    return true;
}

void* alloc_fb(SceUID* uid) {
    // Framebuffers prefer CDRAM (256 KiB-aligned). On real hardware the
    // extended-memory app mode can make CDRAM unavailable — fall back to main
    // RAM (1 MiB-aligned) and LOG the outcome: a silent null here means the
    // game runs blind behind an eternal black screen.
    uint32_t sz = (SCR_W * SCR_H * 4 + 0x3FFFF) & ~0x3FFFFu;
    *uid = sceKernelAllocMemBlock("d2_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, nullptr);
    if (*uid < 0) {
        char m[64]; std::snprintf(m, sizeof m, "fb: CDRAM alloc failed 0x%08x, trying main RAM", (unsigned)*uid);
        d2vita_progress(m);
        uint32_t sz2 = (SCR_W * SCR_H * 4 + 0xFFFFF) & ~0xFFFFFu;
        *uid = sceKernelAllocMemBlock("d2_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz2, nullptr);
    }
    if (*uid < 0) { d2vita_progress("fb: ALL allocs failed — display disabled"); return nullptr; }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0) { d2vita_progress("fb: GetMemBlockBase failed"); return nullptr; }
    return base;
}
} // namespace

// blend_px used to live in radial_menu::draw_detail, shared with the radial
// menu's own CPU blitter -- removed there by b34f0bc ("le compositer sur le
// GPU (81 ms par image supprimes)"), which measured ~814 ns per CDRAM pixel
// READ back to blend, ~57 ms/frame of the menu's 81 ms regression, and says
// explicitly not to reintroduce CPU compositing for THAT rendering path.
// draw_reticle below is a different scale, not the same call: a target
// diamond (~40 px) plus at most one loot bracket (~50 px), a couple hundred
// blended pixels at alpha=220 versus the menu's full wedge+icon atlas -- at
// ~814 ns/px that is worst-case low hundreds of microseconds, not tens of
// milliseconds, so it is kept local (not shared back into radial_menu.h,
// which stays GPU-only on principle) rather than ported to the GPU path.
// Not console-measured; if this overlay is ever seen costing real frame
// time, follow the menu/keyboard's own precedent and move it there.
static void blend_px(uint32_t* fb, int W, int H, int x, int y,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= W || y < 0 || y >= H || a == 0) return;
    const size_t o = (size_t)y * W + x;
    if (a == 255) { fb[o] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r; return; }
    const uint32_t dst = fb[o];
    const uint8_t dr = (uint8_t)(dst & 0xFF), dg = (uint8_t)((dst >> 8) & 0xFF), db = (uint8_t)((dst >> 16) & 0xFF);
    const uint8_t nr = (uint8_t)(((uint32_t)r * a + (uint32_t)dr * (255 - a)) / 255);
    const uint8_t ng = (uint8_t)(((uint32_t)g * a + (uint32_t)dg * (255 - a)) / 255);
    const uint8_t nb = (uint8_t)(((uint32_t)b * a + (uint32_t)db * (255 - a)) / 255);
    fb[o] = 0xFF000000u | ((uint32_t)nb << 16) | ((uint32_t)ng << 8) | nr;
}

// Scheme v2 overlay: a diamond on the current hostile target (gold once the
// game is verified to hover it, white before) and cyan corner brackets on the
// ground-item browse cursor (Phase 2). No ground-aim dot: the cast lands where
// the cursor already is, so the cursor IS that marker.
// Game -> screen uses the same stretch as the presentation (vita_gxm.cpp).
static void draw_reticle(uint32_t* fb) {
    const OverlayPub ov = overlay_read();
    if (!ov.retHas && !ov.lootHas) return;
    const int gw = g_game_w > 0 ? g_game_w : 800, gh = g_game_h > 0 ? g_game_h : 600;
    if (ov.retHas) {
        const int cx = ov.retX * SCR_W / gw, cy = ov.retY * SCR_H / gh - 8;
        const uint8_t r = ov.retVer ? 255 : 240, g = ov.retVer ? 200 : 240, b = ov.retVer ? 60 : 240;
        for (int d = 0; d <= 10; ++d) {
            blend_px(fb, SCR_W, SCR_H, cx - 10 + d, cy - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 10 - d, cy - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - 10 + d, cy + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + 10 - d, cy + d, r, g, b, 220);
        }
    }
    // Corner brackets around the selected item's label: centered on the
    // rect the game itself laid out, sized to it (0 = no label this frame,
    // fall back to a fixed marker on the item's own tile).
    auto bracket = [&](int lootX, int lootY, int lootW, int lootH, uint8_t r, uint8_t g, uint8_t b) {
        const int cx = lootX * SCR_W / gw, cy = lootY * SCR_H / gh;
        const int hw = lootW > 0 ? (lootW * SCR_W / gw) / 2 + 3 : 12;
        const int hh = lootH > 0 ? (lootH * SCR_H / gh) / 2 + 3 : 12;
        for (int d = 0; d <= 6; ++d) {
            blend_px(fb, SCR_W, SCR_H, cx - hw, cy - hh + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - hw + d, cy - hh, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + hw, cy - hh + d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + hw - d, cy - hh, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - hw, cy + hh - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx - hw + d, cy + hh, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + hw, cy + hh - d, r, g, b, 220);
            blend_px(fb, SCR_W, SCR_H, cx + hw - d, cy + hh, r, g, b, 220);
        }
    };
    if (ov.lootHas) bracket(ov.lootX, ov.lootY, ov.lootW, ov.lootH, 80, 220, 255);   // cyan
}

// Overlays for the sceGxm path: the GPU has already written the frame, so
// only the counter and keyboard remain to draw. Same code as the GDI path —
// two different drawings of the same digits would eventually drift apart.
void d2vita_overlay(uint32_t* fb) {
    if (!fb) return;
    static int fps_en = -1;
    if (fps_en < 0) { const char* e = getenv("D2VITA_FPS"); fps_en = (e && *e && std::strcmp(e, "0") != 0); }
    // Frame rate measured HERE: the Glide path doesn't go through the GDI
    // path's counter, and reusing that one would show a frozen number.
    static uint64_t t0 = 0; static unsigned n = 0; static int fps10 = 0;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (!t0) t0 = now;
    if (++n >= 30) { const uint64_t dt = now - t0; if (dt) fps10 = (int)((uint64_t)n * 10000000ull / dt);
                     n = 0; t0 = now; }
    if (fps_en) draw_fps(fb, fps10);
    draw_reticle(fb);
    // Etiquette de build (haut droite) : "<VERSION>+<12 hex>[-dirty]", la
    // meme chaine que le rapport de crash (crashreport/build_id.h). Sert a
    // lever l'ambiguite sur une capture d'ecran de rapport de bug -- le
    // 23/09/2026, un joueur a signale un defaut deja corrige la veille sans
    // qu'on puisse savoir s'il avait la correction ou une build plus vieille.
    // Actif par defaut ; D2VITA_BUILDTAG=0 la masque. Reutilise la police du
    // clavier (d2kb::draw_detail::text) plutot que d'en dupliquer une.
    static int buildtag_en = -1;
    if (buildtag_en < 0) { const char* e = getenv("D2VITA_BUILDTAG"); buildtag_en = !(e && !std::strcmp(e, "0")); }
    if (buildtag_en) {
        const char* id = d2cr::build_id();
        const int n = (int)std::strlen(id);
        const int S = 1, GW = 8, GH = 16;
        const int wpx = n * GW * S + 8, hpx = GH * S + 8;
        const int X0 = SCR_W - wpx - 4, Y0 = 4;
        for (int y = 0; y < hpx; ++y)
            for (int x = 0; x < wpx; ++x)
                fb[(size_t)(Y0 + y) * SCR_W + (X0 + x)] = 0xFF000000u;
        d2kb::draw_detail::text(fb, SCR_W, SCR_H, id, X0 + 4, Y0 + 4, S, 0xFFFFFFFFu);
    }
    // Opaque, or the GPU path isn't armed: draw here, same as always (write
    // only, never reads CDRAM back -- always fast). Translucent AND armed:
    // skip it, d2gxm_submit's own scene-injected quad composes it on the GPU
    // instead (vita_gxm.cpp) -- drawing it here too would blend it TWICE.
    if (g_kb.open && !(kb_alpha() < 100 && d2gxm_kb_active())) draw_keyboard(fb);
    // The radial menu now composes entirely on the GPU (vita_gxm.cpp,
    // d2gxm_ui_active()) -- opening it is already gated on that path being
    // armed (see the Select handler below), so there is no CPU fallback to
    // draw here; doing so would blend it a second time.
}

const d2kb::State* d2vita_kb_state() { return g_kb.open ? &g_kb : nullptr; }
int d2vita_kb_alpha() { return kb_alpha(); }

const radial_menu::State* d2vita_radial_state() { return g_rm.open ? &g_rm : nullptr; }

void d2vita_present_init() {
    if (g_inited) return;
    for (int i = 0; i < 2; ++i) {
        g_fb[i] = (uint32_t*)alloc_fb(&g_fb_uid[i]);
        if (g_fb[i]) std::memset(g_fb[i], 0, SCR_W * SCR_H * 4);
    }
    { const char* z = getenv("D2_PRESENTZC"); g_zc = z ? atoi(z) : 0;
      if (g_zc < 0 || g_zc > 2) g_zc = 0; }
    // D2_PRESENTTHREAD=<core>: pipelined path (see the block above). Absent
    // => nothing new is created and the historical path runs unchanged.
    { const char* pt = getenv("D2_PRESENTTHREAD");
      const char* a0 = getenv("D2VITA_ASYNC");
      // D2VITA_ASYNC=0 is still the MASTER switch for the synchronous
      // fallback (used to put all presentation back on USER_0 for an A/B
      // subtraction): it therefore also disables the pipeline, otherwise the
      // same env.txt would say two contradictory things and the subtraction
      // measurement would be wrong.
      if (pt && *pt && !(a0 && !std::strcmp(a0, "0"))) {
        int core = atoi(pt);
        // `3` = the FOURTH core (0x00080000), never proven live: the 10s
        // window's "4th core probe" must report "0x80000 ACCEPTED" before
        // using it. If refused, the pin's rc is logged and the thread stays
        // wherever the kernel put it.
        if (core < 0 || core > 3) core = 2;
        g_pt_core = core;
        const unsigned mask = core == 0 ? SCE_KERNEL_CPU_MASK_USER_0
                            : core == 1 ? SCE_KERNEL_CPU_MASK_USER_1
                            : core == 3 ? (unsigned)WX86_CPU_MASK_USER_3
                                        : SCE_KERNEL_CPU_MASK_USER_2;
        g_pt_th = sceKernelCreateThread("d2_present_pipe", present_pipe_thread,
                                        0x10000100, 64 * 1024, 0, 0, nullptr);
        if (g_pt_th >= 0) {
            g_pt_run = 1;
            // The pin's rc is READ BACK and logged: discarding the rc of a
            // ChangeThreadCpuAffinityMask call has already let an unverified
            // assumption pass for a measured fact once before. The thread
            // then republishes the core it's ACTUALLY running on.
            g_pt_aff_rc = wx86_vita_pin_thread(g_pt_th, (int)mask, nullptr);
            // Same registry as the other host threads: the "coeurs:" line
            // must cover this thread too, otherwise the read-back inventory
            // has a gap exactly on its heaviest tenant.
            d2vita_core_register("present-pipe", g_pt_th, mask, g_pt_aff_rc);
            int rc = sceKernelStartThread(g_pt_th, 0, nullptr);
            if (rc < 0) { g_pt_run = 0; g_pt_core = -1;
                char m[112]; std::snprintf(m, sizeof m,
                    "present: StartThread KO (rc=0x%08x) — repli chemin historique", (unsigned)rc);
                d2vita_progress(m); }
        } else { g_pt_core = -1;
            d2vita_progress("present: CreateThread KO — repli chemin historique"); }
      } }
    const char* as = getenv("D2VITA_ASYNC");
    if (!g_pt_run && (!as || std::strcmp(as, "0"))) {
        g_sema = sceKernelCreateSema("d2_present", 0, 0, 1, nullptr);
        if (g_zc == 2) g_sema_copied = sceKernelCreateSema("d2_copied", 0, 0, 1, nullptr);
        g_pth  = sceKernelCreateThread("d2_present", present_thread, 0x10000100,
                                       64 * 1024, 0, 0, nullptr);
        if (g_sema >= 0 && g_pth >= 0) {
            g_thread_run = 1;
            // D2_COEURS position 0. Default "2" = USER_2, matching prior
            // behavior exactly. rc CHECKED and LOGGED (the "coeurs:" line):
            // on hardware, a requested pin is not always an obtained one.
            const unsigned pmask = (unsigned)d2vita_core_mask(0);
            const int prc = wx86_vita_pin_thread(g_pth, (int)pmask, nullptr);
            d2vita_core_register("present", g_pth, pmask, prc);
            sceKernelStartThread(g_pth, 0, nullptr);
            char m[128];
            std::snprintf(m, sizeof m, "present: async thread on core %d (rc=0x%08x) (D2_PRESENTZC=%d%s)",
                          pmask == SCE_KERNEL_CPU_MASK_USER_0 ? 0 : pmask == SCE_KERNEL_CPU_MASK_USER_1 ? 1 : 2,
                          (unsigned)prc,
                          g_zc, (g_zc == 2 && g_sema_copied < 0) ? " — sema KO, repli mode 0" : "");
            if (g_zc == 2 && g_sema_copied < 0) g_zc = 0;   // no handshake => no race
            d2vita_progress(m);
        } else d2vita_progress("present: async unavailable, synchronous");
    }
    g_inited = true;
}

namespace {
// Consumer: scale one staged frame to the screen and flip. Runs on the
// present thread (async) or inline (fallback). Same pixel path as before:
// per-frame ABGR32 palette expansion, 4:1 unrolled LUT loop, duplicate
// output rows memcpy'd.
void do_scale_and_flip(const PresentSlot* sfr) {
    const int w = sfr->w, h = sfr->h, bpp = sfr->bpp;
    const uint8_t* pixels = sfr->px;
    uint32_t* dst = g_fb[g_cur];
    // D2_PRESENT_WX86=1: use the engine's shared scaling code
    // (third_party/winx86, src/platform/present_scale.cpp) instead of the
    // local loop below. Both are proven byte-for-byte identical
    // (winx86/tools/present_scale_selftest.cpp, self-verified via bit-flip
    // fault injection).
    //
    // Default is 1. D2_PRESENT_WX86=0 restores the old local path, kept as a
    // fallback until further confirmed in real play; it will be removed once
    // that happens.
    static const int use_wx86 = []{ const char* e = getenv("D2_PRESENT_WX86");
                                    return (e && *e == '0') ? 0 : 1; }();
    // D2_PRESENT_CMP=1: output-identity oracle, on real game frames. Both
    // paths run on the SAME input in the SAME call, and their output bytes
    // are compared.
    //
    // Not FBHASH: FBHASH hashes the DIB, i.e. this function's INPUT, before
    // any scaling — it would report "identical" no matter what the scaler
    // does, making it a comparator that can't actually fail for this check.
    //
    // Not two separate runs compared: the game runs in real time, so two
    // passes are never at the same frame. Comparing within the same call
    // removes that problem entirely.
    //
    // D2_PRESENT_CMPTEST=1 corrupts one pixel of the engine path's output
    // before comparing, to prove the comparator can actually detect a
    // difference — an oracle that has never failed proves nothing.
    static const int cmp_on = []{ const char* e = getenv("D2_PRESENT_CMP");
                                  return e && *e == '1' ? 1 : 0; }();
    static const int cmp_test = []{ const char* e = getenv("D2_PRESENT_CMPTEST");
                                    return e && *e == '1' ? 1 : 0; }();
    // ENGINE leg computed SEPARATELY when the oracle is armed. The historical
    // leg fills `dst` right after, via the normal path; the comparison
    // happens at the end of the function, before overlays.
    // In oracle mode, `dst` ALWAYS gets the historical path and `alt` gets
    // the engine's: the comparison controls this, not the current default.
    const int path_wx86 = cmp_on ? 0 : use_wx86;
    uint32_t* alt = nullptr;
    if (cmp_on) {
        static uint32_t* altBuf = (uint32_t*)std::malloc((size_t)SCR_W * SCR_H * 4);
        if (altBuf) {
            alt = altBuf;
            const wx86::DstRect full{0, 0, SCR_W, SCR_H};
            wx86::scale_blit(alt, SCR_W, full, pixels, w, h, w * (bpp / 8),
                             bpp == 8 ? wx86::SrcFormat::Pal8 : wx86::SrcFormat::Bgra32,
                             sfr->pal);
            if (cmp_test) alt[(size_t)SCR_W * (SCR_H / 2) + SCR_W / 2] ^= 1u;
        }
    }
    if (path_wx86) {
        // Même choix d'aspect que le chemin GXM. fit_rect() existait déjà dans
        // le moteur (et est couvert par present_scale_selftest) mais n'était
        // appelé nulle part : le port étirait toujours en plein écran.
        const wx86::DstRect r = wx86::fit_rect(w, h, SCR_W, SCR_H, !aspect_iso());
        if (r.x > 0 || r.y > 0) {
            // Les bandes ne sont écrites par personne : sans ça elles gardent
            // l'image de la frame précédente.
            std::memset(dst, 0, (size_t)SCR_W * SCR_H * 4);
            d2vita_set_present_map((float)r.x, (float)r.y,
                                   (float)w / (float)r.w, (float)h / (float)r.h);
        }
        wx86::scale_blit(dst, SCR_W, r, pixels, w, h, w * (bpp / 8),
                         bpp == 8 ? wx86::SrcFormat::Pal8 : wx86::SrcFormat::Bgra32,
                         sfr->pal);
    } else {
    static uint32_t pal32[256];
    if (bpp == 8)
        for (int i = 0; i < 256; ++i) {
            const uint8_t* p = sfr->pal + i * 4;        // B,G,R,0
            pal32[i] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
    const uint32_t sx = (uint32_t)((w << 16) / SCR_W);
    const uint32_t sy = (uint32_t)((h << 16) / SCR_H);
    int prev_srcy = -1;
    for (int y = 0; y < SCR_H; ++y) {
        int srcy = (int)((y * sy) >> 16);
        uint32_t* out = dst + (size_t)y * SCR_W;
        if (srcy == prev_srcy) { std::memcpy(out, out - SCR_W, SCR_W * 4); continue; }
        prev_srcy = srcy;
        const uint8_t* row = pixels + (size_t)srcy * w * (bpp / 8);
        uint32_t xacc = 0;
        if (bpp == 8) {
            int x = 0;
            for (; x + 4 <= SCR_W; x += 4) {            // unrolled 4:1
                out[x+0] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+1] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+2] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+3] = pal32[row[xacc >> 16]]; xacc += sx;
            }
            for (; x < SCR_W; ++x, xacc += sx) out[x] = pal32[row[xacc >> 16]];
        } else if (bpp == 32) {
            const uint32_t* r32 = (const uint32_t*)row;
            for (int x = 0; x < SCR_W; ++x, xacc += sx) {
                uint32_t bgr = r32[xacc >> 16];         // DIB is 0x00RRGGBB (BGRA in mem)
                out[x] = 0xFF000000u | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
            }
        }
    }
    }   // end of the historical path (default)
    // ---- Oracle: both legs saw the SAME input, compare their output
    if (alt) {
        static unsigned long long nCmp = 0, nDiffF = 0, nDiffPx = 0;
        unsigned long long d = 0;
        for (size_t i = 0, n = (size_t)SCR_W * SCR_H; i < n; ++i)
            if (dst[i] != alt[i]) ++d;
        ++nCmp; if (d) { ++nDiffF; nDiffPx += d; }
        // Logged to the progress file: a normal on-screen print is invisible
        // on console. Every 200 frames, plus a verdict on the first divergence.
        if (nCmp <= 3 || (nCmp % 50) == 0 || (d && nDiffF == 1)) {
            char m[160];
            std::snprintf(m, sizeof m,
                "presentcmp: images=%llu divergentes=%llu pixels=%llu (%s)",
                nCmp, nDiffF, nDiffPx, nDiffF ? "DIVERGENCE" : "identique");
            d2vita_progress(m);
        }
    }
    static int fps_en = -1;
    if (fps_en < 0) { const char* e = getenv("D2VITA_FPS"); fps_en = (e && *e && std::strcmp(e, "0") != 0); }   // off by default; D2VITA_FPS=1 to show
    if (fps_en) draw_fps(dst, sfr->fps10);
    if (g_kb.open) draw_keyboard(dst);
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size        = sizeof fb;
    fb.base        = dst;
    fb.pitch       = SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width       = SCR_W;
    fb.height      = SCR_H;
    int rc = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    static bool logged = false;
    if (!logged) { logged = true;
        char m[64]; std::snprintf(m, sizeof m, "fb: first SetFrameBuf rc=0x%08x", (unsigned)rc);
        d2vita_progress(m); }
    g_cur ^= 1;
}
} // namespace

void d2vita_present(const uint8_t* pixels, int w, int h, int bpp,
                    const uint8_t* palette) {
    if (!g_inited) d2vita_present_init();
    if (!pixels || w <= 0 || h <= 0 || !g_fb[0]) return;
    // Borne du tampon de transit. Elle ETAIT SILENCIEUSE : une image plus
    // grande disparaissait sans une ligne de journal, ce qui se presente comme
    // un ecran noir sans cause. On le dit maintenant, une fois.
    if ((size_t)w * h * (bpp / 8) > sizeof g_slot[0].px) {
        static bool dit = false;
        if (!dit) { dit = true;
            char m[128]; std::snprintf(m, sizeof m,
                "present: IMAGE JETEE %dx%dx%d (%zu o) > tampon %zu o",
                w, h, bpp, (size_t)w * h * (bpp / 8), sizeof g_slot[0].px);
            d2vita_progress(m); }
        return;
    }
    static int g_frames = 0;
    static int g_lastw = 0, g_lasth = 0;
    if (g_frames == 0) { char m[64]; std::snprintf(m, sizeof m, "first frame presented: %dx%d %dbpp", w, h, bpp); d2vita_progress(m);
        // Post-arena budget sample: what the device still has once the arena,
        // the JIT blocks, the newlib heap and the framebuffers are all up =
        // exactly the headroom the arena could still claim (see mem-budget(pre)).
        SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
        int r = sceKernelGetFreeMemorySize(&fi);
        char b[176]; std::snprintf(b, sizeof b, "mem-budget(post-boot) rc=%d user=%d KB cdram=%d KB phycont=%d KB",
                                   r, fi.size_user >> 10, fi.size_cdram >> 10, fi.size_phycont >> 10);
        d2vita_progress(b); }
    if (w != g_lastw || h != g_lasth) {   // resolution change: 800x600 menu -> 640x480 game world
        char m[80]; std::snprintf(m, sizeof m, "resolution -> %dx%d at frame %d", w, h, g_frames); d2vita_progress(m);
        g_lastw = w; g_lasth = h; g_game_w = w; g_game_h = h; inj_set_bounds(w, h);
    }
    ++g_frames;
    if ((g_frames % 500) == 0) { char m[48]; std::snprintf(m, sizeof m, "frames presented: %d", g_frames); d2vita_progress(m); }

    // Rolling GAME fps (producer side: one call per guest frame).
    static uint64_t t_last = 0; static int n = 0; static int fps10 = 0;
    if (++n >= 32) {
        uint64_t t = sceKernelGetProcessTimeWide();       // µs
        if (t_last) fps10 = (int)((uint64_t)n * 10000000ull / (t - t_last));
        t_last = t; n = 0;
    }

    // Pipelined path (D2_PRESENTTHREAD=<core>): takes priority over the
    // historical async path, which isn't even armed when this one is. Falls
    // back to SYNCHRONOUS if the bounded wait expires ("expire=" counter) —
    // the game must never freeze behind the presenter; this is an emergency
    // path, and slot 0 gets reused under the assumed-stuck consumer.
    if (g_pt_run && pt_publish(pixels, w, h, bpp, palette, fps10)) return;
    if (g_thread_run) {
        // Async: stage into the slot the consumer is NOT reading and publish.
        // If the thread is still busy (g_pending), drop this frame (last-wins
        // display; game state unaffected).
        if (g_pending) return;
        int wslot = 1 - g_pub;
        PresentSlot* sl = &g_slot[wslot];
        // The palette is ALWAYS copied here (1 KB): out of the race, for free.
        if (bpp == 8 && palette) std::memcpy(sl->pal, palette, 1024);
        if (g_zc) sl->src = pixels;                                    // publish: (ptr,w,h,bpp,pal)
        else { std::memcpy(sl->px, pixels, (size_t)w * h * (bpp / 8)); sl->src = sl->px; }
        sl->w = w; sl->h = h; sl->bpp = bpp; sl->fps10 = fps10;
        g_pub = wslot;
        g_pending = 1;
        // Clear a LEFTOVER copy token: if a previous frame's wait expired,
        // the consumer's signal arrived afterward, and the semaphore's count
        // would make the next wait return instantly — a handshake that no
        // longer means anything.
        if (g_zc == 2 && g_sema_copied >= 0)
            while (sceKernelPollSema(g_sema_copied, 1) >= 0) {}
        sceKernelSignalSema(g_sema, 1);
        // Mode 2: wait for the copy (done on USER_2) before returning control
        // to the game, which overwrites the DIB as soon as StretchBlt
        // returns. BOUNDED wait: a stuck presentation thread must never
        // freeze the game.
        if (g_zc == 2 && g_sema_copied >= 0) {
            SceUInt to = 50000;   // 50 ms
            if (sceKernelWaitSema(g_sema_copied, 1, &to) < 0) {
                static bool warned = false;
                if (!warned) { warned = true;
                    d2vita_progress("present: attente de recopie EXPIREE (image possiblement dechiree)"); }
            }
        }
        return;
    }
    // Synchronous fallback.
    PresentSlot* sl = &g_slot[0];
    std::memcpy(sl->px, pixels, (size_t)w * h * (bpp / 8));
    if (bpp == 8 && palette) std::memcpy(sl->pal, palette, 1024);
    sl->w = w; sl->h = h; sl->bpp = bpp; sl->fps10 = fps10; sl->src = sl->px;
    do_scale_and_flip(sl);
}

namespace {
const volatile unsigned long long* g_wd_pump = nullptr;
const volatile int* g_wd_frames = nullptr;
const volatile unsigned long long* g_wd_reads = nullptr;
const volatile unsigned int* g_wd_eip = nullptr;
const volatile unsigned long long* g_wd_sw = nullptr;
const volatile unsigned long long* g_wd_io = nullptr;   // cumulative file-I/O µs
const volatile unsigned long long* g_wd_jit = nullptr;  // cumulative FillBlock ns
const volatile unsigned int* g_wd_jitn = nullptr;       // FillBlock count
// Anti-starvation net (native only — null under coop, fam= field absent).
const volatile unsigned int* g_wd_famd = nullptr;       // heartbeat detections
const volatile unsigned int* g_wd_famn = nullptr;       // naps taken at seams
} // namespace
// jit-time breakdown: icache-sync share of FillBlock, and fills that failed
// (AllocDynarecMap OOM → block retranslated every visit = silent storm).
extern "C" uint64_t dyn86_sync_us;
extern "C" uint32_t dyn86_fill_fail;
// JIT pool occupancy (mman_vita.c). On the heartbeat because nothing ever
// evicts a translated block: the pool only grows, saturation is reached about
// 20 min into a session, and the first block that cannot be translated after
// that kills the guest thread. fail= only moves once that is already
// happening, and on every 0.1.6 report it moved inside the watchdog's own 10 s
// blind window -- too late to be evidence of anything. jitp= shows it coming.
extern "C" unsigned int dyn86_jitpool_size, dyn86_jitpool_used;
extern "C" uint32_t d2rt_va_used_mb(void);              // VA-arena occupancy (B3 growth curve)
extern "C" uint32_t d2rt_va_peak_mb(void);              // et son point haut (rt_boot.cpp)
extern "C" unsigned long long d2rt_hot_stat(int);      // native-port counters
extern "C" uint32_t d2rt_sprite_cache_kb(int which);   // D2's own sprite-cache accounting
extern "C" unsigned long long d2rt_cs_stat(int k);     // critical sections served intrinsically
extern "C" int d2rt_natprof;                            // D2_NATPROF: time per trap slot (bridge.cpp)
extern "C" int d2rt_natprof_lines(char* out, unsigned n, int topk);
// "audio:" line of the 10s window. On Vita a printf proves nothing, and the
// final report never arrives when a run ends via MAXFRAMES: this is the
// ONLY channel that says whether audio is running, whether it's starving the
// driver, and whether the native decoder is armed. Silent when D2_SON isn't
// armed, since a permanent zero would look like a dead counter.
namespace d2rt { namespace dsound { int stat_line(char* out, unsigned n); } }
extern "C" uint32_t dyn86_unal_stat(int k);            // UNAL: 64-bit x87 "parity" accesses (0=total, 1=unaligned, 2=last address, 3=site)
// Translator expansion, published as the run goes (the "emis:" line). A
// proof counter printed only in the final report is useless: on console
// that final report never arrives when a run ends via MAXFRAMES, so the log
// just stops at the last 10s window. Anything that must be readable on
// target has to go through the window instead.
extern "C" {
    extern unsigned long long dyn86_emit_arm_bytes, dyn86_emit_x86_bytes, dyn86_emit_blocks;
    // fastmmu: the base ADD ahead of every guest memory access. Two
    // switches, two tallies, counted at TRANSLATION time (pass 3).
    extern int dyn86_mmufold, dyn86_mmustack;
    extern unsigned long dyn86_mmu_folds, dyn86_mmu_sites, dyn86_mmu_foldable;
    extern unsigned long dyn86_mmu_st_heads, dyn86_mmu_st_links;
    extern unsigned long dyn86_mmu_st_pop, dyn86_mmu_st_push;
    extern unsigned long dyn86_mmu_st_rej_pos, dyn86_mmu_st_rej_pred;
    extern unsigned long dyn86_mmu_st_rej_kind;
    // memintrin (D2_MEMINTRIN): EXECUTION counters for the native helper, not
    // translated sites. Declared here rather than by including
    // dyn86_memintrin.h, since that header pulls in regs.h /
    // x86emu_private.h, outside the platform layer's include path.
    extern int dyn86_memintrin;
    // `appels` is served+fb: the dedicated counter cost a 64-bit
    // read-modify-write per call for a number that is a sum of two others.
    extern unsigned long long dyn86_mi_cpy_served,
                              dyn86_mi_cpy_fb,    dyn86_mi_cpy_bytes;
    extern unsigned long long dyn86_mi_set_served,
                              dyn86_mi_set_fb,    dyn86_mi_set_bytes;
    extern unsigned long long dyn86_mi_rej[4];
    // D2_MEMINTRIN=3: calls served by the INLINE sequence never reach the
    // helper, so they are in NONE of the counters above. What says the
    // mechanism is armed is the number of sequences emitted.
    extern unsigned long long dyn86_mi_fast_blocks, dyn86_mi_fc_n, dyn86_mi_fc_bad;
    // Generic native intrinsics (src/dynarec86/dyn86_intrin.h) + the D2-target
    // cross-oracle (src/runtime/d2_intrin_114.cpp).
    extern int dyn86_nopend;
    extern unsigned long dyn86_nopend_dropped, dyn86_nopend_kept;
    extern int dyn86_intrin_on;
    int dyn86_intrin_report(char* out, unsigned cap);
    extern unsigned long long d2_proj_ver_n, d2_proj_ver_bad, d2_proj_ver_skip;
}
extern "C" size_t d2rt_box86_custommalloc_kb(void);   // custommem.c: box86 allocation categories
extern "C" size_t d2rt_box86_jmptbl_kb(void);
extern "C" unsigned int dyn86_jitpool_size, dyn86_jitpool_used;   // JIT pool (mman_vita.c)
extern "C" unsigned int dyn86_jit_cur, dyn86_rw_cur;   // live JIT/RW memblock bytes (mman_vita)
// Refus du tas de METADONNEES de box86 (dynablock.c). Publie ici parce que
// c'est le seul poste memoire encore demande au noyau en pleine partie :
// tant qu'il ne figurait nulle part, un refus se lisait comme un crash sans
// cause (hfault_sys|SceLibKernel|0x120) au lieu d'un manque de RAM.
extern "C" unsigned int dyn86_meta_allocfail;
// Piscine RW des metadonnees de box86 (mman_vita.c). Publiee parce que
// c'est la seule facon de voir, sur une console qui n'est pas la mienne,
// si la reserve a bien ete prise et en quelle partition.
extern "C" unsigned int dyn86_rwpool_size, dyn86_rwpool_used;
// Optional per-guest-thread dump, registered by rt_boot once the scheduler
// exists; called from the watchdog when the frame counter stalls.
void (*d2vita_wd_threads)(void) = nullptr;
// Hot-address profiling, triggered BY THE WATCHDOG. The remote command
// channel (D2CMDFILE) stops responding after a few dozen actions, so relying
// on it to request profiling makes the tool unusable exactly when it's
// needed. The watchdog always keeps running, so it triggers this instead —
// useful when the game loops silently in its own code with no network calls
// left to observe, and only the hot address can name the loop.
void (*d2vita_wd_eipdump)(void) = nullptr;
// "gil=" field of the alive: line (GIL observability). Registered by rt_boot
// ONLY under the native backend: null under coop, so the field is ABSENT
// from the line — same discipline as fam=, never a misleading zero. Returns
// the number of bytes written (0 = nothing to report).
int (*d2vita_wd_gil)(char*, unsigned) = nullptr;
// "run=<id>:<state>:<blocks>[,...]" field of the alive: line — PER-RUNNER
// liveness (sched_native.h). Same discipline as gil= and fam=: registered by
// rt_boot only under the native backend, null under coop so the field is
// ABSENT, never a misleading zero. This is the field that distinguishes
// STARVATION (at least one counter moves between alive: lines) from
// EVERYTHING STOPPED (none move) — the R/B/P state published alongside it
// then separates a healthy wait (B) from a deadlock.
int (*d2vita_wd_run)(char*, unsigned) = nullptr;
extern "C" { extern uint32_t d2rt_timeprof_base; }   // guest module base (cpu_box86.cpp)
// Timestamped ring buffer for the time sampler.
// A profile averaged over the full 10s window can't answer this: catastrophic
// frames are only 12% of the count and their signature is drowned out by the
// 88% that are fine — a design flaw, not a limitation of the instrument. So
// samples are kept WITH their timestamp, and rt_boot retroactively keeps only
// the ones that fall inside a frame that exceeded the threshold.
extern "C" {
    volatile uint32_t d2rt_ts_w = 0;                  // write index
    uint64_t d2rt_ts_t[4096];                         // timestamp (us)
    uint32_t d2rt_ts_ip[4096];                        // guest EIP
}
namespace {
// Clean watchdog shutdown. Without it, the watchdog keeps running while the
// process tears down: the main thread is already parked in the kernel after
// ExitProcess, and the watchdog still touches pointers whose backing state
// has disappeared -> fault, producing a core dump on every normal exit.
// These "exit-time" dumps are indistinguishable from real crashes — an
// instrument that cries wolf on a clean exit is as costly as a silent one.
volatile bool g_wd_stop = false;


// Time-based sampler (D2_TIMESAMP). The existing sampler (D2_LAGWATCH) reads
// the guest EIP at EVERY block entry: a long block and a short block count
// the same, so its percentages are NOT percentages of time. This sampler
// instead reads at a FIXED CLOCK INTERVAL from a dedicated thread, so every
// sample represents equal time and the resulting shares are genuine time
// shares.
volatile bool g_ts_stop = false;
int timesamp_thread(SceSize, void*) {
    const char* e = std::getenv("D2_TIMESAMP");
    const unsigned per_us = (e && *e && *e != '0') ? (unsigned)std::atoi(e) : 0;
    if (!per_us) return 0;
    d2vita_pin_self(d2vita_core_mask(1), nullptr);
    struct Ent { unsigned eip, n; };
    static Ent tab[512];
    unsigned long long total = 0, nul = 0;
    uint64_t t_pub = sceKernelGetProcessTimeWide();
    while (!g_ts_stop) {
        sceKernelDelayThread(per_us);
        const unsigned ip = g_wd_eip ? *g_wd_eip : 0u;
        { const uint32_t w = d2rt_ts_w & 4095u;
          d2rt_ts_t[w] = sceKernelGetProcessTimeWide(); d2rt_ts_ip[w] = ip;
          d2rt_ts_w = d2rt_ts_w + 1; }
        ++total;
        if (!ip) { ++nul; }
        else {
            unsigned h = (ip * 2654435761u) >> 23, i = 0;
            for (; i < 512; ++i) { Ent& t = tab[(h + i) & 511];
                if (t.eip == ip) { ++t.n; break; }
                if (!t.eip)      { t.eip = ip; t.n = 1; break; } }
        }
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (now - t_pub < 10000000ull) continue;
        t_pub = now;
        int idx[8]; unsigned best[8] = {0};
        for (int k = 0; k < 8; ++k) { idx[k] = -1;
            for (int i = 0; i < 512; ++i) { bool used = false;
                for (int j = 0; j < k; ++j) if (idx[j] == i) used = true;
                if (!used && tab[i].eip && tab[i].n > best[k]) { best[k] = tab[i].n; idx[k] = i; } } }
        char m[400]; int w = 0;
        w += std::snprintf(m + w, sizeof m - w,
                "temps/eip(10s): echantillons=%llu (vides=%llu%%) ",
                total, total ? nul * 100 / total : 0);
        for (int k = 0; k < 8 && idx[k] >= 0; ++k) {
            const Ent& t = tab[idx[k]];
            const unsigned rva = t.eip - d2rt_timeprof_base;
            w += std::snprintf(m + w, (size_t)w < sizeof m ? sizeof m - w : 0,
                    "%s%x=%u%%", k ? " " : "", rva, total ? (unsigned)(t.n * 100 / total) : 0u);
        }
        d2vita_progress(m);
        for (int i = 0; i < 512; ++i) tab[i] = Ent{};
        total = nul = 0;
    }
    return 0;
}
int watchdog_thread(SceSize, void*) {
    { unsigned relu = 0; const int m = d2vita_core_mask(1); const int rc = d2vita_pin_self(m, &relu);
      char s[112]; std::snprintf(s, sizeof s, "watchdog: auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", (unsigned)m, (unsigned)rc, relu);
      d2vita_progress(s); }
    // Self-reported liveness (same pattern as the anti-starvation net,
    // extended to the watchdog): FIRST action, before the first 10s period.
    // Printed BY the thread itself, this line is proof it's running; its
    // ABSENCE from a post-mortem boot_progress says "watchdog never
    // scheduled" without waiting 10s or guessing. The priority read back is
    // informational (0x10000100 resolved).
    { char m[112];
      std::snprintf(m, sizeof m, "watchdog: arme (USER_%d createur [D2_COEURS], prio relue=%d)",
                    d2vita_core_mask(1) == SCE_KERNEL_CPU_MASK_USER_0 ? 0
                      : d2vita_core_mask(1) == SCE_KERNEL_CPU_MASK_USER_1 ? 1 : 2,
                    sceKernelGetThreadCurrentPriority());
      d2vita_progress(m); }
    // 10s period: freeze attribution needs finer windows than the 30s liveness
    // heartbeat (deltas between lines = where the stall time went).
    for (;;) {
        if (g_wd_stop) { d2vita_progress("watchdog: arret demande, fil termine"); return 0; }
        // Blind window: a native fault cuts the log off sharply — everything
        // since the last period is lost. Under D2_HALTWATCH (diagnostic runs
        // only) this tightens to 2 s: five times more log lines, but a blind
        // window five times shorter. Outside diagnostics it stays at 10 s —
        // file writes were the family of cost that made D2_NETWATCH expensive,
        // and this isn't multiplied during normal play.
        static const unsigned periode_us =
            getenv("D2_HALTWATCH") ? 2u * 1000u * 1000u : 10u * 1000u * 1000u;
        sceKernelDelayThread(periode_us);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // no idle dim/suspend mid-game
        char m[512];   // + champs fam=/gil=/run= (run= : 10 runners x <id>:<etat>:<blocs>)
        std::snprintf(m, sizeof m, "alive: pump=%llu frames=%d reads=%llu eip=%08x sw=%llu io=%llums jit=%llums n=%u sync=%llums fail=%u jitp=%u/%uMB va=%u/%uMB spr=%u/%uMB cel=%u/%uKB big=%u jm=%uMB",
                      g_wd_pump ? (unsigned long long)*g_wd_pump : 0ull,
                      g_wd_frames ? *g_wd_frames : -1,
                      g_wd_reads ? (unsigned long long)*g_wd_reads : 0ull,
                      g_wd_eip ? *g_wd_eip : 0u,
                      g_wd_sw ? (unsigned long long)*g_wd_sw : 0ull,
                      g_wd_io ? (unsigned long long)(*g_wd_io / 1000ull) : 0ull,
                      g_wd_jit ? (unsigned long long)(*g_wd_jit / 1000000ull) : 0ull,
                      g_wd_jitn ? *g_wd_jitn : 0u,
                      (unsigned long long)(dyn86_sync_us / 1000ull),
                      dyn86_fill_fail,
                      dyn86_jitpool_used >> 20, dyn86_jitpool_size >> 20,
                      d2rt_va_used_mb(), d2rt_va_peak_mb(),
                      d2rt_sprite_cache_kb(0) >> 10, d2rt_sprite_cache_kb(1) >> 10,
                      // cel=<usage>/<ceiling> KB: the CelData cache, hard-capped
                      // at 512,000 bytes — the only cache in the game that
                      // can saturate.
                      d2rt_sprite_cache_kb(4), d2rt_sprite_cache_kb(2),
                      d2rt_sprite_cache_kb(3), (dyn86_jit_cur + dyn86_rw_cur) >> 20);
        // fam=<detections>/<naps> field: only when the native backend has
        // passed its pointers — absent under coop.
        if (g_wd_famd) {
            size_t l = std::strlen(m);
            std::snprintf(m + l, sizeof m - l, " fam=%u/%u",
                          *g_wd_famd, g_wd_famn ? *g_wd_famn : 0u);
        }
        // gil=<holder>/>=<ms>/acq=<n>[ in=<shim>] field: native only. ">="
        // because the age is DERIVED from two watchdog samples — at most one
        // period less than reality, never more. gil=- means free at sample
        // time. An acq count FROZEN between two alive: lines is the
        // signature of a hold that's no longer progressing.
        if (d2vita_wd_gil) {
            size_t l = std::strlen(m);
            if (l + 8 < sizeof m) d2vita_wd_gil(m + l, (unsigned)(sizeof m - l));
        }
        // run=<id>:<state>:<blocks> field: PER-RUNNER liveness. Two
        // successive alive: lines are enough to read a freeze — a counter
        // that moves while frames= is stuck names STARVATION; none moving
        // means everything is stopped, and the state (R/B/P) says whether
        // that's a healthy wait.
        if (d2vita_wd_run) {
            size_t l = std::strlen(m);
            // Always called whenever there's room left: under the space
            // needed, the field reads "run=?" and stays PRESENT — a field
            // that vanishes because the line was full would be
            // indistinguishable from "coop backend".
            if (l + 8 < sizeof m) d2vita_wd_run(m + l, (unsigned)(sizeof m - l));
        }
        d2vita_progress(m);
        // Native ports: served count per target + total fallbacks. This is
        // what tells you, on console, whether a port even ran at all — and
        // a rising fallback count means the fast-path condition isn't holding.
        { char h[224];
          // blit=: the CELL blit (g_blitN). The single biggest item in the
          // rendering budget, and it used to be published only at shutdown —
          // invisible on any bench run. Without it, "cells per frame" can't
          // be computed, the missing denominator for every rendering lever.
          // lent= is the virtual-path fallback: rising means the fast-path
          // condition isn't holding and the expected gain isn't there.
          std::snprintf(h, sizeof h,
                        "hot: grille=%llu coll=%llu rle=%llu lum=%llu blend=%llu expl=%llu blit=%llu lent=%llu",
                        d2rt_hot_stat(0), d2rt_hot_stat(1), d2rt_hot_stat(2),
                        d2rt_hot_stat(3), d2rt_hot_stat(4), d2rt_hot_stat(5),
                        d2rt_hot_stat(11), d2rt_hot_stat(13));
          d2vita_progress(h);
          // Sized 480, not 288: the engine's line carries several numeric
          // fields (peak, rms, gain0, output-error, flow) and was close to
          // truncation. The engine only appends the embedder's codec
          // counters if there's room left, so a too-short buffer would drop
          // them SILENTLY — exactly the failure mode these fields exist to
          // close. Fixed text (162 B) + fifteen counters: 480 leaves margin.
          { char au[480]; if (d2rt::dsound::stat_line(au, sizeof au) > 0) d2vita_progress(au); }
          // Cell-loop census (D2_CELLCENSUS). A SEPARATE line, not more
          // fields: the hot: line already has eight fields and serves as a
          // stable series comparable across logs. Silent when the census
          // isn't armed, since a permanent zero would look like a dead
          // counter.
          if (d2rt_hot_stat(14)) {
              char cl[176];
              std::snprintf(cl, sizeof cl,
                  "cellules: appels=%llu declarees=%llu pures=%llu | ign=%llu rej=%llu plate=%llu clip=%llu horspiste=%llu",
                  d2rt_hot_stat(14), d2rt_hot_stat(15), d2rt_hot_stat(16),
                  d2rt_hot_stat(17), d2rt_hot_stat(18), d2rt_hot_stat(19),
                  d2rt_hot_stat(20), d2rt_hot_stat(21));
              d2vita_progress(cl);
          }
          // Cell-loop port (NATIVECELLLOOP). The fallback is broken down BY
          // REASON: a bare total would force guessing where it comes from.
          if (d2rt_hot_stat(22) || d2rt_hot_stat(24)) {
              char bl[208];
              std::snprintf(bl, sizeof bl,
                  "boucle: servis=%llu cellules=%llu dont-ECHELLE=%llu repli=%llu (degrade=%llu echelle=%llu vuehote=%llu trop=%llu tables=%llu)",
                  d2rt_hot_stat(22), d2rt_hot_stat(23), d2rt_hot_stat(46), d2rt_hot_stat(24),
                  d2rt_hot_stat(25), d2rt_hot_stat(29), d2rt_hot_stat(26),
                  d2rt_hot_stat(27), d2rt_hot_stat(28));
              d2vita_progress(bl);
          }
          // Fidelity of the flat wrapper at Game+0x415240. Storm serializes
          // ALL threads through ONE global buffer there (ds:0x77903c,
          // critical section 0x779080), so its residue chain is unique.
          // "distinct-threads > 1" with a per-thread buffer means our chain
          // CANNOT match the original's — and this wrapper has never had a
          // cross-checking oracle (D2_EXPLVERIFY only covers 0x41e000).
          // "max-output" guards the other risk: the caller has to invent a
          // 256 KiB capacity figure, since the ABI doesn't provide one.
          if (d2rt_hot_stat(72)) {
              char sf[176];
              std::snprintf(sf, sizeof sf,
                  "scompplat: appels=%llu fils-distincts=%llu sortie-max=%llu o%s",
                  d2rt_hot_stat(72), d2rt_hot_stat(73), d2rt_hot_stat(74),
                  d2rt_hot_stat(73) > 1 ? "  ATTENTION CHAINES MULTIPLES" : "");
              d2vita_progress(sf);
          }
          // Trap takes per frame — the denominator for the whole "traversal"
          // cost line. It used to exist only in the final report, so it was
          // never readable on console (a native+MAXFRAMES run never reaches
          // it). Without it, a per-traversal cost figure can't be converted
          // into a share of frame time, and a porting decision ends up
          // resting on a guess. The sum over the 4096 slots is computed HERE,
          // once per window: the hot path pays nothing beyond the bump
          // that's already in place.
          { static uint64_t s_prevTraps = 0; static int s_prevFrames = 0;
            if (d2rt::trapcnt::base) {
                uint64_t tot = 0;
                for (uint32_t i = 0; i < d2rt::trapcnt::kMax; ++i) tot += d2rt::trapcnt::hits[i];
                const int fr = g_wd_frames ? *g_wd_frames : 0;
                const uint64_t dT = tot - s_prevTraps;
                const int dF = fr - s_prevFrames;
                char tp[160];
                std::snprintf(tp, sizeof tp,
                    "traps: total=%llu fenetre=%llu images=%d par-image=%llu",
                    (unsigned long long)tot, (unsigned long long)dT, dF,
                    (unsigned long long)(dF > 0 ? dT / (uint64_t)dF : 0));
                d2vita_progress(tp);
                s_prevTraps = tot; s_prevFrames = fr;
            } }
          // Cell-loop fork-join (D2_CELLPAR). The final report never arrives
          // on console (a run doesn't exit cleanly): without this line, an
          // armed leg and a dead leg would produce the same log. Published
          // as soon as a worker runs, even at zero calls — "workers=2
          // calls=0" is exactly the diagnostic that would otherwise be
          // missing.
          if (d2rt_hot_stat(85)) {
              char cp[224];
              const unsigned long long joins = d2rt_hot_stat(77);
              std::snprintf(cp, sizeof cp,
                  "cellpar: ouvriers=%llu appels=%llu (seq=%llu) cellules-ouvriers=%llu"
                  " jointures=%llu attente-max=%llu moy=%llu vols=%llu chev=%llu vides=%llu",
                  d2rt_hot_stat(85), d2rt_hot_stat(75), d2rt_hot_stat(84),
                  d2rt_hot_stat(76), joins, d2rt_hot_stat(78),
                  joins ? d2rt_hot_stat(79) / joins : 0ull,
                  d2rt_hot_stat(80), d2rt_hot_stat(81), d2rt_hot_stat(83));
              d2vita_progress(cp);
              // Load balance (D2_CELLPARSHARE). A SEPARATE line: the previous
              // one already has ten fields and serves as a comparable series
              // across logs. "share" = what the workers ACTUALLY received
              // (span bytes, then cells drawn), not what the knob requested.
              // Both wait times should trend toward zero TOGETHER; the
              // "worker-prologue" floor is serial work that no amount of
              // sharing can hand off to the worker.
              { const unsigned long long spW = d2rt_hot_stat(94), spC = d2rt_hot_stat(95);
                const unsigned long long ceW = d2rt_hot_stat(76), ceC = d2rt_hot_stat(93);
                const unsigned long long prises = d2rt_hot_stat(98);
                char eq[208];
                std::snprintf(eq, sizeof eq,
                    "cellpar-part: demandee=%llu%% (0=egale) obtenue-octets=%llu%% obtenue-cellules=%llu%%"
                    " | attente appelant=%llu ouvrier-prologue=%llu ouvrier-hors-appel=%llu (prises=%llu)",
                    d2rt_hot_stat(99),
                    (spW + spC) ? spW * 100ull / (spW + spC) : 0ull,
                    (ceW + ceC) ? ceW * 100ull / (ceW + ceC) : 0ull,
                    joins ? d2rt_hot_stat(79) / joins : 0ull,
                    prises ? d2rt_hot_stat(96) / prises : 0ull,
                    prises ? d2rt_hot_stat(97) / prises : 0ull,
                    prises);
                d2vita_progress(eq); }
          }
          // Read-back core map. A requested pin is not an obtained one: this
          // line publishes, for each host thread, what it asked for, the rc
          // it got, the mask READ BACK, and above all the LAST core it ran
          // on — the only one of these figures that's an actual measurement.
          wx86_vita_core_window_line();
          // Pipelined presentation (D2_PRESENTTHREAD). Published ONLY when
          // the thread is armed: the control leg has no line at all, so an
          // armed leg showing images=0 means a dead thread, not silence.
          // "attente-rendu" is what the RENDER thread still pays: if it stays
          // small next to the roughly one thousand microseconds presentation
          // used to cost, the offload actually happened.
          if (g_pt_run) {
              char pp[208];
              std::snprintf(pp, sizeof pp,
                  "present: images=%u copies=%u attente-rendu=%uus/%u tours retard=%u expire=%u course=%u coeur-relu=%d (demande=%d rc=0x%08x)",
                  g_pt_images, g_pt_copies, g_pt_waitus, g_pt_waitn,
                  g_pt_late, g_pt_expire, g_pt_race, g_pt_cpu,
                  g_pt_core, (unsigned)g_pt_aff_rc);
              d2vita_progress(pp);
          }
          // Critical sections served intrinsically. Silent until something
          // has actually been served: the control leg has no line at all, so
          // an armed leg WITHOUT this line served nothing.
          { const unsigned long long ce = d2rt_cs_stat(0), cl = d2rt_cs_stat(2);
            if (ce || cl) { char cs[160];
                std::snprintf(cs, sizeof cs,
                    "csintrin: enter=%llu (replis=%llu) leave=%llu esp-hisse=%llu",
                    ce, d2rt_cs_stat(1), cl, d2rt_cs_stat(3));
                d2vita_progress(cs); } }
          // Clocks READ BACK every window. The startup "clocks:" line isn't
          // enough: an external tool (PSVshell) can change the clock speed
          // MID-SESSION — this is even part of the clock-ramp workflow
          // (setting 333 during loading). The proof needs to date from the
          // measured window, not from boot.
          // GPU and its xbar clock are in this line too: a GPU running below
          // 222 MHz (or an xbar below 166) would explain part of any GPU
          // wait time, and the startup "clocks:" line only read back ARM and
          // bus — the GPU requests were never verified. A knob that's never
          // read back is a knob you can't be sure took effect.
          { char ck[128];
            std::snprintf(ck, sizeof ck, "clk: arm=%d bus=%d gpu=%d xbar=%d",
                          scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                          scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
            d2vita_progress(ck); }
          // Memory gauge, published every window. Kernel pools AND the
          // newlib heap (uordblks = current malloc'd bytes, the only thing
          // operator new draws from). Out-of-memory here surfaces as an
          // abort (std::bad_alloc), not a typical crash, and a gauge that
          // steps down window over window points at the culprit before it
          // happens.
          { SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
            const int r = sceKernelGetFreeMemorySize(&fi);
            struct mallinfo mi = mallinfo();
            // Breakdown: what comes from box86 vs the rest of the runtime.
            // Without this split, "the heap grew by 8 MiB" doesn't point at
            // anyone in particular.
            char mm[288];   // must fit "dont box86: ..." without truncating
            std::snprintf(mm, sizeof mm,
              "MEM: libre user=%d Ko cdram=%d Ko phycont=%d Ko (rc=%d) | tas newlib: en-cours=%d Ko reserve=%d/%u Ko libre=%d Ko | dont box86: custom=%u Ko (refus=%u) sauts=%u Ko | piscine RW %u/%u Ko | piscine JIT %u/%u Ko",
              fi.size_user>>10, fi.size_cdram>>10, fi.size_phycont>>10, r,
              (int)(mi.uordblks>>10), (int)(mi.arena>>10), _newlib_heap_size_user>>10, (int)(mi.fordblks>>10),
              (unsigned)d2rt_box86_custommalloc_kb(), dyn86_meta_allocfail, (unsigned)d2rt_box86_jmptbl_kb(),
              dyn86_rwpool_used>>10, dyn86_rwpool_size>>10,
              dyn86_jitpool_used>>10, dyn86_jitpool_size>>10);
            d2vita_progress(mm); }
          // D2_NATPROF: TIME per trap slot within the window (native ports +
          // shims), top 8. The denominator is the window duration (~10 s)
          // and the frame count (frames: line): "total" divided by frames
          // gives host ms per frame. A measurement instrument.
          if (d2rt_natprof) {
              static char nb[1400];
              int nl = d2rt_natprof_lines(nb, sizeof nb, 8);
              if (nl > 0) { char* p = nb; while (*p) { char* e = strchr(p, '\n'); if (e) *e = 0;
                                                     d2vita_progress(p); if (!e) break; p = e + 1; } }
          }
          // Translator expansion. Always published: this is a fact about the
          // shipped binary, not an A/B instrument, and it's the only figure
          // that says how much ARM code the translator produces per unit of
          // guest work. Expressed in thousandths, not as a float (no %f in
          // this log).
          if (dyn86_emit_x86_bytes) {
              const unsigned long long r =
                  dyn86_emit_arm_bytes * 1000ull / dyn86_emit_x86_bytes;
              char ex[160];
              std::snprintf(ex, sizeof ex,
                  "emis: blocs=%llu x86=%lluo arm=%lluo expansion=%llu.%03llux",
                  dyn86_emit_blocks, dyn86_emit_x86_bytes, dyn86_emit_arm_bytes,
                  r / 1000ull, r % 1000ull);
              d2vita_progress(ex);
          }
          // fastmmu: same window as "emis:", same reason — the final report
          // never arrives on console when a run ends via MAXFRAMES. Published
          // as soon as a site has been RECOGNIZED, including in the control
          // leg: "folded=0 links=0" there proves the control is clean, and an
          // armed leg reading zero would be an empty pass.
          // The numbers are TRANSLATED SITES, not executions.
          if (dyn86_mmu_sites || dyn86_mmu_st_heads || dyn86_mmu_st_links) {
              char mm[224];
              std::snprintf(mm, sizeof mm,
                  "mmu: fold=%d sites=%lu pliables=%lu plies=%lu | stack=%d tetes=%lu maillons=%lu"
                  " (pop=%lu push=%lu) | refus emis=%lu saut=%lu famille=%lu",
                  dyn86_mmufold, dyn86_mmu_sites, dyn86_mmu_foldable, dyn86_mmu_folds,
                  dyn86_mmustack, dyn86_mmu_st_heads, dyn86_mmu_st_links,
                  dyn86_mmu_st_pop, dyn86_mmu_st_push,
                  dyn86_mmu_st_rej_pos, dyn86_mmu_st_rej_pred, dyn86_mmu_st_rej_kind);
              d2vita_progress(mm);
          }
          // D2_NOPEND: block-end sites where X_PEND was RELEASED or KEPT.
          // Published as soon as the knob is armed, so "released=0" is
          // itself a readable result (an armed but inert leg).
          if (dyn86_nopend) {
              char np[160];
              std::snprintf(np, sizeof np,
                  "nopend: mode=%d relaches=%lu gardes=%lu",
                  dyn86_nopend, dyn86_nopend_dropped, dyn86_nopend_kept);
              d2vita_progress(np);
          }
          // Native intrinsics (D2_INTRIN): SAME window, SAME reason. The
          // line prints AS SOON AS the mechanism is armed, so "served=0" is
          // itself a readable result instead of an unexplained silence.
          if (dyn86_intrin_on) {
              char it[320];
              if (dyn86_intrin_report(it, sizeof it) > 0) d2vita_progress(it);
              if (d2_proj_ver_n) {
                  char v[128];
                  std::snprintf(v, sizeof v,
                                "projverify: compares=%llu divergences=%llu sautes=%llu",
                                (unsigned long long)d2_proj_ver_n,
                                (unsigned long long)d2_proj_ver_bad,
                                (unsigned long long)d2_proj_ver_skip);
                  d2vita_progress(v);
              }
          }
          // memintrin (D2_MEMINTRIN): same window, same reason as "mmu:". The
          // only place this used to be published was main()'s end-of-run
          // report, which never arrives on console once the frame counter
          // stalls and the run gets killed. A separate "FRAME PROFILE" output
          // existing proves nothing about this one. Published as soon as the
          // knob is armed, so "calls=0" is itself a readable result instead
          // of an unexplained silence.
          if (dyn86_memintrin) {
              char mi[288];
              std::snprintf(mi, sizeof mi,
                  "memintrin: mode=%d | memcpy appels=%llu servis=%llu replis=%llu octets=%llu"
                  " | memset appels=%llu servis=%llu replis=%llu octets=%llu"
                  " | rejets arene=%llu taille=%llu"
                  " | en-ligne: sequences=%llu oracle=%llu/%llu",
                  dyn86_memintrin,
                  (unsigned long long)(dyn86_mi_cpy_served + dyn86_mi_cpy_fb),
                  (unsigned long long)dyn86_mi_cpy_served,
                  (unsigned long long)dyn86_mi_cpy_fb,    (unsigned long long)dyn86_mi_cpy_bytes,
                  (unsigned long long)(dyn86_mi_set_served + dyn86_mi_set_fb),
                  (unsigned long long)dyn86_mi_set_served,
                  (unsigned long long)dyn86_mi_set_fb,    (unsigned long long)dyn86_mi_set_bytes,
                  (unsigned long long)dyn86_mi_rej[0],    (unsigned long long)dyn86_mi_rej[1],
                  (unsigned long long)dyn86_mi_fast_blocks,
                  (unsigned long long)dyn86_mi_fc_bad,    (unsigned long long)dyn86_mi_fc_n);
              d2vita_progress(mi);
          }
          // UNAL: 64-bit x87 "parity" accesses (LDRD/STRD that box86 assumes
          // are aligned) routed through host helpers — total count, and how
          // many were actually UNALIGNED (unpredictable on A9, tolerated by
          // qemu). Printed on first occurrence, then on each new unaligned
          // address.
          { static uint32_t s_unalSeen = 0xFFFFFFFFu;
            const uint32_t ut = dyn86_unal_stat(0), un = dyn86_unal_stat(1);
            if (ut && un != s_unalSeen) { s_unalSeen = un; char ua[160];
                std::snprintf(ua, sizeof ua, "UNAL x87q: appels=%u NON-ALIGNES=%u derniere=%08x site=%u",
                              ut, un, dyn86_unal_stat(2), dyn86_unal_stat(3));
                d2vita_progress(ua); } }
          // Decompressor cross-checker (D2_EXPLVERIFY). Published as soon as
          // the comparison runs: "0 divergences" out of N calls is the
          // result, and N needs to be visible to know whether that's
          // meaningful.
          if (d2rt_hot_stat(63)) {
              char xv[128];
              std::snprintf(xv, sizeof xv, "explverify: compares=%llu divergences=%llu",
                            d2rt_hot_stat(63), d2rt_hot_stat(64));
              d2vita_progress(xv);
          }
          // Writes REFUSED by the per-write guard. This is the counter the
          // aggregate guard alone couldn't produce.
          // kind: 0=flat 1=scaled 2=scaled-clipped 3=flat-clipped
          if (d2rt_hot_stat(22) || d2rt_hot_stat(24)) {
              char we[192];
              std::snprintf(we, sizeof we,
                  "ecritures: refusees=%llu | 1ere: feuille=%llu off=%llu len=%llu span=%llu",
                  d2rt_hot_stat(67), d2rt_hot_stat(71), d2rt_hot_stat(68),
                  d2rt_hot_stat(69), d2rt_hot_stat(70));
              d2vita_progress(we);
          }
          // Image guard rail. Published EVEN AT ZERO as long as the port
          // runs: the zero itself is the result ("our writes never leave the
          // DIB"), and a silent line couldn't be told apart from a dead
          // counter. Also prints the DIB's extent and the FIRST offending
          // extent, since a nonzero counter alone wouldn't say how far out
          // of bounds it went or on which side.
          if (d2rt_hot_stat(22) || d2rt_hot_stat(24) || d2rt_hot_stat(56)) {
              char ob[208];
              std::snprintf(ob, sizeof ob,
                  "image: hors-DIB boucle=%llu blit=%llu replis=%llu | DIB=[%08llx,%08llx) | 1ere fautive=[%08llx,%08llx)",
                  d2rt_hot_stat(55), d2rt_hot_stat(56), d2rt_hot_stat(61),
                  d2rt_hot_stat(59), d2rt_hot_stat(59) + d2rt_hot_stat(60),
                  d2rt_hot_stat(57), d2rt_hot_stat(58));
              d2vita_progress(ob);
          }
          // Frame shape (D2_LOOPSHAPE): what decides whether a dedicated
          // thread makes sense. intrus/img = frame writes that landed INSIDE
          // the burst without being part of it — exactly what an offload
          // would break. window% = the share of the frame left AFTER the
          // burst, i.e. the time another core would have to absorb the work.
          if (d2rt_hot_stat(30)) {
              unsigned long long F = d2rt_hot_stat(30);
              char fo[200];
              std::snprintf(fo, sizeof fo,
                  "forme: trames=%llu ev/img=%.1f boucle/img=%.1f avant=%.1f INTRUS=%.1f apres=%.1f | rafale=%.1fms fenetre=%.1fms (%.0f%% de %.1fms)",
                  F,
                  (double)d2rt_hot_stat(31)/F, (double)d2rt_hot_stat(32)/F,
                  (double)d2rt_hot_stat(33)/F, (double)d2rt_hot_stat(34)/F,
                  (double)d2rt_hot_stat(35)/F,
                  (double)d2rt_hot_stat(36)/F/1000.0,
                  (double)d2rt_hot_stat(37)/F/1000.0,
                  d2rt_hot_stat(38) ? 100.0*(double)d2rt_hot_stat(37)/(double)d2rt_hot_stat(38) : 0.0,
                  (double)d2rt_hot_stat(38)/F/1000.0);
              d2vita_progress(fo);
          }
#ifdef D2_TLSWRAP
          // emutls chains ACTUALLY executed, paired with acq in the SAME
          // line: without that pairing, the ratio would depend on a
          // different line whose acq field disappears when the GIL isn't
          // held (sched_native.cpp:52) — exactly the kind of ambiguity this
          // logging discipline avoids.
          { d2rt::gil::Probe pr; d2rt::gil::probe(&pr);
            char e[128];
            std::snprintf(e, sizeof e, "emutls: appels=%u prises=%u",
                          d2_emutls_calls, pr.acq);
            d2vita_progress(e);
            char tp[240]; if (d2_tlswrap_dump(tp, sizeof tp) > 0) d2vita_progress(tp); }
#endif
          // Fallbacks PER TARGET, only when nonzero. A bare total forces
          // guessing which one grew and why; correlating its slope with
          // another line's is a detour this breakdown removes entirely.
          if (d2rt_hot_stat(6)) {
              std::snprintf(h, sizeof h, "hot-replis: lum=%llu blend=%llu SCompExplode=%llu scomp=%llu",
                            d2rt_hot_stat(7), d2rt_hot_stat(8), d2rt_hot_stat(9), d2rt_hot_stat(10));
              d2vita_progress(h); } }
        if (g_wd_stop) return 0;      // demande arrivee pendant la periode
        // Frame counter stalled since the last beat -> dump guest threads.
        static int lastf=-2;
        int f = g_wd_frames ? *g_wd_frames : -1;
        if (f == lastf && d2vita_wd_threads) d2vita_wd_threads();
        lastf = f;
        // D2_EIPAUTO=N: profile every N watchdog periods (10 s each). Frames
        // keep advancing during a network stall, so this dump can't be
        // conditioned on a stalled frame counter: the stall being chased
        // leaves rendering alive.
        if (d2vita_wd_eipdump) {
            static const char* ea = getenv("D2_EIPAUTO");
            static const int every = ea ? atoi(ea) : 0;
            static int beat = 0;
            if (every > 0 && ++beat % every == 0) d2vita_wd_eipdump();
        }
    }
    return 0;
}
} // namespace

void d2vita_watchdog_stop() { g_wd_stop = true; }

// Clean shutdown of the presentation thread (same reason as the watchdog's:
// a thread still running during teardown touches pointers whose state
// vanishes underneath it). Waits for it to exit its loop, bounded to 1 s: a
// stuck thread must not prevent the process from exiting.
void d2vita_present_stop() {
    if (!g_pt_run) return;
    g_pt_run = 0;
    __sync_synchronize();
    for (int i = 0; i < 100 && g_pt_th >= 0; ++i) {
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        if (sceKernelGetThreadInfo(g_pt_th, &ti) < 0) break;
        if (ti.status & (SCE_THREAD_DORMANT | SCE_THREAD_STOPPED)) break;
        sceKernelDelayThread(10000);
    }
    char m[176];
    std::snprintf(m, sizeof m,
        "present(final): images=%u copies=%u attente-rendu=%uus/%u tours retard=%u expire=%u course=%u coeur-relu=%d",
        g_pt_images, g_pt_copies, g_pt_waitus, g_pt_waitn,
        g_pt_late, g_pt_expire, g_pt_race, g_pt_cpu);
    d2vita_progress(m);
}

void d2vita_watchdog_start(const unsigned long long* pump_count, const int* frames, const unsigned long long* reads,
                           const unsigned int* eip, const unsigned long long* switches,
                           const unsigned long long* io_us, const unsigned long long* jit_ns,
                           const unsigned int* jit_count,
                           const unsigned int* fam_detections, const unsigned int* fam_naps) {
    g_wd_pump = pump_count; g_wd_frames = frames; g_wd_reads = reads; g_wd_eip = eip; g_wd_sw = switches;
    g_wd_io = io_us; g_wd_jit = jit_ns; g_wd_jitn = jit_count;
    g_wd_famd = fam_detections; g_wd_famn = fam_naps;
    { const char* e = std::getenv("D2_TIMESAMP");
      if (e && *e && *e != '0') {
          SceUID ts = sceKernelCreateThread("d2_timesamp", timesamp_thread, 0x10000100,
                                            16 * 1024, 0, 0, nullptr);
          if (ts >= 0) { sceKernelStartThread(ts, 0, nullptr);
              char m[120]; std::snprintf(m, sizeof m,
                  "timesamp: ARME — EIP invite preleve toutes les %s us (parts = parts de TEMPS)", e);
              d2vita_progress(m); } } }
    SceUID th = sceKernelCreateThread("d2_watchdog", watchdog_thread, 0x10000100,
                                      16 * 1024, 0, 0, nullptr);
    if (th >= 0) {
        // Pinned to USER_2 by its CREATOR, before start. Default affinity and
        // USER_1 were both found to leave the watchdog silently starved;
        // USER_2 is the core proven live — presentation runs there too.
        // Contention is negligible: presentation (WaitSema), the
        // anti-starvation heartbeat (naps <= 50 ms) and the watchdog (10 s)
        // are all blocking waiters at the same priority, and a DelayThread
        // nap is known to yield to a ready same-priority thread. rc is
        // checked: a possibly-captive watchdog must announce that at boot,
        // not be inferred from its silence.
        // D2_COEURS position 1. Default "2" = USER_2, matching prior behavior
        // exactly.
        const unsigned wmask = (unsigned)d2vita_core_mask(1);
        int prc = wx86_vita_pin_thread(th, (int)wmask, nullptr);
        d2vita_core_register("chien", th, wmask, prc);
        if (prc < 0) {
            char m[112];
            std::snprintf(m, sizeof m,
                "watchdog: epinglage masque 0x%x KO (rc=0x%08x) — chien possiblement captif",
                wmask, (unsigned)prc);
            d2vita_progress(m);
        }
        // Start rc checked too (same discipline as the pin): an ABSENT
        // watchdog must announce itself at boot — its silence is exactly the
        // symptom being instrumented for. On failure: DeleteThread (a
        // created-but-never-started thread still holds its TCB + stack).
        int src = sceKernelStartThread(th, 0, nullptr);
        if (src < 0) {
            char m[96];
            std::snprintf(m, sizeof m,
                "watchdog: StartThread KO (rc=0x%08x) — chien de garde ABSENT", (unsigned)src);
            d2vita_progress(m);
            sceKernelDeleteThread(th);
        }
    }
}


// ============================ physical controls ==============================
// sceCtrl/sceTouch -> the runtime's input-injection grammar (d2vita_inject,
// the exact synth path D2SCRIPT uses). Called once per guest frame from
// rt_boot's dumpFrame (weak hook d2vita_input_tick). Current mapping:
//   left stick  = DIRECT MOVEMENT (cursor orbits the character, left click
//                 held while the stick is pushed — D2 walks continuously
//                 toward a click-held cursor)
//   right stick = free mouse look, no click (aiming, menus)
//   front touch = absolute cursor; short tap = full left click
//   L=left click (held)  R=right click (held)  Square(held)=Alt
//   Cross=R walk/run toggle  Circle=Shift (held)  Triangle=W weapon swap
//   D-pad = belt potions 1-4  |  Start=Escape
//   R is ALSO the combo layer (its own click is independent of this role):
//   R+Triangle=virtual keyboard  R+D-pad=F1..F4  R+Select=Space
//   Select = radial menu (see radial_menu.h): skill tree, quests, automap,
//   inventory, chat, party, character — replaces the older R+Cross=C,
//   R+Circle=S, R+Square=Q, Triangle=Tab bindings. The virtual keyboard opens
//   via R+Triangle (Triangle alone still swaps weapons).
// Remap without a rebuild: ux0:data/d2vita/controls.txt ("cross=rclick",
// "r+triangle=f5", "orbit=140", "sens=10", "deadzone=0.25", "anchor_y=470").
extern "C" { __attribute__((weak)) void d2gxm_shot_request(void); }
extern "C" void d2vita_inject(const char* act, int a, int b);
namespace {
enum { A_NONE=0, A_LMB=1, A_RMB=2, A_KEY=3 };
struct Act { int kind; int vk; };
struct BtnMap { uint32_t bit; Act base, layer; };
// D2_LAGMARK=0 disables the freeze marker (up arrow); it arms automatically
// whenever D2_LAGWATCH is present, since it's meaningless without it. Cost:
// one bit test per controller read (30 Hz).
static int g_lagmark = -1;
static inline int lagmark_on(){
    if (g_lagmark < 0) {
        const char* e = getenv("D2_LAGMARK");
        if (e) g_lagmark = (e[0] && e[0] != '0') ? 1 : 0;
        else   g_lagmark = getenv("D2_LAGWATCH") ? 1 : 0;
    }
    return g_lagmark;
}
constexpr uint32_t B_SELECT=0x000001, B_START=0x000008, B_UP=0x000010, B_RIGHT=0x000020,
                   B_DOWN=0x000040, B_LEFT=0x000080, B_L=0x000100, B_R=0x000200,
                   B_TRI=0x001000, B_CIR=0x002000, B_CROSS=0x004000, B_SQR=0x008000;
BtnMap g_btn[] = {
    // Cross/Circle carry the role L used to hold alone (L/R are now the
    // click buttons, handled separately — see the L/R block further below).
    { B_CROSS,  {A_KEY,0x52}, {A_NONE,0} },      // Cross: R, walk/run (toggle) | R+: free
    { B_CIR,    {A_KEY,0x10}, {A_NONE,0} },     // Circle: Shift (held)         | R+: free
    { B_SQR,    {A_KEY,0x12}, {A_NONE,0} },     // Square: Alt      | R+: (free: Q lives in the radial menu)
    { B_TRI,    {A_KEY,0x57}, {A_NONE,0} },     // Tri  : W weapon swap | R+: keyboard (handled separately)
    { B_UP,     {A_KEY,0x31}, {A_KEY,0x70} },   // ^ potion1       | R+: F1
    { B_LEFT,   {A_KEY,0x32}, {A_KEY,0x71} },   // < potion2       | R+: F2
    { B_DOWN,   {A_KEY,0x33}, {A_KEY,0x72} },   // v potion3       | R+: F3
    { B_RIGHT,  {A_KEY,0x34}, {A_KEY,0x73} },   // > potion4       | R+: F4
    { B_START,  {A_KEY,0x1B}, {A_NONE,0} },     // Start: ESCAPE | R+Start = keyboard (handled separately)
    // Select is handled separately (see below): opens the radial menu
    // (7 sectors) immediately; R+Select = Space.
};
float g_cx=400.f, g_cy=300.f;                    // cursor, GAME coords
bool  g_lmb_stick=false, g_lmb_btn=false, g_lmb_sent=false, g_rmb_sent=false;
uint32_t g_ctl_prev=0; bool g_ctl_init=false;
// Orbit radius for direct-movement mode; still adjustable without a rebuild
// via controls.txt orbit=N.
int   g_orbit=70, g_anchor_y_pm=470;             // character anchor: y = h*470/1000
float g_sens=10.f, g_dz=0.25f;
int   g_itick=0;
// Tick rate (D2_INPUTHZ) and duration thresholds EXPRESSED IN TICKS. The
// base thresholds (15 and 12) assume ~60 Hz — 250 ms and 200 ms. Leaving
// them fixed while running at a lower rate would double the felt duration of
// a "short tap" and of "L held briefly", silently changing how the controls
// feel: they're recomputed from the actual tick rate instead.
int   g_in_hz=0, g_tap_ticks=15;
int   g_t_at=-1, g_t_x=0, g_t_y=0, g_t_x0=0, g_t_y0=0; bool g_t_moved=false;
Act g_engaged[sizeof g_btn / sizeof *g_btn];
// Select: opens the radial menu on press (no direct fallback); R+Select = Space.
enum { SEL_IDLE=0, SEL_SPACE, SEL_RADIAL };
int g_sel_mode = SEL_IDLE;

// ---- scheme v2 ("aim") — mapping documented in docs-site/controles.md
bool         g_scheme_aim = true;          // controls.txt scheme=aim|mouse ; env D2_PAD=0 forces mouse
pad::Config  g_padcfg;
pad::Scheme* g_scheme = nullptr;           // built at first use (after controls.txt)
bool         g_scheme_active = false;      // the scheme currently owns the controls (in game)
uint32_t     g_pad_lastFrame = 0; int g_pad_stale = 0;
int          g_padlog = -1;                // D2_PADLOG

void pad_emit(const pad::Actions& a){
    for (int i = 0; i < a.n; i++) {
        const pad::Action& x = a.v[i];
        switch (x.k) {
            case pad::A_MOVE:    d2vita_inject("move", x.a, x.b); break;
            case pad::A_LDOWN:   d2vita_inject("ldown", x.a, x.b); break;
            case pad::A_LUP:     d2vita_inject("lup", x.a, x.b); break;
            case pad::A_RDOWN:   d2vita_inject("rdown", x.a, x.b); break;
            case pad::A_RUP:     d2vita_inject("rup", x.a, x.b); break;
            case pad::A_CLICK:   d2vita_inject("click", x.a, x.b); break;
            case pad::A_KEYDOWN: d2vita_inject("keydown", x.a, 0); break;
            case pad::A_KEYUP:   d2vita_inject("keyup", x.a, 0); break;
            case pad::A_KEY:     d2vita_inject("key", x.a, 0); break;
        }
    }
}
// Release everything the scheme holds (radial menu / keyboard opening, leaving the game).
void pad_leave(){
    if (g_scheme && g_scheme_active) {
        pad::Actions a; g_scheme->leave(a); pad_emit(a); g_scheme_active = false;
        g_cx = (float)g_scheme->cx(); g_cy = (float)g_scheme->cy();
    }
    overlay_publish(OverlayPub{});
}
// LEVEL TYPES, not level numbers (see padst::Snapshot::levelType). 1 and 12
// are confirmed on console: Act 1 Town and Act 2 Town. 20/26/29 are the Act
// 3/4/5 towns per LvlTypes.txt and are still unconfirmed here — the padlog
// prints the type on every change, so one trip through Kurast settles them.
// 0 means unreadable, and is treated as town on purpose: mistaking a dungeon
// for a town only costs the aim assist, while the reverse turns every
// shopkeeper into a target.
bool pad_is_town(uint32_t t){ return t == 0 || t == 1 || t == 12 || t == 20 || t == 26 || t == 29; }
bool pad_is_merc(uint32_t cls){ return cls == 271 || cls == 338 || cls == 359 || cls == 560; }

bool parse_act(const char* v, Act* out){
    struct E { const char* n; Act a; };
    static const E T[] = {
        {"lclick",{A_LMB,0}},{"rclick",{A_RMB,0}},{"none",{A_NONE,0}},
        {"alt",{A_KEY,0x12}},{"shift",{A_KEY,0x10}},{"tab",{A_KEY,0x09}},{"automap",{A_KEY,0x09}},
        {"esc",{A_KEY,0x1B}},{"echap",{A_KEY,0x1B}},{"inv",{A_KEY,0x49}},{"perso",{A_KEY,0x43}},
        {"skills",{A_KEY,0x54}},{"quests",{A_KEY,0x51}},{"swap",{A_KEY,0x57}},{"space",{A_KEY,0x20}},
        {"run",{A_KEY,0x52}},{"enter",{A_KEY,0x0D}},
        {"pot1",{A_KEY,0x31}},{"pot2",{A_KEY,0x32}},{"pot3",{A_KEY,0x33}},{"pot4",{A_KEY,0x34}},
        {"f1",{A_KEY,0x70}},{"f2",{A_KEY,0x71}},{"f3",{A_KEY,0x72}},{"f4",{A_KEY,0x73}},
        {"f5",{A_KEY,0x74}},{"f6",{A_KEY,0x75}},{"f7",{A_KEY,0x76}},{"f8",{A_KEY,0x77}} };
    for (const E& t : T) if (!strcasecmp(v,t.n)) { *out=t.a; return true; }
    if (!strncasecmp(v,"vk:",3)) { *out={A_KEY,(int)strtol(v+3,nullptr,0)}; return true; }
    return false;
}
uint32_t name_bit(const char* n){
    struct E { const char* n; uint32_t b; };
    static const E N[] = { {"cross",B_CROSS},{"croix",B_CROSS},{"circle",B_CIR},{"rond",B_CIR},
        {"square",B_SQR},{"carre",B_SQR},{"triangle",B_TRI},
        {"up",B_UP},{"down",B_DOWN},{"left",B_LEFT},{"right",B_RIGHT},
        {"start",B_START},{"select",B_SELECT} };
    for (const E& x : N) if (!strcasecmp(n,x.n)) return x.b;
    return 0;
}
void load_controls_txt(){
    FILE* f = fopen("ux0:data/d2vita/controls.txt","r");
    if (!f) { d2vita_progress("input: mapping par defaut"); return; }
    // 1024, not 96: an input script (D2SCRIPT) can run several hundred
    // characters, and silently truncating it would launch a different
    // scenario than the one intended.
    char line[1024]; int n=0;
    while (fgets(line,sizeof line,f)) {
        char* nl=strpbrk(line,"\r\n"); if(nl)*nl=0;
        if(!line[0]||line[0]=='#') continue;
        char* eq=strchr(line,'='); if(!eq||eq==line) continue; *eq=0; char* v=eq+1;
        if      (!strcasecmp(line,"orbit"))   { g_orbit=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"sens"))    { g_sens=(float)atof(v); n++; continue; }
        else if (!strcasecmp(line,"deadzone")){ g_dz=(float)atof(v); n++; continue; }
        else if (!strcasecmp(line,"anchor_y")){ g_anchor_y_pm=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"scheme"))   { g_scheme_aim = strcasecmp(v,"mouse")!=0; n++; continue; }
        else if (!strcasecmp(line,"aim"))      { g_padcfg.aim = atoi(v)!=0; n++; continue; }
        else if (!strcasecmp(line,"orbit_min")){ g_padcfg.orbitMin=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"orbit_max")){ g_padcfg.orbitMax=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"range_min")){ g_padcfg.rangeMin=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"range_max")){ g_padcfg.rangeMax=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"cone"))     { g_padcfg.coneDeg=(float)atof(v); n++; continue; }
        else if (!strcasecmp(line,"hover_h"))  { g_padcfg.hoverH=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"hud_h"))    { g_padcfg.hudH=atoi(v); n++; continue; }
        else if (!strcasecmp(line,"reach"))    { g_padcfg.reach=atoi(v); n++; continue; }
        // slot1..slot7 = hostile | ground | corpse -- what that skill slot
        // aims at. Ground so teleport lands where you point instead of on the
        // monster; corpse so the necromancer's corpse skills see the dead,
        // which the hostile filter drops by construction.
        else if (!strncasecmp(line,"slot",4) && line[4]>='1' && line[4]<='7' && !line[5]) {
            const int idx = line[4]-'1';
            if      (!strcasecmp(v,"ground")) g_padcfg.slotKind[idx]=pad::T_GROUND;
            else if (!strcasecmp(v,"corpse")) g_padcfg.slotKind[idx]=pad::T_CORPSE;
            else                              g_padcfg.slotKind[idx]=pad::T_HOSTILE;
            n++; continue;
        }
        bool layer = !strncasecmp(line,"r+",2);
        uint32_t bit = name_bit(layer?line+2:line);
        Act a; if (!bit || !parse_act(v,&a)) continue;
        for (BtnMap& m : g_btn) if (m.bit==bit) { (layer?m.layer:m.base)=a; n++; }
    }
    fclose(f);
    g_padcfg.deadzone = g_dz; g_padcfg.sens = g_sens;
    char m[64]; snprintf(m,sizeof m,"input: controls.txt applique (%d entrees)",n); d2vita_progress(m);
}
void lmb_update(){
    bool want = g_lmb_stick || g_lmb_btn;
    if (want && !g_lmb_sent){ d2vita_inject("ldown",(int)g_cx,(int)g_cy); g_lmb_sent=true; }
    else if (!want && g_lmb_sent){ d2vita_inject("lup",(int)g_cx,(int)g_cy); g_lmb_sent=false; }
}
void do_press(const Act& a){
    if      (a.kind==A_LMB){ g_lmb_btn=true; lmb_update(); }
    else if (a.kind==A_RMB){ if(!g_rmb_sent){ d2vita_inject("rdown",(int)g_cx,(int)g_cy); g_rmb_sent=true; } }
    else if (a.kind==A_KEY)  d2vita_inject("keydown",a.vk,0);
}
void do_release(const Act& a){
    if      (a.kind==A_LMB){ g_lmb_btn=false; lmb_update(); }
    else if (a.kind==A_RMB){ if(g_rmb_sent){ d2vita_inject("rup",(int)g_cx,(int)g_cy); g_rmb_sent=false; } }
    else if (a.kind==A_KEY)  d2vita_inject("keyup",a.vk,0);
}
// Front touch: absolute cursor; a brief still tap = full left click.
// allowMove=false: the cursor position is tracked but no move is injected
// (the aim scheme owns the cursor while a button is held).
void touch_tick(bool allowMove, bool* moved){
    SceTouchData td; memset(&td,0,sizeof td);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT,&td,1)>=0){
        if (td.reportNum>0){
            int tx=td.report[0].x*g_game_w/1920, ty=td.report[0].y*g_game_h/1088;
            if (g_t_at<0){ g_t_at=g_itick; g_t_x0=tx; g_t_y0=ty; g_t_moved=false; }
            if (std::abs(tx-g_t_x0)>10||std::abs(ty-g_t_y0)>10) g_t_moved=true;
            g_t_x=tx; g_t_y=ty;
            if (allowMove){ g_cx=(float)tx; g_cy=(float)ty; *moved=true; }
        } else if (g_t_at>=0){
            if (g_itick-g_t_at<g_tap_ticks && !g_t_moved) d2vita_inject("click",g_t_x,g_t_y);
            g_t_at=-1;
        }
    }
}
// Scheme v2 tick. Returns true when the scheme handled the tick (in game);
// false = out of game, the legacy path runs (menus, character select).
bool aim_tick(const SceCtrlData& cd, uint32_t b){
    padst::Snapshot s;
    if (!padst::read(s)) return false;
    if (s.frame == g_pad_lastFrame) { if (g_pad_stale < 1000) ++g_pad_stale; }
    else { g_pad_stale = 0; g_pad_lastFrame = s.frame; }
    const bool inGame = s.inGame && g_pad_stale < 20;          // camera hook silent ~0.7 s = not in game
    if (!inGame) { pad_leave(); return false; }
    if (!g_scheme) g_scheme = new pad::Scheme(g_padcfg);
    g_scheme_active = true;

    pad::View v; v.w = g_game_w; v.h = g_game_h;
    v.playerFx = s.playerFx; v.playerFy = s.playerFy; v.viewX = s.viewX; v.viewY = s.viewY;
    pad::Ctx x; x.inGame = true;
    static const int panels[] = { 1, 2, 4, 8, 9, 0x0C, 0x14, 0x19, 0x1A, 0x21, 0x24 };
    for (int p : panels) if (s.uiVars[p]) x.panelOpen = true;
    x.skillTree = s.uiVars[4] != 0;
    x.selValid = s.selValid; x.selId = s.selId; x.selType = s.selType;
    const bool town = pad_is_town(s.levelType);

    static pad::Unit units[padst::MAX_UNITS]; int n = 0;
    // Raw UnitAny+0xC4 kept alongside, for the diagnostic only: `units` is
    // filtered and compacted, so its indices do NOT line up with s.units.
    static uint32_t rawFlags[padst::MAX_UNITS];
    static const padst::Unit* rawUnit[padst::MAX_UNITS];
    for (int i = 0; i < s.nUnits && n < padst::MAX_UNITS; i++) {
        const padst::Unit& q = s.units[i];
        // Never box/target ourselves -- EXCEPT once we are a corpse. Getting
        // our gear back is the whole point, and we do not know yet whether
        // the body reuses the player's unit id or gets one of its own, so
        // handle both: skip the LIVING player only. Modes 0/12/17 are the
        // death and dead animations; the padlog prints the mode of every
        // type-0 unit it sees so the real one can be confirmed.
        const bool corpseMode = (q.mode == 0 || q.mode == 12 || q.mode == 17);
        if (q.type == 0 && q.id == s.playerId && !corpseMode) continue;
        if (q.type == 3 || q.type == 5) continue;                     // missiles, tiles
        pad::Unit& o = units[n]; o = pad::Unit{};
        o.id = q.id; o.type = q.type; o.cls = q.cls;
        pad::world_to_screen(v, q.fx, q.fy, &o.sx, &o.sy);
        if (q.type == 1) {
            const bool alive = q.mode != 0 && q.mode != 12;             // 0 = dying, 12 = dead
            const bool ours  = q.ownerType == 0 && q.ownerId != 0 && q.ownerId == s.playerId;   // ownerType 0 = owned by a PLAYER
            o.hostile  = alive && !town && !ours && !pad_is_merc(q.cls);
            // Town NPCs, filtered by the same targetable bit. The console log
            // of 21/09 split Lut Gholein's type-1 units cleanly in two:
            // classes 175/199/201/202/331 carry bit 1 (real, clickable NPCs)
            // and 195/196/203 do not (decorative passers-by). Offering the
            // latter is what made some townspeople impossible to click.
            // NOT applied to `hostile` above: no combat evidence yet, and
            // being wrong about a monster costs the whole fight.
            o.interact = alive && town && (q.flags & 0x00200002u) == 0x00000002u;
            o.selectable = o.interact;
            // A corpse is a target in its own right (corpse explosion,
            // revive, raise skeleton). OUR OWN summons' corpses count too --
            // the game lets you explode those as readily as any other.
            o.corpse   = !alive && !town;
        } else if (q.type == 0) {
            // A body on the ground. Gated on the same targetable bit as
            // everything else, so a living player standing next to us in a
            // multiplayer game is not mistaken for loot.
            o.interact   = corpseMode && (q.flags & 0x00200002u) == 0x00000002u;
            o.selectable = o.interact;
            o.ownCorpse  = o.interact;
        } else if (q.type == 2) {
            // Ask the GAME whether this object can be hovered at all, instead
            // of offering every torch and shadow and learning the hard way.
            // UNITFLAG_TARGETABLE, maintained from ObjectTxt.Selectable for
            // the object's current mode -- which is also why a looted chest
            // stops being offered once it is open.
            // Game+0x66870 requires BOTH: bit 1 set, and bit 21 clear. Bit 21
            // is a suppression bit whose setter was not located, so it is
            // mirrored rather than assumed clear -- one extra AND for a
            // condition the game really does test.
            o.interact   = true;                       // reachable by pointing at it
            o.selectable = (q.flags & 0x00200002u) == 0x00000002u;   // offered by proximity
        }
        else if (q.type == 4) { o.interact = true; o.selectable = true; }   // items: browsed with Alt
        rawFlags[n] = q.flags; rawUnit[n] = &q;
        if (q.type == 4) {
            // Exact label rect, keyed by unit id -- no proximity, no geometry.
            for (int j = 0; j < s.nLabels; j++) {
                if (s.labels[j].unitId != q.id) continue;
                const padst::Label& L = s.labels[j];
                o.hasLabel = true;
                o.lx = (L.x1 + L.x2) / 2; o.ly = (L.y1 + L.y2) / 2;
                o.lw = L.x2 - L.x1;       o.lh = L.y2 - L.y1;
                break;
            }
        }
        ++n;
    }
    pad::Ctl c;
    c.lx = (cd.lx - 128) / 128.f; c.ly = (cd.ly - 128) / 128.f;
    c.rx = (cd.rx - 128) / 128.f; c.ry = (cd.ry - 128) / 128.f;
    c.buttons = b & ~(uint32_t)pad::B_SELECT;                          // Select = radial menu, handled before us

    pad::Actions a; g_scheme->tick(c, x, v, units, n, a); pad_emit(a);
    g_cx = (float)g_scheme->cx(); g_cy = (float)g_scheme->cy();
    const pad::Target t = g_scheme->target();
    const pad::Target lt = g_scheme->lootCursor();
    overlay_publish(OverlayPub{ t.has, (int)t.verified, t.sx, t.sy,
                                lt.has, lt.sx, lt.sy, lt.w, lt.h });

    bool moved = false;
    touch_tick(!g_scheme->cursorOwned(), &moved);
    if (moved) { d2vita_inject("move", (int)g_cx, (int)g_cy); g_scheme->setCursor((int)g_cx, (int)g_cy); }

    if (g_padlog) {                                                    // capped diagnostics (boot_progress.txt)
        // Kept because each one reproduces a class of bug we can expect again.
        // The investigation-specific dumps that used to live here are gone with
        // the questions they answered: the player's fine position (pPath+0/+4),
        // the level number (Level+0x1C0), and the ground-label geometry, which
        // is no longer computed at all -- the game's own table is read instead.
        static int lines = 0; static uint32_t lastTgt = 0; static uint32_t lastUi[38] = {0};
        static uint32_t lastLvl = 0xffffffffu, lastLvlPtr = 0xffffffffu;
        char m[160];
        if (lines < 2000) {
            // Whether we think we are in town decides whether every NPC is a
            // TARGET or something to talk to, so a level number we misread
            // turns a town inside out. One line per level change answers it.
            if (s.levelType != lastLvl || s.levelPtr != lastLvlPtr) {
                lastLvl = s.levelType; lastLvlPtr = s.levelPtr;
                int nh = 0, ni = 0; for (int i = 0; i < n; i++) { if (units[i].hostile) ++nh; if (units[i].interact) ++ni; }
                snprintf(m, sizeof m, "pad: type_niveau=%u lvptr=%08x ville=%d unites=%d hostiles=%d interactifs=%d",
                         s.levelType, s.levelPtr, (int)town, n, nh, ni);
                d2vita_progress(m); ++lines; }
            // UiVar indices are magic numbers in panels[]: this is the only way
            // to find the one behind "panel X is not detected".
            for (int i = 0; i < 38 && lines < 2000; i++) if (s.uiVars[i] != lastUi[i]) { lastUi[i] = s.uiVars[i];
                snprintf(m, sizeof m, "pad: uivar[%d]=%u", i, s.uiVars[i]); d2vita_progress(m); ++lines; }
            // Which rule Cross applied, every time it changes. The branch is
            // what turns "we are not hitting the nearest mob" into a fact.
            {
                const pad::Scheme::Pick pk = g_scheme->lastPick();
                static uint32_t lastPickId = 0xffffffffu; static int lastBranch = -2;
                if ((pk.id != lastPickId || pk.branch != lastBranch) && lines < 2000) {
                    lastPickId = pk.id; lastBranch = pk.branch;
                    static const char* kWhy[6] = { "cadavre", "sous_curseur", "cone", "objet_proche", "hostile_proche", "rien" };
                    snprintf(m, sizeof m, "pad: croix -> id=%u type=%u dist=%d via=%s",
                             pk.id, pk.type, pk.dist, kWhy[pk.branch < 0 ? 5 : pk.branch]);
                    d2vita_progress(m); ++lines;
                }
            }
            // One line a second while the left stick is pushed: did the walk
            // click go out at all, and did it land on a monster? A left click
            // on a monster is ATTACK, not move, which is the leading theory
            // for "stuck in a group" -- but it is a theory, so measure it.
            {
                const pad::Scheme::Walk wk = g_scheme->walk();
                static uint64_t lastWalkLog = 0;
                const uint64_t now = d2vita_now_us();
                if (wk.pushed && now - lastWalkLog > 1000000ull && lines < 2000) {
                    lastWalkLog = now;
                    snprintf(m, sizeof m, "pad: marche ferme=%d clic=%d sur_unite=%d pt=(%d,%d) unites=%d",
                             (int)wk.gated, (int)wk.clicked, (int)wk.onUnit, wk.px, wk.py, wk.nUnits);
                    d2vita_progress(m); ++lines;
                }
            }
            // Every distinct (mode) a type-0 unit is seen in: which one a
            // player corpse actually uses, and whether it keeps the player's
            // unit id, is the thing that decides whether Cross can ever
            // reach it.
            {
                static uint32_t seenP[16]; static int nSeenP = 0;
                for (int i = 0; i < s.nUnits && lines < 2000; i++) {
                    if (s.units[i].type != 0) continue;
                    const uint32_t key = (s.units[i].mode << 1) | (s.units[i].id == s.playerId ? 1u : 0u);
                    bool known = false;
                    for (int k = 0; k < nSeenP; k++) if (seenP[k] == key) { known = true; break; }
                    if (known || nSeenP >= 16) continue;
                    seenP[nSeenP++] = key;
                    snprintf(m, sizeof m, "pad: joueur id=%u %s mode=%u drapeaux=%08x ciblable=%d",
                             s.units[i].id, s.units[i].id == s.playerId ? "MOI" : "autre",
                             s.units[i].mode, s.units[i].flags,
                             (int)((s.units[i].flags & 0x00200002u) == 2u));
                    d2vita_progress(m); ++lines;
                }
            }
            // One line the first time each kind of unit is seen, so a console
            // run says outright whether the targetable bit tracks what the
            // game actually lets the cursor hover: a chest should read 1 and
            // a torch 0, with the same class reading the same way every time.
            {
                static uint32_t seen[48]; static int nSeen = 0;
                for (int i = 0; i < n && lines < 2000; i++) {
                    const uint32_t key = (units[i].type << 16) | (units[i].cls & 0xffffu);
                    bool known = false;
                    for (int k = 0; k < nSeen; k++) if (seen[k] == key) { known = true; break; }
                    if (known || nSeen >= 48) continue;
                    seen[nSeen++] = key;
                    snprintf(m, sizeof m, "pad: vu type=%u cls=%u drapeaux=%08x ciblable=%d interactif=%d nom=%s accord=%d",
                             units[i].type, units[i].cls, rawFlags[i],
                             (int)((rawFlags[i] & 0x00200002u) == 2u), (int)units[i].interact,
                             units[i].type == 2 ? rawUnit[i]->objName : "-",
                             units[i].type == 2 ? (int)rawUnit[i]->txtAgree : -1);
                    d2vita_progress(m); ++lines;
                }
            }
            // The only window onto the HoverTable's learn/verify loop.
            if (t.id != lastTgt) { lastTgt = t.id;
                int ti = -1; for (int i = 0; i < n; i++) if (units[i].id == t.id) ti = i;
                snprintf(m, sizeof m, "pad: cible id=%u type=%u cls=%u ecran=(%d,%d) verif=%d sel=(%u,%u,%u)", t.id,
                         ti >= 0 ? units[ti].type : 0u, ti >= 0 ? units[ti].cls : 0u, t.sx, t.sy, (int)t.verified, s.selValid, s.selId, s.selType);
                d2vita_progress(m); ++lines; }
        }
    }
    return true;
}
// Ouverture du clavier (automatique ou R+Triangle), D2_KBSIMPLE lu une fois.
void kb_open_now(){
    if (g_kb_simple < 0){ const char* e=getenv("D2_KBSIMPLE"); g_kb_simple = (e&&*e&&strcmp(e,"0"))?1:0; }
    d2kb::open_kb(g_kb, g_kb_simple);
}
} // namespace

extern "C" void d2vita_input_tick(void){
    if (!g_ctl_init){
        g_ctl_init=true;
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        load_controls_txt();
        if (const char* e = getenv("D2_PAD")) g_scheme_aim = (*e && *e != '0');     // env wins over controls.txt
        if (!padst::on()) g_scheme_aim = false;                                         // hooks unarmed: nothing to read
        g_padcfg.deadzone = g_dz; g_padcfg.sens = g_sens;
        g_padlog = getenv("D2_PADLOG") ? 1 : 0;
        d2vita_progress(g_scheme_aim ? "input: schema v2 (visee assistee) actif" : "input: schema souris (legacy)");
        g_cx=g_game_w*0.5f; g_cy=g_game_h*0.5f;
        // Tick rate — D2_INPUTHZ, default 30.
        // Why it's capped: this tick makes TWO syscalls (sceCtrlPeek +
        // sceTouchPeek) and used to run at ~45-60 Hz (once per frame plus the
        // message pump, 16 ms gate) for a game that renders 12 frames per
        // second. It runs on USER_0, the core ALL guest threads are pinned
        // to — squarely on the critical path.
        // FLOOR AT 20 Hz, NOT NEGOTIABLE. This tick is the ONLY controller
        // sampling on a screen D2 has stopped redrawing — it's what makes the
        // pause menu's buttons actionable (see the PeekMessageA shim in
        // rt_boot.cpp for the full reason). Below 20 Hz, button edges get
        // missed and the original problem this fixed comes back.
        { const char* e=getenv("D2_INPUTHZ"); g_in_hz = e?atoi(e):30;
          if (g_in_hz < 20)  g_in_hz = 20;
          if (g_in_hz > 240) g_in_hz = 240;
          g_tap_ticks = g_in_hz*15/60; if (g_tap_ticks < 2) g_tap_ticks = 2;
          char m[80]; snprintf(m,sizeof m,"input: cadence %d Hz (tap<%d ticks)",
                               g_in_hz,g_tap_ticks); d2vita_progress(m); }
        d2vita_progress("input: controles physiques actifs");
    }
    // The rate limiter comes BEFORE any syscall: it's the syscall cost itself
    // that needs to be cut, not just the processing that follows it.
    { static uint64_t lastUs=0; const uint64_t now=d2vita_now_us();
      const uint64_t period = 1000000ull/(uint64_t)(g_in_hz>0?g_in_hz:30);
      if (lastUs && now-lastUs < period) return;
      lastUs = now; }
    ++g_itick;
    SceCtrlData cd; memset(&cd,0,sizeof cd);
    if (sceCtrlPeekBufferPositive(0,&cd,1)<1) return;
    { uint32_t vb = 0; uint8_t va[4]; d2vita_vpad_get(&vb, va);
      cd.buttons |= vb;
      if (va[0] != 128 || va[1] != 128) { cd.lx = va[0]; cd.ly = va[1]; }
      if (va[2] != 128 || va[3] != 128) { cd.rx = va[2]; cd.ry = va[3]; } }
    const uint32_t b=cd.buttons, was=g_ctl_prev; g_ctl_prev=b;
    static int ilog=-1; if(ilog<0) ilog=getenv("D2_INPUTLOG")?1:0;
    // Autopilot (baked D2SCRIPT): cuts out on the FIRST physical action — the
    // script drives all the way into the game if nothing is touched
    // (headless tests), and steps aside as soon as the player takes over.
    static bool script_cut=false;
    if (!script_cut){
        float alx=(cd.lx-128)/128.f, aly=(cd.ly-128)/128.f;
        bool touching=false;
        { SceTouchData tq; memset(&tq,0,sizeof tq);
          if (sceTouchPeek(SCE_TOUCH_PORT_FRONT,&tq,1)>=0 && tq.reportNum>0) touching=true; }
        if (b || touching || alx*alx+aly*aly > g_dz*g_dz){
            script_cut=true; d2vita_inject("scriptoff",0,0);
            d2vita_progress("input: autopilote coupe (prise en main)"); }
    }
    if (ilog && b!=was){ char m[96];
        snprintf(m,sizeof m,"input: buttons=%08x lx=%d ly=%d rx=%d ry=%d",(unsigned)b,cd.lx,cd.ly,cd.rx,cd.ry);
        d2vita_progress(m); }
    // ---- FREEZE MARKER: L + UP ARROW ---------------------------------------
    // The player flags a slowdown; a timestamped line is logged, and analysis
    // looks at the 15 s BEFORE it.
    //
    // Why not the up arrow alone: it's already bound to potion 1 (see the
    // table above). An earlier version marked without intercepting, so the
    // potion would still fire — but every potion drink then logged a FALSE
    // marker too, whether or not anything was actually wrong. A marker that
    // fires on its own is no better than no marker.
    // Intercepted outright, so no lost potion and no spurious marker.
    // L + LEFT, not L + Up: Up is the weapon swap now. Left is free again
    // since the right click moved to L + Down.
    if ((b&B_LEFT) && !(was&B_LEFT) && (b&B_L) && lagmark_on()) {
        static uint64_t lastMark = 0; static unsigned nMark = 0;
        const uint64_t now = d2vita_now_us();
        if (!lastMark || now - lastMark > 1000000ull) {
            lastMark = now; ++nMark;
            char m[128];
            snprintf(m, sizeof m,
                     ">>> MARQUE #%u — ralentissement signale par le joueur "
                     "(il vient de se produire, regarder les secondes PRECEDENTES)",
                     nMark);
            d2vita_progress(m);
        }
        return;                     // consomme : ni potion, ni autre effet
    }
    // ---- L + BAS : marche/course (toggle) ----------------------------------
    // VK 0x52 ('R') est deja la cible de l'entree "run" du parseur controls.txt
    // (parse_act ci-dessous) : meme touche D2 native, juste assignee par defaut
    // ici. Tir simple (tap), pas maintenu : c'est un toggle d'etat cote jeu.
    // Bas seul = potion 2 (voir g_btn/D-pad plus bas) : meme raison qu'au-dessus,
    // on intercepte sur le front descendant et on consomme pour ne pas boire.
    // !(b&B_R) : R+L maintenus = mode Alt, ou le D-pad promene le curseur de
    // butin d'un objet a l'autre. Sans ce test, R+L+Bas basculait la course et
    // consommait l'appui : la navigation vers le bas n'atteignait jamais
    // aim_tick — or c'est justement l'axe qui compte, deux objets sur la meme
    // tuile empilant leurs etiquettes verticalement.
    // Schema v2 : c'est une PRESSION BREVE sur L seul qui bascule la course
    // (pad_core, kTapTicks), L maintenu valant Maj « sur place ». Ce raccourci
    // ne sert donc plus qu'au schema souris (legacy).
    if (!g_scheme_aim && (b&B_DOWN) && !(was&B_DOWN) && (b&B_L) && !(b&B_R)) {
        d2vita_inject("key", 0x52, 0);
        return;                     // consomme : pas de potion 2 non plus
    }
    const bool layer=(b&B_R)!=0;
    bool moved=false;

    // L + Start = on-demand screenshot (ux0:data/d2vita/shot_<frame>.bmp) —
    // for capturing a rendering defect the test bench can't reproduce on its own.
    if ((b&B_START)&&!(was&B_START)&&(b&B_L)){ if(d2gxm_shot_request) d2gxm_shot_request(); return; }   // L+Start: screenshot
    // Ouverture automatique quand un champ texte prend le focus (sonde
    // text_focus_probe.cpp). La fermeture reste manuelle, d'ou le verrou : le
    // dernier focus vu suit la sonde meme vers 0, une fermeture au Select ne
    // rouvre rien, un retour au meme champ (focus passe par 0) est un nouveau
    // front. Jamais sous l'autopilote : les bancs tapent le nom du perso par
    // D2SCRIPT sans manette, le clavier resterait dessine sur toute la partie.
    if (script_cut){
        const uint32_t f = d2vita_text_focus();
        const bool rise = f != 0 && f != g_kb_last_focus;
        g_kb_last_focus = f;
        if (rise && !g_kb.open){ kb_open_now(); d2vita_progress("clavier: ouverture automatique"); return; }
    }
    // R+Triangle OPENS the virtual keyboard (the radial menu's own
    // "keyboard" sector maps to "character"/C instead). Triangle ALONE stays
    // W (weapon swap, generic table below). Only handles the OPEN edge: once
    // open, Triangle (without R) reverts to its keyboard-internal role
    // (toggle echo masking, see the g_kb.open block below) — closing stays on
    // Select.
    // Schema v2 : L + Droite (R+Triangle y est la compétence 7). Legacy :
    // R+Triangle, inchange. Dans les deux cas on consomme l'appui, sinon la
    // Droite partirait aussi en potion 4 de ceinture.
    const bool kb_open_edge = g_scheme_aim
        ? ((b&B_RIGHT) && !(was&B_RIGHT) && (b&B_L) && !(b&B_R))
        : ((b&B_TRI)   && !(was&B_TRI)   && layer);
    if (kb_open_edge && !g_kb.open){
        pad_leave();
        kb_open_now();
        return;
    }
    if (g_kb.open){
        g_lmb_stick=false; g_lmb_btn=false; lmb_update();       // release any held click
        auto edge=[&](uint32_t bit){ return (b&bit)&&!(was&bit); };
        // NEVER LOG THE TYPED CHARACTER, HERE OR ANYWHERE ELSE: this keyboard
        // is used to enter a Battle.net password. The only place the text
        // exists is g_kb's local echo, cleared on close.
        auto emit=[&](d2kb::Act a, char ch){
            switch(a){
                case d2kb::ACT_CHAR:  d2vita_inject("chr",(int)(unsigned char)ch,0); break;
                case d2kb::ACT_SPACE: d2vita_inject("chr",32,0); break;
                case d2kb::ACT_BACK:  d2vita_inject("key",0x08,0); break;
                case d2kb::ACT_ENTER: d2vita_inject("key",0x0D,0); break;
                default: break;                                  // MAJ/MASQUE/FERMER : etat local
            } };
        auto press=[&](int r,int c){ char ch=0; emit(d2kb::activate(g_kb,r,c,&ch), ch); };
        if (edge(B_UP))    d2kb::nav(g_kb,-1,0);
        if (edge(B_DOWN))  d2kb::nav(g_kb, 1,0);
        if (edge(B_LEFT))  d2kb::nav(g_kb, 0,-1);
        if (edge(B_RIGHT)) d2kb::nav(g_kb, 0, 1);
        if (edge(B_SQR))  { d2kb::shift_cycle(g_kb); }           // Square = shift (3 states)
        if (edge(B_TRI))  { d2kb::mask_toggle(g_kb); }           // Tri    = toggle echo mask
        if (edge(B_CIR))  { d2kb::echo_pop(g_kb); d2vita_inject("key",0x08,0); }   // Circle = backspace
        if (edge(B_START)){ d2kb::echo_clear(g_kb); d2vita_inject("key",0x0D,0); } // Start = Enter
        if (edge(B_SELECT)){ d2kb::close_kb(g_kb); return; }     // Select = close
        if (edge(B_CROSS)) press(g_kb.r, g_kb.c);
        SceTouchData tk; memset(&tk,0,sizeof tk);                // tap sur une touche
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT,&tk,1)>=0){
            if (tk.reportNum>0){ if(g_t_at<0){ g_t_at=g_itick; g_t_moved=false; }
                // Le clavier vit en pixels ÉCRAN (pas dans la fenêtre de jeu) :
                // pas de bandes à retirer ici, seulement l'échelle du pavé.
                g_t_x=tk.report[0].x*SCR_W/1920; g_t_y=tk.report[0].y*SCR_H/1088; }
            else if (g_t_at>=0){
                int r=0,c=0;
                if (g_itick-g_t_at<g_tap_ticks &&
                    d2kb::hit(d2kb::layout(g_kb), SCR_W, SCR_H, g_t_x, g_t_y, &r, &c)){
                    g_kb.r=r; g_kb.c=c; press(r,c); }
                g_t_at=-1; }
        }
        return;
    }

    // Select opens the radial menu IMMEDIATELY (no "tap = I" fallback: the
    // radial menu's 7 functions fully replace the old individual controls).
    // R+Select = Space, unchanged.
    {
        const bool selDown=(b&B_SELECT)!=0, selWas=(was&B_SELECT)!=0;
        if (selDown && !selWas) {
            if (layer) { g_sel_mode=SEL_SPACE; d2vita_inject("keydown",0x20,0); }
            // Le menu n'existe QUE sur le GPU. S'il n'est pas armé (.gxp
            // absent, atlas non alloué), ne pas l'ouvrir : un menu invisible
            // qui avale quand même les entrées serait pire que pas de menu.
            else if (d2gxm_ui_active()) { g_sel_mode=SEL_RADIAL; pad_leave(); radial_menu::begin(g_rm); }
        }
        if (g_sel_mode==SEL_RADIAL) {
            g_lmb_stick=false; g_lmb_btn=false; lmb_update();      // release any click in progress
            float slx=(cd.lx-128)/128.f, sly=(cd.ly-128)/128.f;
            radial_menu::tick(g_rm, slx, sly);
        }
        if (!selDown && selWas) {
            if      (g_sel_mode==SEL_SPACE)  d2vita_inject("keyup",0x20,0);
            else if (g_sel_mode==SEL_RADIAL) {
                const int idx = radial_menu::resolved_index(g_rm);
                radial_menu::end(g_rm);
                if (idx>=0) d2vita_inject("key", g_rm_slots[idx].vk, 0);
            }
            g_sel_mode=SEL_IDLE;
        }
        if (g_sel_mode==SEL_RADIAL) return;   // menu open: nothing else passes through to the game
    }
    // Scheme v2 owns everything below while in game; out of game the legacy
    // scheme (menus, character select, autopilot hand-over) runs unchanged.
    if (g_scheme_aim && aim_tick(cd, b)) return;

    // L: left click (held); R: right click (held). Independent of `layer`: R
    // keeps its OWN click while still arming the combo layer for the OTHER
    // buttons (Triangle, D-pad, Select) — R's two effects coexist without
    // conflict since `layer` is re-read every tick, never consumed by this
    // block.
    if ((b&B_L)&&!(was&B_L)){ g_lmb_btn=true; lmb_update(); }
    if (!(b&B_L)&&(was&B_L)){ g_lmb_btn=false; lmb_update(); }
    if ((b&B_R)&&!(was&B_R)){ if(!g_rmb_sent){ d2vita_inject("rdown",(int)g_cx,(int)g_cy); g_rmb_sent=true; } }
    if (!(b&B_R)&&(was&B_R)){ if(g_rmb_sent){ d2vita_inject("rup",(int)g_cx,(int)g_cy); g_rmb_sent=false; } }

    // Mapped buttons (edges, R layer sampled at the moment of press)
    for (size_t i=0;i<sizeof g_btn/sizeof*g_btn;i++){
        bool now=(b&g_btn[i].bit)!=0, before=(was&g_btn[i].bit)!=0;
        if (now&&!before){ g_engaged[i]= layer?g_btn[i].layer:g_btn[i].base; do_press(g_engaged[i]); }
        else if (!now&&before){ do_release(g_engaged[i]); g_engaged[i]={A_NONE,0}; }
    }

    // Left stick: DIRECT MOVEMENT (orbit + held click)
    float lx=(cd.lx-128)/128.f, ly=(cd.ly-128)/128.f;
    float lm=std::sqrt(lx*lx+ly*ly);
    static bool dm=false;
    if (!dm && lm>g_dz+0.05f) dm=true; else if (dm && lm<g_dz) dm=false;
    if (dm){
        float r=g_orbit*(g_game_h/600.f);
        g_cx = g_game_w*0.5f + lx*r;
        g_cy = g_game_h*(g_anchor_y_pm/1000.f) + ly*r;
        moved=true;
        g_lmb_stick=true;
    } else g_lmb_stick=false;

    // Right stick: free mouse, NO click (inactive during direct movement)
    if (!dm){
        float rx=(cd.rx-128)/128.f, ry=(cd.ry-128)/128.f;
        if (std::fabs(rx)>g_dz || std::fabs(ry)>g_dz){
            float sc=g_sens*(g_game_w/800.f);
            g_cx += rx*std::fabs(rx)*sc; g_cy += ry*std::fabs(ry)*sc; moved=true; }
    }

    touch_tick(true, &moved);

    if (g_cx<0)g_cx=0; if (g_cx>g_game_w-1)g_cx=(float)(g_game_w-1);
    if (g_cy<0)g_cy=0; if (g_cy>g_game_h-1)g_cy=(float)(g_game_h-1);
    if (moved) d2vita_inject("move",(int)g_cx,(int)g_cy);
    lmb_update();
}


// Wall-clock UNIX time via SceRtc: newlib's time() returns 0 under Vita3K
// (epoch 1970), which fed D2's save/date paths a degenerate value. SceRtc
// ticks are microseconds since 0001-01-01 UTC.
extern "C" long long d2vita_wall_unix(void) {
    SceRtcTick t;
    if (sceRtcGetCurrentTick(&t) < 0) return 0;
    long long unixsec = (long long)(t.tick / 1000000ull) - 62135596800ll;
    return unixsec > 0 ? unixsec : 0;
}

// System language -> Windows LCID, so SID_AUTH_INFO reports the console's REAL
// language (a French Vita -> French, an English Vita -> English). Reading system
// params needs SceAppUtil loaded+inited; any failure falls back to en-US.
extern "C" uint32_t d2vita_sys_lcid(void) {
    static uint32_t cached = 0; if (cached) return cached;
    static bool inited = false;
    if (!inited) { sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
        SceAppUtilInitParam ip; SceAppUtilBootParam bp;
        memset(&ip, 0, sizeof ip); memset(&bp, 0, sizeof bp);
        sceAppUtilInit(&ip, &bp); inited = true; }
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) < 0)
        lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    uint32_t lc;
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:       lc = 0x0411; break;
        case SCE_SYSTEM_PARAM_LANG_FRENCH:         lc = 0x040C; break;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:        lc = 0x0C0A; break;
        case SCE_SYSTEM_PARAM_LANG_GERMAN:         lc = 0x0407; break;
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:        lc = 0x0410; break;
        case SCE_SYSTEM_PARAM_LANG_DUTCH:          lc = 0x0413; break;
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:  lc = 0x0816; break;
        case SCE_SYSTEM_PARAM_LANG_RUSSIAN:        lc = 0x0419; break;
        case SCE_SYSTEM_PARAM_LANG_KOREAN:         lc = 0x0412; break;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T:      lc = 0x0404; break;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S:      lc = 0x0804; break;
        case SCE_SYSTEM_PARAM_LANG_FINNISH:        lc = 0x040B; break;
        case SCE_SYSTEM_PARAM_LANG_SWEDISH:        lc = 0x041D; break;
        case SCE_SYSTEM_PARAM_LANG_DANISH:         lc = 0x0406; break;
        case SCE_SYSTEM_PARAM_LANG_NORWEGIAN:      lc = 0x0414; break;
        case SCE_SYSTEM_PARAM_LANG_POLISH:         lc = 0x0415; break;
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR:  lc = 0x0416; break;
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB:     lc = 0x0809; break;
        case SCE_SYSTEM_PARAM_LANG_TURKISH:        lc = 0x041F; break;
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_US:
        default:                                   lc = 0x0409; break;
    }
    cached = lc; return lc;
}

// Timezone bias in minutes (Windows sense: UTC = local + Bias). Derived from the
// console's own clock (local vs UTC), so the reported offset is the user's real one.
extern "C" int32_t d2vita_sys_tzbias(void) {
    SceRtcTick utc, local;
    if (sceRtcGetCurrentTick(&utc) < 0) return 0;
    if (sceRtcConvertUtcToLocalTime(&utc, &local) < 0) return 0;
    long long diff_us = (long long)utc.tick - (long long)local.tick;   // Bias = UTC - local
    return (int32_t)(diff_us / 1000000 / 60);
}
#endif // __vita__
