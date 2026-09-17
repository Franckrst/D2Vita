// netguard_lock.h -- THE NETWORK LOCK: official-server traffic is allowed by
// default (this is a public release meant to be played online). The one
// unconditional restriction: no packet may leave for an official server
// while any debugging/automation tool is active, because each one would
// make the client NON-FAITHFUL as seen by the server. D2_LOCAL_ONLY=1 opts
// back into the old restrictive behavior (local allowlist only) for
// development and testing. See netguard_lock.cpp for the full rationale.
// Self-contained: reads only environment variables and calls the engine's
// leaf-level exit lock (net_guard.h); touches no Bridge/Cpu state, so it
// takes no parameters. Called from tools/rt_boot.cpp's main() at the exact
// point this logic used to sit inline, between the D2BNCS_LOCAL redirect
// setup and wx86_net_set_observer(&d2_net_observe).
#pragma once

void netguard_lock_install();
