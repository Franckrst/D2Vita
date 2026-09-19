// src/runtime/alloc_site.h — naming the guest call site of a failed allocation.
//
// When a guest HeapAlloc/VirtualAlloc is refused (region exhausted), the size
// alone doesn't say WHICH allocation ran the region dry. The cheap, robust way
// to attribute it — the same one exit_chain() uses for ExitProcess — is to scan
// the guest thread's stack for values that land inside Game.exe's .text and
// treat them as the return-address chain. This header holds only the PURE half
// (format a stack snapshot into "Game+0x<rva>" text) so it can be unit-tested
// off-device; the live-stack read lives in tools/rt_boot.cpp.
#pragma once
#include <cstdint>
#include <string>

namespace d2diag {

// Keeps the dwords of `stack` (n valid entries) that fall in Game.exe's .text
// [lo, hi) — half-open — and renders each as "Game+0x<rva>", rva = value-base,
// space-separated, in the order they appear in `stack`, at most `max` of them.
// Returns "" when nothing matches (or n<=0, or max<=0). Reads no live state.
std::string format_guest_chain(const uint32_t* stack, int n,
                               uint32_t base, uint32_t lo, uint32_t hi, int max);

}  // namespace d2diag
