# Win32 shim list

!!! warning "Generated page — do not hand-edit"

    Produced by `tools/gen_shim_doc.py` from the sources, via
    `tools/shim_seq.py`. CI regenerates this page and fails if it
    differs from what is committed. To update it:
    `tools/gen_shim_doc.py`.

Diablo II never calls Windows: every function imported by the game
or its DLLs is served by a native implementation. This page says
**who serves it** — the generic engine [winx86](https://winx86-136891.gitlab.io/)
(submodule `third_party/winx86`), or this repository.

This is the engine/game boundary seen from the facts rather than from
intent: a function is on whichever side its code is actually registered.

## Summary

| | Keys | Share |
|---|---:|---:|
| Served by the **engine** (winx86) | 366 | 61 % |
| Served by the **game** (this repository) | 238 | 39 % |
| **Total** | **604** | |

| DLL | Total | Engine | Game |
|---|---:|---:|---:|
| `KERNEL32.dll` | 252 | 137 | 115 |
| `USER32.dll` | 92 | 56 | 36 |
| `ADVAPI32.dll` | 67 | 37 | 30 |
| `GDI32.dll` | 41 | 36 | 5 |
| `native.hook` | 40 | 0 | 40 |
| `WS2_32.dll` | 31 | 31 | 0 |
| `WSOCK32.dll` | 31 | 31 | 0 |
| `IMM32.dll` | 11 | 11 | 0 |
| `binkw32.dll` | 9 | 9 | 0 |
| `smackw32.dll` | 6 | 6 | 0 |
| `d2vhost.dll` | 4 | 0 | 4 |
| `VERSION.dll` | 4 | 4 | 0 |
| `ijl11.dll` | 3 | 3 | 0 |
| `SHELL32.dll` | 3 | 2 | 1 |
| `WINMM.dll` | 3 | 0 | 3 |
| `CRYPT32.dll` | 2 | 0 | 2 |
| `DDRAW.dll` | 2 | 2 | 0 |
| `PSAPI.DLL` | 2 | 1 | 1 |
| `ole32.dll` | 1 | 0 | 1 |

## Multiple registrations

`Bridge::register_shim` **overwrites** the key: when a name is registered
more than once, the LAST registration wins, silently.
A losing registration is dead code that looks alive. This pattern
caused a real Battle.net connection regression — see the
`WinVerifyTrust` comment in `tools/rt_boot.cpp`.

*No key registered more than once.*

## Detail by DLL

### `KERNEL32.dll`

252 keys — 137 engine, 115 game.

| Function | Served by | Unit |
|---|---|---|
| `__d2rt_dllmain_done` | game | `rt_boot.cpp` |
| `CloseHandle` | game | `kernel32_files.cpp` |
| `CompareFileTime` | game | `rt_boot.cpp` |
| `CompareStringA` | engine | `win32_shims_locale.cpp` |
| `CompareStringW` | engine | `win32_shims_locale.cpp` |
| `CopyFileA` | engine | `win32_shims_kernel32.cpp` |
| `CopyFileW` | game | `kernel32_w_variants.cpp` |
| `CreateDirectoryA` | game | `kernel32_files.cpp` |
| `CreateDirectoryW` | game | `kernel32_w_variants.cpp` |
| `CreateEventA` | engine | `win32_shims_sync.cpp` |
| `CreateEventW` | engine | `win32_shims_sync.cpp` |
| `CreateFileA` | game | `kernel32_files.cpp` |
| `CreateFileMappingA` | game | `kernel32_filemapping.cpp` |
| `CreateFileMappingW` | game | `kernel32_filemapping.cpp` |
| `CreateFileW` | game | `kernel32_files.cpp` |
| `CreateIoCompletionPort` | engine | `win32_shims_wait.cpp` |
| `CreateMutexA` | engine | `win32_shims_sync.cpp` |
| `CreateMutexW` | engine | `win32_shims_sync.cpp` |
| `CreateProcessA` | game | `win32_import_remainder.cpp` |
| `CreateProcessW` | engine | `win32_shims_kernel32.cpp` |
| `CreateSemaphoreA` | engine | `win32_shims_sync.cpp` |
| `CreateSemaphoreW` | engine | `win32_shims_sync.cpp` |
| `CreateThread` | game | `rt_boot.cpp` |
| `CreateToolhelp32Snapshot` | game | `toolhelp.cpp` |
| `DecodePointer` | engine | `win32_shims_kernel32.cpp` |
| `DeleteCriticalSection` | game | `rt_boot.cpp` |
| `DeleteFileA` | game | `kernel32_files.cpp` |
| `DeleteFileW` | game | `kernel32_w_variants.cpp` |
| `DuplicateHandle` | engine | `win32_shims_kernel32.cpp` |
| `EncodePointer` | engine | `win32_shims_kernel32.cpp` |
| `EnterCriticalSection` | game | `rt_boot.cpp` |
| `EnumProcessModules` | engine | `win32_shims_psapi.cpp` |
| `EnumSystemLocalesA` | engine | `win32_shims_kernel32.cpp` |
| `ExitProcess` | game | `win32_import_remainder.cpp` |
| `ExitThread` | game | `rt_boot.cpp` |
| `FatalAppExitA` | game | `win32_import_remainder.cpp` |
| `FileTimeToLocalFileTime` | game | `kernel32_time.cpp` |
| `FileTimeToSystemTime` | game | `kernel32_time.cpp` |
| `FindClose` | game | `rt_boot.cpp` |
| `FindFirstFileA` | game | `rt_boot.cpp` |
| `FindFirstFileW` | game | `kernel32_w_variants.cpp` |
| `FindNextFileA` | game | `rt_boot.cpp` |
| `FindResourceA` | game | `win32_import_remainder.cpp` |
| `FlushFileBuffers` | game | `kernel32_files.cpp` |
| `FlushInstructionCache` | engine | `win32_shims_memory.cpp` |
| `FormatMessageA` | game | `win32_import_remainder.cpp` |
| `FreeEnvironmentStringsA` | engine | `win32_shims_kernel32.cpp` |
| `FreeEnvironmentStringsW` | engine | `win32_shims_kernel32.cpp` |
| `FreeLibrary` | engine | `win32_shims_kernel32.cpp` |
| `FreeResource` | game | `win32_import_remainder.cpp` |
| `GetACP` | engine | `win32_shims_locale.cpp` |
| `GetCommandLineA` | game | `rt_boot.cpp` |
| `GetComputerNameA` | game | `kernel32_fsinfo.cpp` |
| `GetConsoleCP` | engine | `win32_shims_locale.cpp` |
| `GetConsoleMode` | engine | `win32_shims_locale.cpp` |
| `GetConsoleOutputCP` | engine | `win32_shims_locale.cpp` |
| `GetCPInfo` | engine | `win32_shims_kernel32.cpp` |
| `GetCurrentDirectoryA` | game | `kernel32_fsinfo.cpp` |
| `GetCurrentProcess` | game | `kernel32_interlocked.cpp` |
| `GetCurrentProcessId` | engine | `win32_shims_kernel32.cpp` |
| `GetCurrentThread` | game | `kernel32_interlocked.cpp` |
| `GetCurrentThreadId` | game | `kernel32_interlocked.cpp` |
| `GetDateFormatA` | engine | `win32_shims_locale.cpp` |
| `GetDiskFreeSpaceA` | game | `kernel32_fsinfo.cpp` |
| `GetDiskFreeSpaceExA` | game | `kernel32_fsinfo.cpp` |
| `GetDriveTypeA` | engine | `win32_shims_kernel32.cpp` |
| `GetEnvironmentStrings` | engine | `win32_shims_locale.cpp` |
| `GetEnvironmentStringsA` | engine | `win32_shims_locale.cpp` |
| `GetEnvironmentStringsW` | engine | `win32_shims_locale.cpp` |
| `GetEnvironmentVariableA` | engine | `win32_shims_locale.cpp` |
| `GetExitCodeProcess` | game | `win32_import_remainder.cpp` |
| `GetExitCodeThread` | engine | `win32_shims_sync.cpp` |
| `GetFileAttributesA` | game | `kernel32_files.cpp` |
| `GetFileAttributesW` | game | `kernel32_w_variants.cpp` |
| `GetFileSize` | game | `kernel32_files.cpp` |
| `GetFileTime` | game | `win32_import_remainder.cpp` |
| `GetFileType` | engine | `win32_shims_kernel32.cpp` |
| `GetFullPathNameA` | game | `kernel32_fsinfo.cpp` |
| `GetLastError` | engine | `win32_shims_kernel32.cpp` |
| `GetLocaleInfoA` | engine | `win32_shims_locale.cpp` |
| `GetLocaleInfoW` | engine | `win32_shims_locale.cpp` |
| `GetLocalTime` | game | `kernel32_time.cpp` |
| `GetLogicalDrives` | game | `kernel32_fsinfo.cpp` |
| `GetLogicalDriveStringsA` | game | `kernel32_fsinfo.cpp` |
| `GetModuleFileNameA` | game | `kernel32_modules.cpp` |
| `GetModuleFileNameExA` | game | `rt_boot.cpp` |
| `GetModuleFileNameExW` | game | `rt_boot.cpp` |
| `GetModuleFileNameW` | game | `kernel32_modules.cpp` |
| `GetModuleHandleA` | game | `kernel32_modules.cpp` |
| `GetModuleHandleExA` | game | `kernel32_modules.cpp` |
| `GetModuleHandleExW` | game | `kernel32_modules.cpp` |
| `GetModuleHandleW` | game | `kernel32_modules.cpp` |
| `GetModuleInformation` | engine | `win32_shims_psapi.cpp` |
| `GetOEMCP` | engine | `win32_shims_locale.cpp` |
| `GetOverlappedResult` | game | `kernel32_files.cpp` |
| `GetPrivateProfileIntA` | game | `win32_import_remainder.cpp` |
| `GetPrivateProfileStringA` | game | `win32_import_remainder.cpp` |
| `GetProcAddress` | game | `kernel32_modules.cpp` |
| `GetProcessAffinityMask` | engine | `win32_shims_kernel32.cpp` |
| `GetProcessHeap` | engine | `win32_shims_memory.cpp` |
| `GetProcessId` | game | `win32_import_remainder.cpp` |
| `GetProfileStringA` | engine | `win32_shims_kernel32.cpp` |
| `GetQueuedCompletionStatus` | engine | `win32_shims_wait.cpp` |
| `GetStartupInfoA` | engine | `win32_shims_kernel32.cpp` |
| `GetStartupInfoW` | engine | `win32_shims_kernel32.cpp` |
| `GetStdHandle` | engine | `win32_shims_locale.cpp` |
| `GetStringTypeA` | engine | `win32_shims_locale.cpp` |
| `GetStringTypeW` | engine | `win32_shims_locale.cpp` |
| `GetSystemDefaultLangID` | engine | `win32_shims_locale.cpp` |
| `GetSystemDefaultLCID` | engine | `win32_shims_locale.cpp` |
| `GetSystemDefaultUILanguage` | engine | `win32_shims_locale.cpp` |
| `GetSystemDirectoryA` | engine | `win32_shims_kernel32.cpp` |
| `GetSystemInfo` | engine | `win32_shims_memory.cpp` |
| `GetSystemTime` | game | `kernel32_time.cpp` |
| `GetSystemTimeAsFileTime` | game | `kernel32_time.cpp` |
| `GetTempFileNameA` | game | `win32_import_remainder.cpp` |
| `GetTempPathA` | engine | `win32_shims_kernel32.cpp` |
| `GetTempPathW` | game | `kernel32_fsinfo.cpp` |
| `GetThreadContext` | game | `toolhelp.cpp` |
| `GetThreadLocale` | engine | `win32_shims_locale.cpp` |
| `GetThreadPriority` | engine | `win32_shims_kernel32.cpp` |
| `GetTickCount` | game | `rt_boot.cpp` |
| `GetTimeFormatA` | engine | `win32_shims_locale.cpp` |
| `GetTimeZoneInformation` | game | `kernel32_time.cpp` |
| `GetUserDefaultLangID` | engine | `win32_shims_locale.cpp` |
| `GetUserDefaultLCID` | engine | `win32_shims_locale.cpp` |
| `GetUserDefaultUILanguage` | engine | `win32_shims_locale.cpp` |
| `GetVersion` | game | `rt_boot.cpp` |
| `GetVersionExA` | engine | `win32_shims_kernel32.cpp` |
| `GetVolumeInformationA` | game | `kernel32_fsinfo.cpp` |
| `GetWindowsDirectoryA` | engine | `win32_shims_kernel32.cpp` |
| `GlobalAlloc` | engine | `win32_shims_memory.cpp` |
| `GlobalFree` | engine | `win32_shims_memory.cpp` |
| `GlobalLock` | game | `win32_import_remainder.cpp` |
| `GlobalMemoryStatus` | engine | `win32_shims_memory.cpp` |
| `GlobalUnlock` | game | `win32_import_remainder.cpp` |
| `HeapAlloc` | engine | `win32_shims_memory.cpp` |
| `HeapCreate` | engine | `win32_shims_memory.cpp` |
| `HeapDestroy` | engine | `win32_shims_kernel32.cpp` |
| `HeapFree` | engine | `win32_shims_memory.cpp` |
| `HeapReAlloc` | engine | `win32_shims_memory.cpp` |
| `HeapSize` | engine | `win32_shims_memory.cpp` |
| `InitializeCriticalSection` | game | `rt_boot.cpp` |
| `InitializeCriticalSectionAndSpinCount` | game | `rt_boot.cpp` |
| `InterlockedAnd` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedCompareExchange` | game | `kernel32_interlocked.cpp` |
| `InterlockedCompareExchangePointer` | game | `kernel32_interlocked.cpp` |
| `InterlockedDecrement` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedExchange` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedExchangeAdd` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedExchangePointer` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedIncrement` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedOr` | engine | `win32_shims_kernel32.cpp` |
| `InterlockedXor` | engine | `win32_shims_kernel32.cpp` |
| `IsBadCodePtr` | engine | `win32_shims_kernel32.cpp` |
| `IsBadReadPtr` | engine | `win32_shims_kernel32.cpp` |
| `IsBadStringPtrA` | game | `win32_import_remainder.cpp` |
| `IsBadWritePtr` | engine | `win32_shims_kernel32.cpp` |
| `IsDebuggerPresent` | engine | `win32_shims_kernel32.cpp` |
| `IsProcessorFeaturePresent` | engine | `win32_shims_kernel32.cpp` |
| `IsValidCodePage` | engine | `win32_shims_kernel32.cpp` |
| `IsValidLocale` | engine | `win32_shims_kernel32.cpp` |
| `K32EnumProcessModules` | engine | `win32_shims_psapi.cpp` |
| `K32GetModuleInformation` | engine | `win32_shims_psapi.cpp` |
| `LCMapStringA` | engine | `win32_shims_locale.cpp` |
| `LCMapStringW` | engine | `win32_shims_locale.cpp` |
| `LeaveCriticalSection` | game | `rt_boot.cpp` |
| `LoadLibraryA` | game | `kernel32_modules.cpp` |
| `LoadLibraryExA` | game | `kernel32_modules.cpp` |
| `LoadLibraryW` | game | `kernel32_modules.cpp` |
| `LoadResource` | game | `win32_import_remainder.cpp` |
| `LocalAlloc` | engine | `win32_shims_memory.cpp` |
| `LocalFree` | engine | `win32_shims_memory.cpp` |
| `LockResource` | game | `win32_import_remainder.cpp` |
| `lstrcmpA` | engine | `win32_shims_locale.cpp` |
| `lstrcpyA` | engine | `win32_shims_locale.cpp` |
| `lstrlenA` | engine | `win32_shims_locale.cpp` |
| `MapViewOfFile` | game | `kernel32_filemapping.cpp` |
| `MemoryBarrier` | engine | `win32_shims_kernel32.cpp` |
| `Module32First` | game | `toolhelp.cpp` |
| `Module32FirstW` | game | `toolhelp.cpp` |
| `Module32Next` | game | `toolhelp.cpp` |
| `Module32NextW` | game | `toolhelp.cpp` |
| `MoveFileA` | game | `kernel32_files.cpp` |
| `MultiByteToWideChar` | engine | `win32_shims_locale.cpp` |
| `OpenEventA` | engine | `win32_shims_kernel32.cpp` |
| `OpenMutexA` | engine | `win32_shims_kernel32.cpp` |
| `OpenProcess` | engine | `win32_shims_kernel32.cpp` |
| `OpenThread` | game | `toolhelp.cpp` |
| `OutputDebugStringA` | game | `rt_boot.cpp` |
| `PostQueuedCompletionStatus` | engine | `win32_shims_wait.cpp` |
| `Process32First` | game | `toolhelp.cpp` |
| `Process32FirstW` | game | `toolhelp.cpp` |
| `Process32Next` | game | `toolhelp.cpp` |
| `Process32NextW` | game | `toolhelp.cpp` |
| `PulseEvent` | engine | `win32_shims_sync.cpp` |
| `QueryPerformanceCounter` | game | `kernel32_time.cpp` |
| `QueryPerformanceFrequency` | engine | `win32_shims_kernel32.cpp` |
| `RaiseException` | game | `win32_import_remainder.cpp` |
| `ReadFile` | game | `kernel32_files.cpp` |
| `ReadProcessMemory` | game | `win32_import_remainder.cpp` |
| `ReleaseMutex` | engine | `win32_shims_sync.cpp` |
| `ReleaseSemaphore` | engine | `win32_shims_sync.cpp` |
| `RemoveDirectoryA` | engine | `win32_shims_kernel32.cpp` |
| `ResetEvent` | engine | `win32_shims_sync.cpp` |
| `ResumeThread` | game | `rt_boot.cpp` |
| `RtlCaptureContext` | game | `toolhelp.cpp` |
| `RtlUnwind` | engine | `win32_shims_kernel32.cpp` |
| `SetConsoleCtrlHandler` | engine | `win32_shims_locale.cpp` |
| `SetCurrentDirectoryA` | engine | `win32_shims_kernel32.cpp` |
| `SetEndOfFile` | engine | `win32_shims_kernel32.cpp` |
| `SetEnvironmentVariableA` | engine | `win32_shims_locale.cpp` |
| `SetErrorMode` | engine | `win32_shims_kernel32.cpp` |
| `SetEvent` | engine | `win32_shims_sync.cpp` |
| `SetFilePointer` | game | `kernel32_files.cpp` |
| `SetFileTime` | engine | `win32_shims_kernel32.cpp` |
| `SetHandleCount` | engine | `win32_shims_kernel32.cpp` |
| `SetLastError` | engine | `win32_shims_kernel32.cpp` |
| `SetPriorityClass` | engine | `win32_shims_kernel32.cpp` |
| `SetStdHandle` | engine | `win32_shims_locale.cpp` |
| `SetThreadAffinityMask` | engine | `win32_shims_kernel32.cpp` |
| `SetThreadLocale` | engine | `win32_shims_kernel32.cpp` |
| `SetThreadPriority` | engine | `win32_shims_kernel32.cpp` |
| `SetUnhandledExceptionFilter` | game | `toolhelp.cpp` |
| `SizeofResource` | game | `win32_import_remainder.cpp` |
| `Sleep` | game | `rt_boot.cpp` |
| `SleepEx` | engine | `win32_shims_wait.cpp` |
| `SuspendThread` | engine | `win32_shims_kernel32.cpp` |
| `SwitchToThread` | engine | `win32_shims_wait.cpp` |
| `SystemTimeToFileTime` | game | `kernel32_time.cpp` |
| `TerminateProcess` | game | `win32_import_remainder.cpp` |
| `TerminateThread` | game | `rt_boot.cpp` |
| `Thread32First` | game | `toolhelp.cpp` |
| `Thread32Next` | game | `toolhelp.cpp` |
| `TlsAlloc` | game | `rt_boot.cpp` |
| `TlsFree` | engine | `win32_shims_kernel32.cpp` |
| `TlsGetValue` | game | `rt_boot.cpp` |
| `TlsSetValue` | game | `rt_boot.cpp` |
| `TryEnterCriticalSection` | game | `rt_boot.cpp` |
| `UnhandledExceptionFilter` | engine | `win32_shims_kernel32.cpp` |
| `UnmapViewOfFile` | game | `kernel32_filemapping.cpp` |
| `VirtualAlloc` | engine | `win32_shims_memory.cpp` |
| `VirtualFree` | engine | `win32_shims_memory.cpp` |
| `VirtualProtect` | engine | `win32_shims_memory.cpp` |
| `VirtualQuery` | engine | `win32_shims_memory.cpp` |
| `WaitForMultipleObjects` | engine | `win32_shims_wait.cpp` |
| `WaitForSingleObject` | engine | `win32_shims_wait.cpp` |
| `WideCharToMultiByte` | engine | `win32_shims_locale.cpp` |
| `WriteConsoleA` | engine | `win32_shims_locale.cpp` |
| `WriteConsoleW` | engine | `win32_shims_kernel32.cpp` |
| `WriteFile` | game | `kernel32_files.cpp` |
| `WritePrivateProfileStringA` | engine | `win32_shims_kernel32.cpp` |

### `USER32.dll`

92 keys — 56 engine, 36 game.

| Function | Served by | Unit |
|---|---|---|
| `AdjustWindowRect` | engine | `win32_shims_window.cpp` |
| `AdjustWindowRectEx` | engine | `win32_shims_window.cpp` |
| `BeginPaint` | engine | `win32_shims_window.cpp` |
| `BringWindowToTop` | engine | `win32_shims_window.cpp` |
| `CallWindowProcA` | game | `win32_shims_user32_d2.cpp` |
| `ChangeDisplaySettingsA` | engine | `win32_shims_window.cpp` |
| `CharLowerBuffA` | engine | `win32_shims_user32.cpp` |
| `CharNextA` | engine | `win32_shims_user32.cpp` |
| `ClientToScreen` | engine | `win32_shims_window.cpp` |
| `ClipCursor` | engine | `win32_shims_window.cpp` |
| `CloseClipboard` | game | `win32_import_remainder.cpp` |
| `CopyRect` | engine | `win32_shims_user32.cpp` |
| `CreateWindowExA` | game | `win32_shims_user32_d2.cpp` |
| `DefWindowProcA` | engine | `win32_shims_window.cpp` |
| `DestroyWindow` | engine | `win32_shims_user32.cpp` |
| `DispatchMessageA` | game | `win32_shims_user32_d2.cpp` |
| `DrawTextA` | game | `win32_import_remainder.cpp` |
| `EmptyClipboard` | game | `win32_import_remainder.cpp` |
| `EnableWindow` | engine | `win32_shims_user32.cpp` |
| `EndPaint` | engine | `win32_shims_window.cpp` |
| `EnumDisplaySettingsA` | engine | `win32_shims_window.cpp` |
| `EqualRect` | engine | `win32_shims_user32.cpp` |
| `FindWindowA` | engine | `win32_shims_user32.cpp` |
| `GetActiveWindow` | game | `win32_shims_user32_d2.cpp` |
| `GetAsyncKeyState` | game | `win32_shims_user32_d2.cpp` |
| `GetClientRect` | engine | `win32_shims_window.cpp` |
| `GetClipboardData` | game | `win32_import_remainder.cpp` |
| `GetCursorPos` | engine | `win32_shims_window.cpp` |
| `GetDC` | engine | `win32_shims_window.cpp` |
| `GetDesktopWindow` | engine | `win32_shims_user32.cpp` |
| `GetForegroundWindow` | game | `win32_shims_user32_d2.cpp` |
| `GetKeyboardLayout` | game | `win32_import_remainder.cpp` |
| `GetKeyboardState` | game | `win32_shims_user32_d2.cpp` |
| `GetKeyState` | game | `win32_shims_user32_d2.cpp` |
| `GetMessageA` | game | `win32_shims_user32_d2.cpp` |
| `GetMonitorInfoA` | game | `win32_import_remainder.cpp` |
| `GetSystemMetrics` | engine | `win32_shims_window.cpp` |
| `GetWindowLongA` | game | `win32_shims_user32_d2.cpp` |
| `GetWindowPlacement` | game | `win32_import_remainder.cpp` |
| `GetWindowRect` | engine | `win32_shims_window.cpp` |
| `GetWindowThreadProcessId` | engine | `win32_shims_window.cpp` |
| `InflateRect` | engine | `win32_shims_user32.cpp` |
| `IntersectRect` | engine | `win32_shims_user32.cpp` |
| `IsClipboardFormatAvailable` | game | `win32_import_remainder.cpp` |
| `IsIconic` | game | `win32_import_remainder.cpp` |
| `IsRectEmpty` | engine | `win32_shims_user32.cpp` |
| `IsWindow` | engine | `win32_shims_user32.cpp` |
| `IsWindowVisible` | game | `win32_import_remainder.cpp` |
| `LoadAcceleratorsA` | engine | `win32_shims_user32.cpp` |
| `LoadCursorA` | engine | `win32_shims_window.cpp` |
| `LoadIconA` | engine | `win32_shims_window.cpp` |
| `LoadImageA` | engine | `win32_shims_window.cpp` |
| `LoadStringA` | engine | `win32_shims_user32.cpp` |
| `LoadStringW` | engine | `win32_shims_user32.cpp` |
| `MapVirtualKeyA` | engine | `win32_shims_window.cpp` |
| `MessageBeep` | engine | `win32_shims_user32.cpp` |
| `MessageBoxA` | game | `win32_shims_user32_d2.cpp` |
| `MonitorFromWindow` | game | `win32_import_remainder.cpp` |
| `MoveWindow` | game | `win32_import_remainder.cpp` |
| `OffsetRect` | engine | `win32_shims_user32.cpp` |
| `OpenClipboard` | game | `win32_import_remainder.cpp` |
| `PeekMessageA` | game | `win32_shims_user32_d2.cpp` |
| `PostMessageA` | game | `win32_shims_user32_d2.cpp` |
| `PostQuitMessage` | engine | `win32_shims_window.cpp` |
| `PtInRect` | engine | `win32_shims_user32.cpp` |
| `RegisterClassA` | game | `win32_shims_user32_d2.cpp` |
| `RegisterClassExA` | game | `win32_shims_user32_d2.cpp` |
| `ReleaseDC` | engine | `win32_shims_window.cpp` |
| `ScreenToClient` | engine | `win32_shims_window.cpp` |
| `SendMessageA` | game | `win32_shims_user32_d2.cpp` |
| `SetClipboardData` | game | `win32_import_remainder.cpp` |
| `SetCursor` | engine | `win32_shims_window.cpp` |
| `SetCursorPos` | engine | `win32_shims_window.cpp` |
| `SetFocus` | game | `win32_shims_user32_d2.cpp` |
| `SetForegroundWindow` | engine | `win32_shims_window.cpp` |
| `SetRect` | engine | `win32_shims_user32.cpp` |
| `SetRectEmpty` | engine | `win32_shims_user32.cpp` |
| `SetWindowLongA` | game | `win32_shims_user32_d2.cpp` |
| `SetWindowPos` | engine | `win32_shims_window.cpp` |
| `SetWindowTextA` | engine | `win32_shims_window.cpp` |
| `ShowCursor` | engine | `win32_shims_window.cpp` |
| `ShowWindow` | game | `win32_shims_user32_d2.cpp` |
| `SystemParametersInfoA` | engine | `win32_shims_window.cpp` |
| `TrackMouseEvent` | engine | `win32_shims_window.cpp` |
| `TranslateAccelerator` | engine | `win32_shims_user32.cpp` |
| `TranslateAcceleratorA` | engine | `win32_shims_user32.cpp` |
| `TranslateMessage` | engine | `win32_shims_window.cpp` |
| `UnionRect` | engine | `win32_shims_user32.cpp` |
| `UnregisterClassA` | game | `win32_shims_user32_d2.cpp` |
| `UpdateWindow` | engine | `win32_shims_window.cpp` |
| `wsprintfA` | game | `win32_import_remainder.cpp` |
| `wvsprintfA` | game | `win32_import_remainder.cpp` |

### `ADVAPI32.dll`

67 keys — 37 engine, 30 game.

| Function | Served by | Unit |
|---|---|---|
| `AddAccessAllowedAce` | engine | `win32_shims_advapi32.cpp` |
| `AddAccessDeniedAce` | engine | `win32_shims_advapi32.cpp` |
| `AddAce` | engine | `win32_shims_advapi32.cpp` |
| `AdjustTokenPrivileges` | engine | `win32_shims_advapi32.cpp` |
| `AllocateAndInitializeSid` | game | `win32_shims_advapi32_d2.cpp` |
| `CheckTokenMembership` | engine | `win32_shims_advapi32.cpp` |
| `CloseServiceHandle` | engine | `win32_shims_advapi32.cpp` |
| `ControlService` | engine | `win32_shims_advapi32.cpp` |
| `CopySid` | engine | `win32_shims_advapi32.cpp` |
| `CreateServiceA` | game | `win32_shims_advapi32_d2.cpp` |
| `CryptAcquireContextW` | game | `checkrevision_crypto.cpp` |
| `CryptCreateHash` | game | `checkrevision_crypto.cpp` |
| `CryptDestroyHash` | game | `checkrevision_crypto.cpp` |
| `CryptGetHashParam` | game | `checkrevision_crypto.cpp` |
| `CryptHashData` | game | `checkrevision_crypto.cpp` |
| `CryptReleaseContext` | game | `checkrevision_crypto.cpp` |
| `DuplicateToken` | engine | `win32_shims_advapi32.cpp` |
| `DuplicateTokenEx` | engine | `win32_shims_advapi32.cpp` |
| `EqualSid` | engine | `win32_shims_advapi32.cpp` |
| `FreeSid` | engine | `win32_shims_advapi32.cpp` |
| `GetLengthSid` | engine | `win32_shims_advapi32.cpp` |
| `GetSecurityDescriptorLength` | engine | `win32_shims_advapi32.cpp` |
| `GetSecurityInfo` | engine | `win32_shims_advapi32.cpp` |
| `GetSidLengthRequired` | engine | `win32_shims_advapi32.cpp` |
| `GetSidSubAuthority` | game | `win32_shims_advapi32_d2.cpp` |
| `GetSidSubAuthorityCount` | game | `win32_shims_advapi32_d2.cpp` |
| `GetTokenInformation` | engine | `win32_shims_advapi32.cpp` |
| `GetUserNameA` | engine | `win32_shims_advapi32.cpp` |
| `ImpersonateLoggedOnUser` | engine | `win32_shims_advapi32.cpp` |
| `InitializeAcl` | engine | `win32_shims_advapi32.cpp` |
| `InitializeSecurityDescriptor` | engine | `win32_shims_advapi32.cpp` |
| `InitializeSid` | engine | `win32_shims_advapi32.cpp` |
| `IsValidAcl` | engine | `win32_shims_advapi32.cpp` |
| `IsValidSecurityDescriptor` | engine | `win32_shims_advapi32.cpp` |
| `IsValidSid` | engine | `win32_shims_advapi32.cpp` |
| `LookupAccountNameA` | engine | `win32_shims_advapi32.cpp` |
| `LookupPrivilegeValueA` | engine | `win32_shims_advapi32.cpp` |
| `OpenProcessToken` | engine | `win32_shims_advapi32.cpp` |
| `OpenSCManagerA` | game | `win32_shims_advapi32_d2.cpp` |
| `OpenServiceA` | game | `win32_shims_advapi32_d2.cpp` |
| `OpenThreadToken` | engine | `win32_shims_advapi32.cpp` |
| `QueryServiceStatus` | engine | `win32_shims_advapi32.cpp` |
| `RegCloseKey` | game | `win32_shims_advapi32_d2.cpp` |
| `RegCreateKeyA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegCreateKeyExA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegDeleteKeyA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegDeleteValueA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegEnumValueA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegFlushKey` | game | `win32_shims_advapi32_d2.cpp` |
| `RegisterServiceCtrlHandlerA` | game | `win32_import_remainder.cpp` |
| `RegNotifyChangeKeyValue` | game | `win32_shims_advapi32_d2.cpp` |
| `RegOpenKeyA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegOpenKeyExA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegOpenKeyW` | game | `win32_shims_advapi32_d2.cpp` |
| `RegQueryValueA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegQueryValueExA` | game | `win32_shims_advapi32_d2.cpp` |
| `RegSetValueExA` | game | `win32_shims_advapi32_d2.cpp` |
| `RevertToSelf` | engine | `win32_shims_advapi32.cpp` |
| `SetEntriesInAclA` | game | `win32_shims_advapi32_d2.cpp` |
| `SetSecurityDescriptorDacl` | engine | `win32_shims_advapi32.cpp` |
| `SetSecurityDescriptorGroup` | engine | `win32_shims_advapi32.cpp` |
| `SetSecurityDescriptorOwner` | engine | `win32_shims_advapi32.cpp` |
| `SetSecurityDescriptorSacl` | engine | `win32_shims_advapi32.cpp` |
| `SetSecurityInfo` | engine | `win32_shims_advapi32.cpp` |
| `SetServiceStatus` | game | `win32_import_remainder.cpp` |
| `StartServiceA` | engine | `win32_shims_advapi32.cpp` |
| `StartServiceCtrlDispatcherA` | game | `win32_import_remainder.cpp` |

### `GDI32.dll`

41 keys — 36 engine, 5 game.

| Function | Served by | Unit |
|---|---|---|
| `AnimatePalette` | engine | `win32_shims_gdi32.cpp` |
| `BitBlt` | game | `rt_boot.cpp` |
| `CreateBitmap` | engine | `win32_shims_gdi32.cpp` |
| `CreateCompatibleBitmap` | engine | `win32_shims_gdi32.cpp` |
| `CreateCompatibleDC` | engine | `win32_shims_gdi32.cpp` |
| `CreateDCA` | engine | `win32_shims_gdi32.cpp` |
| `CreateDIBSection` | game | `rt_boot.cpp` |
| `CreateFontA` | engine | `win32_shims_gdi32.cpp` |
| `CreatePalette` | engine | `win32_shims_gdi32.cpp` |
| `CreatePen` | engine | `win32_shims_gdi32.cpp` |
| `CreateSolidBrush` | engine | `win32_shims_gdi32.cpp` |
| `DeleteDC` | engine | `win32_shims_gdi32.cpp` |
| `DeleteObject` | engine | `win32_shims_gdi32.cpp` |
| `GdiFlush` | engine | `win32_shims_gdi32.cpp` |
| `GdiSetBatchLimit` | engine | `win32_shims_gdi32.cpp` |
| `GetCharWidthA` | engine | `win32_shims_gdi32.cpp` |
| `GetDeviceCaps` | engine | `win32_shims_gdi32.cpp` |
| `GetDeviceGammaRamp` | engine | `win32_shims_gdi32.cpp` |
| `GetDIBColorTable` | engine | `win32_shims_gdi32.cpp` |
| `GetPixel` | engine | `win32_shims_gdi32.cpp` |
| `GetStockObject` | engine | `win32_shims_gdi32.cpp` |
| `GetSystemPaletteEntries` | engine | `win32_shims_gdi32.cpp` |
| `GetTextExtentPoint32A` | engine | `win32_shims_gdi32.cpp` |
| `GetTextExtentPointA` | engine | `win32_shims_gdi32.cpp` |
| `LineTo` | engine | `win32_shims_gdi32.cpp` |
| `MoveToEx` | engine | `win32_shims_gdi32.cpp` |
| `PatBlt` | engine | `win32_shims_gdi32.cpp` |
| `RealizePalette` | engine | `win32_shims_gdi32.cpp` |
| `SelectObject` | engine | `win32_shims_gdi32.cpp` |
| `SelectPalette` | engine | `win32_shims_gdi32.cpp` |
| `SetBkColor` | engine | `win32_shims_gdi32.cpp` |
| `SetBkMode` | engine | `win32_shims_gdi32.cpp` |
| `SetDeviceGammaRamp` | engine | `win32_shims_gdi32.cpp` |
| `SetDIBColorTable` | game | `rt_boot.cpp` |
| `SetPaletteEntries` | engine | `win32_shims_gdi32.cpp` |
| `SetStretchBltMode` | engine | `win32_shims_gdi32.cpp` |
| `SetTextColor` | engine | `win32_shims_gdi32.cpp` |
| `StretchBlt` | game | `rt_boot.cpp` |
| `StretchDIBits` | engine | `win32_shims_gdi32.cpp` |
| `TextOutA` | game | `rt_boot.cpp` |
| `UnrealizeObject` | engine | `win32_shims_gdi32.cpp` |

### `native.hook`

40 keys — 0 engine, 40 game.

| Function | Served by | Unit |
|---|---|---|
| `cdkey_file` | game | `cdkeys_file.cpp` |
| `celdata_evict` | game | `native_hooks_codec.cpp` |
| `d2_cell_blend_114` | game | `native_hooks_cellengine.cpp` |
| `d2_cell_blit_114` | game | `native_hooks_cellengine.cpp` |
| `d2_cell_loop_114` | game | `native_hooks_cellengine.cpp` |
| `d2_cell_loop_census` | game | `native_hooks_cellengine.cpp` |
| `d2_cell_rle_114` | game | `native_hooks_cellengine.cpp` |
| `d2_coll_lookup_114` | game | `native_hooks_cellengine.cpp` |
| `d2_coll_verify_exit` | game | `native_hooks_cellengine.cpp` |
| `d2_grid_verify_exit` | game | `native_hooks_cellengine.cpp` |
| `d2_light_grid_114` | game | `native_hooks_cellengine.cpp` |
| `d2_light_verify_exit` | game | `native_hooks_cellengine.cpp` |
| `d2_lightblob_114` | game | `native_hooks_cellengine.cpp` |
| `d2_lightgrid_114` | game | `native_hooks_cellengine.cpp` |
| `d2_lightmap_verify_exit` | game | `native_hooks_cellengine.cpp` |
| `d2_rle_verify_exit` | game | `native_hooks_cellengine.cpp` |
| `dcc_alloc_ret` | game | `native_hooks_codec.cpp` |
| `dcc_decode_114` | game | `native_hooks_codec.cpp` |
| `dcc_enter` | game | `native_hooks_codec.cpp` |
| `dcc_exit` | game | `native_hooks_codec.cpp` |
| `dcc_verify_exit` | game | `native_hooks_codec.cpp` |
| `fog_raise_snap` | game | `native_hooks_codec.cpp` |
| `memintrin_verify_exit` | game | `native_hooks_codec.cpp` |
| `native!adpcm_mono_114` | game | `native_hooks_codec.cpp` |
| `native!adpcm_stereo_114` | game | `native_hooks_codec.cpp` |
| `native!huff_114` | game | `native_hooks_codec.cpp` |
| `pp_draw_exit` | game | `phase_hooks.cpp` |
| `pp_sim_exit` | game | `phase_hooks.cpp` |
| `pp_tour` | game | `phase_hooks.cpp` |
| `pp_tour_exit` | game | `phase_hooks.cpp` |
| `proj_verify_exit` | game | `native_hooks_codec.cpp` |
| `r60_cam` | game | `phase_hooks.cpp` |
| `r60_cam_exit` | game | `phase_hooks.cpp` |
| `r60_unit` | game | `phase_hooks.cpp` |
| `r60_unit_exit` | game | `phase_hooks.cpp` |
| `room_coord_guard` | game | `native_hooks_codec.cpp` |
| `scomp2_verify_exit` | game | `native_hooks_codec.cpp` |
| `scomp_explode2_114` | game | `native_hooks_codec.cpp` |
| `scomp_explode_114` | game | `native_hooks_codec.cpp` |
| `sim_step` | game | `phase_hooks.cpp` |

### `WS2_32.dll`

31 keys — 31 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `#1` | engine | `win32_shims_wsock32.cpp` |
| `#2` | engine | `win32_shims_wsock32.cpp` |
| `#3` | engine | `win32_shims_wsock32.cpp` |
| `#4` | engine | `win32_shims_wsock32.cpp` |
| `#5` | engine | `win32_shims_wsock32.cpp` |
| `#6` | engine | `win32_shims_wsock32.cpp` |
| `#7` | engine | `win32_shims_wsock32.cpp` |
| `#8` | engine | `win32_shims_wsock32.cpp` |
| `#9` | engine | `win32_shims_wsock32.cpp` |
| `#10` | engine | `win32_shims_wsock32.cpp` |
| `#11` | engine | `win32_shims_wsock32.cpp` |
| `#12` | engine | `win32_shims_wsock32.cpp` |
| `#13` | engine | `win32_shims_wsock32.cpp` |
| `#14` | engine | `win32_shims_wsock32.cpp` |
| `#15` | engine | `win32_shims_wsock32.cpp` |
| `#16` | engine | `win32_shims_wsock32.cpp` |
| `#17` | engine | `win32_shims_wsock32.cpp` |
| `#18` | engine | `win32_shims_wsock32.cpp` |
| `#19` | engine | `win32_shims_wsock32.cpp` |
| `#20` | engine | `win32_shims_wsock32.cpp` |
| `#21` | engine | `win32_shims_wsock32.cpp` |
| `#22` | engine | `win32_shims_wsock32.cpp` |
| `#23` | engine | `win32_shims_wsock32.cpp` |
| `#52` | engine | `win32_shims_wsock32.cpp` |
| `#57` | engine | `win32_shims_wsock32.cpp` |
| `#101` | engine | `win32_shims_wsock32.cpp` |
| `#111` | engine | `win32_shims_wsock32.cpp` |
| `#112` | engine | `win32_shims_wsock32.cpp` |
| `#115` | engine | `win32_shims_wsock32.cpp` |
| `#116` | engine | `win32_shims_wsock32.cpp` |
| `#151` | engine | `win32_shims_wsock32.cpp` |

### `WSOCK32.dll`

31 keys — 31 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `#1` | engine | `win32_shims_wsock32.cpp` |
| `#2` | engine | `win32_shims_wsock32.cpp` |
| `#3` | engine | `win32_shims_wsock32.cpp` |
| `#4` | engine | `win32_shims_wsock32.cpp` |
| `#5` | engine | `win32_shims_wsock32.cpp` |
| `#6` | engine | `win32_shims_wsock32.cpp` |
| `#7` | engine | `win32_shims_wsock32.cpp` |
| `#8` | engine | `win32_shims_wsock32.cpp` |
| `#9` | engine | `win32_shims_wsock32.cpp` |
| `#10` | engine | `win32_shims_wsock32.cpp` |
| `#11` | engine | `win32_shims_wsock32.cpp` |
| `#12` | engine | `win32_shims_wsock32.cpp` |
| `#13` | engine | `win32_shims_wsock32.cpp` |
| `#14` | engine | `win32_shims_wsock32.cpp` |
| `#15` | engine | `win32_shims_wsock32.cpp` |
| `#16` | engine | `win32_shims_wsock32.cpp` |
| `#17` | engine | `win32_shims_wsock32.cpp` |
| `#18` | engine | `win32_shims_wsock32.cpp` |
| `#19` | engine | `win32_shims_wsock32.cpp` |
| `#20` | engine | `win32_shims_wsock32.cpp` |
| `#21` | engine | `win32_shims_wsock32.cpp` |
| `#22` | engine | `win32_shims_wsock32.cpp` |
| `#23` | engine | `win32_shims_wsock32.cpp` |
| `#52` | engine | `win32_shims_wsock32.cpp` |
| `#57` | engine | `win32_shims_wsock32.cpp` |
| `#101` | engine | `win32_shims_wsock32.cpp` |
| `#111` | engine | `win32_shims_wsock32.cpp` |
| `#112` | engine | `win32_shims_wsock32.cpp` |
| `#115` | engine | `win32_shims_wsock32.cpp` |
| `#116` | engine | `win32_shims_wsock32.cpp` |
| `#151` | engine | `win32_shims_wsock32.cpp` |

### `IMM32.dll`

11 keys — 11 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `ImmGetCandidateListA` | engine | `win32_shims_misc.cpp` |
| `ImmGetCandidateListCountA` | engine | `win32_shims_misc.cpp` |
| `ImmGetCompositionStringA` | engine | `win32_shims_misc.cpp` |
| `ImmGetContext` | engine | `win32_shims_misc.cpp` |
| `ImmGetConversionStatus` | engine | `win32_shims_misc.cpp` |
| `ImmGetOpenStatus` | engine | `win32_shims_misc.cpp` |
| `ImmIsIME` | engine | `win32_shims_misc.cpp` |
| `ImmReleaseContext` | engine | `win32_shims_misc.cpp` |
| `ImmSetConversionStatus` | engine | `win32_shims_misc.cpp` |
| `ImmSetOpenStatus` | engine | `win32_shims_misc.cpp` |
| `ImmSimulateHotKey` | engine | `win32_shims_misc.cpp` |

### `binkw32.dll`

9 keys — 9 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `_BinkClose@4` | engine | `win32_shims_misc.cpp` |
| `_BinkCopyToBuffer@28` | engine | `win32_shims_misc.cpp` |
| `_BinkDDSurfaceType@4` | engine | `win32_shims_misc.cpp` |
| `_BinkDoFrame@4` | engine | `win32_shims_misc.cpp` |
| `_BinkNextFrame@4` | engine | `win32_shims_misc.cpp` |
| `_BinkOpen@8` | engine | `win32_shims_misc.cpp` |
| `_BinkOpenDirectSound@4` | engine | `win32_shims_misc.cpp` |
| `_BinkSetSoundSystem@8` | engine | `win32_shims_misc.cpp` |
| `_BinkWait@4` | engine | `win32_shims_misc.cpp` |

### `smackw32.dll`

6 keys — 6 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `_SmackClose@4` | engine | `win32_shims_misc.cpp` |
| `_SmackDoFrame@4` | engine | `win32_shims_misc.cpp` |
| `_SmackNextFrame@4` | engine | `win32_shims_misc.cpp` |
| `_SmackOpen@12` | engine | `win32_shims_misc.cpp` |
| `_SmackToBuffer@28` | engine | `win32_shims_misc.cpp` |
| `_SmackWait@4` | engine | `win32_shims_misc.cpp` |

### `d2vhost.dll`

4 keys — 0 engine, 4 game.

| Function | Served by | Unit |
|---|---|---|
| `d2vGlideFlush` | game | `rt_boot.cpp` |
| `d2vGlideInit` | game | `rt_boot.cpp` |
| `d2vGlideNop` | game | `rt_boot.cpp` |
| `d2vGlideTexUpload` | game | `rt_boot.cpp` |

### `VERSION.dll`

4 keys — 4 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `GetFileVersionInfoSizeW` | engine | `win32_shims_version.cpp` |
| `GetFileVersionInfoW` | engine | `win32_shims_version.cpp` |
| `VerQueryValueA` | engine | `win32_shims_version.cpp` |
| `VerQueryValueW` | engine | `win32_shims_version.cpp` |

### `ijl11.dll`

3 keys — 3 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `#2` | engine | `win32_shims_misc.cpp` |
| `#3` | engine | `win32_shims_misc.cpp` |
| `#5` | engine | `win32_shims_misc.cpp` |

### `SHELL32.dll`

3 keys — 2 engine, 1 game.

| Function | Served by | Unit |
|---|---|---|
| `SHAppBarMessage` | engine | `win32_shims_shell32.cpp` |
| `ShellExecuteA` | engine | `win32_shims_shell32.cpp` |
| `SHGetFolderPathA` | game | `win32_shims_shell32_d2.cpp` |

### `WINMM.dll`

3 keys — 0 engine, 3 game.

| Function | Served by | Unit |
|---|---|---|
| `timeBeginPeriod` | game | `kernel32_time.cpp` |
| `timeEndPeriod` | game | `kernel32_time.cpp` |
| `timeGetTime` | game | `kernel32_time.cpp` |

### `CRYPT32.dll`

2 keys — 0 engine, 2 game.

| Function | Served by | Unit |
|---|---|---|
| `CryptBinaryToStringW` | game | `checkrevision_crypto.cpp` |
| `CryptStringToBinaryW` | game | `checkrevision_crypto.cpp` |

### `DDRAW.dll`

2 keys — 2 engine, 0 game.

| Function | Served by | Unit |
|---|---|---|
| `DirectDrawCreate` | engine | `win32_shims_misc.cpp` |
| `DirectDrawEnumerateA` | engine | `win32_shims_misc.cpp` |

### `PSAPI.DLL`

2 keys — 1 engine, 1 game.

| Function | Served by | Unit |
|---|---|---|
| `GetModuleFileNameExA` | game | `win32_import_remainder.cpp` |
| `GetModuleInformation` | engine | `win32_shims_psapi.cpp` |

### `ole32.dll`

1 keys — 0 engine, 1 game.

| Function | Served by | Unit |
|---|---|---|
| `CoTaskMemFree` | game | `win32_import_remainder.cpp` |

---

`native.hook` is not a DLL: these are the native replacements placed
at precise addresses of the game binary (hot-path ports).
They are specific to Diablo II by construction.
