// src/crashreport/cr_io_posix.cpp — POSIX IoApi, used by the PC tests.
//
// This file is also the reference for the IoApi contract that the Vita
// implementation (sceIo*) must mirror, because the library code relies on it:
//   read_file       false if missing, unreadable, or LARGER than max_bytes
//                   (never a silently truncated read);
//   read_head_tail  whole file if size <= head + tail, otherwise the first
//                   `head` bytes followed directly by the last `tail` bytes;
//   write_file      creates or truncates; false if the directory is missing;
//   rename          moves; false if the destination already exists (callers
//                   remove first, so no platform-specific replace semantics);
//   remove          a file, or an EMPTY directory;
//   mkdir           one level; true if the directory already exists;
//   list_dir        entries without "." and "..", sorted by name;
//   stat            name = last path component, mtime in Unix seconds;
//   read_stream     one buffer of `chunk` bytes, callback per chunk; false if
//                   the file is missing, chunk == 0, a read fails, or the
//                   callback aborts.
#include "crashreport/cr_io.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace d2cr {

namespace {

bool file_size(FILE* f, uint64_t* out) {
    struct stat st;
    if (::fstat(fileno(f), &st) != 0) return false;
    *out = (uint64_t)st.st_size;
    return true;
}

bool read_exact(FILE* f, size_t n, std::string* out) {
    const size_t old = out->size();
    out->resize(old + n);
    if (n && std::fread(&(*out)[old], 1, n, f) != n) { out->resize(old); return false; }
    return true;
}

std::string base_name(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

void fill_entry(const std::string& name, const struct stat& st, DirEntry* e) {
    e->name = name;
    e->is_dir = S_ISDIR(st.st_mode);
    e->size = e->is_dir ? 0 : (uint64_t)st.st_size;
    e->mtime_unix = (int64_t)st.st_mtime;
}

class PosixIo final : public IoApi {
 public:
    bool read_file(const std::string& path, std::string* out, size_t max_bytes) override {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint64_t size = 0;
        bool ok = file_size(f, &size) && size <= max_bytes;
        std::string data;
        if (ok) ok = read_exact(f, (size_t)size, &data);
        std::fclose(f);
        if (ok) out->swap(data);
        return ok;
    }

    bool read_head_tail(const std::string& path, size_t head, size_t tail, std::string* out) override {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint64_t size = 0;
        std::string data;
        bool ok = file_size(f, &size);
        if (ok && size <= (uint64_t)head + tail) {
            ok = read_exact(f, (size_t)size, &data);
        } else if (ok) {
            ok = read_exact(f, head, &data) &&
                 std::fseek(f, (long)(size - tail), SEEK_SET) == 0 &&
                 read_exact(f, tail, &data);
        }
        std::fclose(f);
        if (ok) out->swap(data);
        return ok;
    }

    bool write_file(const std::string& path, const std::string& data) override {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = (std::fclose(f) == 0) && ok;
        return ok;
    }

    bool rename(const std::string& from, const std::string& to) override {
        struct stat st;
        if (::lstat(to.c_str(), &st) == 0) return false;
        return std::rename(from.c_str(), to.c_str()) == 0;
    }

    bool remove(const std::string& path) override {
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0) return false;
        return S_ISDIR(st.st_mode) ? ::rmdir(path.c_str()) == 0 : ::unlink(path.c_str()) == 0;
    }

    bool mkdir(const std::string& path) override {
        if (::mkdir(path.c_str(), 0777) == 0) return true;
        struct stat st;
        return errno == EEXIST && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool list_dir(const std::string& path, std::vector<DirEntry>* out) override {
        DIR* dir = ::opendir(path.c_str());
        if (!dir) return false;
        std::vector<DirEntry> es;
        while (struct dirent* de = ::readdir(dir)) {
            const std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            struct stat st;
            if (::stat((path + "/" + name).c_str(), &st) != 0) continue;
            DirEntry e;
            fill_entry(name, st, &e);
            es.push_back(e);
        }
        ::closedir(dir);
        std::sort(es.begin(), es.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
        out->swap(es);
        return true;
    }

    bool stat(const std::string& path, DirEntry* out) override {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return false;
        fill_entry(base_name(path), st, out);
        return true;
    }

    bool read_stream(const std::string& path, size_t chunk,
                     bool (*cb)(const uint8_t*, size_t, void*), void* ud) override {
        if (chunk == 0 || !cb) return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::unique_ptr<uint8_t[]> buf(new uint8_t[chunk]);
        bool ok = true;
        for (;;) {
            const size_t n = std::fread(buf.get(), 1, chunk, f);
            if (n > 0 && !cb(buf.get(), n, ud)) { ok = false; break; }
            if (n < chunk) { ok = !std::ferror(f); break; }
        }
        std::fclose(f);
        return ok;
    }
};

}  // namespace

std::unique_ptr<IoApi> make_posix_io() { return std::unique_ptr<IoApi>(new PosixIo()); }

}  // namespace d2cr
