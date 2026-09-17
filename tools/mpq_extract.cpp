// tools/mpq_extract — pull one file out of an MPQ archive to disk.
// Usage: mpq_extract <mpq> <internal-path> <output-path>
#include <StormLib.h>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s <mpq> <internal-path> <output-path>\n", argv[0]);
        return 2;
    }
    HANDLE h = nullptr;
    if (!SFileOpenArchive(argv[1], 0, MPQ_OPEN_READ_ONLY, &h)) {
        std::fprintf(stderr, "open mpq failed\n"); return 1;
    }
    HANDLE f = nullptr;
    if (!SFileOpenFileEx(h, argv[2], 0, &f)) {
        std::fprintf(stderr, "file not in archive\n");
        SFileCloseArchive(h); return 1;
    }
    DWORD sz = SFileGetFileSize(f, nullptr);
    std::vector<uint8_t> buf(sz);
    DWORD got = 0;
    SFileReadFile(f, buf.data(), sz, &got, nullptr);
    SFileCloseFile(f);
    SFileCloseArchive(h);
    FILE* o = std::fopen(argv[3], "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    std::fwrite(buf.data(), 1, got, o);
    std::fclose(o);
    std::fprintf(stderr, "wrote %s (%u bytes)\n", argv[3], unsigned(got));
    return 0;
}
