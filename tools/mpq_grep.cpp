// tools/mpq_grep — enumerate every file across a list of MPQs and print
// entries whose lowercased path matches ALL of the given substrings.
// Used to hunt down FrontEnd DC6s, fonts, and TBL strings without having
// to open the archives in a viewer.
//
// Usage:
//   mpq_grep <mpq-dir> <substr1> [substr2 ...]
//
// Prints one line per match: <archive>: <path>  <size>

#include <StormLib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool contains_all(const std::string& hay,
                  const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (hay.find(n) == std::string::npos) return false;
    }
    return true;
}

std::vector<std::string> list_mpqs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        const std::string ln = to_lower(n);
        if (ln.size() < 4) continue;
        if (ln.compare(ln.size() - 4, 4, ".mpq") != 0) continue;
        out.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

int scan(const std::string& mpq_path,
         const std::vector<std::string>& needles) {
    HANDLE h = nullptr;
    if (!SFileOpenArchive(mpq_path.c_str(), 0, MPQ_OPEN_READ_ONLY, &h)) {
        std::fprintf(stderr, "open failed: %s\n", mpq_path.c_str());
        return 0;
    }
    int matches = 0;
    SFILE_FIND_DATA fd{};
    HANDLE fh = SFileFindFirstFile(h, "*", &fd, nullptr);
    if (fh) {
        do {
            const std::string name(fd.cFileName);
            if (contains_all(to_lower(name), needles)) {
                std::printf("%s :: %s  (%u bytes)\n",
                             mpq_path.c_str(), name.c_str(),
                             unsigned(fd.dwFileSize));
                ++matches;
            }
        } while (SFileFindNextFile(fh, &fd));
        SFileFindClose(fh);
    }
    SFileCloseArchive(h);
    return matches;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "Usage: %s <mpq-dir> <substr1> [substr2 ...]\n"
                     "  Substrings are matched case-insensitively; ALL must match.\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::vector<std::string> needles;
    for (int i = 2; i < argc; ++i) needles.push_back(to_lower(argv[i]));

    const auto mpqs = list_mpqs(dir);
    if (mpqs.empty()) {
        std::fprintf(stderr, "no .mpq files found in %s\n", dir.c_str());
        return 1;
    }
    int total = 0;
    for (const auto& p : mpqs) total += scan(p, needles);
    std::fprintf(stderr, "-- %d matches across %zu archives --\n",
                 total, mpqs.size());
    return 0;
}
