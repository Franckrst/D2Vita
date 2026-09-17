// src/platform/vita_gxm.h — Glide ring rendering on the Vita GPU (sceGxm).
//
// KNOB: D2_GLIDEGXM=1. When unset (default), nothing is initialized, the ring
// flush stays a plain counting sink, and not a byte of the shipped path
// changes. The knob is Vita-only: off-target, every entry point below is an
// empty inline function, so the (portable) batch builder in
// src/glide_ring/glide_atlas.h still runs under qemu with all its counters
// measurable — only GPU submission is missing.
//
// READ BEFORE TOUCHING THIS FILE: this backend implements ONLY the Glide
// states D2 1.14d actually emits, deliberately — a shader that covers all of
// Glide is one nobody could validate or maintain. Pipelined submission
// (D2_GXMASYNC=1, see the ASYNC SUBMISSION section below) duplicates or
// guards state to stay safe while hiding GPU wait time.
#pragma once
#include <cstdint>
#include "glide_ring/glide_atlas.h"

#ifdef __vita__

// Enabled? (reads the knob once)
bool d2gxm_knob();
// Initializes sceGxm, shaders, and buffers. Returns false on any failure, so
// the caller falls back to the sink, never to a black screen.
bool d2gxm_init(int gameW, int gameH);
bool d2gxm_ready();
// The game window is only known when grSstWinOpen replays, i.e. AFTER init,
// so it's set separately.
void d2gxm_set_window(int w, int h);
// Creates atlas page `page` (dim x dim, 8-bit). Returns 0 on failure, after
// which the atlas stops allocating pages and evicts instead.
int  d2gxm_page_create(int page, int dim);
void d2gxm_page_upload(int page, int x, int y, int w, int h, const uint8_t* src, int srcPitch);
// `slot` in [0, D2GXM_PALETTES). argb256 = 256 words of 0xAARRGGBB.
void d2gxm_palette(int slot, const uint32_t* argb256);
// Submits and presents ONE frame. clearARGB = grBufferClear color.
// `frame` is the caller-side frame number: it timestamps atlas cells, so it's
// also the reference the eviction barrier (d2gxm_busy_from below) must use.
void d2gxm_submit(const d2gr::Vtx* v, uint32_t nv,
                  const uint16_t* idx, uint32_t ni,
                  const d2gr::Batch* b, uint32_t nb, uint32_t clearARGB,
                  uint64_t frame);
// ---- ASYNC SUBMISSION (D2_GXMASYNC=1) --------------------------------------
// Enabled? Read once, like the main knob.
bool d2gxm_async();
// FIRST frame still IN FLIGHT (submitted but not yet presented, so not yet
// GPU-finished). Any atlas cell with `used >= d2gxm_busy_from()` is read by a
// frame the GPU hasn't finished with: overwriting it would produce an
// intermittently wrong sprite. Returns UINT64_MAX when nothing is in flight
// (always the case in synchronous mode).
uint64_t d2gxm_busy_from();
// Drains the queue: waits for all in-flight frames to be presented. Called by
// the atlas when the barrier rules out its last possible eviction — a
// measured wait beats a wrong sprite.
void d2gxm_drain();
// Counter line for the 10 s window. Returns the number of bytes written.
int  d2gxm_counters(char* out, unsigned n);
void d2gxm_shutdown();

#define D2GXM_PALETTES 16

#else   // ---- off Vita: everything is inert, callers stay unchanged ----

static inline bool d2gxm_knob() { return false; }
static inline bool d2gxm_init(int, int) { return false; }
static inline bool d2gxm_ready() { return false; }
static inline void d2gxm_set_window(int, int) {}
static inline int  d2gxm_page_create(int, int) { return 1; }
static inline void d2gxm_page_upload(int, int, int, int, int, const uint8_t*, int) {}
static inline void d2gxm_palette(int, const uint32_t*) {}
static inline void d2gxm_submit(const d2gr::Vtx*, uint32_t, const uint16_t*, uint32_t,
                                const d2gr::Batch*, uint32_t, uint32_t, uint64_t) {}
static inline bool d2gxm_async() { return false; }
static inline uint64_t d2gxm_busy_from() { return ~(uint64_t)0; }
static inline void d2gxm_drain() {}
static inline int  d2gxm_counters(char*, unsigned) { return 0; }
static inline void d2gxm_shutdown() {}
#define D2GXM_PALETTES 16

#endif
