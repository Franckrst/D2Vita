// tools/mpq_hex — dump the first N bytes of a file inside an MPQ as hex.
// Useful for validating parser assumptions (palette byte order, header
// layout, etc.) without extracting the whole file.
#include <StormLib.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "Usage: %s <mpq> <path> [nbytes=32]\n", argv[0]);
        return 2;
    }
    const int nbytes = (argc == 4) ? std::atoi(argv[3]) : 32;
    HANDLE h = nullptr;
    if (!SFileOpenArchive(argv[1], 0, MPQ_OPEN_READ_ONLY, &h)) return 1;
    HANDLE f = nullptr;
    if (!SFileOpenFileEx(h, argv[2], 0, &f)) return 1;
    std::vector<uint8_t> buf(nbytes);
    DWORD got = 0;
    SFileReadFile(f, buf.data(), nbytes, &got, nullptr);
    SFileCloseFile(f);
    SFileCloseArchive(h);
    for (DWORD i = 0; i < got; ++i) {
        std::printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) std::putchar('\n');
    }
    if (got % 16) std::putchar('\n');
    return 0;
}
