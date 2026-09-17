// kernel32_modules.h -- KERNEL32 module/library resolution: GetProcAddress,
// GetModuleHandleA/W, GetModuleHandleExA/W, GetModuleFileNameA/W,
// LoadLibraryA/LoadLibraryExA/LoadLibraryW. All of these query or populate
// the same module tables (g_modByBase/g_modByName/g_sysLib); the A/W pairs
// share either a helper (loadlib, module_path_for) or, for the GetModuleHandle
// family, identical by-name resolution logic -- kept together as one
// "module mapping" responsibility rather than split by encoding.
#pragma once
namespace d2rt { class Bridge; }

void kernel32_modules_install(d2rt::Bridge& br);
