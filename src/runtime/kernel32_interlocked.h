// kernel32_interlocked.h -- the two remaining D2-specific Interlocked* shims
// (InterlockedCompareExchange/-Pointer -- the generic Interlocked* family
// moved to the engine already) plus a handful of trivial thread/process
// identity one-offs (GetCurrentThreadId/GetCurrentProcess/GetCurrentThread)
// grouped in with them since none is individually worth its own file.
#pragma once
namespace d2rt { class Bridge; }

void kernel32_interlocked_install(d2rt::Bridge& br);
