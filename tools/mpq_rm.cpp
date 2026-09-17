// tools/mpq_rm.cpp — remove one or more named files from an existing MPQ.
// Used to produce a DLL-only copy of d2patch.mpq (drop patch.lst so BNUpdate
// applies only the hdfiles/DLL portion of the official patch — the data-MPQ
// portion is irrelevant to extracting the 1.13c DLLs and fails on our install's
// mismatched game data).
//
//   mpq_rm <archive.mpq> <internal-name> [<internal-name> ...]
#include <StormLib.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: mpq_rm <mpq> <name> [name...]\n"); return 2; }
    HANDLE h = nullptr;
    if (!SFileOpenArchive(argv[1], 0, 0, &h)) {
        std::printf("open failed: %s (err %u)\n", argv[1], SErrGetLastError());
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; ++i) {
        if (SFileRemoveFile(h, argv[i], 0)) {
            std::printf("removed: %s\n", argv[i]);
        } else {
            std::printf("NOT removed: %s (err %u)\n", argv[i], SErrGetLastError());
            rc = 1;
        }
    }
    SFileCompactArchive(h, nullptr, false);
    SFileCloseArchive(h);
    return rc;
}
