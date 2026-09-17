// win32_shims_shell32.h -- SHELL32 shim specific to d2vita (SHGetFolderPathA,
// which hardcodes the "C:\Diablo II" install path). The purely generic subset
// of the same group (SHAppBarMessage, ShellExecuteA -- no D2/Blizzard
// literal, identical body for any Win32 guest) lives in
// third_party/winx86/src/runtime/win32_shims_shell32.* (win32_shims_shell32_install),
// called separately from rt_boot.cpp.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_shell32_d2_install(d2rt::Bridge& br);
