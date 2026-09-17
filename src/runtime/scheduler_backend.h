// scheduler_backend.h -- D2SCHED=native (default) | coop scheduler backend
// selection. The ONLY assignment point for g_sched/g_schedCoop. See
// scheduler_backend.cpp for the full rationale (native is the only
// supported game configuration; coop remains a bench/oracle instrument).
#pragma once
#include <cstdint>
namespace d2rt { struct Cpu; class Bridge; class ThreadScheduler; }

d2rt::ThreadScheduler* make_scheduler(d2rt::Cpu* cpu, d2rt::Bridge* br,
                                       uint32_t stacks, uint32_t stack_each, uint32_t tibs);
