// win32_shims_user32_d2.h -- USER32 shims that stay specific to d2vita:
// MessageBoxA (diagnostics tied to g_locp/g_dump_fring), and everything that
// touches D2's window/wndproc state (GetForegroundWindow, GetActiveWindow,
// SetWindowLongA/GetWindowLongA, CallWindowProcA, PostMessageA/SendMessageA).
// The purely generic subset of the same original group (RECT geometry,
// strings, honest stubs) lives in
// third_party/winx86/src/runtime/win32_shims_user32.* (win32_shims_user32_install).
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_user32_d2_install(d2rt::Bridge& br);
