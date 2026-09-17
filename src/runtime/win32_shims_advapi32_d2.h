// win32_shims_advapi32_d2.h -- ADVAPI32 shims that stay specific to d2vita:
// full registry emulation (hardcoded "C:\Diablo II\..." paths, persistence in
// registry.txt, D2_PERSPECTIVE knob) plus the handful of security/SCM calls
// that depend on misc()/set_lasterr() (helpers hosted by d2vita, not winx86 --
// see the file header of third_party/winx86/src/runtime/win32_shims_advapi32.cpp).
// The purely generic subset of the same original group (about thirty
// stateless security/ACL/SCM functions with no D2 literal) lives in that
// winx86 file.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_advapi32_d2_install(d2rt::Bridge& br);

// Effective paths of the two registry persistence files. Exposed for the
// D2KEYSTORETEST self-test, which must verify ON DISK that the CD key is not
// in registry.txt and is indeed in the separate keystore. Valid only after
// win32_shims_advapi32_d2_install().
const char* d2_registry_file();
const char* d2_keystore_file();
