// native_hooks_codec.h -- native D2 1.14d hooks: celwatch (CelData LRU
// evictions), native codec decoders (PKWARE explode x2, huffman/adpcm audio),
// Halt-316 guard (room_coord_guard), native DCC decoder, ARM intrinsics
// checks (memintrin/projectile), Fog snapshot (fog_raise_snap). The
// "nested" group (cell loop/lightgrid/blend/RLE/collision) stays in
// rt_boot.cpp. Declarations for what rt_boot.cpp provides live in
// runtime/rt_host.h.
#pragma once
namespace d2rt { struct Cpu; class Bridge; }

// Called from main() at three points: their relative order, and their
// order versus the d2_cell_blit_114/lightgrid/blend/RLE/collision block left
// in rt_boot.cpp, must stay EXACTLY this, or tools/shim_seq.sh fails
// without ALLOW_REORDER.
void native_hooks_codec_install_celwatch(d2rt::Cpu* cpu, d2rt::Bridge& br);
void native_hooks_codec_install_codecs(d2rt::Cpu* cpu, d2rt::Bridge& br);
void native_hooks_codec_install_post(d2rt::Cpu* cpu, d2rt::Bridge& br, int* framePtr);
