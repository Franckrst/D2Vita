// native_hooks_cellengine.h -- native D2 1.14d hooks for the "nested" group:
// cell blit, light grid, blend, RLE, collision lookup, lightgrid, cell loop
// (counters + NATIVECELLLOOP port), lightblob -- plus the shared fork-join
// engine (D2ParLane, d2_par_start, etc.) and the walkers (d2_room_at,
// d2_light_fill, d2_rle_walk, etc.) these nine hooks depend on. Declarations
// for what rt_boot.cpp provides live in runtime/rt_host.h.
#pragma once
namespace d2rt { struct Cpu; class Bridge; }

// Called from main() at the two points flanking
// native_hooks_codec_install_codecs (which stays in place between them):
// the relative order must stay EXACTLY this, or tools/shim_seq.sh fails
// without ALLOW_REORDER.
void native_hooks_cellengine_install_blit(d2rt::Cpu* cpu, d2rt::Bridge& br);
void native_hooks_cellengine_install_rest(d2rt::Cpu* cpu, d2rt::Bridge& br);
