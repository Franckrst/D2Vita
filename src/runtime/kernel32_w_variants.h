// kernel32_w_variants.h -- the last plain KERNEL32 Unicode (W) filesystem
// shims that mirror an existing A-variant by narrowing UTF-16 to ASCII:
// GetFileAttributesW, CreateDirectoryW, DeleteFileW, CopyFileW, FindFirstFileW.
// (CreateFileW/GetModuleHandleW/LoadLibraryW/GetModuleFileNameW and friends
// already moved elsewhere, alongside their A-variant -- see kernel32_files.h
// and kernel32_modules.h.) No shared local state beyond the already-resident
// wnarrow(); each shim is otherwise independent.
#pragma once
namespace d2rt { class Bridge; }

void kernel32_w_variants_install(d2rt::Bridge& br);
