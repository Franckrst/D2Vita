// kernel32_files.h -- KERNEL32 file-open shims: CreateFileA/CreateFileW (both
// funnel through one shared do_create_file() helper -- see kernel32_files.cpp
// for why they're kept together instead of forced apart), plus the small
// family of file-handle shims that share its local state (the file-op ring,
// the seek/read race detector, and the SetFilePointer/ReadFile/WriteFile
// OVERLAPPED-completion helper): GetFileAttributesA, SetFilePointer, ReadFile,
// WriteFile, GetOverlappedResult, FlushFileBuffers, GetFileSize, CloseHandle,
// CreateDirectoryA, DeleteFileA, MoveFileA.
#pragma once
namespace d2rt { class Bridge; }

void kernel32_files_install(d2rt::Bridge& br);
