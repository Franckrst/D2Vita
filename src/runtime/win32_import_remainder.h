// win32_import_remainder.h -- the KERNEL32/USER32/ADVAPI32/ole32/PSAPI
// leftovers of "FULL IMPORT COVERAGE": faithful Win32 implementations or
// clean failures for imports with no other natural home (each DLL's real
// generic/stateful subset already moved elsewhere -- win32_shims_kernel32.h,
// win32_shims_user32(_d2).h, win32_shims_advapi32(_d2).h, win32_shims_version.h,
// win32_shims_psapi.h). Kept as one file: none of these four small groups is
// individually worth its own module, and (KERNEL32 remainder aside) they
// share no state with each other anyway -- this is a "nowhere better to put
// it" grouping, not a real single responsibility.
//
// Deliberately NOT included here: the glide3x.dll / d2vhost.dll ring-crossing
// shims (d2vGlideInit/Flush/Nop/TexUpload) that sit in the same enclosing
// FULL IMPORT COVERAGE block in tools/rt_boot.cpp -- those are a distinct,
// substantial subsystem (src/glide_ring/) with their own extensive state
// (frame ticking, phase profiling, texture upload accounting) and stay there.
#pragma once
namespace d2rt { class Bridge; }

void win32_import_remainder_install(d2rt::Bridge& br);
