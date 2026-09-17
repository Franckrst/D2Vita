// kernel32_fsinfo.h -- KERNEL32 filesystem/volume/system-info stubs:
// GetDiskFreeSpaceA/ExA, GetFullPathNameA, GetVolumeInformationA,
// GetLogicalDriveStringsA, GetLogicalDrives, GetCurrentDirectoryA,
// GetComputerNameA, GetTempPathW. All stateless, one-shot answers (no shared
// local helper, no global state) -- grouped as one "what does this fake C:
// drive look like" responsibility.
#pragma once
namespace d2rt { class Bridge; }

void kernel32_fsinfo_install(d2rt::Bridge& br);
