#pragma once
// Networking stack for the D2Vita runtime.
//
// Three reasons this exists:
//  1. The Sony stack needs its own dedicated memory pool, and it must be
//     initialized BEFORE the arena claims memory (297 MB of the ~330 MB user
//     budget) or it silently fails to allocate.
//  2. Not all POSIX primitives that rt_boot's socket layer relies on have a
//     working equivalent on Vita. The helpers try the POSIX path first, then
//     the Sony path, and keep whichever one worked.
//  3. On PC everything compiles as pass-through: a single code path.
#include <cstdint>

// Initializes the stack. Returns 0 if networking is usable, otherwise:
//   -1 NET module not loaded, -2 pool rejected or sceNetInit failed, -3 sceNetCtlInit failed,
//   -4 link state unusable: no connection after wait_ms, or sceNetCtlInetGetState
//      failed. d2vita_net_status() distinguishes the two.
int d2vita_net_init(int wait_ms);

// Last human-readable status line, for boot_progress.
const char* d2vita_net_status(void);

// Local IP address ("0.0.0.0" if unavailable).
const char* d2vita_net_local_ip(void);

// Non-blocking mode and name resolution are generic (no D2-specific data);
// they live in runtime/net_nonblock.h (d2rt::wx86_net_set_nonblock / d2rt::wx86_net_resolve), not here.

// Exercises the primitives against a BNCS server. Returns the failure count.
int d2vita_net_selftest(const char* host, int port);
