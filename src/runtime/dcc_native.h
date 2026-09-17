// src/runtime/dcc_native.h — native DCC decoder (port of tools/dcc_decode.py,
// itself a faithful port of Paul Siramy's algorithm as Game.exe 1.14d runs it
// at 0x60bff0, Codec.cpp). Produces the "DC6-in-memory" CEL BLOCK CHAIN that
// the game places in the buffer returned by the allocator, one direction at
// a time:
//   block = 0x20 header + RLE data [+0x1c] bytes + 3 bytes 0xEE
//   [+0x00] var0   [+0x04] width  [+0x08] height  [+0x0c] xoff  [+0x10] yoff
//   [+0x14] 0      [+0x18] 0      [+0x1c] CodedBytes (= RLE length)
// No runtime dependency: testable on the host (tools/dcc_native_test.cpp) and
// against the guest decoder under the D2_DCCVERIFY cross-oracle (rt_boot.cpp).
#pragma once
#include <cstdint>
#include <vector>

namespace d2rt {

struct DccResult {
    std::vector<uint8_t>  chain;      // all decoded directions, back to back
    std::vector<uint32_t> dir_off;    // offset of each decoded direction in `chain`
    std::vector<uint32_t> dir_size;   // OutSizeCoded read from the stream, per direction
    // Per cel block: (offset in `chain`, 0x20 + CodedBytes). The game does
    // NOT write a block's 3 trailing bytes (the guest leaves whatever the
    // allocator put there): the copy to the guest and the cross-oracle only
    // cover these segments; `chain` keeps 0xEE 0xEE 0xEE (DC6 format,
    // see tools/dcc_decode.py).
    std::vector<std::pair<uint32_t,uint32_t>> blocks;
    uint32_t frames = 0, cells = 0;   // stats
};

// data/len: the DCC bytes starting at the FIRST requested direction (as the
// game passes them: the file read starts at that direction's offset).
// nframes: frames per direction (DCC header / object +0x18). ndirs:
// consecutive directions to decode. Directions are read IN SEQUENCE (byte
// alignment between them), like 0x60bff0.
// Returns 0 on success, otherwise an error code (>0): out-of-bounds read,
// block sum != OutSizeCoded, nonsensical dimensions... the caller then falls
// back to the guest decoder.
int dcc_decode_native(const uint8_t* data, uint32_t len, uint32_t nframes, uint32_t ndirs,
                      DccResult& out);
const char* dcc_error_name(int code);

} // namespace d2rt
