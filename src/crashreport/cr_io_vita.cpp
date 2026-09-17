// src/crashreport/cr_io_vita.cpp — Vita IoApi, sceIo* calls.
//
// Mirrors the contract cr_io_posix.cpp documents at the top of that file:
//   read_file       false if missing, unreadable, or LARGER than max_bytes;
//   read_head_tail  whole file if size <= head + tail, else head then tail;
//   write_file      creates or truncates;
//   rename          sceIoRename; false if the destination already exists
//                   (callers remove first — spec: std::rename is mortal on
//                   the Vita libc, sceIoRename is the one this project uses);
//   remove          a file, or an EMPTY directory;
//   mkdir           one level; true if the directory already exists;
//   list_dir        entries without "." and "..", sorted by name;
//   stat            name = last path component, mtime in Unix seconds
//                   (SceDateTime -> SceRtcTick -> the same epoch subtraction
//                   as rt_boot.cpp's d2vita_wall_unix());
//   read_stream     one buffer of `chunk` bytes, callback per chunk.
#include "crashreport/cr_io.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

namespace d2cr {

namespace {

// Same conversion as tools/rt_boot.cpp d2vita_wall_unix(): SceRtcTick is
// microseconds since 0000-01-01; 62135596800 is that epoch's distance from
// 1970-01-01 in seconds. Clamped to 0 rather than going negative — a
// pre-1970 stamp has no Unix meaning and callers only compare mtimes.
int64_t datetime_to_unix(const SceDateTime& dt) {
    SceDateTime d = dt;
    SceRtcTick t; t.tick = 0;
    if (sceRtcGetTick(&d, &t) < 0) return 0;
    long long unixsec = (long long)(t.tick / 1000000ull) - 62135596800ll;
    return unixsec > 0 ? unixsec : 0;
}

std::string base_name(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

void fill_entry(const std::string& name, const SceIoStat& st, DirEntry* e) {
    e->name = name;
    e->is_dir = SCE_S_ISDIR(st.st_mode) != 0;
    e->size = e->is_dir ? 0 : (uint64_t)st.st_size;
    e->mtime_unix = datetime_to_unix(st.st_mtime);
}

class VitaIo final : public IoApi {
 public:
    bool read_file(const std::string& path, std::string* out, size_t max_bytes) override {
        SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        if (fd < 0) return false;
        SceIoStat st; std::memset(&st, 0, sizeof st);
        bool ok = sceIoGetstatByFd(fd, &st) >= 0 && (uint64_t)st.st_size <= (uint64_t)max_bytes;
        std::string data;
        if (ok) ok = read_exact(fd, (size_t)st.st_size, &data);
        sceIoClose(fd);
        if (ok) out->swap(data);
        return ok;
    }

    bool read_head_tail(const std::string& path, size_t head, size_t tail, std::string* out) override {
        SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        if (fd < 0) return false;
        SceIoStat st; std::memset(&st, 0, sizeof st);
        bool ok = sceIoGetstatByFd(fd, &st) >= 0;
        std::string data;
        if (ok && (uint64_t)st.st_size <= (uint64_t)head + (uint64_t)tail) {
            ok = read_exact(fd, (size_t)st.st_size, &data);
        } else if (ok) {
            ok = read_exact(fd, head, &data) &&
                 sceIoLseek(fd, (SceOff)(st.st_size - (SceOff)tail), SCE_SEEK_SET) >= 0 &&
                 read_exact(fd, tail, &data);
        }
        sceIoClose(fd);
        if (ok) out->swap(data);
        return ok;
    }

    bool write_file(const std::string& path, const std::string& data) override {
        SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd < 0) return false;
        bool ok = write_exact(fd, data);
        // sceIoClose's return is not folded into ok: a write already reported
        // short above is the real failure signal; a close failure after a
        // fully-written file is not treated as data loss here (unlike the
        // POSIX reference's fclose, sceIoClose flushes on every write instead
        // of buffering, so there is nothing left for close to lose).
        sceIoClose(fd);
        return ok;
    }

    bool rename(const std::string& from, const std::string& to) override {
        SceIoStat st; std::memset(&st, 0, sizeof st);
        if (sceIoGetstat(to.c_str(), &st) >= 0) return false;   // destination exists
        return sceIoRename(from.c_str(), to.c_str()) >= 0;
    }

    bool remove(const std::string& path) override {
        SceIoStat st; std::memset(&st, 0, sizeof st);
        if (sceIoGetstat(path.c_str(), &st) < 0) return false;
        return SCE_S_ISDIR(st.st_mode) ? sceIoRmdir(path.c_str()) >= 0 : sceIoRemove(path.c_str()) >= 0;
    }

    bool mkdir(const std::string& path) override {
        if (sceIoMkdir(path.c_str(), 0777) >= 0) return true;
        SceIoStat st; std::memset(&st, 0, sizeof st);
        return sceIoGetstat(path.c_str(), &st) >= 0 && SCE_S_ISDIR(st.st_mode);
    }

    bool list_dir(const std::string& path, std::vector<DirEntry>* out) override {
        SceUID fd = sceIoDopen(path.c_str());
        if (fd < 0) return false;
        std::vector<DirEntry> es;
        for (;;) {
            SceIoDirent de; std::memset(&de, 0, sizeof de);
            int rc = sceIoDread(fd, &de);
            if (rc <= 0) break;   // 0 = no more entries, < 0 = error: stop either way
            const std::string name = de.d_name;
            if (name == "." || name == "..") continue;
            DirEntry e;
            fill_entry(name, de.d_stat, &e);
            es.push_back(e);
        }
        sceIoDclose(fd);
        std::sort(es.begin(), es.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
        out->swap(es);
        return true;
    }

    bool stat(const std::string& path, DirEntry* out) override {
        SceIoStat st; std::memset(&st, 0, sizeof st);
        if (sceIoGetstat(path.c_str(), &st) < 0) return false;
        fill_entry(base_name(path), st, out);
        return true;
    }

    bool read_stream(const std::string& path, size_t chunk,
                     bool (*cb)(const uint8_t*, size_t, void*), void* ud) override {
        if (chunk == 0 || !cb) return false;
        SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        if (fd < 0) return false;
        std::unique_ptr<uint8_t[]> buf(new uint8_t[chunk]);
        bool ok = true;
        for (;;) {
            int n = sceIoRead(fd, buf.get(), (SceSize)chunk);
            if (n < 0) { ok = false; break; }
            if (n > 0 && !cb(buf.get(), (size_t)n, ud)) { ok = false; break; }
            if ((size_t)n < chunk) break;   // short read: end of file
        }
        sceIoClose(fd);
        return ok;
    }

 private:
    static bool read_exact(SceUID fd, size_t n, std::string* out) {
        const size_t old = out->size();
        out->resize(old + n);
        size_t got = 0;
        while (got < n) {
            int r = sceIoRead(fd, &(*out)[old + got], (SceSize)(n - got));
            if (r <= 0) { out->resize(old); return false; }   // short/failed read: never a silent partial
            got += (size_t)r;
        }
        return true;
    }

    static bool write_exact(SceUID fd, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int w = sceIoWrite(fd, data.data() + sent, (SceSize)(data.size() - sent));
            if (w <= 0) return false;
            sent += (size_t)w;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IoApi> make_vita_io() { return std::unique_ptr<IoApi>(new VitaIo()); }

}  // namespace d2cr
