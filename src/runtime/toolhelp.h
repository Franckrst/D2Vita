// toolhelp.h -- Toolhelp32 module/thread/process enumeration
// (Module32First/NextW, Process32First/NextW, Thread32First/Next) and the
// canonical x86 CONTEXT writer shared by RtlCaptureContext/GetThreadContext.
// See toolhelp.cpp for the full rationale of each. toolhelp_install()
// registers every KERNEL32 shim built on these helpers (CreateToolhelp32Snapshot,
// Process32First/Next(W), OpenThread, RtlCaptureContext, Thread32First/Next,
// GetThreadContext, Module32First/Next(W), SetUnhandledExceptionFilter).
// fault_note_seh_chain() is the scheduler's fault-dispatcher callback, wired
// from tools/rt_boot.cpp: it records an installed SEH chain and nothing else —
// no exception is ever dispatched to guest code.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace d2rt { struct Cpu; class Bridge; class GuestThread; }

struct M32E { uint32_t base,size; std::string name; };
extern std::vector<M32E> g_mod32list;
extern size_t g_mod32cur;
void mod32_snapshot();
uint32_t mod32_fill(d2rt::Cpu& c, uint32_t p, const M32E& m, bool wide);

uint32_t te32_fill(d2rt::Cpu& c, uint32_t p, uint32_t tid);      // fills one THREADENTRY32
uint32_t pe32_fill(d2rt::Cpu& c, uint32_t p, bool wide);         // fills one PROCESSENTRY32(W)

void ctx_write(d2rt::Cpu& c, uint32_t p, uint32_t edi, uint32_t esi, uint32_t ebx,
        uint32_t edx, uint32_t ecx, uint32_t eax, uint32_t ebp, uint32_t eip,
        uint32_t efl, uint32_t esp);

int fault_note_seh_chain(d2rt::Cpu& c, d2rt::GuestThread* t, uint32_t code);

void toolhelp_install(d2rt::Bridge& br);
