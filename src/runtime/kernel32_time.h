// kernel32_time.h -- KERNEL32/WINMM wall-clock and calendar shims:
// SystemTimeToFileTime, FileTimeToSystemTime, FileTimeToLocalFileTime,
// WINMM timeGetTime/timeBeginPeriod/timeEndPeriod, QueryPerformanceCounter,
// GetSystemTimeAsFileTime, GetLocalTime, GetSystemTime, GetTimeZoneInformation.
// These all convert between the same three time representations (Win32
// SYSTEMTIME, FILETIME, and Unix time) via one local libc-free civil-calendar
// helper (civil_from_unix) and one timezone bias (g_tzbias) -- kept together
// as one "wall-clock" responsibility. The guest TICK counter (GetTickCount,
// Tier-1 intrinsic) is a separate concern and stays elsewhere (tick_intrinsic.cpp).
#pragma once
namespace d2rt { class Bridge; }

void kernel32_time_install(d2rt::Bridge& br);
