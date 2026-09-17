// eip_time_profile.h -- D2_EIPPROF (guest-code address profile, by Diablo II
// 1.14d subsystem) and D2_TIMEPROF (time-based address profile) reports,
// plus D2_EIPPROF_FROM/TO windowing. See eip_time_profile.cpp for the full
// rationale.
#pragma once

// The FAMILY MAP for the address profile (winx86 prof_map.h): six Diablo II
// 1.14d subsystem ranges inside Game.exe. Called once from main() after the
// module is loaded (g_d2base known).
void d2_prof_map();

void eipprof_report(const char* tag);
void timeprof_report(const char* tag);

// Called once per frame: opens/closes the D2_EIPPROF_FROM/TO window and
// reports at closure.
void ep_tick(int frame);
