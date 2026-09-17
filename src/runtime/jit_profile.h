// jit_profile.h -- D2_JITPROFILE: where does dynarec time go (PRE-ROGUE cold
// cache vs POST-ROGUE warm cache), plus the D2_SIGNTAG and fastmmu (D2_MMUFOLD
// / D2_MMUSTACK) translation-time censuses. See jit_profile.cpp for the full
// rationale. jpline/jp_fr are general-purpose report helpers used well beyond
// this file (jpline is declared for every extracted unit via runtime/rt_host.h).
#pragma once
#include <cstddef>
#include <cstdint>

void jpline(const char* fmt, ...);   // stdout + d2vita_progress
// Integer fraction "num/den" formatted as N.NN (no floating point in the log).
void jp_fr(char* o, size_t n, uint64_t num, uint64_t den);

void jp_tick(int frame);    // called every frame: detects the Rogue boundary, dumps at window end
void jp_finish(int frame);  // safety net: dumps (marked truncated) if the run stops before jp_tick did
void signtag_dump();        // D2_SIGNTAG translation-time census
void mmu_dump();            // fastmmu (D2_MMUFOLD/D2_MMUSTACK) translation-time census
