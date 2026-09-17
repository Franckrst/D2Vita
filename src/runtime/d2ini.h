// d2ini.h -- D2.ini: the same 58 options as the Diablo II command line, read
// BEFORE it (GetPrivateProfileStringA/IntA shims, see d2ini.cpp). Wired to a
// real file via host_path(); a missing file or key keeps the caller's
// default, so behavior is unchanged without a D2.ini on disk.
#pragma once
#include <string>

// Returns true + the raw value when the key REALLY exists; false otherwise
// (the caller then serves the guest's default, as Win32 does).
bool ini_lookup(const std::string& guestPath, const std::string& section,
                 const std::string& key, std::string& out);
