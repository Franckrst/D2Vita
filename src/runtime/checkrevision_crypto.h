// checkrevision_crypto.h -- CRYPT32/ADVAPI32 CryptoAPI shims and the real
// Authenticode/VERSION wiring the lockdown CheckRevision.dll needs to run to
// completion: CryptBinaryToStringW, CryptStringToBinaryW (CRYPT32.dll),
// CryptAcquireContextW/CryptReleaseContext/CryptCreateHash/CryptHashData/
// CryptGetHashParam/CryptDestroyHash (ADVAPI32.dll, real streaming SHA-1),
// plus wiring win32_shims_version_install()/win32_shims_wintrust_install()
// to real, guest-path-resolved file sources.
//
// SECURITY-SENSITIVE: CryptHashData/CryptGetHashParam are where the
// D2CR_REQUIRE_VERSION safety net (armed by the netguard network lock in
// tools/rt_boot.cpp, see src/runtime/netguard_lock.cpp) is actually
// enforced -- if the string CheckRevision hashes doesn't match, the network
// is cut (wx86_net_set_enabled(false)) before any traffic goes out. Moved
// here VERBATIM, zero logic change; the coupling with netguard_lock is only
// through the D2CR_REQUIRE_VERSION environment variable (set by one, read by
// the other), which is unaffected by which translation unit reads/writes it,
// and both sides run only from Bridge shim callbacks well after all of
// main()'s registration-time setup has completed -- so relocating this
// registration call changes no observable behavior or order.
#pragma once
namespace d2rt { class Bridge; }

void checkrevision_crypto_install(d2rt::Bridge& br);
