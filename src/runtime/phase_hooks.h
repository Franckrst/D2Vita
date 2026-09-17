// phase_hooks.h — phase hooks (D2_PHASEPROF, D2_ONEDRAW, D2_PHASEPROF_X,
// simulation step shared with D2_LOOPWATCH) and ring-tagging hooks
// (D2_REPLAY60 / D2_RINGTAG / D2_RINGPHASE_X). Implementation in phase_hooks.cpp.
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; class Bridge; }

bool pp_on();             // is D2_PHASEPROF set?
int  od_mode();           // D2_ONEDRAW: 0 off, 1 skip-draw, 2 skip-draw + sleep
// Called by rt_boot.cpp: present-path boundary (BitBlt / StretchBlt / ring
// flush) and frame boundary (grBufferSwap, StretchBlt, d2vGlideFlush).
// No-op unless D2_PHASEPROF is set.
void pp_flush_begin();
void pp_flush_end();
void pp_frame();
// Installs hooks (called from main, after [inlinehot], before D2_NOCAP):
//   phase_hooks_install   : simulation step Game+0x12fd90 (LOOPWATCH + PHASEPROF
//                           + ONEDRAW + ring TICK), draw 0x44c990/0x45fde0/
//                           0x460190, frame 0x44efa0, D2_PHASEPROF_X;
//   ringtag_hooks_install : unit Game+0xdc7b0, camera Game+0x5b440, D2_RINGPHASE_X.
void phase_hooks_install(d2rt::Cpu* cpu, d2rt::Bridge& br);
void ringtag_hooks_install(d2rt::Cpu* cpu, d2rt::Bridge& br);
