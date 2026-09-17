// tools/mpq_probe — check specific filenames across MPQs (works even
// when the target isn't in the archive's (listfile)). Useful because
// D2 patch/data MPQs frequently omit paths from the listfile.
//
// Usage: mpq_probe <mpq-dir> <path1> [path2 ...]
// Prints one line per (archive, path) pair: FOUND (bytes) or MISS.

#include <StormLib.h>
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <vector>

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <mpq-dir> <path1> [path2 ...]\n", argv[0]);
        return 2;
    }
    std::vector<std::string> mpqs;
    DIR* d = opendir(argv[1]);
    if (!d) { std::perror("opendir"); return 1; }
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (lower(n).size() < 4 ||
            lower(n).compare(lower(n).size() - 4, 4, ".mpq") != 0) continue;
        mpqs.push_back(std::string(argv[1]) + "/" + n);
    }
    closedir(d);
    std::sort(mpqs.begin(), mpqs.end());

    for (const auto& mpq : mpqs) {
        HANDLE h = nullptr;
        if (!SFileOpenArchive(mpq.c_str(), 0, MPQ_OPEN_READ_ONLY, &h)) continue;
        for (int i = 2; i < argc; ++i) {
            const char* path = argv[i];
            if (SFileHasFile(h, const_cast<char*>(path))) {
                HANDLE f = nullptr;
                DWORD sz = 0;
                if (SFileOpenFileEx(h, path, 0, &f)) {
                    sz = SFileGetFileSize(f, nullptr);
                    SFileCloseFile(f);
                }
                std::printf("%s :: %s  FOUND (%u bytes)\n",
                             mpq.c_str(), path, unsigned(sz));
            }
        }
        SFileCloseArchive(h);
    }
    return 0;
}
