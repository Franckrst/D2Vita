# Third-party components

Code you write in this repository is GPL-3.0 (see [LICENSE](LICENSE)). This
page covers everything else linked into the shipped binary.

## The shared engine (`third_party/winx86`)

The generic runtime — PE32 loader, Box86-derived ARMv7 dynarec, Bridge,
schedulers — lives in a separate project, consumed here as a git submodule.
Its own third-party dependencies (Box86 dynarec core, `khash.h`, the Realmode
X86 Emulator Library) are winx86's to track, not d2-vita's: see
[`third_party/winx86/THIRD-PARTY-NOTICES.md`](third_party/winx86/THIRD-PARTY-NOTICES.md)
for that inventory and the corresponding license texts. This file does not
duplicate it.

## What d2-vita itself adds

### Monocypher

Used by the crash-report subsystem for response verification and signing
(`src/crashreport/cr_monocypher.c`, `cr_verify.cpp`, `cr_seal.cpp`).

- License: dual CC0-1.0 / BSD-2-Clause (upstream author's choice; either
  applies).
- Text: [`third_party/monocypher/LICENCE.md`](third_party/monocypher/LICENCE.md).

### zlib

Used by the crash-report subsystem for dump compression
(`src/crashreport/cr_psp2dmp.cpp`). Not vendored in this repository — linked
from VitaSDK's own sysroot (`arm-vita-eabi/include/zlib.h`), the same bucket
as newlib/libstdc++ below. Reproduced here because zlib's license lives
inline in its header rather than as a separate file:

```
zlib.h -- interface of the 'zlib' general purpose compression library

Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Jean-loup Gailly        Mark Adler
jloup@gzip.org          madler@alumni.caltech.edu
```

### VitaSDK runtime (newlib / libstdc++)

Linked for the C/C++ standard library on-target. Covered by VitaSDK's own
licensing (a mix of permissive licenses inherited from newlib and GCC's
runtime library exception) — not an obligation this repository reproduces;
see the [VitaSDK project](https://vitasdk.org/) for its own terms.

## Desktop-only tooling (not shipped in the VPK)

`StormLib` and the zlib/bzip2/libtommath it bundles are used by the
desktop MPQ-inspection tools (`mpq_grep`, `mpq_probe`, etc., built via the
top-level `CMakeLists.txt`). They are not linked into the Vita binary and
carry no obligation for the VPK; they matter only if you build and
redistribute those desktop tools yourself.
