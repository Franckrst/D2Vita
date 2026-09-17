// tools/rt_dyn86_layout.h — shared fixed memory layout for the Box86-dynarec
// differential PoC (rt_dyn86_ref.cpp on host/Unicorn vs rt_dyn86_d2common.cpp
// on ARM/qemu). Both worlds use IDENTICAL absolute addresses so the 8 final
// GPRs and the arena CRC are directly comparable.
#pragma once
#include <cstdint>

namespace dyn86poc {

constexpr uint32_t IMG_BASE   = 0x10000000;  // D2Common.dll mapped flat here
constexpr uint32_t ARENA_BASE = 0x20000000;  // UNIT/CHILD data arena
constexpr uint32_t ARENA_SIZE = 0x1000;
constexpr uint32_t EXIT_BASE  = 0x21000000;  // exit-bridge stub page (own page:
constexpr uint32_t EXIT_SIZE  = 0x1000;      //  it gets dynarec write-protected)
constexpr uint32_t STACK_BASE = 0x30000000;
constexpr uint32_t STACK_SIZE = 0x10000;
constexpr uint32_t ESP0       = STACK_BASE + 0x8000;

constexpr uint32_t UNIT_ADDR  = ARENA_BASE + 0x000;
constexpr uint32_t CHILD_ADDR = ARENA_BASE + 0x100;
constexpr uint32_t VALUE      = 0xDEADBEEF;
constexpr uint32_t ORDINAL    = 10019;

// Same CRC32 (poly 0xEDB88320) in both binaries.
inline uint32_t crc32buf(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static const char* const GPR_NAMES[8] =
    { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };

} // namespace dyn86poc
