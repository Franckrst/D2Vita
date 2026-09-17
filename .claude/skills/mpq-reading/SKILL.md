---
name: mpq-reading
description: Use when reading Blizzard MPQ archives (Diablo II d2data.mpq/d2exp.mpq, patch_d2.mpq, Warcraft, StarCraft) — extracting files by name, listing contents, or diagnosing "file not found in MPQ" / bad hash / decompression failures.
---

# mpq-reading

## Overview

MPQ (Mo'PaQ) is Blizzard's archive format. Files are located via a hash table (name → block index) rather than a directory. Data blocks are optionally compressed (multi-algo) and/or encrypted (key derived from filename). D2 uses **MPQ v1** — the simpler variant, no HET/BET tables. `patch_d2.mpq` overrides files in `d2data.mpq` / `d2exp.mpq`.

## When to Use

- Loading Diablo II data at runtime
- Building a tool to list/extract from user-supplied MPQs
- Errors: `MPQ header not found`, `file not in archive`, `bad hash`, `decompression failed`, `unknown compression 0x??`
- Choosing between StormLib, libmpq, and rolling your own
- Diagnosing why `patch_d2.mpq` overrides don't apply

## Key facts

| Fact | Value |
|---|---|
| Header signature | `MPQ\x1A` (0x1A51504D LE) |
| D2 archive version | 1 (16-byte extension header, no v2/v3/v4 fields) |
| Sector size | `512 << header.sector_size_shift` bytes (typically 4 KiB) |
| Hash table entry | 16 bytes: `hash_a`, `hash_b`, locale, platform, block_index |
| Block table entry | 16 bytes: `file_pos`, `csize`, `fsize`, `flags` |
| Filename hash | Blizzard-specific algorithm; 3 hashes per filename (offset/verify_a/verify_b) — see StormLib `HashString` |
| Encryption key | Derived from the base filename (no path) unless `MPQ_FILE_FIX_KEY` — then also from `file_pos` and `fsize` |
| Compressions | 0x01 huffman, 0x02 zlib, 0x08 PKWARE implode, 0x10 bzip2, 0x40/0x80 audio — bitmask, applied in a specific order |
| Listing files | `(listfile)` entry if present; otherwise you need external filename dictionary |
| D2 load order | `patch_d2.mpq` > `d2exp.mpq` > `d2xmusic.mpq` > `d2xtalk.mpq` > `d2xvideo.mpq` > `d2data.mpq` > `d2char.mpq` > `d2sfx.mpq` > `d2music.mpq` > `d2video.mpq` |

## Library choice

| Lib | License | Verdict for D2Vita |
|---|---|---|
| **StormLib** (Ladislav Zezula) | MIT | ✅ Default choice. C++, portable, actively maintained, handles every MPQ version. Cross-compiles to Vita. |
| libmpq | LGPL-2.1 | ⚠️ Copyleft — viable but constrains D2Vita license. |
| Rolling your own | — | Only if binary size matters (StormLib is ~200 KB stripped). MPQ v1 for D2 is ~800 LoC minimum with all decompressors. |

**Recommendation:** StormLib, statically linked, with `-DFULL=OFF` to strip codecs D2 doesn't use.

## Minimal read (StormLib)

```cpp
#include <StormLib.h>

HANDLE mpq = nullptr;
if (!SFileOpenArchive("ux0:data/d2vita/d2data.mpq", 0, MPQ_OPEN_READ_ONLY, &mpq)) {
    log_error("open MPQ failed: %d", GetLastError());
    return false;
}

// Chain patch_d2.mpq — StormLib merges override transparently
SFileOpenPatchArchive(mpq, "ux0:data/d2vita/patch_d2.mpq", nullptr, 0);

HANDLE file = nullptr;
if (!SFileOpenFileEx(mpq, "data\\global\\excel\\levels.txt", 0, &file)) {
    log_error("levels.txt not in archive");
    SFileCloseArchive(mpq);
    return false;
}

DWORD size = SFileGetFileSize(file, nullptr);
std::vector<char> buf(size);
DWORD read = 0;
SFileReadFile(file, buf.data(), size, &read, nullptr);

SFileCloseFile(file);
SFileCloseArchive(mpq);
```

## Common mistakes

- **Forgetting `patch_d2.mpq`** — without it, you're reading pre-patch data (broken skills, wrong drop tables).
- **Wrong path separator** — MPQ uses `\`, not `/`. `data\global\excel\...` even on Linux/Vita.
- **Case sensitivity** — the hash algo lowercases and slash-normalizes the name. Passing `Data/Global/…` still works via StormLib; your own hasher must normalize first.
- **Loading whole archive into RAM** — d2data.mpq is ~250 MB. On Vita (512 MB total, ~256 MB usable) this OOMs. Stream reads or cache per-file with an LRU.
- **Trusting the sector count** — a "single-unit" file (flag `MPQ_FILE_SINGLE_UNIT`) has no sector table; treat separately.
- **Ignoring `MPQ_FILE_FIX_KEY`** — some files' encryption key depends on their block offset. Wrong key = garbage output that looks half-decompressed.
- **Assuming `(listfile)` exists** — some MPQs don't carry it. For D2 files you need an external listfile (community-maintained; ship one with D2Vita for asset discovery).

## References

- StormLib source: https://github.com/ladislav-zezula/StormLib
- Zezula's format doc: `StormLib/doc/`
- OpenDiablo2 MPQ parser (Go, MIT) — good cross-check for edge cases
- Zezula's history writeup on the algorithm origins is required reading before implementing yourself
