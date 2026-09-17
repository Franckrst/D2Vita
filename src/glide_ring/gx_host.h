// gx_host.h — host-side GPU path interface (src/glide_ring/gx_host.cpp).
// Only the symbols rt_boot.cpp needs (d2vhost.dll shims, fp_tick, r60 phase
// hooks, final report) are exported; everything else stays static in
// gx_host.cpp. What gx_host.cpp expects from rt_boot.cpp is declared in
// runtime/rt_host.h.
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; }

#define D2GR_MAGIC_H 0x52473244u

// knobs (read once): D2_GLIDEGXM (GXM sink), D2_GRPROF (fine-grained timing)
bool grgx_on();
bool grprof_on();
// Approximate size of a Glide texture upload (GrTexInfo).
uint64_t gl_texbytes(d2rt::Cpu& c, uint32_t info);
// d2vGlideInit: arms the reader (header validated by the caller); returns the
// data VA and ring size for the arm-confirmation log line.
void gr_ring_arm(d2rt::Cpu& c, uint32_t hdr, uint32_t* dataVA, uint32_t* size);
// d2vGlideFlush: D2_GRPROF timing around the flush; `between` (pp_flush_begin)
// runs between the timer start and the replay walk, gr_replay, tail/stalls/dropped.
void gr_flush(d2rt::Cpu& c, uint32_t head, void (*between)());
// d2vGlideNop: timestamp of the last no-op call (round-trip benchmark).
extern uint64_t g_gbT1;
// d2vGlideTexUpload: ring texup/byte counters, then atlas upload.
void gr_tex_upload_count(uint64_t nb);
void gx_tex_upload(d2rt::Cpu& c, uint32_t tmuAddr, uint32_t info);
// Profiling lines (10s window, from fp_tick), D2_GLIDEINV report,
// [ring-final] (end of run), [ring] line of the exit report.
// Flush thread (D2_FLUSHFIL=1): clean shutdown on process exit, window line.
// Silent when the knob is unset (default).
void gr_flush_stop();
void gr_flush_line(uint32_t frames);
void gr_line(uint32_t frames);
void gr_inventory_dump();
void gr_final_line();

// ---- ring side of the D2_REPLAY60 / D2_RINGTAG hooks (installed by rt_boot.cpp) ----
bool gr_emit(d2rt::Cpu& c, uint32_t op, const uint32_t* w, uint32_t n);
struct RPhase { uint32_t rva; uint64_t draws, verts, enters, nested; bool open;
  // D2_CACHEPROBE: hash of the ring records emitted during this phase
  // (abs = raw bytes, rel = x,y in camera space), compared against the
  // previous frame's hash. Tells whether skipping this phase on the guest
  // side would be pixel-neutral.
  uint64_t hA, hR, pA, pR, frames, sameA, sameR, dr, vt;
  uint64_t fr0, sm0, fr1, sm1;   // split: fixed camera / moving camera
 };
extern RPhase g_rph[8]; extern int g_rphN;
extern bool g_r60TagOn, g_r60Active;
extern uint32_t g_r60UiRva;
extern uint64_t g_r60Reent;
