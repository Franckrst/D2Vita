// src/runtime/scomp_audio.h — see scomp_audio.cpp.
#pragma once
#include <cstdint>
namespace d2rt {
uint32_t audio_huff_decompress(const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outCap);
uint32_t audio_adpcm_decompress(const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outCap, int channels);
}
