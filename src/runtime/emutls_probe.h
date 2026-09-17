// emutls_probe.h -- the D2VPK_TLSWRAP measurement-only emutls call counter
// (link-time --wrap=__emutls_get_address, never shipped) and the emutls
// inertness self-test run once at boot (two pthreads proving their
// thread_local copies are distinct and alive simultaneously). See
// emutls_probe.cpp for the full rationale.
#pragma once

// 0 = per-thread TLS OK; -1 = emutls INERT; >0 = failed pthread_create rc.
int d2rt_tls_probe();
