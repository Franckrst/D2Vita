// tools/tests/alloc_site_test.cpp -- host-side tests for src/runtime/
// alloc_site.cpp (naming the guest call site of a failed allocation).
//
// format_guest_chain() is the pure half of the allocation-failure diagnostic:
// given a snapshot of the guest stack (dwords already read from the CPU), it
// keeps the values that fall inside Game.exe's .text [lo,hi) and renders each
// as "Game+0x<rva>", in stack order, capped at `max`. The impure half (reading
// the live stack from the Cpu) lives in tools/rt_boot.cpp and is exercised by
// the qemu-arm boot, not here. This runs on any dev machine.
// Run through tools/oracle_alloc_site.sh.
#include "runtime/alloc_site.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The real compact-layout bounds: Game.exe at 0x01900000, .text scanned over
// [base+0x1000, base+0x2cb5a1) -- the same window exit_chain() uses.
static const uint32_t kBase = 0x01900000;
static const uint32_t kLo   = kBase + 0x1000;
static const uint32_t kHi   = kBase + 0x2cb5a1;

static std::string chain(const std::vector<uint32_t>& v, int max = 8) {
    return d2diag::format_guest_chain(v.data(), (int)v.size(), kBase, kLo, kHi, max);
}

static void t_empty() {
    CHECK(chain({}) == "", "empty stack must give empty string");
    const uint32_t* p = nullptr;
    CHECK(d2diag::format_guest_chain(p, 0, kBase, kLo, kHi, 8) == "",
          "null+0 must be safe and empty");
}

static void t_none_in_range() {
    // Below lo, above hi, and a plausible heap/data pointer -- none are code.
    CHECK(chain({0x50u, kBase, kBase + 0x2cb5a1u, 0xe9452be3u, 0x02450e80u}) == "",
          "no in-range return address -> empty");
}

static void t_filters_and_formats() {
    // base itself is < lo (excluded); two real code addresses; a garbage ptr.
    std::string s = chain({kBase, kBase + 0x1000u, 0x02450e80u, kBase + 0x115fd0u});
    CHECK(s == "Game+0x1000 Game+0x115fd0", "got \"%s\"", s.c_str());
}

static void t_rva_is_addr_minus_base() {
    CHECK(chain({kBase + 0x1234u}) == "Game+0x1234", "rva must be addr-base");
    CHECK(chain({kBase + 0x2cb5a0u}) == "Game+0x2cb5a0", "just under hi is included");
}

static void t_boundaries() {
    CHECK(chain({kLo}) == "Game+0x1000", "v==lo is included");
    CHECK(chain({kHi}) == "", "v==hi is excluded (half-open)");
    CHECK(chain({kLo - 1u}) == "", "one below lo is excluded");
}

static void t_max_cap_and_order() {
    // Five in-range values; max=3 keeps the FIRST three, in stack order.
    std::string s = d2diag::format_guest_chain(
        (std::vector<uint32_t>{kBase + 0xa0u + 0x1000u, kBase + 0xb0u + 0x1000u,
                               kBase + 0xc0u + 0x1000u, kBase + 0xd0u + 0x1000u,
                               kBase + 0xe0u + 0x1000u}).data(),
        5, kBase, kLo, kHi, 3);
    CHECK(s == "Game+0x10a0 Game+0x10b0 Game+0x10c0", "got \"%s\"", s.c_str());
}

static void t_zero_max() {
    CHECK(chain({kBase + 0x1000u}, 0) == "", "max=0 -> empty");
}

int main() {
    t_empty();
    t_none_in_range();
    t_filters_and_formats();
    t_rva_is_addr_minus_base();
    t_boundaries();
    t_max_cap_and_order();
    t_zero_max();
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
